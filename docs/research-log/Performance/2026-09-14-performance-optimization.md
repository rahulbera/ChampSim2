# Performance optimization project log — 2026-09-14

Work proceeds on `feat/perf-fix` in `/home/rbera/work/alakazam/champsim-perf-fix`.
The starting revision is `b1fb06b9`. The
[initial investigation](2026-09-14-fixed-ptw-hotspots.md) supplies the measured targets.

## Ground rules and common validation

Each optimization is implemented, regression tested, measured and recorded before
the next begins. A change is retained only after its complete phase statistics
match the preceding implementation. A mismatch is investigated before proceeding;
changing expected statistics is not an acceptable way to pass this gate.
“Inert” below means identical observed simulator results over the stated checks,
not a proof covering every possible workload or configuration.

Every simulation uses `dram-model=legacy`; release/test builds use
`WITH_RAMULATOR2=0` and separate object directories. Primary KIPS measurements use
**detailed PTW**, the same four protected traces and GLC comparison overlay as the
initial investigation, 1M warmup + 3M ROI, three before/after repetitions, and a pinned CPU (8 for steps 1–2; 14 from
step 3 onward, following the SMT-contention finding below).
Order alternates within each pair. KIPS counts actual warmup plus ROI retirement
over whole-process wall time. Builds and other experiments do not overlap timing.
Record each pair's fresh baseline, rather than comparing against historical timing.

Additional regressions cover detailed/fixed PTW on all four traces and variations
in page seeds, clocks, prefetching and legacy DRAM geometry. Compare complete
reported phase-statistics hashes and effective configuration, plus warmup retirement
and cycle counts, not just IPC or cycle count. The TOML exporter omits warmup
cache/DRAM counters; these checks do not claim coverage of unexported counters.
Use focused tests where an implementation has additional boundary contracts, and
run the C++ suite after each production change. All raw commands, binaries, input
hashes, outputs and measurements are saved outside Git at:

```
/home/rbera/work/alakazam/champsim-perf-results/2026-09-14-optimizations/
```

The initial sequence is: debug-only allocations; cycle-loop temporary allocations;
bandwidth helpers; repeated DRAM decoding; unnecessary LSQ scans; trace instruction
copies. Larger scheduling rewrites require separate designs and are not folded
into these bounded patches. Every entry below records the actual retained change.

## 1. Eliminate cache allocations used only for debug printing

**Issue.** `CACHE::operate()` grows `channels_bandwidth_consumed` on every
cycle, although its only consumer is compiled out unless `DEBUG_PRINT` is enabled.
The measured one-core hierarchy performs 25 such allocations per global tick.

**Fix.** Guard the append with the existing compile-time `champsim::debug_print`
condition. Bandwidth consumption, channel traversal/rotation, request handling,
and debug-build output are unchanged.

**Files touched.**

1. `src/cache.cc`: compile out debug-only vector growth in normal builds.
2. `docs/research-log/Performance/2026-09-14-performance-optimization.md`: record
   this change and its validation evidence.

**Implementation commit.** `fbcc11ff` (`perf: skip cache bandwidth debug allocations
in release builds`). Common measurement infrastructure was established separately
in `3991ee75`, `cf23636c`, and `e6f937e4`.

**Regression verdict.** The C++ suite passes: 17,049 assertions, 863 passed cases,
7 native-backend cases skipped in this legacy-only build. All 16 paired regression
cases (32 runs, 100k warmup + 500k ROI each) have identical complete reported phase
statistics, effective configuration, and warmup/ROI retirement and cycle counts.
The short SQLite allocation probe also matches the pre-optimization statistics
exactly: allocation calls fall from **1,519,975 to 562,350**, a reduction of
**957,625 = 25 × 38,305 global ticks**. Allocation counting is excluded from timing.
Read-only code review found no production-code blocker; its harness finding was
fixed in `e6f937e4` so parity gates cannot disappear under Python `-O`.

**KIPS before/after.** Medians of three paired 1M/3M runs; parentheses give
minimum–maximum KIPS. All 24 timing runs also pass the complete parity gate.

| Trace | Before | After | Change |
|---|---:|---:|---:|
| sqlite | 188.20 (181.45–193.44) | 202.61 (195.85–207.08) | +7.66% |
| omnetpp | 199.07 (188.46–209.17) | 223.48 (216.53–229.15) | +12.26% |
| gcc | 248.09 (229.28–248.14) | 261.97 (246.67–263.67) | +5.59% |
| mcf | 65.54 (62.69–68.35) | 67.24 (64.75–69.61) | +2.60% |

**Retained: inert over the checked cases.** All 12 individual pairs favor the
candidate. Unrelated unpinned simulations ran concurrently on this shared host,
so a quiet-host replication is still needed for precise speedup estimates,
especially the small mcf gain. A separate `DEBUG_PRINT` syntax build also passes;
this check does not claim a full debug-build simulation comparison.

**Evidence.** `01-debug/{build.log,cpp-tests.log,regression/,alloc-before/,
alloc-after/,timing/}` beneath the evidence root above. `source-commit.txt` and
`champsim` preserve the exact measured implementation.

## 2. Reuse phase-local scheduler and CPU views

**Issue.** Each global tick rebuilds the operable and CPU views, copies the trace
index vector, and allocates a new phase-completion bit vector. The initial
allocation investigation attributed ten allocations per tick to these temporary
containers in the one-core hierarchy.

**Fix.** Cache the component references for the duration of a phase, pass CPU and
trace-index vectors by const reference, and reuse the working sort and completion
buffers. Copy the canonical operable order into the working buffer before every
unchanged `std::sort`; retaining the previous sorted order would alter equal-time
ties. Phase callbacks continue traversing the canonical order. This relies on the
fixed component membership/order supplied by `static_environment` within a phase.

**Files touched.**

1. `src/champsim.cc`: reuse phase-local storage and eliminate by-value invariant
   arguments, preserving scheduling, trace feeding, watchdog and phase boundaries.
2. `test/cpp/src/502-phase-order.cc`: add synthetic mixed-clock/two-CPU tests for
   scheduling ties, callback order and exact callback tick, staggered completion,
   and EOF-triggered completion of every remaining CPU.
3. `docs/research-log/Performance/2026-09-14-performance-optimization.md`: record
   this change and its evidence.

**Implementation commit.** `482d080f` (`perf: reuse phase-local scheduler and CPU
views without changing order`).

