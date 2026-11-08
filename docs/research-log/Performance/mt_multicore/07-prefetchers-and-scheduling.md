# 7. Prefetchers, burstiness and what to schedule next

Every earlier measurement ran with every prefetcher off. A prefetching championship turns them on
at every level, so this report asks two questions:
- what do prefetchers change in a parallel tick;
- which of the remaining costs can a scheduler remove?

It measures before designing anything. The answer rules out the k-cycle synchronization proposed
after [report 6](06-memory-intensive-16-core.md), and points at work stealing instead, which
changes no statistic.

## Setup

- **Workloads.** Both are on the 16-core study machine:
  - the 16-core mix of report 5;
  - 16 copies of `734.vpr_r.sp1`.
- **Prefetchers.** Each workload runs without prefetchers (`configs/mc16.toml`) and with them
  (`configs/mc16-pfa.toml`): `ip_stride` at every L1D, `spp_dev` at every L2C and `next_line`
  at the LLC.
- **Timing.** The PGO binaries of report 6 at `-w 1000000 -i 2000000`, timed on a quiet host.
- **Probe.** A measurement-only copy of `5d6c3387` (`results/.../lprobe/`, `instrument.py`). It
  runs 8 threads under `taskset -c 0-7,16-23` at `-w 1000000 -i 500000`. Every tick, it records
  in TSC cycles:
  - each lane's cost (its operables plus its trace refill);
  - the main thread's LLC, PTW and memory-backend time;
  - the time inside the parallel run;
  - the whole tick.
- **What-ifs.** `analyze.py`, `analyze2.py` and `steal.py` replay those per-tick lane costs under
  other schedules.

## Speed with prefetchers

| Workload | Serial | 8 threads | Speedup |
|---|---|---|---|
| Mix, no prefetchers | 329.3 s | 69.5 s | 4.74× |
| Mix, prefetchers | 265.9 s | 56.5 s | 4.71× |
| vpr ×16, no prefetchers | 449.2 s | 85.7 s | 5.24× |
| vpr ×16, prefetchers | 459.7 s | 92.4 s | 4.98× |

- **The mix finishes sooner with prefetchers.** It takes 5.85M ticks against 7.75M, because the
  prefetchers speed up its slowest core.
- **vpr loses speedup with prefetchers.** Its mean IPC falls from 0.150 to 0.143, since 16 cores'
  `next_line` LLC prefetches compete for four DRAM channels. Its serial share also grows (below).

## Where a tick goes

TSC cycles per tick at 8 threads:

| | Mix | Mix + prefetchers | vpr | vpr + prefetchers |
|---|---|---|---|---|
| Tick | 25,041 | 27,851 | 15,321 | 17,063 |
| Parallel run (dispatch to last worker done) | 22,559 | 24,895 | 12,555 | 13,323 |
| Serial: LLC / PTWs / DRAM | 444 / 499 / 842 | 776 / 520 / 946 | 504 / 624 / 910 | 1,014 / 642 / 1,357 |
| Serial share | 7.1% | 8.0% | 13.3% | 17.7% |
| Mean worker load (lane total / 8) | 12,127 | 13,844 | 7,211 | 7,399 |
| Busiest lane in the tick | 15,927 | 17,238 | 8,179 | 8,861 |

The probe's two time-stamp reads around each serial step inflate the PTW figure. Sixteen idle PTWs
are skipped, but each skip is still timed.

- **Prefetchers grow the serial part, as expected.** LLC time roughly doubles, and DRAM time grows
  by 12–49%. On the memory-intensive workload, the serial share reaches 17.7%.
- **The parallel run is still the largest cost by far.** It runs at 1.7–1.9× the mean worker
  load. The lanes stay balanced on average, so rebalancing every 4,096 ticks has done
  its job: identical vpr cores average 3.5k–3.7k each. The waste comes from tick-to-tick
  variation.
- **A lane's cost is bursty.**

  | Lane 0 | Median | 90th percentile | 99th percentile |
  |---|---|---|---|
  | Mix | 7,021 | 14,849 | 21,480 |
  | vpr | 2,371 | 8,100 | 13,710 |

- **Bursts are predictable from one tick to the next, then fade.** Autocorrelation by lag:

  | Workload | Lag 1 | Lag 16 | Lag 256 | Lag 4,096 |
  |---|---|---|---|---|
  | Mix | 0.90 | 0.55 | 0.18 | 0.04 |
  | vpr | 0.6–0.76 | 0.27–0.35 | 0.05–0.07 | 0 |

