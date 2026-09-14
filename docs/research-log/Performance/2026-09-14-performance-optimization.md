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
phase-statistics hashes and effective configuration, not just IPC or cycle count.
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
