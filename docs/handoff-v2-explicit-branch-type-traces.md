# Handoff: the new SPEC CPU 2026 v2 traces and what ChampSim had to change to read them

**To:** the Claude Code instance that owns `/home/rbera/work/bpeval/ChampSim`
**From:** the instance that owns `/home/rbera/work/bpeval/champsim-infra` (the tracer side)
**Date:** 2026-08-06
**ChampSim commits:** `f602b806` (parsing, landed inside an unrelated commit) and `ce6d9dae` (guard + tests)

---

## 1. Why any of this happened

The v2 traces you had were **unusable for branch-predictor evaluation and looked fine**. ChampSim
saw zero conditional branches on them, so the direction predictor was never consulted and every
predictor scored identically — bimodal, gshare, perceptron and hashed_perceptron all reported
exactly 21.93 MPKI. Nothing asserted, nothing looked wrong.

Two independent causes:

1. **The tracer dropped the flags register.** ChampSim infers branch type from register usage
   (`inc/instruction.h`), and a conditional branch that reads no flags is indistinguishable from an
   unconditional direct jump — so it matched the `BRANCH_DIRECT_JUMP` arm, which then *overwrites*
   `branch_taken = true` and discards the real direction. The tell: that bucket's taken rate was
   **49.17%**, where a genuine unconditional jump is always 100%.
2. **Branch type was inferred at all.** A semantic property (what kind of control transfer is this?)
   was being reconstructed from an incidental encoding property (which registers happened to fit in
   a 2- and 4-slot array).

Both are fixed on the tracer side. ChampSim now reads the answer instead of guessing it.

---

## 2. What changed in the v2 trace format

**The record layout is unchanged: still 512 bytes, same field offsets.** Three previously-zero bytes
of `reserved[8]` now carry meaning.

| byte | field | meaning |
|---|---|---|
| `reserved[0]` | `branch_type` | ChampSim's `branch_type` enum **verbatim**: 0 `DIRECT_JUMP`, 1 `INDIRECT`, 2 `CONDITIONAL`, 3 `DIRECT_CALL`, 4 `INDIRECT_CALL`, 5 `RETURN`, 6 `OTHER`, 7 `NOT_BRANCH` |
| `reserved[1]` | feature bitmask | `0x01` explicit branch type present, `0x02` flags register recorded |
| `reserved[2]` | tracer identity | which pintool wrote the record (2 or 3) |
| `reserved[3..7]` | zero | reserved |

Two more content changes inside existing fields:

- **The flags register is now recorded**, as both a source and a destination. Not for classification
  (`reserved[0]` handles that) but for *dependency modelling*: ChampSim gates execution on
  `all_of(source_registers, isValid)` and starts the misprediction penalty at
  `do_complete_execution`, so a `jcc` with no flags source is not gated by the `cmp` that computes
  its condition. It resolved too early and **misprediction cost came out too low** — precisely the
  quantity branch-predictor work measures.
- **`is_branch` now covers calls and returns** and marks them taken. PIN's `INS_IsBranch()` excludes
  them, and the old tracer additionally hard-coded `branch_taken = 0` for them. Harmless while
  ChampSim re-derived everything; wrong the moment a consumer trusts `reserved[0]`, since a
  `BRANCH_DIRECT_CALL` with `branch_taken = 0` is a not-taken call.

### There is deliberately no `--trace-version 3`

The layout did not change, so a version number would describe nothing. More importantly, a version
passed on the command line is **asserted by the user** and can disagree with the data, while a
feature bit is **self-describing**. The original bug was exactly a silent mismatch between what a
trace contained and what the consumer assumed; a CLI version reproduces that failure mode. These
traces are read with `--trace-version 2`, like the old ones.

---

## 3. What changed in ChampSim

### `f602b806` — the parsing half (⚠️ landed inside an unrelated commit)

Heads up: this arrived as part of a commit titled *"cbp6: audit fixes"*, which swept up my edits to
`inc/trace_instruction.h` and `inc/instruction.h` alongside unrelated cbp6 work. If you ever bisect
or revert around branch-type behaviour, that is the commit, despite its message.

- `inc/trace_instruction.h`: `champsim::TRACE_RESERVED_*` indices, `TRACE_FEATURE_*` bits, the
  `has_trace_reserved<>` detector and `has_explicit_branch_type()`.
- `inc/instruction.h`: a new first arm in the `ooo_model_instr` classification cascade. When the
  feature bit is set it takes `branch` from `reserved[0]`, sets `is_branch = (branch != NOT_BRANCH)`,
  and takes `branch_taken` **verbatim** from the record. Otherwise it falls through to the existing
  register-pattern inference, unchanged.

