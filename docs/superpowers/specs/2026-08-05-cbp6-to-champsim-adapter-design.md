# CBP6 → ChampSim Branch Predictor Adapter — Design

**Date:** 2026-08-05
**Status:** Approved design, pending implementation plan
**Repo:** `ChampSim` (branch `bpeval`)

---

## 1. Goal

Evaluate CBP2025 (CBP6) branch predictors — starting with the winner, RUNLTS — inside our
existing ChampSim ecosystem (our traces, our core, prefetcher and replacement models).

The deliverable is **not a RUNLTS port**. It is a reusable *CBP6 host adapter* for ChampSim,
such that any CBP6 submission drops in by adding a directory of vendored, unmodified sources.
RUNLTS and the bundled CBP2016 TAGE-SC-L are the first two tenants.

### Non-goals

- Reproducing CBP6's absolute MPKI numbers. The microarchitectures differ in every dimension
  that matters (16-wide vs 6-wide fetch, 1024- vs 352-entry window, perfect indirect
  prediction, AArch64 vs x86); agreement would not indicate correctness.
- Feeding RUNLTS real architectural register values. See §2.

---

## 2. Decisions already made, and the evidence

### 2.1 Register values: measured, not assumed

RUNLTS ("Register-value-aware predictor Utilizing Nested Large TableS") consumes destination
register **values** via `execute_notify(seq_no, piece, dst_reg, value)`, hashing them into a
`RBias` table indexed by `(PC ^ PC>>8 ^ reg_value) % 4096`, gated by a per-PC register
selector `WR` (`my_cond_branch_predictor.cc:974-1023, 1202-1228, 1629-1652`).

ChampSim traces carry register *indices* and memory *addresses* only — never data values
(`inc/trace_instruction.h:36-49`). `ooo_model_instr` has no value field, and ChampSim does not
functionally execute. The value does not exist anywhere in the stack.

It degrades cleanly rather than breaking: `RegFileState[i].valid` is set only inside
`execute_notify`, so never calling it leaves `register_values[i] = -1` at
`my_cond_branch_predictor.cc:1605`, failing the guard at `:1210`; `best_reg[]` stays `-1` and
the RBias loop skips. No assert fires, no code change is needed.

**We measured the cost** by running three builds of RUNLTS against 18 CBP6 traces (16 `infra`
+ 2 sample) in CBP6's own simulator, scored on the second half of each trace exactly as the
championship did (`scripts/trace_exec_training_list.py`, the `50 Perc` columns):

| Config | MPKI (AMean) | vs. full | CycWpPKI (AMean) | vs. full |
|---|---|---|---|---|
| **full** — stock RUNLTS | 2.0313 | — | 187.13 | — |
| **loadonly** — values for loads only (our v2 traces) | 2.1134 | **+4.04%** | 187.26 | **+0.07%** |
| **noval** — no values (our v1 traces) | 2.1598 | **+6.33%** | 188.70 | **+0.84%** |

Four conclusions:

1. **The register component is worth roughly RUNLTS's entire winning margin.** It beat a 192KB
   TAGE-SC-L by 5.3% BrMisPKI in the championship; removing register values costs 6.33% here.
   Different trace sets and baselines, so this is an order-of-magnitude argument, not an
   identity — but RUNLTS-without-values plausibly lands near a well-scaled TAGE-SC-L.
2. **Load values recover only ~36% of the loss**, and almost none of it where it matters:
   `infra_3` is +51.4% without values and still +47.3% with load values; `infra_0` is
   +40.8% → +34.8%. The signal lives in ALU-computed values, not raw loaded data.
3. **The effect is a minority-of-workloads phenomenon.** 6 of 18 traces degrade >10%; the other
   12 are within ±4%, several marginally *better* without register values.
4. **On cycles it is nearly invisible: +0.84% CycWpPKI.** Even `infra_3`, at +51% MPKI, is only
   +9.8% CycWpPKI. The mispredictions this component fixes are cheap ones.

