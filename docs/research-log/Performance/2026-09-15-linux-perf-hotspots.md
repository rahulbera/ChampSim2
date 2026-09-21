# Linux perf hotspot analysis — 2026-09-15

The largest shared opportunity is **cache queue-helper overhead**. A narrower
first patch is also clear: the v2 trace reader probes a roughly 128 KiB stack
frame on **every returned instruction**, even when no refill is needed. Register
query inlining is another bounded candidate. For mcf, nonzero DRAM swizzling and
ROB scans remain substantial costs.

This report profiles the completed six-optimization implementation. **No simulator
code or build flags changed, and no new optimization speedup is claimed.** Proposed
changes below must follow the running project's one-fix-at-a-time parity and KIPS
gates. The [running log](2026-09-14-performance-optimization.md) records the previous
fixes; the [machine-readable summary](2026-09-15-linux-perf-summary.json) preserves
these measurements and ranges.

## Provenance and method

- Checkout: `/home/rbera/work/alakazam/champsim-perf-fix`, `feat/perf-fix`,
  `2b4cbcb4` (production implementation `f7ee81b9`).
- Executable: the archived `06-trace/champsim`, SHA-256
  `e9ab12cd4326dff3e8b92a07f4b6e430a100090820dc85558e7e95c6849d375b`.
  This is the same GCC 13.3 `-O3` legacy-only binary used in the previous final
  comparison. Assertions remain enabled. Its symbols and `.eh_frame` support
  unwinding; it lacks source-line DWARF, so source mapping uses disassembly and
  inspection of the corresponding C++ functions.
- Host: AMD Ryzen AI Max+ 395, Linux 7.0.0-30-generic, perf 7.0.12.
  `perf_event_paranoid=-1`; NMI watchdog remains enabled. CPU 14, SMT sibling 30;
  `powersave` / `balance_performance`, boost enabled. No host settings changed.
  The physical core is not exclusively reserved.
- All simulations: **legacy DRAM, detailed PTW**, default payload-disabled build,
  original GLC overlay, the same protected SQLite/omnetpp/GCC v2 and mcf v1 traces,
  **1M warmup + 3M ROI**. Commands, configs, binary and trace hashes come from the
  completed cumulative campaign. Collection covers the whole invocation.
- Per workload: three core-counter runs, two cache-counter runs, two store/load
  conflict-counter runs, two 499 Hz cycle/call-stack runs, and one plain control.
  SQLite additionally has a 249 Hz cycle run and a conflict-event sampling run:
  **42 simulation runs** in total. Simulations run sequentially.
- Four-event core group: `cycles:u,instructions:u,branches:u,branch-misses:u`.
  Separate two-event group: `cache-references:u,cache-misses:u`. Native conflict
  group: `cycles:u,instructions:u,ls_bad_status2.stli_other:u,ls_stlf:u`.
  All accepted counters report **100.00% running time**. A six-event preflight
  returned `<not counted>`; no multiplexed estimate enters the tables.
- Sampling: `cycles:u`, `-F 499`, `--call-graph dwarf,16384`, `-m 256`.
  Perf launches a `taskset -c 14` child that execs the simulator. Counts include
  its negligible pre-exec setup; a startup CPU migration can therefore appear.
  `DEBUGINFOD_URLS` is empty. No system-wide profile was collected.

Every run matches the reference's **reported phase statistics, effective
configuration, and warmup/ROI retirement and cycle counts exactly**. The trace
and binary hashes match before and after collection. The main eight cycle
profiles contain **67,113 samples with zero lost samples**. The 249 Hz check adds
3,309 samples; the conflict profile has 6,421 samples. All ten profiles have zero
lost samples and no perf report/script warnings. The final audit independently
matches parsed period totals to `perf report`, and matches exclusive symbol
periods for the release ELF. Library inline names/aliases differ between report
and script, so that symbol-by-symbol comparison is restricted to the release ELF.

Tables use **sample-period weights**, then average the two main per-workload
profile percentages. They do not average raw sample counts or mix event types.
The lower-frequency SQLite check reproduces the broad ranking (cache 39.39%,
core 42.55%, cache-helper union 23.48%, ROB union 19.70%).

## Host counter results

These are **host** instructions, branches and cache events, not simulated IPC or
simulated cache MPKI. Core values are medians of three runs. Generic cache MPKI
uses the median count from two separate cache passes divided by median host
instructions; it is not labeled as LLC or DRAM traffic. Conflict density is the
mean of two native-counter passes.