**Regression verdict.** The initial scheduling test passes on the old loop before
implementation. The final strengthened tests and C++ suite pass: **17,059
assertions, 865 passed cases, 7 native skips**. All **16 paired workload cases / 32
runs** match complete reported statistics, effective configuration and retirement/
cycle counts. Review found no source issue; its two test-coverage findings were
addressed and re-reviewed. The short SQLite allocation probe matches the original
pre-optimization statistics and drops from **562,350 to 179,298** calls, removing
383,052 calls (ten per tick plus a net two phase-level calls). These synthetic
phase tests exercise two CPU records, not a full multicore cache/DRAM simulation.

**KIPS before/after.** Medians of three paired 1M/3M runs; parentheses give
minimum–maximum KIPS. All 24 timing runs pass the complete parity gate.

| Trace | Before | After | Change |
|---|---:|---:|---:|
| sqlite | 186.62 (185.53–187.52) | 191.56 (191.00–193.33) | +2.65% |
| omnetpp | 206.09 (204.60–207.30) | 212.65 (210.11–213.53) | +3.19% |
| gcc | 249.75 (249.26–254.25) | 259.20 (257.03–260.05) | +3.78% |
| mcf | 66.00 (65.77–66.65) | 67.45 (67.45–67.80) | +2.19% |

**Retained: inert over the checked cases.** All 12 individual pairs favor the
candidate. The measured gain is modest, and the shared-host limitation still
applies; confirm its size on a quiet machine.


**Evidence.** `02-cycle/{build.log,cpp-before.log,cpp-reviewed.log,regression/,
alloc-after/,timing/,source-commit.txt,champsim}` under the common evidence root.

## 3. Inline small bandwidth-accounting helpers

**Issue.** Tiny `bandwidth` operations are defined in a separate translation unit.
Release assembly therefore pays function-call overhead for a comparison, load,
subtraction or reset in cache and core loops, and the compiler cannot simplify the
surrounding loops across those calls. The initial profiles attributed 9–15% of
exclusive samples collectively to these helpers; that sampling share is not a
prediction of achievable speedup.

**Fix.** Put the small definitions in the class header and leave only construction
of the exhaustion exception in an out-of-line private helper. Preserve signed
accounting, underflow detection, the state update before an exception, its exact
message, and reset behavior. Compiler flags and LTO settings are unchanged.

**Files touched.**

1. `inc/bandwidth.h`: inline constructor, consumption, predicates, accessors and
   reset; declare the throwing slow-path helper and required header dependencies.
2. `src/bandwidth.cc`: retain the original exception construction in that helper.
3. `test/cpp/src/036-bandwidth.cc`: pin exception type/message, post-underflow state,
   negative-consumption recovery and reset through the public API.
4. `docs/research-log/Performance/2026-09-14-performance-optimization.md`: record
   implementation and evidence.

**Implementation commit.** `bb3f195720d134bd0d46a0f9649062234832f856`.

**Regression verdict.** The added boundary test passes against the old helpers
before implementation. The final C++ suite passes **17,068 assertions, 866 cases,
7 native skips**. All **16 paired workload cases / 32 runs** preserve complete
reported statistics, effective configuration, and instruction/cycle counts.
Read-only review found no issue. The short allocation probe remains at 179,298
calls with the original phase-statistics hash. `nm -C` now finds only
`bandwidth::throw_exceeded` as an out-of-line bandwidth method in the release
binary, confirming the small helpers were inlined in this build.

**KIPS before/after.** A complete CPU-8 campaign passed all 24 parity checks but
showed a late performance reversal on GCC and a simultaneous slowdown on mcf.
A process observation confirmed another unpinned simulation executing on CPU 24,
the SMT sibling of CPU 8. Its raw data are retained in `timing/`: median KIPS
202.58→266.19 (SQLite), 221.78→287.17 (omnetpp), 271.09→343.57 (GCC),
73.12→97.98 (mcf). GCC's candidate range is 229.81–345.85 and includes a slower
individual pair. Those medians do not establish precise single-run gains.

The complete CPU-14 repeat passes all 24 parity checks. Medians and ranges:

| Trace | Before | After | Change |
|---|---:|---:|---:|
| sqlite | 208.80 (205.58–209.04) | 266.50 (266.48–266.69) | +27.64% |
| omnetpp | 229.64 (228.73–230.58) | 293.57 (292.72–293.91) | +27.84% |
| gcc | 281.11 (278.83–281.55) | 352.21 (352.07–353.93) | +25.29% |
| mcf | 72.62 (72.24–72.92) | 100.50 (100.39–100.73) | +38.38% |

**Retained: inert over all checked cases.** All 12 CPU-14 pairs favor the
candidate. The repeated campaign has tighter ranges, but the host is still
shared and the core is not reserved. A contemporaneous process snapshot found
no other ChampSim process on CPU 14 or its sibling 30; this is an observation,
not continuous proof of exclusive use.


**Evidence.** `03-bandwidth/{build.log,cpp-before.log,cpp-after.log,regression/,
alloc-after/,symbols.txt,timing/,timing-cpu14/,source-commit.txt,champsim}` beneath the common
root. Source-level neutrality still needs broader workload/compiler coverage
before being treated as universal.

## 4. Skip zero-width legacy DRAM swizzling

**Issue.** The mapper iterates over row slices even when the selected channel,
bank-group or bank field has zero width. Every XOR operand is then zero. The
one-group primary configuration pays this cost repeatedly during request lookup
and scheduling.

**Fix.** Return the input field immediately for zero-width fields with a nonzero
segment size, after preserving row-slice construction/validation. All nonzero-width
hashing, arbitration and request state remain unchanged. This is the narrow first
part of the repeated-decoding target; caching request indices or rewriting the
mapper is deferred for a separate change with stronger lifetime contracts.

**Files touched.**

1. `src/dram_controller.cc`: bypass row XORs that cannot change the field.
2. `test/cpp/src/703-dram-address-mapping.cc`: compare six decoded coordinates
   against an independent integer/XOR oracle over 198 geometries × 64 addresses;
   cover partial row segments, high address bits and zero-width fields, including
   an arbitrary nonzero input field to the public swizzle helper.
3. `docs/research-log/Performance/2026-09-14-performance-optimization.md`: record
   the measured scope, validation and the separate zero-step issue.

**Implementation commit.** `63b04767` (`perf: skip zero-width legacy DRAM swizzle
work`).

**Regression verdict.** The new oracle passes on the previous implementation.
The final C++ suite passes **29,746 assertions, 868 cases, 7 native skips**. All
**16 paired workload cases / 32 runs** preserve complete reported statistics,
effective configuration and instruction/cycle counts, including multichannel,
multirank and nonzero-bank-group configurations. Review found no issue. The short
allocation probe remains at 179,298 calls and matches the original statistics.

