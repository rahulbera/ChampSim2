# ITTAGE Indirect Target Predictor In ChampSim — Implementation Plan

**Date:** 2026-08-09 · **Branch:** `rbdev` · **Authors:** Rahul Bera and Claude Opus 5
**Paper:** A. Seznec, "A 64-Kbytes ITTAGE indirect branch predictor," JWAC-2:
Championship Branch Prediction (CBP-3), 2011.

---

## 1. Why This, Why Now

The CBP2025 campaign measured the ceiling: **branches cost 12.22% of all cycles**
on SPEC `specrate-int`, and **41.6% of that headroom is branch targets, not
directions**. No CBP2025 submission — RUNLTS, DD-TAGE, TAGE-SC-L — addresses any
of it. Those predictors capture 8.1–9.7% of the *direction* headroom and 0% of
the target headroom, by construction.

Breaking down what survives a *perfect direction predictor* (measured, 32 traces):

| Branch type | MPKI | Share of residual |
|---|---|---|
| BRANCH_CONDITIONAL (target miss on a taken conditional) | 0.604 | 35.2% |
| **BRANCH_INDIRECT** | **0.410** | **23.9%** |
| BRANCH_RETURN | 0.298 | 17.3% |
| BRANCH_DIRECT_JUMP | 0.150 | 8.7% |
| **BRANCH_INDIRECT_CALL** | **0.149** | **8.7%** |
| BRANCH_DIRECT_CALL | 0.107 | 6.2% |

Indirect branches are **0.559 MPKI — 32.6% of the post-perfect-direction
residual, 13.7% of all mispredictions**. That is the ITTAGE target on SPEC.

**On agentic AI workloads it is not 13.7%, it is 86–88%.** From the QEMU capture
campaign (`qemu-tracing/docs/workloads/swe-agent-capture-results.md`):

| Trace | conditional MPKI | indirect MPKI | indirect % of misses |
|---|---|---|---|
| **agent w1** | 0.30 | 2.91 | **86.3%** |
| **agent w2** | 0.25 | 2.95 | **87.7%** |
| agent w0 | 4.88 | 1.61 | 24.4% |
| agent w3 | 0.91 | 0.45 | 25.9% |
| 714.cpython (SPEC proxy) | 0.97 | 1.29 | 41.0% |

In the compute-heavy agent windows conditional prediction is *already solved*
(0.25–0.30 MPKI) and essentially every remaining misprediction is a polymorphic
indirect jump from the dispatch loop. SPEC is the regression set here; the
agentic traces are the target.

**What ChampSim has today is very weak.** `btb/basic_btb/indirect_predictor.h` is
a **tagless, single-table, 4096-entry direct-mapped target cache** indexed by
`PC ^ 12-bit conditional-taken history`. No tags (so no aliasing detection), no
confidence, one history length, and — per the paper's §2.1 — the *wrong history*:
conditional taken/not-taken bits rather than indirect-target path bits.

## 2. Key Finding: Do Not Reimplement — Vendor Seznec's Own Code

