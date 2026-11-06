# 2. Scaling study: where multi-core time goes

**Setup.**
- **Binaries:** the serial baseline at `122aaed3` plus `CHAMPSIM_NUM_CPUS`, a fast `PGO=0` build
  (frozen as `bin/baseline/`).
- **Machine:** `mkconfig.py` gives each core `lnc.toml`'s core and caches, with
  `hashed_perceptron` and `basic_btb`. The LLC is scaled ×N, and legacy DRAM has one channel per
  four cores.
- **Runs:** `-w 1000000 -i 2000000` per core, one run each, timed by `campaign.py` on
  2026-11-06. Foreign CPU use during every run had a mean of 0.12–2.31 CPUs; one contaminated
  run was repeated.

**Mixes:**
- three rate mixes: N copies of 710.omnetpp sp2 (memory-heavy, IPC 0.57 alone), 707.ntest sp1
  (compute-bound, IPC 2.34) or 734.vpr sp1 (very memory-bound, IPC 0.28);
- one heterogeneous mix: the first N of a seeded permutation of the 32 SPEC26 slices.

## Results

"ns/tick/core" is wall time divided by global ticks and by N: the cost of simulating one core
for one cycle.

| Workload | 1 | 2 | 4 | 8 | 16 cores |
|---|---|---|---|---|---|
| **rate-omnetpp2** wall | 6.4 s | 12.9 s | 27.1 s | 54.8 s | 128.2 s |
| ns/tick/core | 1,634 | 1,566 | 1,571 | 1,586 | 1,850 |
| mean IPC | 0.570 | 0.550 | 0.520 | 0.521 | 0.520 |
| **rate-ntest1** wall | 2.9 s | 6.0 s | 12.0 s | 24.8 s | 53.4 s |
| ns/tick/core | 2,550 | 2,547 | 2,537 | 2,613 | 2,810 |
| **rate-vpr1** wall | 14.8 s | 37.5 s | 110.2 s | 233.6 s | 527.4 s |
| ns/tick/core | 1,981 | 1,872 | 1,894 | 2,012 | 2,279 |
| mean IPC | 0.280 | 0.210 | 0.143 | 0.141 | 0.143 |
| **mix** wall | 4.9 s | 16.2 s | 36.4 s | 75.0 s | 360.8 s |
| ticks | 1.51M | 3.54M | 3.57M | 3.63M | 7.74M |
| ns/tick/core | 3,254 | 2,290 | 2,546 | 2,586 | 2,912 |
| peak RSS | 119 MB | 130 MB | 152 MB | 304 MB | 634 MB |

## What this says

1. **Each core costs about the same to simulate at any core count.** Cost per core per tick is
   flat from 1 to 8 cores and rises 10–17% at 16, where the run's footprint (about 630 MB) no
   longer fits the host's caches. Nothing in the simulator grows faster than linearly: the
   shared LLC, DRAM and the scheduling loop are under 5% of a tick
   ([report 4](04-parallel-tick.md)).
2. **The slowdown is N cores per tick, times more ticks.** A run of a fixed number of
   instructions per core costs N × ticks, and ticks grow in two ways:
   - **Contention lowers IPC.** For rate-vpr1, IPC falls from 0.28 to 0.14 and ticks double.
   - **Mixes run at the pace of their slowest core.** A phase ends only when every core has
     finished, and cores that finished early keep simulating. At 16 cores the mix contains
     734.vpr sp1, so it needs 7.74M ticks, while most of its cores finish their 3M instructions
     in under 2M. About three-quarters of the core-ticks are spent after a core's own region of
     interest. They model the interference that core would otherwise stop providing, so they
     are not wasted, but they make a 16-core mix cost 74× a one-core run, not 16×.
3. **So the serial loop is the lever.** The work grows linearly, and most of it, the cores and
   their private caches, is per core.

### A methodology note for the championship

The run-until-slowest rule also makes per-core results sensitive to the slowest core. A small
change in it moves every other core's measurement window
([report 3](03-tick-order.md)). Longer regions of interest (10M or more per core) make
per-core comparisons meaningful, and a fixed-length mode (every core simulates the same number
of cycles) would bound a run's cost. Both are methodology choices, not simulator speed.