**KIPS before/after.** CPU-14 medians of three paired 1M/3M runs; parentheses
give minimum–maximum KIPS. All 24 timing runs pass the complete parity gate.

| Trace | Before | After | Change |
|---|---:|---:|---:|
| sqlite | 271.59 (270.22–271.86) | 280.29 (279.20–281.86) | +3.20% |
| omnetpp | 298.82 (298.03–299.19) | 308.67 (308.61–309.44) | +3.30% |
| gcc | 358.72 (356.82–360.76) | 369.00 (366.35–370.25) | +2.86% |
| mcf | 101.87 (101.43–103.06) | 115.30 (114.86–115.63) | +13.18% |

**Retained: inert over the checked terminating cases.** All 12 paired timings
favor the candidate. Shared-host replication limits still apply.


**Separate pre-existing issue.** `banks=1, bankgroups=1` produces a zero-bit
segment, so the legacy loop never advances. A 1k/4k SQLite invocation with the
pre-patch `03-bandwidth/champsim` binary timed out after three seconds; the source
loop explains the nontermination. This patch deliberately excludes `segment_size=0`
from its fast path and does not claim regression coverage for that nonterminating
case. Handle it as a separate correctness fix before relying on that geometry.

**Evidence.** `04-dram/{build.log,cpp-before.log,cpp-after.log,regression/,
alloc-after/,zero-step-before/,timing/,source-commit.txt,champsim}` under the common
root.

## 5. Avoid LSQ readiness scans for absent memory operands

**Issue.** Every executing instruction scans the entire load queue and store queue
to mark matching entries ready, including arithmetic/branch instructions with no
memory operations. Load-only instructions also scan the store queue and vice versa.

**Fix.** Guard the load-queue scan with a nonempty `source_memory` list and the
store-queue scan with a nonempty `destination_memory` list. `do_memory_scheduling`
creates entries from these lists, and the lists remain intact through execution;
completion updates `completed_mem_ops` rather than removing operands. Preserve
instruction readiness/execution marking, all remaining loop bodies and traversal
order. Broader ROB/ready-queue rewrites are deferred.

**Files touched.**

1. `src/ooo_cpu.cc`: skip only the queue scans with no possible matching entry.
2. `test/cpp/src/251-execution-lsq-readiness.cc`: use actual memory scheduling to
   cover zero/two loads × zero/two stores × warmup/ROI, verify all matching entries
   receive the correct deadline, and unrelated entries remain unready.
3. `docs/research-log/Performance/2026-09-14-performance-optimization.md`: record
   the ownership invariant, checks and measurements.

**Implementation commit.** `2a7940b2efc521057f0a263d09e29386c05d4e06`.

**Regression verdict.** The new test passes on the previous implementation. The
final C++ suite passes **29,810 assertions, 869 cases, 7 native skips**. All
**16 paired workload cases / 32 runs** match complete reported statistics,
effective configuration and instruction/cycle counts. Review confirmed the
operand-list invariant and found no issue. The allocation probe remains at
179,298 calls with the original statistics. Arbitrary external mutation of
instruction operand lists after creating LSQ entries is outside that invariant.

**KIPS before/after.** CPU-14 medians of three paired 1M/3M runs; parentheses
give minimum–maximum KIPS. All 24 timing runs pass the complete parity gate.

| Trace | Before | After | Change |
|---|---:|---:|---:|
| sqlite | 281.07 (278.44–281.13) | 283.54 (282.38–283.67) | +0.88% |
| omnetpp | 309.04 (308.85–310.59) | 313.64 (312.73–315.52) | +1.49% |
| gcc | 369.92 (368.19–371.16) | 376.13 (373.48–377.05) | +1.68% |
| mcf | 114.94 (114.09–116.08) | 117.48 (117.16–117.89) | +2.20% |

**Retained: inert over the checked cases.** All 12 pairs favor the candidate.
The small gain warrants quiet-host replication before relying on its exact size.


**Evidence.** `05-lsq/{build.log,cpp-before.log,cpp-after.log,regression/,
alloc-after/,timing/,source-commit.txt,champsim}` beneath the common root.

## 6. Move consumed trace instructions through the frontend

**Issue.** Branch-target assignment uses reverse `std::adjacent_difference`,
which copies instruction objects and their operand vectors. The bulk reader and
frontend also copy queue entries immediately before removing them. These copies
allocate storage and move data without extending any instruction's useful lifetime.

**Fix.** Visit adjacent instructions forward, moving the current instruction into
the unchanged branch-target helper while reading the next instruction by reference.
The helper reads only the next IP, so original adjacent pairs remain available;
leave the final lookahead untouched. Move the consumed bulk-reader entry on return
and the initialized input-queue entry into `IFETCH_BUFFER`. Keep branch prediction,
warmup folding, stop-fetch decisions, queue removal and readiness timestamps in
their original order. This relies on the current helper's read-only next-instruction
contract and on no references being retained to the consumed queue entry.

**Files touched.**

1. `inc/tracereader.h`: remove branch-target copies and move the consumed reader
   entry; include the iterator/move utilities explicitly.
2. `src/ooo_cpu.cc`: move the initialized input-queue entry into the fetch buffer.
3. `test/cpp/src/082-branch-targets.cc`: cover empty/singleton ranges and preservation
   of an arbitrary final lookahead target.
4. `test/cpp/src/087-tracereader-v2.cc`: check operands and targets over 260 records,
   including taken branches at both refill boundaries; check optional payload fields
   when compiled with `CHAMPSIM_TRACE_MEMORY_VALUES=1`.
5. `docs/research-log/Performance/2026-09-14-performance-optimization.md`: record
   checks, review correction and measurements; correct step 3's evidence reference.
6. `CLAUDE.md`: link this running log for future project orientation.

**Implementation commit.** `f7ee81b9da04de212f89972795b9cdb2e4c56f9a`.

**Regression verdict.** The strengthened tests pass on both old and optimized
implementations. The normal C++ suite passes **32,664 assertions, 871 cases, 7
native skips**; the payload-enabled suite passes **34,757 assertions, 876 cases,
7 native skips**. Their object roots and binaries are separate. Python passes
**66 tests with 4 native skips**, and all **6 performance-tool tests** pass.
All **16 paired workload cases / 32 runs** match complete reported statistics,
effective configuration, and instruction/cycle counts. Review found the initial
refill test lacked taken branches at its boundaries; records 126 and 253 now
explicitly exercise nonzero targets from the next refill, and review confirmed
the correction. No source blocker remains.

The short allocation probe falls from **179,298 to 134,612 calls** (44,686 fewer,
24.92%) while retaining the original pre-optimization statistics. The throughput
runs use the normal, payload-disabled release build; payload coverage here is
from the C++ suite, not a separate full-simulation timing campaign.

