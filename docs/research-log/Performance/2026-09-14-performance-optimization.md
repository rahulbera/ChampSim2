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
alloc-after/,symbols.txt,timing/,source-commit.txt,champsim}` beneath the common
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