| Workload | Host IPC | Host instructions / simulated instruction | Host branch-miss rate | Generic cache misses / 1k host instructions | Store/load conflicts / 1k host instructions |
|---|---:|---:|---:|---:|---:|
| sqlite | 3.177 | 50,715 | 0.838% | 0.541 | 7.40 |
| omnetpp | 3.223 | 46,770 | 0.705% | 0.400 | 7.94 |
| gcc | 3.083 | 36,976 | 0.867% | 0.583 | 7.33 |
| mcf | 3.960 | 152,903 | 0.283% | 0.127 | 4.70 |

The simulator executes tens of thousands of host instructions per simulated
instruction; mcf also simulates more cycles per retired instruction. Branch
mispredictions and the generic cache-miss counter are relatively infrequent,
which supports investigating repeated host work. These counters alone do **not**
exclude memory dependencies or establish a frontend/backend stall decomposition.
A store/load conflict is an occurrence, not a count of stalled cycles.

The single plain controls are 299.43, 333.34, 404.21 and 124.21 KIPS in table order.
They confirm ordinary execution during this session, not an improvement over
yesterday's medians. There is no changed candidate in this analysis. Profiler
elapsed times are retained separately and are not optimization KIPS results.

## Disjoint component shares

Allocator and helper work is charged to its enclosing component in this table.
Trace classification recognizes both the bulk reader and decompression stream.

| Workload | Core | Cache | DRAM | Trace | PTW | Framework | Startup/other |
|---|---:|---:|---:|---:|---:|---:|---:|
| sqlite | 42.32% | 40.53% | 2.36% | 9.12% | 2.73% | 2.73% | 0.21% |
| omnetpp | 34.84% | 46.92% | 2.75% | 8.99% | 3.02% | 3.30% | 0.19% |
| gcc | 40.96% | 39.79% | 2.38% | 10.70% | 2.54% | 3.39% | 0.24% |
| mcf | 42.65% | 31.95% | 19.38% | 1.77% | 2.46% | 1.72% | 0.08% |

The large trace-reader frames exceed the 16 KiB stack snapshot. Roughly 8–9% of
weighted stacks on v2 workloads do not unwind all the way to `main`; most end in
an identifiable bulk-reader or decompression-stream frame and remain attributable
to trace work. Unknown leaf share is below 0.15% across the ten profiles. Remaining
unattributed/startup work is shown explicitly. PTW's own share excludes the cache
and DRAM work caused by its requests.

## Specific hotspots

“Exclusive” means the sampled instruction lies in that symbol. A call-path union
counts a sample once when any relevant function appears in its stack. These rows
**overlap** each other and the component table; they must not be added together.
For example, register queries are children of ROB scans, and the combined cache
helper union includes span, extraction, transformation and partition helpers.

| Symbol or call path | SQLite | omnetpp | GCC | mcf |
|---|---:|---:|---:|---:|
| Tag-queue `get_span` — exclusive | 10.40% | 10.99% | 10.22% | 8.61% |
| `extract_if` — call-path union | 6.86% | 7.82% | 6.40% | 4.89% |
| Cache queue helpers combined — union | 24.33% | 29.30% | 24.16% | 19.40% |
| ROB schedule/execute/complete — union | 19.22% | 13.75% | 16.01% | 24.67% |
| Register queries — union | 4.49% | 3.21% | 3.50% | 3.18% |
| DRAM address mapping — union | 0.02% | 0.00% | 0.02% | 13.19% |
| Trace reader `operator()` — exclusive | 4.37% | 4.30% | 5.22% | 0.07% |
| Stable partition — union | 4.18% | 7.17% | 4.83% | 3.32% |
| Allocator — union | 5.32% | 4.88% | 5.47% | 2.17% |

The summary JSON retains both-repeat ranges. Some individual helper shares vary
more than component totals, especially stable partition; these are ranking
estimates, not predicted speedups or precise per-instruction stall attribution.

## Recommended sequence of experiments

### 1. Isolate trace refill work from the per-instruction fast path

**Evidence.** The v2 `bulk_tracereader::operator()` prologue subtracts/probes
`0x20000` bytes in 4 KiB steps, then reserves another `0x148` bytes. This happens
before the buffer-size/refill condition. In SQLite and GCC, approximately 68% and
70% of that symbol's local cycle samples land in the probe-loop compare/branch.
The function itself occupies about 4–5.5% of total cycles in these v2 profiles.
Additional `rep movsq` samples expose raw-record copying.