`/home/rbera/work/bpeval/cbp2025/lib/ittage.h` (436 lines) is a complete,
working ITTAGE **written by Seznec himself** and shipped inside the CBP2025
framework we already vendor from (`bp.cc` header: *"Modified by A. Seznec … to
include TAGE-SC-L predictor and the ITTAGE indirect branch predictor"*).

Its interface is three methods:

```cpp
uint64_t GetPrediction(uint64_t PC);
void     UpdatePredictor(uint64_t PC, uint64_t branchTarget);
void     TrackOtherInst(uint64_t PC, uint64_t next_pc);   // history for non-indirect branches
```

That is nearly a 1:1 map onto ChampSim's BTB module hooks, and it is the *same
adapter shape* we already built and validated for the four CBP6 conditional
tenants. **Vendor it unmodified, wrap it, document every deviation** — exactly
the `branch/cbp6_*` pattern.

### 2.1 The reference is a smaller ITTAGE than the paper's

| Parameter | Paper (64KB submission) | `cbp2025/lib/ittage.h` |
|---|---|---|
| Tagged tables | 15 (+ tagless T0) | `NHIST 8` (+ table 0, *tagged*) |
| History lengths | {0,0,10,…,2146,3881} | geometric, `MINHIST 2` … `MAXHIST 300` |
| Entries | 14K total | `LOGG 10` → 1K per bank |
| Tag width | 9 / 13 / 15 bits | `TBITS 11` |
| Confidence | 2-bit | `CWIDTH 3` |
| Useful bit | 1-bit | `UWIDTH 2` |
| Target storage | region table (18b offset + 7b pointer) | full `uint64_t` |
| IUM | yes | no |
| Table sharing | yes | no |

So the reference is a practical mid-size ITTAGE, not the championship monster.
That is fine — arguably better as a first data point — but **the storage budget
must be accounted honestly** before any result is quoted, and scaling toward the
paper's configuration is a later milestone, not the starting point.

## 3. Two ChampSim Facts That Simplify The Paper Substantially

**3.1 The IUM (paper §2.3.2) is unnecessary. Do not implement it.**
ChampSim calls `impl_update_btb(...)` from `do_predict_branch`
(`src/ooo_cpu.cc:174`) — the *same function* that produced the prediction at
line 139, at fetch, with the architectural outcome. There is no wrong-path
execution and no delayed update. The entire premise of the IUM ("predictor tables
are updated at retire time to avoid pollution of the predictor by the wrong
path") does not hold. The paper's speculative-history machinery (circular buffer,
separate speculative and retire pointers) is likewise moot.

*Caveat to state in any writeup:* this makes ChampSim's BTB results **optimistic
in absolute terms** for every predictor equally. Comparisons are fair; absolute
numbers are an upper bound.

**3.2 The indirect predictor is gated behind a direct-BTB hit.**
`basic_btb.cc:24` only consults the indirect predictor when
`direct.check_hit(ip)` returns an entry *and* that entry's type is `INDIRECT`.
An indirect branch that missed or was evicted from the 8K-entry direct BTB never
reaches ITTAGE at all — it returns `{address{}, false}` and takes a target
misprediction regardless of how good ITTAGE is. **This caps achievable coverage
and must be measured before tuning ITTAGE itself** (milestone M1.5). If it binds,
the fix is a presence/type structure for indirect branches rather than a larger
direct BTB.

## 4. Design

New module `btb/ittage_btb/`, selected by JSON `"btb": "ittage_btb"`.

```
btb/ittage_btb/
├── ittage_btb.h / .cc        the module: routes types, owns the pieces
├── vendor/ittage.hpp         cbp2025/lib/ittage.h, vendored, .h -> .hpp
└── (reuses direct_predictor + return_stack from basic_btb)
```

Structure mirrors `basic_btb`: a `direct_predictor` for presence/type/direct
targets, a `return_stack` for returns, and **ITTAGE in place of
`indirect_predictor`**.

```cpp
std::pair<champsim::address,bool> ittage_btb::btb_prediction(champsim::address ip)
{
  auto e = direct.check_hit(ip);
  if (!e.has_value())                    return {champsim::address{}, false};
  if (e->type == RETURN)                 return ras.prediction();
  if (e->type == INDIRECT)               return {champsim::address{ittage.GetPrediction(ip.to<uint64_t>())}, true};
  return {e->target, e->type != CONDITIONAL};
}

void ittage_btb::update_btb(ip, target, taken, type)
{
  if (type == BRANCH_DIRECT_CALL || type == BRANCH_INDIRECT_CALL) ras.push(ip);
  if (type == BRANCH_INDIRECT || type == BRANCH_INDIRECT_CALL)
    ittage.UpdatePredictor(ip.to<uint64_t>(), target.to<uint64_t>());
  else
    ittage.TrackOtherInst(ip.to<uint64_t>(), target.to<uint64_t>());   // history only
  if (type == BRANCH_RETURN) ras.calibrate_call_size(target);
  direct.update(ip, target, type);
}
```

Three correctness details, each of which silently degrades accuracy if missed:

- **A not-taken conditional has `branch_target == 0`** (`src/tracereader.cc`).
  Feeding that into `TrackOtherInst` poisons the path history with a constant.
  Either skip not-taken branches or pass the fall-through address.
- **Storage duration.** The CBP6 tenants required static storage because their
  constructors do not zero every member. `IPREDICTOR` has a `reinit()`; call it
  from `initialize_btb()` and verify no member is read before written (the
  RUNLTS campaign found three such members by heap-poisoning — reuse that harness).
- **`GetPrediction` returns `0xdeadbeef` for an unseeded entry** (`ientry()`
  constructor). That must not be handed to ChampSim as a target; treat it as
  "no prediction" or let the confidence gate suppress it. Decide explicitly and
  test it.

## 5. Milestones

**M0 — Measurement scaffolding (half day).** Extend `rollup.py` to report
`BRANCH_INDIRECT + BRANCH_INDIRECT_CALL` MPKI as a first-class metric. Build a
`perfect_indirect` oracle — `perfect_btb` restricted to indirect types — so the
indirect-only ceiling is separable from the total target ceiling we already have.
*Exit:* indirect MPKI reported per trace; indirect ceiling measured.

**M1 — Vendor and wire (1–2 days).** Vendor `ittage.h` → `vendor/ittage.hpp`,
create `btb/ittage_btb/`, document every deviation at its site. Unit tests for
type routing, the `0xdeadbeef` sentinel, and not-taken history handling.
*Exit:* builds; suite green; runs end to end; storage budget computed and written
down.

**M1.5 — Coverage check (half day).** Instrument how often an indirect branch
reaches ITTAGE at all versus being lost to a direct-BTB miss (§3.2). *Exit:* a
number. If coverage is well below 100%, fix that before tuning ITTAGE — otherwise
every later measurement is confounded.

**M2 — Validation (1 day).** Differential replay against the CBP2025 framework
driving the same predictor on the same PC/target sequence, reusing the
`tools/cbp6_replay/` pattern — predictions must be bit-identical. This is the
check that the wrapper, not the algorithm, is correct.
*Exit:* N predictions replayed identically.

**M3 — Evaluate on SPEC (half day + ~2h machine).** 32 traces, three
configurations: `basic_btb` (baseline), `ittage_btb`, `perfect_btb` (ceiling).
Also pair each with `perfect_branch` to isolate the target effect from direction
noise. Report indirect MPKI, overall MPKI, CycWPKI, IPC, and **fraction of
indirect headroom captured** (pooled cycles, per the established recipe).
*Exit:* SPEC numbers, with the caveat that 8 of 14 workloads have ~0 indirect
MPKI and cannot move.

**M4 — Evaluate on agentic traces (half day).** The real target:
`qemu-tracing/images/champsim_out/swe_agent_w0000{0..3}.champsim2.zst`. Validate
them first with `trace_sanity_check --check` (§5 of `cbp6-runs/README.md`).
*Exit:* the number that justifies the work — how much of the 86–88% indirect
miss population ITTAGE removes.

**M5 — Scale toward the paper (optional, 2–3 days).** Only if M3/M4 justify it.
In rough order of expected value: (a) the paper's history vector — 10 bits from
indirect targets + 5 from calls, worth ~16% per §2.1; (b) more tables and longer
histories toward the 64KB configuration; (c) region-table target compression
(§2.2), which is a *storage* optimization, not an accuracy one — it buys budget,
7 bits per entry; (d) table sharing (§2.3.1), worth <1% by the paper's own
admission. **Do not implement the IUM** (§3.1).

## 6. Evaluation Protocol

Reuse the CBP2025 campaign's infrastructure wholesale — `cbp6-runs/run_sweep.sh`,
`rollup.py`, `headroom.py`, the ggplot house style. Same 50M/200M windows, same
SimPoint weighting.

**Trace sets.** SPEC is the regression set but 8 of 14 workloads have ~0.000
indirect MPKI and are dead weight for this study. The six that matter:

| Workload | indirect MPKI |
|---|---|
| 710.omnetpp_r | 1.686 |
| 714.cpython_r | 1.446 |
| 734.vpr_r | 1.120 |
| 753.ns3_r | 1.094 |
| 723.llvm_r | 0.930 |
| 735.gem5_r | 0.854 |

Report the full 14 for honesty, but **lead with these six plus the agentic
traces**, and say plainly that the aggregate over all 14 is diluted by workloads
with no indirect branches to mispredict.

**Aggregation.** Pooled ratio of SimPoint-weighted cycle sums for capture;
geometric mean for speedup. Not an arithmetic mean of per-trace percentages —
that inverted a ranking in the CBP2025 campaign on near-zero-MPKI traces, and
this study has *more* such traces, not fewer.

## 7. Risks

| Risk | Why it matters | Mitigation |
|---|---|---|
| Direct-BTB gating caps coverage | ITTAGE never consulted; result reads as "ITTAGE does not help" | M1.5 measures it before tuning |
| SPEC dilutes the result | 8/14 workloads unmovable; aggregate looks flat | Lead with the 6 + agentic; report both |
| Storage not comparable | ITTAGE ~64KB vs a 32KB target cache — an unfair win | Account both budgets; add an iso-storage point |
| `0xdeadbeef` sentinel leaks | A garbage target predicted as real | Explicit test |
| Optimistic update timing | §3.1 — no wrong-path, no delayed update | State it; the comparison is still fair |
| Agentic traces unvalidated | v2 metadata has shipped broken before | `trace_sanity_check --check` first, always |

## 8. Open Questions For You

1. **Storage budget.** Match the paper's 64KB, or hold iso-storage with the
   current 4K×64-bit (32KB) target cache? I lean: report both, lead with iso-storage,
   since a 2× budget win is not an algorithmic win.
2. **Scope of M5.** Is a faithful 64KB championship ITTAGE a goal, or is
   "a good indirect predictor, well measured" enough? Affects M5 by ~3 days.
3. **Do the agentic traces have SimPoint weights?** If not, per-workload
   aggregation there is unweighted and must be labelled as such.
4. **Returns.** BRANCH_RETURN is 0.298 MPKI on SPEC (17.3% of the residual) and
   0.414 on cpython — handled by the 64-entry RAS, not ITTAGE. Worth a separate
   look, out of scope here.
