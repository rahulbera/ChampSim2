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