- **The bursts are simulation work, not interference.** Two placements were compared with today's,
  using a measurement-only pinning patch (`pin.py`, `pin2.py`):
  - each worker pinned to its own physical core, with the decoders on the SMT siblings;
  - each worker pinned to its own physical core, with the decoders on the other CCD.

  | Placement | Mix + prefetchers: wall / busiest lane | vpr + prefetchers: wall / busiest lane |
  |---|---|---|
  | `taskset -c 0-7,16-23` | 18.8 s / 16.9k | 10.5 s / 8.8k |
  | pinned, decoders on SMT siblings | 18.9 s / 17.0k | 10.4 s / 8.7k |
  | pinned, decoders on the other CCD | 18.2 s / 17.6k | 10.2 s / 9.0k |

## What a scheduler could remove

The table replays the measured per-tick lane costs under other schedules. Each entry is the mean
per-tick time of the parallel part. Fewer is better, and the busiest lane is the floor.

| Schedule | Mix | Mix + prefetchers | vpr | vpr + prefetchers | Lanes moved per tick |
|---|---|---|---|---|---|
| **Today:** fixed assignment by mean cost | 21,828 | 24,039 | 11,726 | 12,483 | 0 |
| Rebalance every 256 ticks from the last 256 | 21,570 | 23,754 | 11,668 | 12,400 | small |
| Rebalance every 16 ticks from the last 16 | 19,017 | 21,355 | 11,300 | 11,827 | small |
| Fresh LPT per tick from the previous tick's costs | 16,879 | 18,809 | 9,678 | 10,207 | 10.3–10.8 |
| **Work stealing:** own lanes first, idle workers take the costliest unstarted lane | 16,915 | 18,706 | 9,779 | 10,269 | 1.9–3.9 |
| Self-scheduling in the previous tick's cost order | 16,263 | 17,859 | 9,398 | 9,891 | about 10 |
| Ideal: fresh LPT per tick on the true costs | 16,061 | 17,512 | 9,336 | 9,815 | |
| Busiest lane (floor) | 15,927 | 17,238 | 8,179 | 8,861 | |
| **k-cycle sync:** fixed assignment, serial overlapped, k = 1 / 8 / 64 | 22.2k / 21.2k / 19.5k | 24.4k / 23.2k / 21.4k | 12.1k / 11.4k / 9.8k | 12.9k / 12.1k / 10.5k | 0 |

**Synchronizing every k cycles is not worth a model change.** A realistic L2C–LLC link allows k of
about 8–16. At k = 8 the saving over k = 1 is only 5–6%. Most of what the k-cycle design would buy
is the serial overlap that k = 1 already gives. The bursts last tens to hundreds of ticks, so only
much longer epochs would average them out.

**Dynamic scheduling removes most of the waste with no model change.** Work stealing comes within
5–7% of the ideal, moving 2–4 lanes per tick. Against today's fixed assignment, it cuts the
parallel run by 17–23%. Per tick that projects to:

| Workload | Today | With work stealing | Speedup |
|---|---|---|---|
| Mix | 25.0k | 20.1k | 1.24× |
| Mix + prefetchers | 27.9k | 22.5k | 1.24× |
| vpr | 15.3k | 13.4k | 1.15× |
| vpr + prefetchers | 17.1k | 14.8k | 1.15× |

Two costs are left out of that projection:
- the cache cost of a lane's state moving to another core;
- the stealing synchronization itself.

A steal moves a lane for one tick, and the lane comes back to its owner the next tick.

## What it means

1. **Next target: work stealing in `src/parallel_tick.cc`.** Which thread runs a lane does not
   change what it computes, so the gate is unchanged: identical statistics at every thread count.
2. **After that, the busiest lane in each tick is the limit.** More threads cannot help, because
   the ideal schedule already sits within 1–14% of that floor. Reducing a core's burst cost
   (whatever makes one of its ticks expensive) helps serial and parallel runs alike.
3. **The serial part is the second lever for memory-intensive workloads with prefetchers.** It is
   3.0k of 17.1k per tick. It can overlap the lanes only if the shared side sees the cores one
   cycle later, which is a small model change and needs the 5% gate. It is worth doing only
   after 1.
