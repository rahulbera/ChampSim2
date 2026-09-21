# ITTAGE Indirect Target Predictor In ChampSim — Design

**Date:** 2026-08-09 · **Branch:** `rbdev` · **Authors:** Rahul Bera and Claude Opus 5
**Paper:** A. Seznec, "A 64-Kbytes ITTAGE indirect branch predictor," JWAC-2 / CBP-3, 2011.
**Companion:** `2026-08-09-ittage-indirect-target-predictor-plan.md` (motivation, opportunity sizing).

---

## 1. Problem

Branches cost **12.22% of all cycles** on SPEC `specrate-int`, and **41.6% of that
headroom is targets, not directions**. No CBP2025 submission addresses any of it.
On agentic AI traces the imbalance is extreme: conditional prediction is already
solved (0.25–0.30 MPKI) while indirect branches are **86–88% of all
mispredictions**.

ChampSim's current indirect predictor
(`btb/basic_btb/indirect_predictor.h`) is a tagless, single-table, 4096-entry
direct-mapped target cache indexed by `PC ^ 12 bits of conditional taken/not-taken
history` — no tags, no confidence, one history length, and the history the paper's
§2.1 shows is the wrong one.

## 2. Approach

`cbp2025/lib/ittage.h` is a complete ITTAGE **written by Seznec** and shipped in
the CBP2025 framework we already vendor from. Vendor it once, convert its
`#define` configuration block into template/struct parameters, and instantiate at
two storage budgets.

**Decision (user):** one parameterized copy, not two vendored copies and not a
reimplementation. This avoids duplicated source at the cost of modifying the
reference — a risk retired by the equivalence harness in §5.

### 2.1 Configurations

Storage is counted as an **idealized hardware entry**, not the literal C++ struct.
Measured over `723.llvm_r.sp1`, `714.cpython_r.sp0` and `swe_agent_w00001`,
instruction addresses need **47 significant bits** (max observed
`0x7ffff7fdc84e`), so:

    entry = target(47) + ctr(3) + tag(11) + u(2) = 63 bits

The simulator stores a `uint64_t` internally; that is a simulation artifact, not
architectural state, and is excluded from the budget.

| Config | Tagged tables | Entries/table | Storage |
|---|---|---|---|
| *(current `basic_btb`)* | 1, tagless | 4096 | **23.5 KB** |
| `ittage_32kb` | 8 | 512 | **31.5 KB** |
| `ittage_64kb` | 8 | 1024 | **63.0 KB** |

8 tables rather than the reference's 9 so the budgets land on round numbers.
History lengths remain geometric (`MINHIST 2 … MAXHIST 300`), recomputed for 8
tables. A paper-verbatim 9-table version is deferred until these two justify it.

Parameterized knobs are exactly those that move storage: table count,
entries/table, tag width, counter widths, history bounds. Everything else is
untouched, which is what keeps the equivalence harness meaningful.

## 3. Module Layout

```
btb/ittage/
├── ittage.h / .cc         adapter (~80 lines): routing, history feed, guards
└── vendor/ittage.hpp      Seznec's ittage.h, #defines -> parameters
```

## 4. Adapter And Its Failure Modes

```
btb_prediction(ip):
  e = direct.check_hit(ip)
  if !e          -> {0, false}            // not known to be a branch
  if e.RETURN    -> ras.prediction()
  if e.INDIRECT  -> ittage.GetPrediction(ip)
  else           -> {e.target, e.type != CONDITIONAL}

update_btb(ip, target, taken, type):
  calls             -> ras.push(ip)
  INDIRECT|IND_CALL -> ittage.UpdatePredictor(ip, target)
  everything else   -> ittage.TrackOtherInst(ip, next_pc)   // history only
  RETURN            -> ras.calibrate_call_size(target)
  direct.update(ip, target, type)
```

Four modes that degrade accuracy **silently**, each with a guard and a test:

1. **`0xdeadbeef` sentinel.** `ientry()` initialises `target = 0xdeadbeef`. An
   unseeded entry that tag-matches would hand ChampSim a garbage target that
   merely mispredicts rather than announcing itself. **Decision:** treat the
   sentinel as "no prediction" and return `{0,false}`, taking the misprediction,
   rather than falling back to the BTB's stale target. ITTAGE's measured accuracy
   must not borrow from the BTB.
2. **Not-taken branches carry `branch_target == 0`** (`src/tracereader.cc`).
   Feeding that to `TrackOtherInst` injects a constant into path history. Pass the
   architectural next PC — fall-through when not taken.
3. **Storage duration / initialisation.** The CBP6 tenants needed static storage
   because their constructors do not zero every member; three such members were
   found in RUNLTS by heap-poisoning. Call `reinit()` from `initialize_btb()` and
   re-run that harness against `IPREDICTOR`.
