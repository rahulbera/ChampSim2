# 1. How multi-core is modeled

This was read from the code at `122aaed3`, and the tick order was observed in real 2- and
8-core builds. It describes what a parallel tick has to preserve, and what it can ignore.

## Components

`NUM_CPUS` is a compile-time constant (`inc/defs.h`). On this branch a build can set it with
`CPPFLAGS=-DCHAMPSIM_NUM_CPUS=<n>`, which gives each count its own build directory.
`src/static_environment.cc` builds:

| Per core (×N) | Shared (×1) |
|---|---|
| `O3_CPU` | LLC, whose upper levels are every core's L2C→LLC channel |
| DTLB, ITLB, L1D, L1I, L2C, STLB | memory backend (legacy DRAM or Ramulator 2) |
| page-table walker (PTW) | one LLC→DRAM channel |
| 12 channels | `VirtualMemory` |

That gives 8N+2 operables: 66 at 8 cores and 130 at 16.

## Where cores interact

- **The LLC**, through the per-core L2C→LLC channels.
- **DRAM**, behind the LLC.
- **`VirtualMemory`**. Every core's page faults and page-table pages draw from one free list
  (`src/vmem.cc:134-182`), so where a core's pages land depends on the order of faults
  across all cores. Its callers are the PTWs (`src/ptw.cc:221,237,249`) and a perfect TLB
  (`src/cache.cc:300`).
- **`tracereader::instr_unique_id`** (`inc/tracereader.h:34`). This one counter numbers the
  instructions of all cores, in the order the main loop refills their input queues.
- **The heartbeat listener** (`inc/listeners/heartbeat.h`). Every core's `RETIRE` event
  updates one object. Its `add_cpu` grows vectors on demand, and it prints and flushes
  from inside `O3_CPU::operate()`.
- **Return-stack debug counters.** `static int num_times_returned_backwards` appears in
  `btb/basic_btb/return_stack.cc:29`, `inc/ittage/ittage_btb.h:170`,
  `inc/blbp/blbp_btb.h:148` and `inc/perfect_group/perfect_group.h:208`.
- **Vendored predictors.** `cbp6_*`, `ittage_*` and the BLBP BTBs keep one static
  instance and throw for any core but 0 (`inc/cbp6/cbp6_host.h:73`). So `configs/lnc.toml`,
  which selects `cbp6_tagescl64` and `ittage_64kb`, cannot run multi-core at all. The
  campaign machine uses `hashed_perceptron` and `basic_btb`.

Nothing else is shared:
- Each component's modules, statistics and channels are per instance.
- `replacement/random` keeps its generator as a member.
- `ship` and `drrip` keep per-CPU tables.
- The only other generator is the seeded shuffle of the free-page list, which runs at
  construction.

## The tick

`champsim::do_phase` (`src/champsim.cc:64`) advances one global clock by the smallest period
among the operables, which is the 250 ps core period. Each tick, `do_cycle` does three
things:
1. It copies the canonical operable list: cores, then caches (the LLC, then each core's six
   caches alphabetically), then PTWs, then DRAM.
2. It sorts the copy by `current_time` with `std::sort`.
3. It calls `operate_on` on each operable, then refills each core's input queue from its
   trace, decoding on the main thread.

An operable runs while its `current_time` is behind the clock (`src/operable.cc`). DRAM
runs at 1600 MHz (625 ps) against the cores' 250 ps. So whether DRAM is due changes from
tick to tick, and when it is due early, the sort moves it ahead of everything else.

### The order is scrambled at two or more cores

libstdc++'s `std::sort` finishes with insertion sort, which is stable, but it uses that
alone only for 16 elements or fewer. One core has 10 operables, so its order is "earliest
time first, ties in canonical order". Two or more cores have at least 18 operables, and
introsort's partitioning permutes the components that are tied.

**Two cores.** These are executed orders captured with the tick probe (each tick lists only
the operables due that tick):

```
cpu1 cpu1_PTW cpu0_PTW cpu1_STLB cpu1_L2C cpu1_L1I cpu1_L1D cpu1_ITLB cpu1_DTLB cpu0_STLB cpu0_L2C cpu0_L1I cpu0_L1D cpu0_ITLB cpu0_DTLB LLC cpu0
cpu1_DTLB memory cpu1_PTW cpu0_PTW cpu1_STLB cpu1_L2C cpu1_L1I cpu1_L1D cpu1_ITLB cpu0 cpu0_STLB ... LLC cpu1
```

**Eight cores.** The order changes from tick to tick. On some ticks a core's L2C runs before
its own L1D and before the LLC; on others the cores run from `cpu7` down to `cpu0`. The
full orders are in `order/order8.jsonl` in the evidence root.

**What this means:**
- **Latency asymmetry.** Within a tick, an operable sees what earlier operables did that
  tick and misses what later ones did. Different cores therefore see different hop
  latencies, by up to one tick per hop, and the pattern changes with DRAM's phase.
- **It is deterministic.** Introsort is deterministic, so repeated runs agree.
- **It is arbitrary.** No parallel schedule could reproduce it, and for a championship that
  compares cores under contention it is a fairness problem in its own right.

## What a parallel tick can keep

In a stable "time, then canonical" order, the per-core components of different cores do not
interact within a phase:
- the cores touch only their own L1 and TLB channels;
- the caches touch only their own core's channels, except the L2C, which writes its own
  L2C→LLC channel;
- the LLC and DRAM run alone.

So the stable order is equivalent, statistic for statistic, to this schedule:

| Phase | Runs | Why it is safe |
|---|---|---|
| A | every core, in parallel | a core touches only its own channels, predictor and BTB |
| — | LLC, serially | it is the only reader of the L2C→LLC channels |
| B | per core, in parallel: its six caches, then its trace refill | only core *k*'s operables and trace *k* are touched |
| C | the PTWs, serially in core order | they keep `VirtualMemory`'s fault order exactly |
| — | DRAM, serially, first or last as the time order puts it | — |

Instruction IDs stay identical, because phase A fixes each core's refill count. A prefix
sum over cores, taken in the serial LLC step, gives each core its ID base.

That leaves four pieces of shared state to deal with:
- the return-stack statics, which become members;
- the heartbeat listener, which gets fixed-size per-core state and per-core output that is
  flushed in core order;
- perfect TLBs, which call `va_to_pa` from phase B, so the run falls back to one thread when
  one is configured;
- printing from `operate()`, which only debug builds do.