For scale, the register machinery is 53,877 bits ≈ **6.6 KiB of RUNLTS's 191.75 KiB budget**
(`TotalSC − TraditionalSC` from its own `print_predictorsize` output) — a remarkable return per
bit, and correspondingly little budget freed by dropping it.

**Decision: port RUNLTS with the register-value channel inert. Do no trace-format work.**
The v2 load-value path buys 2.3 points of MPKI and ~zero cycles for real plumbing; full values
would require re-tracing every workload for a component worth <1% of wrong-path cycles.

### 2.2 Scope

Adapter + RUNLTS (no-RV) + the bundled CBP2016 TAGE-SC-L at 64KB and 192KB. The TAGE-SC-L port
is not optional: comparing a 192KB RUNLTS against a small stock ChampSim predictor measures
budget, not ideas.

### 2.3 Context for interpreting results

CBP6 final standings vs. a 192KB scaled TAGE-SC-L (673-trace set): RUNLTS −5.3% BrMisPKI /
−2.5% CycWpPKI; TAGE2025 (Seznec) −2.2% / −1.35%; LVCP −1.3% / −0.4%; three entries regressed.

Since RUNLTS's edge is concentrated in a channel ChampSim cannot feed, **TAGE2025 may transfer
at full strength and be the stronger predictor in our infrastructure.** Worth confirming if its
sources become available; we do not currently have them.

---

## 3. How the two APIs differ

| Concern | CBP6 | ChampSim |
|---|---|---|
| init | `beginCondDirPredictor()` | `initialize_branch_predictor()` (`ooo_cpu.cc:55-60`) |
| predict | `get_cond_dir_prediction(...)` — **conditional branches only** (`bp.cc:80-89`) | `predict_branch(...)` — **every instruction**, incl. non-branches (`ooo_cpu.cc:135-138`) |
| spec history | `spec_update(...)` for all branches, immediately after predict, with the **resolved** direction (`bp.cc:112,128,164`) | `last_branch_result(...)` — same function, same cycle, program order, resolved direction, all branch types (`ooo_cpu.cc:166`) |
| table update | `notify_instr_execute_resolve(...)` at **execute**, out of order (`uarchsim.cc:352`) | *none* — collapsed into the above |
| per-instr hooks | fetch / decode / agen / execute / commit | *none* (but `predict_branch` covers fetch) |
| teardown | `endCondDirPredictor()` | *none for branch modules* (`modules.h:46-72`) |

Two corrections to first impressions, both verified:

- ChampSim's `last_branch_result` **is** `spec_update`'s structural equivalent. What is missing
  is CBP6's *second*, execute-time, out-of-order update. The fidelity cost is loss of update
  delay, not loss of speculative history.
- **Neither simulator models wrong-path execution.** ChampSim freezes fetch
  (`ooo_cpu.cc:156-160`; `register_allocator.cc:86` still carries a
  `"once wrong path is implemented"` TODO); CBP6 advances `fetch_cycle` and bills
  `cycles_on_wrong_path` (`uarchsim.cc:775-778`). Both feed resolved directions in program
  order, so history-repair semantics are identical. This is not a risk.

---

## 4. Architecture

### 4.1 Module layout

```
branch/cbp6_common/          shared, header-only
  cbp6_compat.h              InstClass, DecodeInfo, ExecuteInfo, is_br/is_cond_br
                             — source-compatible with cbp2025/lib/sim_common_structs.h
  cbp6_host.h                the adapter: ChampSim hooks -> CBP6 callbacks
branch/cbp6_tagescl64/       vendored cbp2016_tage_sc_l.h + binding
branch/cbp6_tagescl192/      vendored cbp2016_tage_sc_l_192kb.h + binding
branch/cbp6_runlts_norv/     vendored RUNLTS sources + binding
```

**Invariant: vendored CBP6 sources stay byte-identical to the submission.** The adapter supplies
the environment they expect. This is what makes the next submission a directory add rather than
a porting exercise, and keeps upstream diffs reviewable.

### 4.2 Call mapping