4. **Direct-BTB gating (measured, bounds the result).** The indirect predictor is
   only consulted when `direct.check_hit(ip)` hits and types the PC as INDIRECT
   (`basic_btb.cc:24`). On `710.omnetpp_r.sp0` a *perfect* indirect oracle still
   leaves **3.8%** of `INDIRECT` and **34%** of `INDIRECT_CALL` mispredicts
   standing — those branches missed the 8K direct BTB. ITTAGE cannot beat that
   floor. Deliberately not papered over; it is a BTB-capacity finding in its own
   right.

The history feed carries the paper's §2.1 benefit (~16%): path history from
indirect targets and call PCs, not conditional direction bits. This requires
`TrackOtherInst` on non-indirect branches — easy to omit, and omitting it costs
most of the gain silently.

## 5. Verification

Threat model: **a subtly wrong ITTAGE produces plausible MPKI.** It will not crash.

| Layer | What it catches |
|---|---|
| **1. Equivalence harness** (`tools/ittage_equiv/`) | The parameterization risk. Pristine reference and parameterized copy at default config, same `(PC,target)` stream, **bit-identical predictions over ≥5M indirect branches**. Namespace-per-TU handles the `IPREDICTOR`/`ientry`/`folded_history` collisions. |
| **2. Protocol checker** (`ITTAGE_PROTOCOL_CHECK=1`) | Wiring: consulted for exactly the indirect classes, never fed a zero target, sentinel never escapes, history updated once per branch. |
| **3. Unit tests** (`test/cpp/src/1xx-ittage-*.cc`) | Routing, sentinel suppression, not-taken history, `reinit()` completeness via heap poisoning. |
| **4. Oracle bound** (`perfect_indirect`) | A result better than the oracle is a bug report. Asserted, not eyeballed. |
| **5. Adversarial audit** (multi-agent) | What tests do not express. Four lenses: parameterization diff, adapter, storage accounting, and one agent tasked with producing plausible-but-wrong MPKI. |

**Layers 1 and 4 are the only ones that catch a wrong-but-plausible predictor.**
2 and 3 catch wiring, not numerics. Stated so the coverage is not overclaimed.

## 6. Milestones

Ordering is a constraint, not a preference: no measurement before the equivalence
harness is green.

| # | Work | Exit criterion |
|---|---|---|
| M1 | Parameterize; equivalence harness | **Bit-identical over ≥5M indirect branches.** Hard gate. |
| M2 | Adapter, protocol checker, unit tests, poisoning harness | Suite green; invariants hold on a full trace |
| M3 | Storage accounting; both configs build | 31.5 / 63.0 KB confirmed by explicit bit count |
| M4 | Evaluate on SPEC | vs baseline and vs `perfect_indirect` |
| M5 | Evaluate on agentic traces | The number that justifies the work |
| M6 | Adversarial audit | Findings triaged; blockers fixed |

## 7. Evaluation

Five configurations, all on **real direction** (`cbp6_tagescl64`) — the headroom
question for a target predictor is what remains on a machine predicting directions
as well as we currently can:

`basic_btb` (23.5 KB) · `ittage_32kb` (31.5 KB) · `ittage_64kb` (63.0 KB) ·
`perfect_indirect` (ceiling) · `perfect_btb` (loose all-target bound)

**Traces.** 32 SPEC, reported in full but led by the six with non-trivial indirect
MPKI (omnetpp 1.686, cpython 1.446, vpr 1.120, ns3 1.094, llvm 0.930, gem5 0.854);
the other eight are ~0.000 and can only dilute. Plus 4 agentic traces, validated
with `trace_sanity_check --check` first.

**Metrics.** Indirect MPKI (`BRANCH_INDIRECT + BRANCH_INDIRECT_CALL`) primary;
then overall MPKI, CycWPKI, IPC, and fraction of indirect headroom captured —
**pooled cycle ratio** against `perfect_indirect`, never an arithmetic mean of
per-trace percentages, which inverted a ranking in the CBP2025 campaign on exactly
this kind of sparse-signal set.

**Success criteria, fixed in advance:**

- *Necessary:* beats the 4096-entry gshare cache on indirect MPKI on the agentic
  traces. If not, the premise is wrong.
- *Interesting:* 32 KB captures most of what 64 KB does — separates algorithm from
  capacity.
- *Expected null:* little movement on the eight SPEC workloads with no indirect
  mispredicts. Report it; do not average it away.

## 8. Out Of Scope

- **The IUM (paper §2.3.2).** ChampSim calls `impl_update_btb` from
  `do_predict_branch` (`src/ooo_cpu.cc:174`) — same function as the prediction, at
  fetch, with the architectural outcome. No wrong path, no delayed update. The
  IUM's premise does not hold, and neither does the speculative-history machinery.
  *This makes ChampSim's BTB results optimistic in absolute terms for every
  predictor equally; comparisons stay fair.*
- **Region-table target compression (§2.2)** — buys storage, not accuracy.
- **Table sharing (§2.3.1)** — <1% by the paper's own account.
- **Returns.** 0.298 MPKI on SPEC, 0.414 on cpython, handled by the 64-entry RAS.
  Worth a separate study.