### `ce6d9dae` — the guard and the tests

- `src/tracereader.cc`: `get_tracereader()` now reads the first record of a v2 trace and prints a
  loud warning, naming the file, if the explicit-branch-type bit is clear.
- `test/cpp/src/090-explicit-branch-type.cc`: 13 cases pinning the contract.

**Do not move that warning into `bulk_tracereader`.** It was there originally and it was wrong:
`champsim::repeatable` reconstructs the reader on every wraparound (`repeatable.h:41`), re-arming any
per-object one-shot, and `main.cc:119` enables repeat whenever `-i/--simulation-instructions` is
given — i.e. on every real run. Measured: **26 banners over 25 wraps**, 11,596 bytes of stderr.
`get_tracereader()` runs exactly once per trace file.

---

## 4. How to run these traces

**Location:** `/home/rbera/work/bpeval/simpoint/traces/` — 32 traces, 38 GB, 14 SPEC CPU 2026
`intrate` workloads, named `<benchmark>.sp<cluster>.champsim2.zst`.

```sh
./bin/champsim --trace-version 2 -w <warmup> -i <sim> \
    /home/rbera/work/bpeval/simpoint/traces/750.sealcrypto_r.sp0.champsim2.zst
```

**Each trace is exactly 300,000,000 instructions — one SimPoint region, nothing else.** There is no
warmup prefix baked in; split the region internally. Something like `-w 50000000 -i 250000000` gives
a 50M warmup and a 250M measured region. Whatever you choose, `warmup + sim ≤ 300M` or you hit
end-of-trace (and with `-i` given, ChampSim will silently *wrap the trace* and keep going — see
pitfalls).

**Aggregating across a workload's regions is weighted, not averaged.** Each workload has 1-3 regions
with SimPoint weights that sum to its coverage:

```
weighted_MPKI(workload) = Σ_r weight_r × MPKI_r   /   Σ_r weight_r
```

The weights live in `/home/rbera/work/bpeval/simpoint/<benchmark>.simpoints.json`, alongside the
interval index and instruction offset of each region. Regions below 5% weight were dropped during
collection, so coverage is 0.917-1.000 depending on workload — **normalise by the kept weight**, as
above, rather than assuming the weights sum to 1.

### The trace set

| workload | thread | regions | weight coverage |
|---|---|---|---|
| 706.stockfish_r | **1 (worker)** | 3 | 1.000 |
| 707.ntest_r | 0 | 3 | 1.000 |
| 708.sqlite_r | 0 | 3 | 1.000 |
| 710.omnetpp_r | 0 | 2 | 0.979 |
| 714.cpython_r | 0 | 2 | 0.994 |
| 721.gcc_r | 0 | 1 | 0.948 |
| 723.llvm_r | 0 | 2 | 0.982 |
| 727.cppcheck_r | 0 | 2 | 0.979 |
| 729.abc_r | 0 | 2 | 0.994 |
| 734.vpr_r | 0 | 2 | 0.960 |
| 735.gem5_r | 0 | 1 | 0.917 |
| 750.sealcrypto_r | 0 | 3 | 1.000 |
| 753.ns3_r | 0 | 3 | 1.000 |
| 777.zstd_r | 0 | 3 | 1.000 |

Built with **GCC 14.2.0, `-g -O3 -march=x86-64-v3`** (AVX2/BMI2/FMA, deliberately **no AVX-512** —
verified 0 `%zmm`/`%k` references in the binaries). SPEC config `bpeval-gcc14.cfg`, label `gcc14v3`,
ref inputs, **invocation 0 only**, single-threaded (`OMP_NUM_THREADS=1`).

---

## 5. Pitfalls

**1. Check the warning. It is the whole point.**
If you see

```
*** WARNING: '<file>' carries no explicit branch type ***
```

the trace predates this work and **any branch-predictor number from it is meaningless** — every
conditional is misclassified as an always-taken direct jump. Do not compare such a run against these
traces. There is a standalone checker in the infra repo:

```sh
champsim-infra/tools/trace_sanity_check/trace_sanity_check -i <trace> -f v2 --check
```

It exits non-zero on failure and enforces six invariants. The load-bearing one is that the
**conditional-branch taken rate is strictly inside (0, 100)%** — the check that would have caught the
original bug.