| ChampSim hook | Adapter action |
|---|---|
| `initialize_branch_predictor()` | `beginCondDirPredictor()` |
| `predict_branch(ip, tgt, always_taken, type)` | if `type != BRANCH_CONDITIONAL` → return `true`, do nothing else; else `get_cond_dir_prediction(instr_id, 0, ip, cycle)`, cache the answer |
| `last_branch_result(ip, target, taken, type)` | map `type` → `InstClass`; `spec_update(instr_id, 0, ip, cls, taken, cached_pred, next_pc)` |
| *(new)* execute-resolve | `notify_instr_execute_resolve(...)` → the predictor's `update()` |
| *(new)* final-stats | `endCondDirPredictor()` |

`seq_no` ← `instr_id`; `piece` ← `0` (ChampSim has no uop concept).
Returning `true` for non-conditionals matches CBP6, which forces `pred_taken = true` on every
unconditional branch (`bp.cc:128,164`).

Branch-type mapping (from the RUNLTS interface, `cond_branch_predictor_interface.cc:64-96`):
`BRANCH_DIRECT_JUMP`→0, `BRANCH_CONDITIONAL`→1, `BRANCH_INDIRECT`→2, `BRANCH_DIRECT_CALL`→4,
`BRANCH_INDIRECT_CALL`→6, `BRANCH_RETURN`→10. `BRANCH_CONDITIONAL` routes to `history_update()`;
all others to `TrackOtherInst()`.

### 4.3 Mandatory gates

These are correctness requirements, not optimisations.

1. **Gate `predict_branch` on `BRANCH_CONDITIONAL`.** `RUNLTS::predict` emplaces a full
   `cbp_hist_t` checkpoint into `pred_time_histories` (`:1608`) and **only `update()` erases it**
   (`:1627`), which runs only for conditional branches. Ungated, the map grows without bound and
   the run slows by roughly an order of magnitude.
2. **Skip `is_branch == true && branch == NOT_BRANCH`** — a classifier quirk at
   `instruction.h:189-191` that yields branches with no valid type.
3. **`next_pc`.** ChampSim's `branch_target` is zero for not-taken branches
   (`tracereader.cc:29-32`) where CBP6 passes `pc+4`. RUNLTS gates every `nextPC` use on `taken`
   so it is immune — but that is luck, not design, and a future tenant using `nextPC`
   unconditionally would silently corrupt backward-branch detection, path history and IMLI.

   ChampSim cannot compute `pc+4`: the trace carries no instruction length, and x86 is
   variable-length. The fall-through address is only obtainable as the *successor instruction's
   IP*. The adapter therefore takes it from `intern_->input_queue`, and this is permitted
   **only inside `last_branch_result`** — the resolved-outcome path, where ChampSim already
   hands the module the true direction and true target. Reading the successor at
   `predict_branch` time would be a causality violation (§4.3.4). Implementation note: the
   branch under evaluation is still `input_queue.front()` when the hook fires (popped at
   `ooo_cpu.cc:102`), so the successor is `input_queue[1]`, which may be absent at buffer
   boundaries — fall back to passing `branch_target` unchanged.
4. **No oracle leakage.** `intern_->input_queue` holds future correct-path instructions. Any read
   of it must be confined to the update path and code-reviewed as such; CBP6's rules explicitly
   forbid causality violations, and a violation here would be invisible in the output.

---

## 5. ChampSim core changes

Roughly 50 lines, following the prefetcher traits as the template.

1. **`branch_predictor::has_execute_resolve` trait + dispatcher + call in
   `O3_CPU::do_complete_execution` (`ooo_cpu.cc:614`).** Driven from
   `complete_inflight_instruction` (`:628-639`), which already fires per-instruction,
   out of order, bandwidth-limited by `EXEC_WIDTH` — a faithful analogue of CBP6's `eval_exec`.
   Moving the table update here recovers both the delay and the out-of-orderness.

   Without it, `spec_update` and `update` collapse into one zero-latency program-order call, so
   every branch predicts against tables its immediate predecessor already updated. That bias
   **flatters** the port and could easily be mistaken for the port working well.

   **Behind a JSON knob, default off**, so existing predictors keep current semantics and
   baselines stay comparable. Run both and report the delta.

