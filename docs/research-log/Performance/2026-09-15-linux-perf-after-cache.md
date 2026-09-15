# Linux perf after cache no-work guards — 2026-09-15

ROB scans remain the next cross-workload target. Cache queue-helper call paths
have fallen to about 1–2% of sampled cycles; the schedule/execute/complete ROB
paths now account for 21–32%. The conservative next experiment will reuse an
executed prefix observed by the immediately preceding completion scan only when
that scan completes no instruction. Any completion retains the original scan
from the head, preserving callbacks, wakeups and bandwidth-limited ordering.
No persistent frontier or scheduling-policy change is proposed.

## Evidence and interpretation

The release is source commit `461e16d0` (documentation `8d15d222`), SHA-256
`fe657e62bb275213fda52348c18ef3f411c724e00d34d8243d962d38f5263e79`.
It retains optimizations 1–8 and 11. Register and extent inlining (9–10) were
rejected; their production changes are absent. The comparison profile below is
the [initial Linux perf analysis](2026-09-15-linux-perf-hotspots.md), taken after
optimization 6. Differences therefore include retained changes 7, 8 and 11.
The [running log](2026-09-14-performance-optimization.md) contains each patch's
separate unprofiled before/after KIPS measurement.

All 16 new invocations are serial, pinned to CPU 14, with legacy DRAM, detailed
PTW, the same two configs and four protected traces, 1M warmup / 3M ROI, and the
same GCC 13.3 release build settings. Four core-counter and four native-conflict
counter passes each report 100% running time; eight cycle profiles use 499 Hz,
DWARF 16 KiB stack snapshots and a 256-page buffer. No own builds, tests or other
campaigns overlapped collection. Unrelated jobs share the host. Profiled elapsed
times are not optimization KIPS.

All 16 invocations reproduce the reference's full exported phase statistics,
effective configuration, and warmup/ROI instruction and cycle counts. Input and
binary hashes match. Independent perf report checks confirm all eight parsed
period totals and release-ELF exclusive symbol totals: **76,113 samples, zero
lost samples**, no report/script diagnostics, maximum unknown leaf share 0.103%.

The tables average the two period-weighted profile percentages per workload.
The call-path rows overlap and must not be added together. A larger remaining
share does not mean that a component became slower; removing cache work changes
the denominator, and the shared host also limits fine comparisons.

| Workload | Cache helpers: initial → current | ROB paths: initial → current | Current execute loop, exclusive | Current completion loop, exclusive |
|---|---:|---:|---:|---:|
| sqlite | 24.33% → 1.44% | 19.22% → 28.42% | 6.89% | 5.97% |
| omnetpp | 29.30% → 1.93% | 13.75% → 20.89% | 4.70% | 3.95% |
| gcc | 24.16% → 1.08% | 16.01% → 23.72% | 6.29% | 4.31% |
| mcf | 19.40% → 1.10% | 24.67% → 31.73% | 13.22% | 11.34% |

| Workload | Core | Cache | DRAM | Trace | PTW | Framework | Startup/other |
|---|---:|---:|---:|---:|---:|---:|---:|
| sqlite | 60.05% | 22.32% | 3.40% | 6.74% | 3.37% | 3.66% | 0.46% |
| omnetpp | 50.89% | 29.67% | 3.54% | 7.18% | 3.85% | 4.44% | 0.44% |
| gcc | 56.64% | 22.62% | 3.53% | 9.08% | 3.49% | 4.17% | 0.48% |
| mcf | 53.99% | 15.47% | 22.89% | 2.28% | 2.66% | 2.56% | 0.14% |

Host instruction counts support a reduction in executed host work, independent
of the changing wall-clock rates. These are initial three-run medians versus one
new core-counter pass, normalized by actual warmup+ROI retired instructions:

| Workload | Initial host instructions / simulated instruction | Current |
|---|---:|---:|
| sqlite | 50,715 | 34,421 |
| omnetpp | 46,770 | 30,477 |
| gcc | 36,976 | 24,960 |
| mcf | 152,903 | 121,913 |

DRAM mapping remains workload-specific: about 15.12% for mcf and at most 0.02%
for the three v2 windows. It remains a separate arithmetic/geometry experiment;
no mapping or request-coordinate cache is folded into the ROB change. Allocator
and stable-partition paths also remain visible, but their unions span components.
The now-small cache-helper share does not justify a cache scratch-buffer rewrite
without a more specific allocation-path study.

## Startup sample and parser recovery

The initial analysis failed closed on one header-only `taskset` sample before
ChampSim's first sample, in SQLite's second profile. Its period is 2,224 out of
44,015,455,819 (about 0.000005%). The corrected copied parser keeps that period in
startup/other and unknown weight, with explicit missing-stack counters. It creates
no artificial frame or IP and rejects missing ChampSim stacks, other commands,
after-launch missing stacks, and startup missing weight above 0.01%.

The original parser, failure log and raw artifacts are preserved. Synthetic
acceptance/corruption tests pass under Python `-O`; independent review found no
blocker. Re-running analysis and the independent audit over the original captures
passes without recollection. Copied-source and executed-parser hashes and exact
recovery commands are saved separately.

## Next regression gate and limits

The cache binary's 5M/50M successor runs are establishing the immediate-parent
reference on one trace from every one of the 14 SPEC26 workloads, plus the mcf v1
control. Any ROB candidate must match all 15 references, pass both C++ suites and
the 16-case short matrix, and match additional narrow-bandwidth/backpressure
cases in both PTW modes before its fresh serial KIPS can justify retention.

This profile is four short single-core windows, with payloads disabled. It does
not establish performance across all workloads, multicore execution or arbitrary
modules. Source inlining and sampling skid limit instruction-level attribution;
16 KiB stack snapshots can truncate large trace-reader ancestry. Exported TOML
omits some warmup cache/DRAM counters. No top-down stall decomposition or precise
memory-latency conclusion is claimed from the native conflict event.

Raw data, commands, copied scripts, parser recovery, hashes, all stack ranges and
independent audit are preserved under:

```
/home/rbera/work/alakazam/champsim-perf-results/2026-09-15-optimizations/after-cache-perf/
```

`reprofile-summary.json` contains the tables' unrounded means and ranges;
`final-validation.json` contains collection/parity/sampling verdicts.