**KIPS before/after.** CPU-14 medians of three paired 1M/3M runs; parentheses
give minimum–maximum KIPS. All 24 timing runs pass the complete parity gate.

| Trace | Before | After | Change |
|---|---:|---:|---:|
| sqlite | 282.50 (282.06–283.27) | 289.46 (289.25–291.28) | +2.46% |
| omnetpp | 314.00 (312.79–314.85) | 321.04 (320.67–322.22) | +2.24% |
| gcc | 376.51 (376.20–377.13) | 388.13 (387.05–388.61) | +3.09% |
| mcf | 117.36 (115.94–117.46) | 119.07 (118.73–119.74) | +1.45% |

**Retained: inert over the checked cases.** All 12 pairs favor the candidate.
As with the small LSQ gain, quiet-host replication should precede reliance on
its exact magnitude.

**Evidence.** `06-trace/{build.log,test-build.log,payload-build.log,
cpp-before-reviewed.log,payload-before-reviewed.log,cpp-after.log,payload-after.log,
python-tests.log,perf-python-tests.log,regression/,alloc-after/,timing/,
source-commit.txt,champsim}` beneath the common root. Earlier pre-review test logs
are retained as well; the `*-before-reviewed.log` files contain the strengthened
boundary tests against the old implementation.

## Commit trail

These are separate implementation and measurement-log commits for each retained
optimization; the implementation snapshots in the evidence archive name the
corresponding source commit.

| Optimization | Implementation | Measurement log |
|---|---|---|
| 1. Cache debug allocations | `fbcc11ff` | `ebe6081e` |
| 2. Phase-local containers | `482d080f` | `c9d643fc` |
| 3. Bandwidth helpers | `bb3f1957` | `5da1d1fc` |
| 4. Zero-width DRAM swizzle | `63b04767` | `d49f2ea5` |
| 5. LSQ scan guards | `2a7940b2` | `6ead68fa` |
| 6. Trace/frontend moves | `f7ee81b9` | `2c275ed0` |

Shared instrumentation and log setup: `3991ee75`; counter-output validation:
`cf23636c`; parity checks that survive Python `-O`: `e6f937e4`. The step-6 log
commit also corrects step 3's CPU-14 evidence reference and links this document
from `CLAUDE.md`. No optimization changes compiler flags or simulation settings.

## Combined result against the original baseline

A fresh CPU-14 campaign compares the original `b1fb06b9` release snapshot
(`00-baseline`, a binary file) directly with the final `f7ee81b9` release snapshot
(`06-trace/champsim`). Both endpoints match the hashes recorded by their individual
campaigns. This comparison measures the combined result directly; it does not
multiply the incremental medians obtained at different times or on different CPUs.

Every run uses legacy DRAM and detailed PTW, 1M warmup + 3M ROI, with three
alternating-order paired repetitions per trace. Medians and minimum–maximum KIPS:

| Trace | Original | Final | Throughput change |
|---|---:|---:|---:|
| sqlite | 191.53 (191.12–191.86) | 290.33 (290.24–290.64) | +51.58% |
| omnetpp | 211.46 (209.88–213.24) | 319.11 (317.27–321.29) | +50.91% |
| gcc | 259.05 (258.09–261.52) | 388.18 (386.87–390.76) | +49.85% |
| mcf | 69.25 (68.50–69.67) | 119.22 (118.51–119.55) | +72.16% |

All **24 cumulative runs / 12 pairs** preserve reported phase statistics,
effective configuration and instruction/cycle counts exactly, and every pair
favors the final binary. The short SQLite probe's total allocation count falls
from **1,519,975 to 134,612** across the six changes: **91.14% fewer allocation
calls**, measured separately from throughput. These are allocation counts for the
stated short probe, not an estimate of bytes saved or a whole-workload memory bound.

The final audit rereads all saved statistics and verifies their configuration and
count fingerprints, each campaign's KIPS medians, and the archived binary chain.
It covers **384 comparison runs**: 192 short regression runs and 192 timing runs,
including the retained CPU-8 bandwidth campaign and the final cumulative campaign.
All have within-case result parity. Historical regression manifests name the
mutable build output; their recorded binary hashes match the archived snapshots.
The current release binary also matches the final measured snapshot.

The final C++ results are 871 passed cases in the normal build and 876 in the
payload-enabled build, each with seven native-backend skips. Python reports 66
tests with four native skips and no failures; all six performance-tool tests pass.
Read-only review of all six production changes found no remaining blocker.
The verdict is **behavior-neutral over this checked scope**.

**Evidence.** `cumulative/{manifest.json,runs.json,summary.csv,...}`,
`cumulative.log`, `audit_evidence.py`, and `final-evidence-audit.json` under the
common evidence root. The original binary SHA-256 is
`6b1ccf78b6e19c739566a9315193711fd1006350d6562a2f5dfd85c236727bbd`;
the final binary SHA-256 is
`e9ab12cd4326dff3e8b92a07f4b6e430a100090820dc85558e7e95c6849d375b`.

## Remaining validation before mainline

1. **Longer and broader workloads.** Repeat exact statistics comparisons over
   longer trace regions and additional workloads, especially unusual control flow,
   high memory-level parallelism, and trace EOF/restart boundaries. Four traces
   and the current seeds cannot prove universal neutrality.
2. **Full multicore execution.** Exercise real shared-cache/DRAM traffic with
   staggered core completion and mixed clocks. The new phase tests use two CPU
   records and synthetic operables; the measured workloads are single-core.
3. **Independent performance replication.** Repeat on an exclusive physical core
   with controlled SMT and frequency settings, more repetitions, and another
   compiler/host. This shared-host study supports the observed gains but cannot
   establish their exact magnitude everywhere, particularly the small LSQ/copy gains.
4. **Build modes and native backend.** Payload-enabled coverage is currently unit
   testing; add full payload simulation comparisons and sanitizer checks for the
   move/lifetime paths. Step 1's debug check is a syntax build, not a full debug-output
   comparison. Native Ramulator2 was disabled throughout this user-requested legacy
   campaign and needs its separate integration checks before mainline.
5. **Unexported state.** The exported TOML omits warmup cache/DRAM counters.
   Tests compare the available full ROI statistics plus warmup retirement/cycles,
   not hidden warmup counters or every transient internal state.
6. **Separate correctness issue.** Fix and test the pre-existing zero-step DRAM
   mapping hang for `banks=1, bankgroups=1` independently; it is explicitly outside
   these terminating-case performance comparisons.