2. **`branch_predictor::has_final_stats`, called from `O3_CPU::end_phase` (`ooo_cpu.cc:75`).**
   Branch predictors are the only module kind lacking one (`modules.h:46-72` vs `:150`). Gives
   `endCondDirPredictor()` a home and a place for custom counters.

3. **A conditional-only direction-mispredict counter.** ChampSim's misprediction check fires on
   target mismatch *or* direction mismatch across all branch types (`ooo_cpu.cc:151-153`), so BTB
   and RAS misses land in the same "MPKI". CBP6 counts conditional-direction only, with perfect
   indirect prediction. Without this, an A/B against our current predictors compares two
   different things.

`has_decode` is deferred — it exists only to feed register values.

---

## 6. Validation

**Differential replay is the primary oracle.** The adapter dumps every call it makes —
`(seq_no, piece, pc, inst_class, resolve_dir, pred_dir, next_pc)` plus the returned prediction. A
small standalone harness replays such a dump into a bare RUNLTS and asserts bit-identical
predictions. This answers "is the adapter driving the predictor faithfully?" exactly, and keeps
it separate from "do the two simulators agree?", which is not answerable. The same dump format
works against a CBP6-side dump, so the harness doubles as a self-test.

Supporting checks:

- **Port TAGE-SC-L before RUNLTS.** No exotic hooks, known quantity. If it does not comfortably
  beat `hashed_perceptron`, the adapter is broken — learned before RUNLTS's complexity is in play.
- **Budget assertion.** RUNLTS self-reports 191.75 KiB; assert it still does under ChampSim.
- **Self-labelling.** A dead register channel degrades *silently* — RBias indexes on a constant,
  `WR` counters never train up from their −24 start, and nothing asserts. The module is therefore
  named `cbp6_runlts_norv`, prints an explicit banner, and reports an RBias-activation-rate stat
  that must read 0%. Cheap insurance against a result being written up later as "the CBP2025
  winner".
- **Determinism.** Same trace + config → identical stats.

---

## 7. Metrics and methodology

Report all three; do not let them substitute for one another.

- **Conditional-only MPKI** — the only number comparable *in kind* to CBP's BrMisPKI.
- **ChampSim native MPKI and IPC** — what our ecosystem actually cares about.
- **Wrong-path cycles** (optional) — accumulate the fetch-freeze duration around
  `ooo_cpu.cc:156-160`, `:358`, `:624` for a CycWpPKI analogue.

Always against a **192KB TAGE-SC-L** baseline, plus 64KB and our current predictor.

Note for cross-referencing published numbers: CBP6 scores the **second half** of each trace
(first half is warmup), arithmetic mean across traces.

---

## 8. Milestones

| # | Milestone | Rationale |
|---|---|---|
| 0 | Measure `BRANCH_OTHER` share on our traces; baseline current predictors | Caps everything downstream; hours of work |
| 1 | Adapter + CBP2016 TAGE-SC-L 64KB | De-risks the adapter on a hook-free, known-good predictor |
| 2 | Differential replay harness | Adapter correctness proven before complexity arrives |
| 3 | RUNLTS (no-RV, self-labelled) + TAGE-SC-L 192KB | The evaluation, with its iso-budget baseline |
| 4 | Core hooks: execute-resolve, final-stats, conditional-MPKI counter | Quantifies the update-timing bias instead of inheriting it |
| 5 | Encapsulate RUNLTS's namespace-scope globals | Required for multi-core; silently wrong until done |

### Deferred indefinitely — not resourced

**CBP6 → ChampSim trace converter.** No work on this until milestones 0–5 are complete, and only
then if a concrete need appears. It is genuinely larger than it first appears: the CBP2025 format
adds base-update flags, a store-only register-offset flag, and reader-synthesised pieces, and its
AArch64 register numbering would have to be mapped onto ChampSim's x86 sentinels
(`REG_STACK_POINTER=6`, `REG_FLAGS=25`, `REG_INSTRUCTION_POINTER=26`) for ChampSim's *inferred*
branch classification to work at all.

