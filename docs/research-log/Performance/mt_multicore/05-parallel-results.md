# 5. Parallel results

**Code.** `dd0ea3a2` on `feat/mt-multicore`: the stable order (report 3), the phase-parallel
tick with one run per tick, lane rebalancing, background trace decoding and idle-PTW skipping
(report 4).

**Setup.** Binaries are fast `PGO=0` builds, frozen as `bin/mt5/`. Each run is
`-w 1000000 -i 2000000` per core, on the study machine of [report 2](02-scaling-study.md),
timed on 2026-11-06 with foreign CPU use between 0.09 and 0.17. Speedups are against the
serial baseline at `122aaed3` (`bin/baseline/`).

## Correctness

**Every multi-threaded run is bit-identical to one thread.** The thread-invariance gate
(`run_threads_gate.sh`, `gate.py`, results in `gate/mt5-*`) compares the complete `[phase]`
statistics and `[config]`:

| Comparison | Runs | Bit-identical |
|---|---|---|
| `--threads 1` against the stable-order build ([report 3](03-tick-order.md)) | 32 (1 to 16 cores) | 32 |
| `--threads 2` against `--threads 1` | 24 (2 to 16 cores) | 24 |
| `--threads 4` | 18 (4 to 16 cores) | 18 |
| `--threads 8` | 12 (8 and 16 cores) | 12 |
| `--threads 16` | 6 (16 cores) | 6 |

Each of the three earlier scheduler versions passed the same gate. The only change to
multi-core results is the tick-order fix, which is measured in report 3.

## Speed

| Workload | Serial | 1 thread | 2 | 4 | 8 | 16 |
|---|---|---|---|---|---|---|
| 16-core mix | 360.8 s | 0.99× | 1.92× | 3.30× | **4.57×** | 4.45× |
| 16-core rate omnetpp | 128.2 s | 1.01× | 1.92× | 3.01× | **4.01×** | 3.78× |
| 8-core mix | 75.0 s | 1.00× | 1.80× | 2.78× | **3.36×** | — |
| 8-core rate omnetpp | 54.8 s | 1.03× | 1.76× | 2.55× | **3.16×** | — |
| 8-core rate vpr | 233.6 s | 1.02× | 1.95× | 3.17× | **4.41×** | — |
| 4-core mix | 36.4 s | 1.02× | 1.67× | **2.30×** | — | — |
| 4-core rate omnetpp | 27.1 s | 1.09× | 1.71× | **2.35×** | — | — |
| 4-core rate vpr | 110.2 s | 1.07× | 1.81× | **2.79×** | — | — |
| 2-core mix | 16.2 s | 1.01× | **1.44×** | — | — | — |
| 2-core rate omnetpp | 12.9 s | 1.03× | **1.53×** | — | — | — |
| 2-core rate vpr | 37.5 s | 1.05× | **1.63×** | — | — | — |

**Compared with the first parallel version** (`5c419f7f`, two runs per tick, sorted every tick),
best case:

| Workload | First version | Now |
|---|---|---|
| 16-core mix | 3.50× | 4.57× |
| 16-core rate omnetpp | 3.16× | 4.01× |
| 8-core rate vpr | 3.70× | 4.41× |
| 4-core rate vpr | 2.38× | 2.79× |
| 2-core rate vpr | 1.46× | 1.63× |

At one thread, every core count is as fast as the serial baseline or faster. With 16 threads
the first version lost half its speedup crossing CCDs; now it loses little.

## Placement

Measured on the 16-core mix with 8 or 16 simulation threads. With `--threads` above 1, each
trace also has its own decoder thread, which mostly sleeps.

| Placement | Wall | Speedup |
|---|---|---|
| unconfined, 8 threads | 78.9 s | 4.57× |
| `taskset -c 0-7` (one CCD's eight cores), 8 threads | 112.2 s | 3.22× |
| **`taskset -c 0-7,16-23` (one CCD with its SMT siblings), 8 threads** | **76.6 s** | **4.71×** |
| unconfined, 16 threads | 81.0 s | 4.45× |
| `taskset -c 0-7,16-23`, 16 threads | 126.4 s | 2.85× |

> **Use as many threads as one CCD or CCX has physical cores, and give the process those
> cores' SMT siblings (or other free cores) for the decoders.** On this Ryzen AI Max+ 395:
> `taskset -c 0-7,16-23 champsim --threads 8`. Never run more simulation threads than
> physical cores.

## What it means for a championship

- **Speedup at the target sizes.** An 8- or 16-core run with 8 threads takes about a quarter
  of the serial time: 3.2–4.6× on this host, with statistics independent of the thread count,
  so results stay reproducible on any machine.
- **Latency against throughput.** Threads buy wall time per run, not CPU time: a
  4.6×-faster run uses 8 cores. A study of many mixes that fills a cluster gains nothing
  from threads; one that waits on a few long runs (an agent iterating on a design, a deadline)
  gains the full factor.
- **Contestants' modules.** They must keep their state per instance. A static or global shared
  between instances is a data race in a multi-threaded run. Comparing one run at
  `--threads 1` with one at `--threads 8` is a cheap check that every submission should pass.
