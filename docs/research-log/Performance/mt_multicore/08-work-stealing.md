# 8. Work stealing

[Report 7](07-prefetchers-and-scheduling.md) found that a parallel run waits on whichever lane is
busiest in that tick, and that work stealing would remove most of that wait without changing the
model. This report implements it (`88d9121c`), gates it, and times it.

## What changed

Only the scheduler changed (`src/parallel_tick.cc` and its header). Lanes still have owners, and rebalancing still reassigns them
every 4,096 ticks. Within a run:
- **Own lanes first.** Each worker runs its own lanes, the one that cost most the last time it ran
  first.
- **Then steal.** A worker with nothing left takes the lane no worker has started that cost most
  the last time it ran, and repeats until none is left.
- **One claim per lane.** A lane is claimed with one atomic compare-and-exchange per run, so it
  runs exactly once. Nothing passes between claimants: a lane's state reaches its runner through
  the end of the previous run, as before.
- **What surrounds a lane moves with it.** Setting the core's deferred instruction IDs, refilling
  its input queue and checking its walkers' `idle()` used to be the owner's job; now whoever runs
  the lane does them. The calling thread does them for a core in no lane of the run, which the
  standard machine never has.
- **Every lane is timed on every run**, with the time-stamp counter on x86 and the steady clock
  elsewhere. That cost both orders the next run and feeds rebalancing, which used to sample one
  tick in 64.

Which thread runs a lane does not change what it computes, so no statistic can change.

## Correctness

- **Thread invariance: 104 of 104 runs are bit-identical**, every `[phase]` statistic and every
  `[config]` value:
  - the 32 workloads of the earlier gate at one thread, against the stable-order build;
  - 60 runs at 2, 4, 8 and 16 threads (2 to 16 cores), against one thread;
  - 12 runs of four 16-core workloads with prefetchers (`mc16-pfa.toml`) at 4, 8 and 16 threads,
    against one thread.
- **Test 503** gains a case in which core 0's lane is slow, so the other worker takes lane 2 from
  its owner. That lane then refills its core, and with interactions declared it also sets the
  core's IDs and checks its walker. Two mutations fail the test:
  - the owner running its lanes without claiming them (a stolen lane runs twice);
  - a stolen lane reporting its walker idle.
- **ThreadSanitizer** reports nothing on 503 (three runs), on 079, or on 4-core simulations at 2,
  3 and 4 threads, with and without prefetchers. The simulations' statistics are identical at
  every thread count, and equal to the TSan runs before the change. TSan needs address-space
  randomization off on this host's kernel: `setarch -R`.
- The full suite passes (966 cases, 20 skipped), as do 50 repetitions of 503. GCC 9 and Clang 12
  accept the changed files.

## Speed

16 cores, 8 threads, `taskset -c 0-7,16-23`, `-w 1000000 -i 2000000`, on a quiet host (foreign
CPU use under 1.0 for two minutes before each run; runs over a 1.5 mean or a 12 peak repeated).
Before and after alternate.

**Gate: fast, `PGO=0`, three runs each.** Before is `5627f9fa`, after is `88d9121c`. Every one
of the twelve pairs favours stealing, and the ranges do not overlap.

| Workload | Before | After | Speedup | Projected in report 7 |
|---|---|---|---|---|
| Mix, no prefetchers | 77.1 s | 70.3 s | 1.097× | 1.24× |
| Mix, prefetchers | 62.3 s | 58.0 s | 1.075× | 1.24× |
| vpr ×16, no prefetchers | 101.9 s | 97.7 s | 1.043× | 1.15× |
| vpr ×16, prefetchers | 107.2 s | 103.1 s | 1.040× | 1.15× |

**Production build: fast with PGO, two runs each.** Before is `pgo16/bin/champsim-mt`
(`b6ef94dc`, report 7's binary), after is `88d9121c`. The profile predates the change, so GCC
builds the changed functions without it.

| Workload | Before | After | Speedup | Over the serial simulator (was) |
|---|---|---|---|---|
| Mix, no prefetchers | 69.9 s | 64.0 s | 1.09× | 5.15× (4.74×) |
| Mix, prefetchers | 58.1 s | 52.1 s | 1.12× | 5.10× (4.71×) |
| vpr ×16, no prefetchers | 86.2 s | 83.8 s | 1.03× | 5.36× (5.24×) |
| vpr ×16, prefetchers | 92.7 s | 89.4 s | 1.04× | 5.14× (4.98×) |

- The serial times are report 7's (329.3 s, 265.9 s, 449.2 s and 459.7 s, measured the day
  before on the same host and binary). Today's PGO "before" runs match report 7's within 1%,
  except mix with prefetchers: one of its two runs took 59.6 s against report 7's 56.5 s, with
  a foreign mean of 0.74.
- Stealing shortens only the parallel run. vpr's serial part (LLC, PTWs and DRAM, 13–18% of a
  tick) stays as it was.

## Why it gains less than projected

