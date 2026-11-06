# 4. The phase-parallel tick

`--threads N` runs a multi-core simulation on N threads. The statistics are identical at every
thread count, because the threads execute the stable serial order of
[report 3](03-tick-order.md) exactly. The design changed over three commits. This describes
the current one; the measurements behind each change are under *How it got here*.

## Design

### What the environment tells the scheduler

| Method | Says | Static environment |
|---|---|---|
| `operable_lanes()` | each operable's lane: the core whose private state it touches, or none | each core and its six caches are that core's lane; the LLC, the PTWs (they share the page allocator), the memory backend and a perfect TLB (it calls `va_to_pa` itself) are in none |
| `operable_interactions()` | which operables may touch the same state | operables sharing a channel, derived from the wiring; every PTW and perfect TLB, through the virtual memory |
| `operable_idle_lanes()` | which lane may evaluate a no-lane operable's `idle()` after a run | each PTW, in its core's lane |

The defaults are no lanes, unknown interactions and no idle lanes, which leaves an environment
serial.

### Schedule

1. **Form steps.** The stable order splits into steps. A no-lane operable runs alone on the main
   thread. A run of neighbouring laned operables is split by lane across workers, and each lane
   keeps its own order.
2. **Hoist.** A no-lane operable that interacts with nothing in the whole run before it commutes
   with that run, so it moves ahead of it. The LLC shares nothing with the cores, so a standard
   tick becomes **LLC → [each core with its caches and refill] → PTWs → DRAM**. That is one
   parallel run per tick, and the serial work of consecutive ticks becomes one contiguous block.
3. **Build once.** An operable on the fastest clock is always exactly one quantum behind at the
   start of a tick, so only a slower one can move, and only when it is less far behind. The
   schedule is built once and rebuilt only when that happens, which never does in the standard
   machine, where DRAM, the one slower clock, is already last.

### Refill and instruction IDs

Each core's refill joins the tick's last run, provided nothing after that run can see a core.
Instruction IDs are global and numbered in core order, which needs every core's refill count,
so:
1. a lane reads with provisional IDs;
2. after the run, the main thread numbers the tick's instructions from the actual counts;
3. each lane writes its IDs at the start of its next run (or the main thread writes them
   immediately, when some no-lane operable could see a core).

Load and store completion is matched by instruction ID, so a provisional ID reaching a core
trips an assertion; 503 checks this by mutation.

### Load balance

With fewer threads than lanes:
- every 64th tick, each worker times its lanes;
- every 4,096 ticks, lanes are reassigned heaviest first to the least loaded worker, if that cuts
  the most loaded worker's cost by 5%.

Which thread runs a lane does not change what it computes.

### Idle no-lane operables

A PTW is idle on 99.9% of ticks, but the main thread used to call it, reading channels the
workers had just written. Now its lane's worker calls `PageTableWalker::idle()` at the end of
the run, which is true only when `operate()` would only look, and records the answer as a bit
in its own slot. The main thread then advances an idle PTW's clock without calling it.

### Trace decoding

With `--threads` above 1, each trace decodes on a thread of its own (`tracereader::decode_ahead`),
up to 1,024 instructions ahead, into a single-producer, single-consumer ring:
- the producer sleeps when the ring is full and is woken at half;
- the stream is identical, and `eof()` and errors behave as before.

Without this, a lane decoded 127 records (about 44k TSC cycles) whenever its buffer ran low, so
some lane hit a burst most ticks.

### Shared state removed

- **The heartbeat listener:** per-core state in its own cache line, sized before the threads
  start, with lines held per core and written in core order.
- **Return-stack warning counters:** atomic in `basic_btb`, `perfect_indirect` and
  `perfect_group`.

### Threads

- **Flag, not key.** The thread count is a flag (`--threads`), so it never enters `[config]` or
  the `build_id`.
- **Workers.** The main thread is worker 0. Workers spin about 2,000 pauses between runs, then
  sleep on a futex (on Linux; elsewhere they yield).
- **Cap.** The count is capped at the processors `sched_getaffinity` reports.

## Correctness

**`test/cpp/src/503-parallel-tick.cc`.** A three-core machine with mock caches is checked
against the serial loop at every tick, at 2, 3 and 8 threads. It has:
- a no-lane probe that sees every core;
- per-lane recorders of queued instruction IDs;
- idle-checked walkers.

The cases cover:
- a slower operable that must move;
- a trace ending mid-refill;
- an exception from a lane;
- declared interactions (one run per tick);
- lanes moving between threads (and a forced variant that rotates them at every rebalance);
- idle skipping.

Mutations of the moved schedule, the renumbering, the deferred IDs and the idle skip each make
it fail.

**`test/cpp/src/079-tracereader-decode-ahead.cc`.** The stream, `eof()` and the exception
position must match without decode-ahead, through an 8-entry ring.

**Stress and sanitizers:**
- 300 repetitions of 079 and 30 of 503, all passing;
- ThreadSanitizer reports nothing on 503, on 079, or on a 4-core, 4-thread simulation with and
  without `ip_stride`/`spp_dev` prefetchers.

**Real machines.** Every core count and mix, at 1, 2, 4, 8 and 16 threads, against the serial
build: see [report 5](05-parallel-results.md).

## How it got here

Breakdowns are at sixteen cores and eight threads (16-core mix, `-w 200000 -i 500000`) in
TSC cycles per tick, with an instrumented copy (`mtprobe/instrument.py`).

| Version | Tick | Serial | Critical path | Wall |
|---|---|---|---|---|
| Two runs per tick, sorted every tick | 35,515 | 3,955 | 18,543 + 12,228 | 16.2 s |
| + lane rebalancing | 34,416 | 4,191 | 16,991 + 12,437 | 15.8 s |
| + one run per tick | 32,650 | 4,065 | 27,985 | 14.9 s |
| + background trace decoding | 29,945 | 4,470 | 24,859 | 13.7 s |
| + skipping idle PTWs | 26,420 | 1,913 | 23,896 | 12.7 s |

Per-tick scheduling cost fell from about 1,700 cycles (the per-tick sort) to about 50. In a clean
A/B on a quiet host, the idle skip took the median from 13.1 s to 12.1 s.

**What is left.** The critical path is still about 1.8× the average worker's load (about 13,000
cycles). That is not average imbalance, which rebalancing removed: it is variance from tick to
tick, since a run waits for whichever lane is busiest on that tick, and a core's work comes in
bursts (mispredict recoveries, retire bursts, fills).

**Synchronization:**
- A dispatch round trip costs about 600–780 cycles among 8 threads on one CCD of the Ryzen AI
  Max+ 395, and about 1,500 across CCDs.
- Two simultaneous jobs sharing four processors at 4 threads each take 75 s, against 6.2 s for
  one alone.

So:

> Use as many threads as one CCD or CCX has physical cores, and give the process those cores'
> SMT siblings (or other free cores) for the trace decoders:
> `taskset -c 0-7,16-23 champsim --threads 8 ...`. Confining the threads to bare cores
> (`taskset -c 0-7`) makes the decoders compete with them, which cost 30% at 16 cores
> ([report 5](05-parallel-results.md)).
