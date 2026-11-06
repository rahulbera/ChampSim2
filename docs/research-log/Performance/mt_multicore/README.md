# Multi-threaded multi-core simulation

Campaign log, started 2026-11-05 on branch `feat/mt-multicore` (from `feat/perf-fix` at
`122aaed3`). Raw evidence is outside the repository, in
`/home/rbera/work/alakazam/results/2026-11-05-mt-multicore/`.

## Goal

Make 8- and 16-core simulations fast enough for the next Data Prefetching Championship. The
championship targets 8 and 16 cores because:
- prefetchers that help under heavy cross-core interference and DRAM contention matter more
  to industry;
- those prefetchers are harder for AI agents to design.

ChampSim ticks every component of every core on one thread, so a run's wall time grows with
the core count.

## Correctness criterion

The criterion was set on 2026-11-05.

- **Deterministic.** A run must give identical statistics every time it is repeated, and
  under every thread count. The championship depends on this.
- **Close to today's model.** A multi-threaded run may differ from today's serial
  multi-core model, but by less than 5% in IPC and the other headline statistics.
  Cycle-for-cycle identity with today's model is not required.
- **No relaxed synchronization.** Schemes that are only statistically equivalent, with
  cores running ahead and results varying run to run, are out.

## Reports

| # | Report | Status |
|---|---|---|
| 1 | [How multi-core is modeled](01-multicore-model.md) | done |
| 2 | [Scaling study](02-scaling-study.md) | done |
| 3 | [A stable tick order](03-tick-order.md) | retained |
| 4 | [The phase-parallel tick](04-parallel-tick.md) | retained |
| 5 | [Parallel results](05-parallel-results.md) | done: 4.6–4.7× at 16 cores, statistics identical at every thread count |

## Tools

Everything below is in the evidence root and is not committed.

| Tool | Purpose |
|---|---|
| `mkconfig.py N` | the N-core study machine (`configs/mc<N>.toml`) |
| `freeze_binaries.sh NAME` | builds fast `PGO=0` binaries at 1, 2, 4, 8 and 16 cores from the main checkout and copies them to `bin/NAME/`, with the commit, diff and hashes |
| `campaign.py` | timing runs; `characterize` for one-core statistics |
| `gate.py` | statistics gates: `run`, then `compare` (parity, headline statistics, census) |
| `probe/` | a worktree with a per-operable TSC tick probe (`CHAMPSIM_TICK_PROBE=<file>`), measurement only |

**Timing runs.** Each one waits until foreign CPU use (busy CPUs minus the run's own) has
stayed under 0.5 for 30 s. It samples that load during the run and is repeated if the mean
exceeds 0.75 or a sample exceeds 3. Timing runs hold `host.lock` exclusively. Builds and
statistics gates take it shared, so they can never overlap a timing run.

## Plan

Each step is gated before the next one starts.

1. **Measure.** Build 1-, 2-, 4-, 8- and 16-core binaries. Time them on rate and mixed
   workloads, then profile where the time goes as the core count grows.
2. **Fix the tick order.** Today's per-tick `std::sort` is not stable above 16 operables, so
   at two or more cores the cores tick in a scrambled, core-dependent order
   ([report 1](01-multicore-model.md)). A stable order is identical at one core and defines
   the order at N cores. This is the one intended model change, and it is gated against
   today's results at the 5% limit.
3. **Remove the shared mutable state that the parallel phases would touch.** None of these
   changes may alter any statistic, at any core count.
4. **Phase-parallel tick.** The cores tick in parallel, then the LLC serially, then each
   core's caches and trace refill in parallel, then the PTWs and DRAM serially. This is the
   same order as step 2's serial model, so every thread count must give identical
   statistics.
5. **Tune**, in the order the measurements rank them: barrier cost, load balance, the
   serial share (LLC, PTWs, DRAM), trace decode and host memory layout.