A measurement-only copy of `88d9121c` (`ws/instrument_ws.py`, PGO build like report 7's probe)
records the same per-tick fields as report 7's probe, plus which lanes were stolen. Both probes
ran `-w 1000000 -i 500000` at 8 threads under `taskset -c 0-7,16-23`. Results are identical tick for
tick, so tick k does the same work under both, and `ws/pair.py` compares each lane with itself
on the same tick.

TSC cycles per tick in the region of interest:

| | Mix | Mix + prefetchers | vpr | vpr + prefetchers |
|---|---|---|---|---|
| Tick: static → stealing | 25,041 → 22,535 | 27,851 → 25,150 | 15,321 → 14,055 | 17,063 → 15,233 |
| Parallel run | 22,559 → 19,993 | 24,895 → 22,152 | 12,555 → 11,293 | 13,323 → 11,542 |
| Busiest lane | 15,927 → 17,216 | 17,238 → 18,402 | 8,179 → 8,737 | 8,861 → 9,164 |
| All lanes' work | +9.0% | +8.9% | +3.0% | +1.8% |
| Steals per tick | 3.70 | 3.64 | 1.77 | 1.96 |

The probe runs themselves took 18.6 → 17.2 s, 18.9 → 17.6 s, 10.5 → 9.9 s and 10.5 → 9.8 s.

- **Stealing happens as often as report 7's replay predicted** (3.9, 3.9, 1.9 and 2.1 per tick).
- **A moved lane costs about a quarter more, twice.** Its state lives in the caches of the core
  that ran it last, so it pays when it is stolen and again when it returns to its owner on the next
  tick:

  | Lane on this tick, against itself on the same tick under static | Mix | Mix + prefetchers | vpr | vpr + prefetchers |
  |---|---|---|---|---|
  | Stolen | 1.28× | 1.30× | 1.21× | 1.17× |
  | Back with its owner after a steal | 1.24× | 1.23× | 1.26× | 1.22× |
  | With its owner | 1.04× | 1.03× | 1.01× | 1.00× |

- **The busiest lane got busier, by 3–8%.** The lanes worth stealing are the costliest ones, so
  the penalty lands on the critical path. The replay could not see this, since it charged no
  migration cost. The measured parallel run fell 10–13%, not the 17–23% projected.

## Keeping a stolen lane where it went

`ws/policies.py` replays report 7's static-probe costs (the first 200,000 ticks of each region of
interest) with a 1.25× penalty for running a lane on a different worker than on its previous
tick. Mean parallel time per tick:

| Policy | Mix | Mix + prefetchers | vpr | vpr + prefetchers | Moves per tick |
|---|---|---|---|---|---|
| Static (rebalancing only) | 21,717 | 23,089 | 13,269 | 14,044 | 0 |
| **Stealing, `88d9121c`** | 18,002 | 19,302 | 10,753 | 11,417 | 3.0–3.5 |
| Sticky: the thief becomes the lane's owner | 17,122 | 18,246 | 10,138 | 10,769 | 1.7–2.2 |
| Stealing the cheapest unstarted lane | 19,252 | 20,725 | 11,293 | 11,981 | 3.4–3.6 |

- **The model is still optimistic.** It puts stealing at 0.81–0.84 of static, where the probe
  measured 0.87–0.90, and it leaves out the up to 4% that even unmoved lanes paid.
- **Sticky ownership is the one refinement it favours.** A stolen lane no longer goes back to its
  owner, which cuts the moves by 37–45% and saves a further 5–6% of the parallel run. Stealing the
  cheapest lane loses.

## What it means

1. **Retained.** Work stealing gives 1.03–1.12× at 8 threads with every statistic unchanged:
   5.1–5.4× over the serial simulator in the PGO build, up from 4.7–5.2×.
2. **Moving a lane is not free on this host.** About a quarter of a lane's cost is cache misses
   when it changes core, so schedules should be judged with that penalty, not on lane costs alone.
3. **Next for the scheduler: sticky ownership.** It is a small change to `88d9121c`, also with no
   model change, and worth at most about 5% of the parallel run. The larger levers are unchanged
   from report 7: the busiest lane in each tick, and the serial part, 13–18% of a tick on
   memory-intensive workloads, which needs a small model change.

## Evidence

All in `results/2026-11-05-mt-multicore/` (not committed):
- `ws/chain.sh`: the builds, the gate and both timings. The binaries and their hashes are in
  `ws/bin/` and `bin/ws1/`.
- `ws/timing/` and `ws/timing-pgo/`: `results.jsonl`, including two contaminated attempts.
- `gate/ws1-*`: the gate's runs and comparisons; `gate-pfa.json` is the prefetcher set.
- `build/ws-tsan*`: the ThreadSanitizer builds and runs.
- `ws/instrument_ws.py`, `ws/probe-runs/`, `ws/pair.py` and `ws/pair.txt`: the probe.
- `ws/policies.py` and `ws/pol-*.txt`: the policy replay.