---

## 9. Risks

| Risk | Detail | Mitigation |
|---|---|---|
| `BRANCH_OTHER` bypass | `basic_btb` returns `always_taken` for any non-CONDITIONAL entry (`basic_btb.cc:27`), with `BRANCH_OTHER` defaulting to `ALWAYS_TAKEN` (`direct_predictor.cc:12-19`), and `predict_branch(...) \|\| always_taken` (`ooo_cpu.cc:138`) overrides the predictor. Those branches never reach RUNLTS, *and* are excluded from printed aggregates (`plain_printer.cc:43-44`) though counted internally. | Milestone 0 measures the share before anything is trusted |
| Silent no-RV misreporting | Nothing asserts that the register channel is dead | Self-labelling module, banner, 0% activation stat (§6) |
| Update-timing bias | Collapse makes the port look better than its CBP number | Milestone 4 quantifies it; knob defaults to ChampSim semantics |
| Multi-core corruption | `RBias`, `WR`, `RegFileState`, `bias_components`, `IMLI_components`, `bim_pred/bim_hyst` are namespace-scope, outside `class RUNLTS`; ChampSim builds one module per core | Milestone 5; single-core is safe until then |
| Metric substitution | ChampSim MPKI conflates direction and target misses | Conditional-only counter (§5.3) |
| Upstream tracer bug | `WriteToSet` (`tracer/pin/champsim_tracer.cpp:130-136`) writes one past the array when an instruction has >2 destination or >4 source registers, corrupting `source_registers[0]` / `destination_memory[0]`. Since branch type is inferred from register patterns, this feeds misclassification. **From code reading, not from running the pintool**; relevant only if our traces came from this tracer. | Verify provenance of our traces at Milestone 0 |
| Uop granularity | RUNLTS's digest decay counter is calibrated in decoded uops (CBP6 cracks 1.14–1.32×); ChampSim has none | Inert while register values are unused; revisit only if wired |

---

## 10. Reproducing the ablation

Three builds of the CBP6 simulator with the RUNLTS submission dropped in, differing **only** in
`notify_instr_execute_resolve` in `cond_branch_predictor_interface.cc`. Everything else —
including `decode_notify`, which ChampSim *could* drive since it needs only register indices —
is left untouched.

Baseline (`full`), as shipped in the submission:

```cpp
const std::optional<uint64_t>& dst_reg = _exec_info.dec_info.dst_reg_info;
if (dst_reg.has_value() && 0 <= *dst_reg && *dst_reg <= 64) {
    cbp2025_RUNLTS.execute_notify(seq_no, piece, *dst_reg, _exec_info.dst_reg_value.value());
}
```

`loadonly` — models a ChampSim v2 trace carrying load values only:

```cpp
const std::optional<uint64_t>& dst_reg = _exec_info.dec_info.dst_reg_info;
if (is_load(_exec_info.dec_info.insn_class) && dst_reg.has_value() && 0 <= *dst_reg && *dst_reg <= 64) {
    cbp2025_RUNLTS.execute_notify(seq_no, piece, *dst_reg, _exec_info.dst_reg_value.value());
}
```

`noval` — models a stock ChampSim trace: the block is deleted entirely.

Build each with `make clean && make`, run over `cbp2025/traces/**/*.gz`, parse the
`DIRECT CONDITIONAL BRANCH PREDICTION MEASUREMENTS (50 Perc instructions)` block, and take the
arithmetic mean of the `MPKI` and `CycWPPKI` columns across traces — matching
`scripts/trace_exec_training_list.py`.

The 18 traces used here are the 16 `infra` traces plus the two shipped samples. **Re-run against
the full 105-trace training set** (the CBP2025 README has a `gdown` link) before any of these
numbers are published; the direction of the result is unlikely to change, but the magnitudes
may.