**Proposed fix.** First isolate the refill into a cold/non-inlined helper so normal
returns do not reserve the large frame. Confirm the resulting assembly: merely
extracting a function that the compiler reinlines would not solve this. In a
separate subsequent patch, assess reading directly into the typed trivial buffer
and passing raw records by const reference through constructor/transform layers.
Keep stack-protection flags and the model unchanged.

**Expected files.** [inc/tracereader.h](../../../inc/tracereader.h), potentially
[src/tracereader.cc](../../../src/tracereader.cc); raw-copy work also touches
[inc/instruction.h](../../../inc/instruction.h).

**Regression requirements.** Empty/singleton streams, exact and partial refills,
trailing lookahead, taken branches across refills, trace restart, v1/v2/CloudSuite
records, and payload-enabled builds. Preserve instruction IDs, fields and branch
targets. Reuse the existing refill tests and full workload parity gate.

### 2. Remove avoidable cache queue-helper and iterator work

**Evidence.** The combined helper union is 19–29% of total cycles across all four
workloads. The tag-queue `get_span` specialization alone is roughly 9–11% exclusive.
It computes deque distance, copies two multiword iterators, calls iterator advance,
and returns an iterator pair even for cases that can do no work. The extraction
helper similarly copies/returns iterator state. SQLite annotations concentrate
around vector loads of that state; this is not evidence of a long linear scan
inside `get_span` (deque distance is constant-time).

The AMD native event describes loads unable to complete because of a
non-forwardable conflict with an older store. In the SQLite conflict profile,
**52.46% of weighted conflict events** fall in the combined cache-helper union,
including **28.98% in extraction** and **8.87% in `get_span`**. Another 22.78% is
exclusive in `CACHE::operate`. These are conflict-event shares, not cycle shares.
Assembly has scalar iterator updates followed by wider loads; this is a plausible
mechanism worth isolating, not a precise latency diagnosis. Ordinary PMU samples
can skid past the instruction responsible for an event.

**Proposed fix.** Start with narrowly scoped empty-queue/zero-work call-site guards,
then examine iterator-copy elimination and compiler inlining separately. Measure
both conflict density and unprofiled KIPS after each patch. Retain assertions and
avoid replacing every deque based solely on this profile.

**Expected files.** [src/cache.cc](../../../src/cache.cc),
[inc/util/span.h](../../../inc/util/span.h), and
[inc/util/algorithm.h](../../../inc/util/algorithm.h).

**Regression requirements.** Preserve negative-bandwidth validation, predicate
invocation order/count, queue order, partial admission and backpressure. Keep
upstream rotation and prefetcher cycle hooks running at the original times.
Do not skip an entire cache tick merely because its current queues are empty.
Use the span/extract/transform tests plus cache translation, fairness and retry cases.

### 3. Inline the small register-allocation queries

**Evidence.** `isValid`, `isAllocated` and `count_free_registers` jointly account
for 3.2–4.5% of sampled cycles, largely from repeatedly traversed ROB paths. They
are small out-of-line definitions, similar in shape to the earlier bandwidth
helpers, although their eventual speedup must be measured independently.

**Proposed fix.** Move the small definitions to the header while retaining the
checked `vector::at` lookup and its exception behavior. Keep query/caller order
unchanged. Do not combine this with a scheduling-policy change.

**Expected files.** [inc/register_allocator.h](../../../inc/register_allocator.h)
and [src/register_allocator.cc](../../../src/register_allocator.cc).

**Regression requirements.** Invalid physical-register indices, rename/free/valid
transitions, register exhaustion, and the existing register/scheduling tests.
The allocation check preceded the `scheduled` test in
`O3_CPU::schedule_instruction` when this was measured. It was moved behind that
test on 2026-09-21, a scheduler fix that removes most `isAllocated` and
`count_free_registers` calls from the ROB walk, so the evidence above is pre-fix;
re-profile before acting on it, and keep `isAllocated`'s new index bound.

### 4. Reduce repeated nonzero DRAM mapping work for mcf

**Evidence.** Mapping is approximately 13.2% of mcf's cycles and negligible in the
other three windows. Swizzling repeatedly constructs dynamic extents, checks
bounds and calls extent-size helpers for row segments. `schedule_packet` computes
bank indices for both comparator operands repeatedly during `min_element`.
The earlier zero-width shortcut removed only the zero-width portion of this work.

**Proposed fix.** First investigate precomputed immutable geometry/shift/mask
information or a cheaper equivalent swizzle. Treat per-request coordinate caching
as a separate patch with explicit lifetime/update rules. Preserve the existing
queue comparison and tie handling.