The remaining candidates include repeated ROB/scheduler scans and repeated DRAM
request-coordinate decoding. They require separate designs for ordering and
object lifetime; no broader rewrite is included in this pass. Preserve the raw
results archive with the eventual review materials before deleting scratch
checkouts: the source, tests and this log are tracked in Git, while raw evidence
currently lives at the external path documented above.

## Linux perf follow-up — 2026-09-15

The [Linux perf investigation](2026-09-15-linux-perf-hotspots.md) profiles the
unchanged final release with hardware counters and call stacks. All 42 collected
simulation runs preserve reported-result parity; the main eight cycle profiles
contain 67,113 samples with zero loss. It identifies cache iterator/helper costs,
per-instruction trace-reader stack probing, register-query calls, nonzero DRAM
swizzling on mcf, and remaining ROB scans.

These are candidate targets, not applied optimizations. No new before/after KIPS
gain is claimed. The recommended next patch isolates trace-refill stack work,
followed by cache empty-work/iterator changes, with the same per-fix regression,
fresh KIPS and commit-recording gates used above. The report and its portable JSON
summary contain methods, ranges, source locations, proposed fixes and required
regressions; raw evidence is in `champsim-perf-results/2026-09-15-perf/`.

## Second optimization pass — 2026-09-15

The user approved proceeding from low to high risk after the Linux perf study.
Each candidate still needs a separate source patch, exact reported-result
regressions, fresh paired KIPS, review and this log before the next candidate.
All runs use `dram-model=legacy`; native Ramulator2 remains disabled in the build.
The new raw-evidence root is:

```
/home/rbera/work/alakazam/champsim-perf-results/2026-09-15-optimizations/
```

