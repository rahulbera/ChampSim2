# 6. Sixteen memory-intensive cores at full length

This report answers one question: how long does a 16-core simulation of a memory-intensive
workload take at a realistic length, serially and with threads, in the fastest build?

## Setup

- **Workload.** Sixteen copies of `734.vpr_r.sp1`, one per core. It is the most
  memory-intensive of the 32 SPEC26 traces, with 86 LLC MPKI and IPC 0.24 on one core. The trace
  holds at least 60M instructions, so no core wraps around.
- **Length.** `-w 10000000 -i 50000000` per core.
- **Machine.** The 16-core study machine (`configs/mc16.toml`, legacy DRAM, 4 channels).
- **Binaries.** Fast builds with every fast optimization, PGO included: the shipped
  `pgo/legacy-dram/gcc-13.3` profile in use, LTO, tcmalloc, and assertions off.
  - `champsim-base`: `a0f3ed6b`, today's serial simulator. That is `122aaed3` plus only the
    `CHAMPSIM_NUM_CPUS` switch.
  - `champsim-mt`: `b6ef94dc`, this branch. Its code is the same as `dd0ea3a2`.
- **Timing.** Each run started only after foreign CPU use stayed under 1.0 for two minutes. A run
  was kept only if the mean stayed under 1.5 and no sample reached 12.
- **Evidence.** `results/2026-11-05-mt-multicore/pgo16/`: `build.sh`, `run.sh`, the spec
  `pgo16.json`, and `runs/results.jsonl` with every attempt, including discarded ones.

## Wall time

| Run | Wall | Speedup over serial | Foreign load (mean / peak) |
|---|---|---|---|
| Serial simulator (`champsim-base`) | 10,175 s (2 h 50 min) | 1.00× | 0.78 / 9.6 |
| This branch, `--threads 1` | 9,759 s (2 h 43 min) | 1.04× | 0.83 / 9.4 |
| This branch, `--threads 8`, `taskset -c 0-7,16-23` | **1,811 s (30 min)** | **5.62×** | 0.48 / 6.3 |
| This branch, `--threads 16`, unconfined | 1,997 s (33 min) | 5.10× | 0.14 / 1.4 |

- **The serial run was repeated three times.** Two attempts exceeded the peak limit. The three
  took 10,160 s, 10,188 s and 10,175 s, which agree to within 0.3%.
- **A loaded host hurts 16 threads.** The first 16-thread attempt, made under a foreign mean of
  2.0, took 2,137 s.
- **One thread is 4% faster than the serial simulator.** The run takes 0.7% fewer ticks, and each
  tick costs 3.4% less now that the per-tick `std::sort` is gone.
- **Simulation rate.** Over 960M instructions (16 × 60M), the serial simulator retires 94 KIPS
  (5.9 per core) and 8 threads retire 530 KIPS.

The speedup is larger than for the 16-core mix (4.71×, [report 5](05-parallel-results.md)). Here
the 16 lanes cost the same, so nothing waits for one busy core. Even so, 16 threads still lose to
8. On this host, crossing the CCD boundary costs more than a balanced second thread per core
gains.

## Statistics

- **Thread invariance.** `--threads 1`, `--threads 8` and `--threads 16` have identical `[phase]`
  statistics, and all four runs have identical `[config]` and `build_id`.
- **Against the serial simulator.** The only difference is the tick-order fix
  ([report 3](03-tick-order.md)):

| | Serial simulator | This branch | Change |
|---|---|---|---|
| Mean IPC | 0.16246 | 0.16273 | +0.17% |
| Per-core IPC change | | | −0.05% to +0.62% (mean magnitude 0.17%) |
| Per-core IPC spread (lowest to highest) | 0.72% | 0.17% | |
| LLC MPKI | 52.37 | 52.54 | +0.33% |
| DRAM reads | 39.73M | 39.64M | −0.22% |

Every change is far inside the 5% limit. The spread shrinks again: sixteen identical cores
should run at the same speed, and with the stable order they nearly do.
