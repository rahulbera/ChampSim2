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
initial investigation, 1M warmup + 3M ROI, three before/after repetitions, and CPU 8.
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