`00-baseline` archives the unchanged `d1c80c4a` release (same binary as the
first pass's `06-trace/champsim`). Short checks retain the existing four-trace,
16-case configuration matrix. Throughput comparisons remain three alternating
paired 1M/3M runs per trace on CPU 14, with no concurrent builds or test campaigns.

**Expanded high-risk gate.** Before retaining DRAM mapping or ROB traversal
changes, compare before and after with **5,000,000 warmup + 50,000,000 simulation
instructions on one trace from each of the 14 workloads** in the supplied SPEC26
directory. Select the lowest available simpoint per workload, record the complete
inventory and hash each selected input in `spec26-traces.json`. The selected
workloads are stockfish, ntest, sqlite, omnetpp, cpython, gcc, llvm, cppcheck, abc,
vpr, gem5, sealcrypto, ns3 and zstd. GCC, LLVM, cppcheck and gem5 begin at sp1;
the other selected workloads use sp0.

These long checks compare complete exported phase statistics, effective
configuration, and actual warmup/ROI retirement and cycles. They are additional
regression coverage, not proof of equality of unexported transient state. Long
regression pairs may run on separate physical cores to finish the wider suite;
any such runs are excluded from reported KIPS. The paired runner now accepts
`--timeout` (seconds per simulation, default still 900) so the larger window need
not be constrained by the old short-run watchdog.

## 7. Isolate trace-refill stack work from instruction reads

**Issue.** Linux perf identified a roughly 128 KiB stack frame and its page-probe
loop on every v2 `bulk_tracereader::operator()` call. The large temporary buffers
are needed only when the 127-record buffer refills.

**Fix.** Extract the existing refill body into a private non-inlined helper.
The per-instruction path keeps its existing refill condition and move/pop; read
size, byte copying, conversion, EOF handling and branch lookahead remain unchanged.
GCC/Clang receive the noinline attribute. Compiler flags and stack protections
are unchanged. The release assembly has an 8-byte local stack adjustment in the
v2 ifstream operator; the helper retains the 4 KiB probe loop for its large frame.

**Files touched.**

1. `inc/tracereader.h` — move refill work into the private non-inlined helper.
2. `test/cpp/src/087-tracereader-v2.cc` — characterize a taken branch across the
   127-record refill boundary with a one-record final refill.
3. `docs/research-log/Performance/2026-09-14-performance-optimization.md` — record
   methods, commit, regression verdict and measured KIPS.

**Implementation commit.** `78679b11c9dbad609c5c4e7f531981bbccb1a1c8`.
The separate campaign setup/timeout change is `dae97740`; it changes no simulator
behavior and preserves the runner's previous 900-second default.

**Regression verdict.** **Retained: inert over the checked scope.** The new
boundary test passes on both the original and optimized headers. The full normal
C++ suite passes **32,666 assertions / 872 cases**, with 7 native skips; the separate
payload-enabled suite passes **34,759 assertions / 877 cases**, with 7 native skips.
Python passes 66 tests with 4 native skips; the 6 performance-tool tests pass.
All **32 short regression runs and 24 timing runs** preserve full exported phase
statistics, effective configuration and actual instruction/cycle counts. The
saved-output audit recomputes fingerprints and KIPS and checks archived binary
hashes. Read-only review found no behavior blocker. GCC/Clang were the supported
attribute path assessed here; MSVC performance and empty-stream misuse of the
existing reader interface were not newly validated.

**KIPS before/after.** Three paired CPU-14 1M/3M runs per trace, detailed PTW,
legacy DRAM. Medians with minimum–maximum ranges:

| Trace | Before | After | Change |
|---|---:|---:|---:|
| sqlite | 303.46 (301.59–304.29) | 318.76 (317.90–318.94) | +5.04% |
| omnetpp | 332.23 (331.48–333.15) | 353.59 (353.23–354.66) | +6.43% |
| gcc | 402.48 (401.60–403.03) | 432.67 (431.52–433.00) | +7.50% |
| mcf | 124.74 (121.87–124.89) | 125.40 (123.20–125.64) | +0.53% |

All nine v2 pairs favor the candidate. mcf's +0.53% median shift is smaller than
its observed variation and is not evidence of a reliable v1 speedup.

**Evidence.** `07-refill/` beneath the second-pass root contains the archived
`champsim`, `source-commit.txt`, before/after trace tests, normal/payload/Python
logs, release assembly, `review.txt`, `regression/`, `timing/`, and `audit.json`.

## 8. Remove raw trace-record copies

**Issue.** After refill isolation, each refill still copied bytes from a character
buffer into aligned records and passed each record by value through conversion.
The v2 record is 512 bytes; the release had three argument-copy sequences and a
second large refill buffer.

**Fix.** Read through the character representation of the existing aligned trivial
record array, retaining the exact read size and complete-record boundary. Pass
records by const reference into the instruction constructors. Construct owned
nonzero memory operands in their original order, retaining raw payload-slot
interpretation, ASIDs, classification, branch lookahead and EOF behavior.

**Files touched.**

1. `inc/tracereader.h` — direct record-buffer input and reference conversion;
   remove the redundant character buffer and memcpy.
2. `inc/instruction.h` — borrow raw constructor inputs and build owned operand
   vectors without modifying them.
3. `test/cpp/src/087-tracereader-v2.cc` — characterize sparse operands, input
   ownership and incomplete trailing records for v1, v2 and CloudSuite.
4. `docs/research-log/Performance/2026-09-14-performance-optimization.md` — record
   regression evidence, timing repeat and limitations.

**Implementation commit.** `1655c953bf96d2326dc3ec008da1ade7d022d66d`.
Step 7's accompanying log commit is `df1474b1`.

**Regression verdict.** **Retained: inert over the checked scope.** The expanded
trace characterizations pass on both old and new headers: **2,912 assertions /
9 cases**. The full normal suite passes **32,695 assertions / 875 cases**, with
7 native skips; the payload suite passes **34,788 assertions / 880 cases**, with
7 native skips. Python passes 66 tests with 4 native skips. All **32 short
regression + 24 initial timing + 40 repeated timing runs** preserve complete
exported phase statistics, effective configuration and actual instruction/cycle
counts. Independent saved-output auditing verifies all 96 runs and the binary
hashes. Read-only source review found no blocker. The formatted release rebuild
is byte-identical to the measured snapshot.

**KIPS before/after.** The first timing batch coincided with external build bursts
and has large ranges; it is retained as parity evidence but excluded from the
speed verdict. The fresh repeat uses **five alternating pairs**, CPU 14, detailed
PTW, legacy DRAM and 1M/3M. Our builds, reference runs and diagnostic probes had
finished. Other users' simulations still shared the host. Medians (min–max):

| Trace | Before | After | Change |
|---|---:|---:|---:|
| sqlite | 302.34 (298.29–304.72) | 305.87 (300.60–309.20) | +1.17% |
| omnetpp | 335.24 (329.73–338.23) | 339.95 (336.57–342.80) | +1.40% |
| gcc | 406.62 (397.53–411.86) | 416.91 (397.39–421.99) | +2.53% |
| mcf | 117.94 (117.09–119.46) | 119.62 (116.11–120.27) | +1.42% |

All five SQLite and all five omnetpp pairs favor the candidate; four of five GCC
pairs do (the remaining pair is −0.034%). This supports a modest v2 improvement,
but the precise magnitude needs a quieter-host repeat. mcf has overlapping ranges
and mixed pair directions; its median shift is not treated as a reliable v1 gain.

The v2 refill stack frame falls from roughly 128 KiB to 64 KiB, and the observed
memcpy/argument-copy sequences disappear. Separate perf counter replays preserve
full reported parity and have 100% active counters: host instructions fall 0.196%
on SQLite and 0.265% on GCC. These four replays overlapped regression preparation
and are mechanism evidence only, not KIPS measurements. Their initial scratch
checker rejected matching NaN sentinels; canonical full-field comparison, already
used by the main runner, corrected that checker without excluding any statistics.

**Evidence.** `08-raw-trace/` contains the archived binary/source patch/commit,
normal/payload/Python and before/after characterization logs, assembly, review,
`format-verification.json`, the original `timing/`, fresh `timing-repeat/`, paired
deltas, `instruction-counters/`, and `audit.json`. Completed campaign paths were
kept intact because saved commands contain their absolute working directories.

## 9. Register-query inlining — deferred after measurement

**Issue.** Linux perf attributed 3.2–4.5% of sampled cycles to `isValid`,
`isAllocated` and `count_free_registers`. Each has a short out-of-line body, but
inlining can also change the compiler's treatment of the surrounding ROB loops.

**Experiment and disposition.** First move all three unchanged expressions into
the class definition, preserving const signatures, checked physical-register
access, exceptions and scheduler ordering. Then test a narrower variant that
keeps only `isValid` and `isAllocated` inline. **Neither prototype is retained.**
The three-query variant slows mcf in all three pairs; the narrower variant has
no convincing overall benefit and its later measurements are badly affected by
shared-host variation. Restore the original production definitions and retain
the useful public-query characterizations.

**Files touched.**

1. `inc/register_allocator.h` — trial inline query definitions; restored to the
   original declarations in the retained tree.
2. `src/register_allocator.cc` — remove the trial definitions, then restore all
   three original out-of-line bodies.
3. `test/cpp/src/201-register-rename.cc` — retain const-query, bounds, exhaustion,
   rename/validity/retirement and actual RAT reset restoration checks.
4. `docs/research-log/Performance/2026-09-14-performance-optimization.md` — retain
   both experiments and the decision to defer.

**Commit.** Retained tests: `1d47029745a53c4ea2d03c7a6ed210b289c88243`. The rejected production
patches are archived beside their immutable trial binaries; they have no retained
implementation commit. Step 8's log commit is `3bcb0f7b`.

**Regression verdict.** Both prototypes are **inert over their checked simulation
scope**: each passes 32 short regression and 24 timing runs with complete reported
phase/configuration/count parity, **112 runs total**, independently audited. The
three-query prototype also passes the full normal and payload suites. Initial
focused tests pass on original and inline definitions; additional nonempty-bound
and real-reset cases pass against the original source in a separate small oracle.
After restoring production code, the normal suite passes **32,714 assertions /
876 cases** and the payload suite passes **34,807 assertions / 881 cases**, with
7 native skips each. The restored release is **byte-identical to step 8's binary**
(SHA-256 `daabf72fe90dcd67bbce71f2eda392ce7d0ea8fefcb1867a3a4bc4e349f634fe`).
The prior Python result applies to that identical release: 66 tests, 4 native
skips. Scoped review and re-review found no correctness blocker.

**KIPS before/after: all three queries.** Three alternating CPU-14 pairs, legacy
DRAM, detailed PTW, 1M/3M. Medians (min–max):

| Trace | Before | Trial | Change |
|---|---:|---:|---:|
| sqlite | 301.38 (299.40–303.16) | 307.27 (302.14–312.51) | +1.95% |
| omnetpp | 335.11 (333.89–337.14) | 341.35 (338.26–344.04) | +1.86% |
| gcc | 413.58 (411.28–419.19) | 419.88 (418.18–424.73) | +1.52% |
| mcf | 118.60 (115.62–118.66) | 115.38 (114.88–117.40) | -2.71% |

All nine v2 pairs favor the first candidate, while mcf's pair changes are
**−0.64%, −2.71%, −1.06%**. Removing call instructions alone is insufficient
evidence of an improvement: the release schedule function grows from 135 to
275 disassembly lines and its execution function also changes substantially.
That is a code-generation observation, not a demonstrated cause of mcf's loss.

**Narrower variant: noisy measurements, excluded from any speedup claim.** Same
method, keeping the free-count query out of line:

| Trace | Before | Trial | Change |
|---|---:|---:|---:|
| sqlite | 289.97 (243.41–305.38) | 304.54 (301.91–304.72) | +5.02% |
| omnetpp | 334.59 (286.53–335.09) | 334.09 (257.58–335.44) | -0.15% |
| gcc | 411.04 (307.53–413.93) | 407.60 (348.40–411.03) | -0.84% |
| mcf | 98.56 (93.55–118.43) | 100.87 (71.41–117.52) | +2.34% |

The unchanged baseline itself varies from 413.93 to 307.53 KIPS on GCC and
118.43 to 93.55 KIPS on mcf. Its first, steadier pair is slightly negative on
all four traces. These results do not establish a useful retained optimization.
**Production KIPS improvement from step 9: none claimed; production code is
restored.** Revisit individual query/call-site choices on a quieter host if a
later profile still justifies them.

**Evidence.** `09-register/` preserves the three-query snapshot, patch, full-suite
logs, review, assembly, short matrix, timing and audit. `two-queries/` preserves
its separate snapshot/patch/campaign/audit; `restored/` contains final-suite logs
and binary identity verification. `old-query-oracle/` checks the additional
characterizations against original source without changing the measured binaries.

### Long regression reference status

The initial 14 SPEC26 workloads plus mcf completed 5M/50M on the second-pass
baseline. `long-reference/reference-audit.json` validates complete reference
captures and their hashes. The retained trace changes have now also completed
all 15 comparisons in `08-raw-trace/long-regression/`: complete reported phase
statistics, effective configuration and warmup/ROI counts match the references.
`successor-audit.json` independently confirms those comparisons and their hashes.
These concurrent regression runs are excluded from KIPS. They supply the validated
immediate-parent references for subsequent changes; the required SPEC26 workload
set was checked against the current source directory and still covers all 14.

## 10. Extent-query inlining — deferred after measuring workload tradeoffs

**Issue.** Address slicing and legacy DRAM mapping repeatedly call tiny
out-of-line extent-size wrappers. They only subtract two bounds, but their call
boundaries prevent some caller simplifications. Linux perf identified repeated
dynamic slicing in mcf's nonzero DRAM swizzle path.

**Experiment and disposition.** First inline all five runtime size overloads with
the exact original expression. Preserve by-value signatures, std::size_t results,
and the absence of constexpr/noexcept. Derived types must continue reading their
publicly mutable bounds. This removes 174 release call sites, but all nine v2
pairs slow down while mcf improves. Then isolate dynamic_extent inlining, restoring
the four derived wrappers and original helper; 16 derived call sites remain.
The narrower experiment's timing is badly contended and cannot establish its
benefit. **Neither production prototype is retained.** Restore all original
query definitions, retain the characterization tests, and defer a more targeted
DRAM experiment or a quiet-host repetition.

**Files touched.**

1. `inc/extent.h` — trial inline definitions, then restore all original declarations.
2. `src/extent.cc` — trial removal/restoration of wrappers and the anonymous helper;
   the retained production file matches its predecessor.
3. `test/cpp/src/034-extent.cc` — retain checks for widths 0, 1, 32, 64, zero-width
   extents at nonzero offsets, and mutations of all four derived types.
4. `docs/research-log/Performance/2026-09-14-performance-optimization.md` — record
   both experiments, the long comparisons, and the disposition.

**Commit.** Retained characterizations: `3c1646dc35c6b75ee75224791251021695e6adbb`. Rejected production patches
are saved alongside their immutable binaries and have no retained implementation
commit. Step 9's log commit is `f8d7e932`.

**Regression verdict.** Both prototypes match the retained step-8 production
binary in each of the **32 short regression and 24 timing runs**: **112 runs**
with exact full reported phase/configuration/retirement/cycle parity, independently
audited. The broad prototype additionally matches its immediate parent on **all
14 SPEC26 workloads plus mcf at 5M/50M**; independent checks verify the required
workload set, successful completion, counts, output and input hashes. The
narrower rejected prototype did not receive its own long campaign or full payload
suite; no such result is claimed for it.

Focused extent tests pass on the original source and on both prototypes:
**491 assertions / 89 cases**. The broad prototype's full normal suite passes
**32,727 assertions / 881 cases**, and its payload suite passes **34,820 assertions /
886 cases**, with 7 native skips each. Python passes 66 tests with 4 native skips.
Scoped reviews found no source blocker. After restoring production, both full suites pass with the same counts. The
release is byte-identical to the retained step-8 binary (SHA-256
`daabf72fe90dcd67bbce71f2eda392ce7d0ea8fefcb1867a3a4bc4e349f634fe`), whose
Python suite was already validated. This closes the issue with no production
behavior or speed change retained.

**KIPS before/after: broad inlining.** Three alternating CPU-14 pairs, legacy DRAM,
detailed PTW, 1M/3M. Medians (min–max):

| Trace | Before | Trial | Change |
|---|---:|---:|---:|
| sqlite | 287.32 (284.20–287.82) | 282.98 (281.42–287.39) | -1.51% |
| omnetpp | 317.91 (315.78–318.52) | 314.77 (313.12–315.63) | -0.99% |
| gcc | 391.66 (390.58–393.21) | 387.22 (383.91–389.76) | -1.13% |
| mcf | 110.01 (107.31–111.20) | 117.78 (117.20–121.06) | +7.06% |

All three mcf pairs improve (+8.87%, +9.75%, +6.53%); all nine v2 pairs slow down.
The mcf benefit does not justify applying these helpers globally without examining
the v2 cost. Call elimination is an assembly observation, not a cause-of-speedup
proof. These measurements still share a host with other simulations.

**Narrower dynamic-only trial: excluded from speed claims.** Same method:

| Trace | Before | Trial | Raw change |
|---|---:|---:|---:|
| sqlite | 221.52 (183.63–276.06) | 214.11 (184.52–270.93) | -3.34% |
| omnetpp | 237.46 (198.82–306.67) | 204.70 (204.50–272.61) | -13.79% |
| gcc | 249.45 (244.70–282.95) | 253.35 (245.61–303.39) | +1.56% |
| mcf | 69.98 (68.15–102.15) | 73.90 (73.03–110.09) | +5.61% |

The unchanged baseline alone spans 183.63–276.06 KIPS on SQLite and
68.15–102.15 on mcf. A host observation records 25 busy processes, including
compiler work on CPU 30, the timing CPU's SMT sibling. Our builds/tests/long runs
had finished before timing. Keep the whole batch as parity evidence; neither its
positive nor negative raw median changes establish a reliable speed effect.
**Retained production KIPS gain from step 10: none claimed.**

**Evidence.** `10-extent/` saves the broad binary (SHA-256
`62ebdd72891e32903ef1e505d60283197a9f533bd79c014c5d896ed3657df48c`), patch,
focused/full-suite logs, review, disassembly, both paired campaigns, long campaign,
and audits. `dynamic-only/` saves the separate binary (SHA-256
`daa174a1e46849cdd6c1ec9d79de26ba93efca23eeb53dc1d8224ff546cf760c`), patch,
focused test, review, short/timing campaigns, host observation and audit.
`restored/` records final checks. Completed campaign directories and snapshots
remain intact. The long harnesses now explicitly refuse Python -O/PYTHONOPTIMIZE;
prior executed sources are preserved, and the final long audit was repeated with
assertions enabled.

### Independent finding: oversized physical register files

A separate diagnostic confirmed a **pre-existing scheduler array-bound violation**
in the pre-performance baseline `b1fb06b9`. With
`ooo_cpu.cpu0.register_file_size=512`, SQLite v2 reaches
`RegisterAllocator::isAllocated(short)` with argument **256** from
`O3_CPU::schedule_instruction()`. The architectural frontend RAT has only 256
entries, indexed 0–255. The scheduler counts source allocations before checking
`scheduled`, although scheduling replaces those source names with physical IDs.
An already-renamed physical source can therefore be outside the architectural
RAT's bounds.

A GDB breakpoint at the function entry, its backtrace, and disassembly confirm the
argument and pending lookup in the unmodified old release; independent review
confirms the interpretation. The diagnostic deliberately stops before the read;
it is not a completed simulation or a measured result discrepancy. No scheduler
fix is included in this behavior-neutral pass. The benchmark configs retain 128
physical registers, and no effect on those measurements is inferred. A correctness
fix needs separate scheduler-policy and regression review.

`preexisting-register-rat/` preserves the exact command, GDB script/output,
original source, binary hash and review. To reproduce on the archived old binary,
use the ordinary two benchmark configs, legacy DRAM, the original SQLite v2 trace,
10k warmup/10k ROI, and the 512-register override. The breakpoint is
`break *'RegisterAllocator::isAllocated(short) const' if (short)$rsi > 255` on
this Linux x86-64 build; the saved script records the argument and backtrace.

## Optimization 11 — skip cache helper calls when no work is possible

**Issue.** Every cache tick called span, transformation, extraction and stable
partition helpers even for empty queues or exhausted bandwidth. Linux perf
attributed a large share of host cycles to those helper call paths.

**Fix.** Guard the fill and tag-check blocks, translation-stash and input-queue
transforms, and empty tag extraction. Preserve the nonempty algorithms and
predicate order. Bandwidth construction and assertions, zero-consumption
accounting, translation-capacity capture, upstream rotation and the prefetcher
cycle hook retain their original placement and behavior. There is no early
return from the whole cache tick.

**Files touched.**

1. `src/cache.cc`: skip helper blocks for empty queues or zero bandwidth in
   `CACHE::operate`, retaining all per-cycle side effects.
2. `test/cpp/src/427-cache-idle-work.cc`: characterize idle upstream fairness
   and cycle hooks, queued and admitted work held by zero tag bandwidth, and
   ready fills held until fill bandwidth is restored.
3. `docs/research-log/Performance/2026-09-14-performance-optimization.md`:
   record the change, validation, measurements and limits.

**Commits.** `461e16d0` (source and tests); the following documentation
commit records this entry.

**Regression verdict: INERT within the tested configurations.** The new tests
pass on the original and optimized code (3 cases / 18 assertions), and the
existing dirty-writeback retry test passes (1 case / 8 assertions). Full normal
C++: 884 passed / 7 native-backend skips / 32,745 assertions. Payload C++: 889
passed / 7 skips / 34,838 assertions. Python: 66 tests / 4 native-backend skips.
The 16-case short matrix has 32 successful runs and exact before/after equality
of every exported phase statistic, effective configuration, and warmup/ROI
instruction and cycle count. Independent review found no blocker. The 24 longer timing runs (1M warmup / 3M ROI) also match exactly. Independent saved-artifact audits pass for both campaigns. A separate all-workload 5M/50M comparison will establish the immediate-parent reference before any ROB change is retained.

**KIPS: retained.** Three alternating pairs per workload, pinned to CPU 14, legacy DRAM and detailed PTW; actual warmup+ROI retired instructions divided by whole-process elapsed time. No own build, test, long campaign or profiling overlapped these timings.

| Workload | Before median (range) | After median (range) | Median change |
|---|---:|---:|---:|
| sqlite | 282.27 (180.01–294.55) | 403.81 (245.81–416.65) | +43.06% |
| omnetpp | 313.85 (225.43–322.75) | 463.75 (282.19–476.36) | +47.76% |
| gcc | 391.75 (379.28–399.18) | 552.97 (534.99–562.61) | +41.15% |
| mcf | 114.94 (110.17–115.04) | 145.43 (137.14–146.62) | +26.53% |

All twelve paired changes are positive: SQLite +36.55/+43.06/+41.45%, omnetpp +25.18/+47.76/+47.59%, GCC +41.05/+41.15/+40.94%, and mcf +24.49/+27.56/+26.41%. Unrelated jobs shared the host, and SQLite/omnetpp absolute rates rose markedly after the first repetition. The gain is supported by every local pair, including the slower interval; the exact percentage is not an isolated-host estimate. Do not compare these absolute KIPS with earlier campaigns or add the percentage to earlier improvements.

The short matrix covers both PTW modes, clock ratios, VM randomization, multiple memory channels/ranks/bank groups/banks and next-line prefetching. C++ tests cover payloads and cache-specific side effects. This does not prove neutrality for every module, topology or instruction window; exported TOML also omits some warmup cache/DRAM counters.

**Evidence.** `2026-09-15-optimizations/11-cache-guards/` under the external
results root contains the immutable candidate, source patch, focused/full-suite
logs, implementation and review reports, both paired campaigns, host observations
and independent audits. The candidate SHA-256 is
`fe657e62bb275213fda52348c18ef3f411c724e00d34d8243d962d38f5263e79`;
its immediate parent is the retained step-8 binary, SHA-256
`daabf72fe90dcd67bbce71f2eda392ce7d0ea8fefcb1867a3a4bc4e349f634fe`.
The rejected step-9 and step-10 source experiments are not part of this comparison.