**Expected files.** [inc/dram_controller.h](../../../inc/dram_controller.h),
[src/dram_controller.cc](../../../src/dram_controller.cc), and possibly extent
helpers after separate measurement.

**Regression requirements.** Reuse the independent mapping oracle; extend it for
geometry boundaries, partial row segments, channel/rank/group combinations, request
merging/reuse, refresh and write-drain transitions. Keep the already documented
one-bank/one-group nontermination issue separate from a behavior-neutral patch.

### 5. Reduce ROB traversal while preserving exact selection order

**Evidence.** Schedule/execute/complete paths occupy 13.7–24.7% inclusively;
the three loop bodies themselves account for about 9–21% exclusively. Each scans
from the ROB head repeatedly. mcf is the strongest case.

**Proposed fix.** Design explicit candidate/frontier tracking for one stage at a
time. Begin only after writing down its readiness, ordering and invalidation
invariants; an event queue is not automatically equivalent to program-order scans.
Register-query inlining should be measured first so its gain is not counted twice.

**Expected files.** [src/ooo_cpu.cc](../../../src/ooo_cpu.cc),
[inc/ooo_cpu.h](../../../inc/ooo_cpu.h), and focused scheduling tests.

**Regression requirements.** Multiple same-cycle completions, register and memory
wakeup, execution-width limits, register pressure, branch stalls, warmup transitions,
and staggered multicore progress. Preserve the current checks and exact selection
order before considering any modeling correction.

### 6. Revisit partition buffers and remaining allocations

**Evidence.** Stable-partition call paths are roughly 3–7%; allocator call paths
are roughly 2–5.5%. Both overlap other rows. A sampled allocation path alone does
not prove every call is avoidable, and container code may account for cycles
outside allocator functions.

**Proposed fix.** After the empty-work changes, reprofile before introducing reusable
partition scratch storage or specialized stable compaction. A generic replacement
must preserve the side effects of `try_hit` and `handle_miss`, not just final order.

**Expected files.** `src/cache.cc`, its queue/scratch declarations in `inc/cache.h`,
and targeted stable-order/side-effect tests if a prototype is justified.

## Evidence and reproduction

Raw evidence and scratch analysis scripts are preserved at:

```
/home/rbera/work/alakazam/champsim-perf-results/2026-09-15-perf/
```

`references.json` and `reference-manifest.json` contain the original exact commands
and input hashes. `run_perf.py` replays them with a fresh statistics path and
checks complete reported-result parity after every run. Each `core-*`, `cache-*`,
`stalls-*`, `cycles-*`, `stlf-*` and `plain-*` directory contains its command,
stdout/stderr, statistics and result metadata. Sampled directories also contain
`perf.data`, decoded stacks, exclusive reports and selected assembly annotations.
`analyze_stacks.py`, `summarize_results.py`, `verify_profiles.py`, the CSV summaries
and `final-validation.json` preserve the reduction and independent checks.
These are analysis scripts; no new simulator functionality was introduced.

The essential collection/report commands are:

```bash
perf stat -x ';' -e '{cycles:u,instructions:u,branches:u,branch-misses:u}' \
  -e task-clock,context-switches,cpu-migrations,page-faults -- \
  taskset -c 14 <archived simulator command with a fresh --toml path>

perf record -e cycles:u -F 499 --call-graph dwarf,16384 -m 256 \
  -o perf.data -- taskset -c 14 <same simulator command>

DEBUGINFOD_URLS= perf report -i perf.data --stdio --no-children --call-graph none
DEBUGINFOD_URLS= perf annotate -i perf.data --stdio --percent-type local-period \
  --symbol '<exact demangled symbol>'
```

The attempted IBS `perf mem record` probe could not open its event in per-thread
mode and requested CPU-wide collection. That failed probe is retained and supplies
no performance evidence. The accepted conflict profile instead samples
`ls_bad_status2.stli_other:u` on the owned process.

## Limits and next action

These results prioritize experiments; they do not establish achievable speedup.
The workload windows are short, single-core and payload-disabled, and the host
core is shared. Component/call-path weighting is approximate, source inlining and
sample skid limit instruction-level attribution, and the large trace stack frames
truncate some ancestor chains. No top-down stall decomposition or precise memory
latency conclusion is claimed. Exported TOML still omits warmup cache/DRAM counters.

Start with the trace-refill fast-path separation, then the cache empty-work/iterator
changes. For every candidate, keep `dram-model=legacy`, run focused contract tests
and complete paired statistics comparisons, measure fresh unprofiled KIPS, and
append the actual result and commits to the running optimization log before
starting the next patch. Retain only improvements that pass that gate.