**2. `-i` makes ChampSim loop the trace, silently.**
`main.cc:119` sets `repeat = simulation_given`, so asking for more instructions than a trace contains
does not stop — it wraps to the beginning and keeps simulating, warming predictors on a repeat of the
same 300M instructions. With `-i 5000000` on a 200k-instruction trace we measured 25 wraparounds.
Keep `warmup + sim ≤ 300M`, or know that you are measuring a loop.

**3. `reserved[0] == 0` is a valid branch type.**
`BRANCH_DIRECT_JUMP` is 0, so a zeroed `reserved[]` is byte-identical to a record describing a direct
jump. **Never key presence off the value** — always off `reserved[1] & 0x01`. There is a test pinning
this (`An all-zero v2 record is inferred, not read as a direct jump`). If you extend the contract,
keep that property.

**4. `is_branch` counts differ from v1 traces.**
Calls and returns are now flagged, so any tool counting branches via `is_branch` will report more
than it used to. Count `reserved[0] != NOT_BRANCH` instead. This is why `reserved[1]` bit 1 exists.

**5. Some regions are legitimately branch-free, and the checker will say so.**
A region can be 300M executions of a `rep`-string instruction (one IP, `branch_type = NOT_BRANCH`,
reads FLAGS). That fails the acceptance checks *for a real reason* — it contributes nothing to
branch-predictor evaluation. None of the shipped 32 traces is in that state, but if you regenerate,
do not "fix" the checker when it reports this.

**6. `706.stockfish_r` is traced on thread 1, not the main thread.**
Its main thread runs 3 SimPoint intervals of setup; the worker runs 1,479. Profiling and tracing
thread 0 would sample nothing but process startup. It is the only multi-threaded workload in the
suite — `723.llvm_r` carries `cxxthreads`/`pthread` tags but runs serially. If you regenerate
anything, per-thread instruction counters mean **the profiled thread and the traced thread must be
the same one**.

**7. The `CHAMPSIM_TRACE_MEMORY_VALUES` build flag is off by default.**
The v2 record is always parsed, but the 384-byte memory-value payload is only carried into
`ooo_model_instr` when built with `CPPFLAGS=-DCHAMPSIM_TRACE_MEMORY_VALUES=1` (it roughly doubles
wall-clock). Branch-type support does **not** depend on it — `reserved[]` is read either way.

---

## 6. Things you will want to know

**Register values are deliberately absent, and that is a known limitation.**
RUNLTS (`branch/cbp6_runlts_norv/`) needs the 64-bit architectural *destination register value* of
every instruction, delivered at the producer's execute. These traces do not carry it, so that channel
stays inert. Measured cost in CBP6's own simulator: **+6.33% MPKI** with no values, **+4.04%** with
load values only — load values recover just ~36%, because the signal is in ALU results, not loaded
data. Capturing it would cost ~+38-51% compressed trace size and needs two new ChampSim module hooks.
Deferred deliberately; the full analysis is in
`champsim-infra/docs/superpowers/specs/2026-08-05-branch-type-and-flags-tracing-design.md` §9.

**If you ever build that channel: values must be gated by modelled execute time.** Handing a register
value to the predictor at fetch leaks the outcome — EFLAGS at an x86 `je` trivially determines the
direction. CBP2025 avoids this because `execute_notify` fires at the producer's execute cycle.

**The 2-destination / 4-source register budget still truncates.** The tracer now *counts* the drops
and reports them per run (a 200k-instruction `/bin/ls` trace loses 270 registers, 0.135%), but the
record cannot hold more. Roughly 91% of records have a free destination slot, so restoring flags cost
almost nothing — but the ceiling is real.

**Trace provenance.** `reserved[2]` says which pintool wrote a record. Both pintools emit the
identical `.champsim2.zst` filename, so before this byte existed there was no way to tell them apart
on disk.

**The infra side.** Tracer sources, the acceptance checker, and the full design rationale are in
`/home/rbera/work/bpeval/champsim-infra` (commits `58ba05b`, `1736d04`, `a983fd5`). The SimPoint
driver used to produce this set is `bpsim.py` in that session's scratchpad; per-workload manifests
(`*.simpoints.json`, `*.thread.json`, `*.traces.json`) sit beside the traces and record every
region's interval index, instruction offset, weight, byte size and check result.

**A closing note on how these bugs were found.** Every defect in this effort produced output that
passed structural checks: well-formed 512-byte records, correct branch types, exactly 300M
instructions — of the wrong code. Self-consistency checks caught none of them. What caught them were
**differential** tests: one-pass versus N-pass collection of the same regions, and old-trace versus
new-trace behaviour. If you extend this format, test it that way.
