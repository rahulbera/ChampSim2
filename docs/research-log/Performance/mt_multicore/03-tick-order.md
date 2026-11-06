# 3. A stable tick order

**Verdict: retained.** One-core results are bit-identical. Multi-core results move once, and
the change removes a per-core bias.

## The change

`do_cycle` (`src/champsim.cc`) used to order each tick's operables with `std::sort` by
`current_time`. libstdc++'s sort is stable only up to 16 elements, which covers one core and
nothing more ([report 1](01-multicore-model.md)). It is now an insertion sort, which is
stable and linear here, because the canonical list is almost always already sorted:
- **one core:** it reproduces the old order exactly;
- **N cores:** it defines the order as cores, LLC, each core's caches, PTWs, DRAM, with an
  operable that is behind on a slower clock moving to the end.

`test/cpp/src/502-phase-order.cc` adds a 21-operable case that fails under `std::sort` and
passes with the stable sort.

## The gate

Evidence is in `results/2026-11-05-mt-multicore/gate/`, built with `freeze_binaries.sh` and run
with `gate.py`.

**Setup.** Each core runs `lnc.toml`'s core and caches with `hashed_perceptron` and
`basic_btb`. The LLC is scaled ×N, and legacy DRAM has one channel per four cores
(`mkconfig.py`). Each run is `-w 1000000 -i 2000000` per core. The mixes:
- 8 one-core traces;
- at each of 2, 4, 8 and 16 cores, three rate mixes (N copies of 710.omnetpp sp2, 707.ntest sp1
  or 734.vpr sp1) and three heterogeneous mixes drawn from a seeded permutation of the 32
  SPEC26 slices.

**Noise floor.** To measure the noise, I compared the baseline against itself with another
physical-page seed (`vmem.randomization=2` against the default 1). Two equally valid machines
differ by at most 1.93% in per-core IPC and 0.33% in mean IPC. One headline counter,
`dram.dbus_cycle_congested`, is noisy at up to 10% even here.

| | One core (8 runs) | Rate mixes (12) | Heterogeneous mixes (12) |
|---|---|---|---|
| Parity | all 8 bit-identical | — | — |
| Mean IPC change | 0 | −0.41% to +2.48% | −0.69% to +4.96% |
| Largest per-core IPC change | 0 | 3.93% | 15.49% |
| Noise floor, largest per-core | 0.24% | 0.70% | 1.93% |

### The heterogeneous outliers are a shifted window

**How warmup works.** In a multi-core run, warmup ends only when the *slowest* core has retired
its warmup instructions. A fast core keeps executing until then, so its region of interest
starts later in its trace, at a point set by the other cores' speed.

**What changed here.** With the stable order, the slowest core in 8-core mix-b finished warmup
3.3% sooner (462,501 cycles, against 478,078). 750.sealcrypto sp1 therefore entered its region
of interest after 3.18M instructions instead of 3.29M, and measured a different 2M-instruction
stretch. Its IPC fell from 5.20 to 4.48 (−13.9%). Its cumulative IPC during warmup was the
same in both runs (6.892 and 6.884).

**Longer windows confirm it.** Rerun with a 10M-instruction region of interest
(`gate/long-*`), the window shift shrinks:

| 8-core mix | Mean IPC change | Largest per-core | sealcrypto's change, 2M → 10M ROI |
|---|---|---|---|
| mix-b | +1.39% | +5.64% (723.llvm sp2) | −13.86% → −0.94% |
| mix-c | +1.67% | +5.86% (777.zstd sp0) | +18.32% → +0.36% |

### What remains is the old order's per-core bias

Cores that run the same trace should see the same IPC, apart from interference. In the
baseline, they do not:

| Rate mix | Baseline spread (CV) | Stable order spread (CV) |
|---|---|---|
| 8 × 707.ntest sp1 | 1.31% (0.44%) | 0.70% (0.22%) |
| 16 × 707.ntest sp1 | 1.10% (0.28%) | 0.93% (0.26%) |
| 16 × 710.omnetpp sp2 | 1.38% (0.40%) | 0.45% (0.13%) |
| 16 × 734.vpr sp1 | 3.35% (1.21%) | 0.89% (0.25%) |

In the baseline's 16 × vpr, the cores fall into two groups, at 0.145–0.146 and 0.141–0.142
IPC. Which group a core lands in depends on where introsort left its components in the tick
order: the cores whose L2C ran before their L1D, or before the LLC, paid an extra tick on those
hops. The stable order gives every core the single-core hop latencies, so the groups disappear.

So the multi-core results move by up to about 6% for a few cores, and by 1.4–2.5% in mean
IPC on memory-bound mixes. That is the bias being removed, not an error being added, and it
is the order a phase-parallel tick can reproduce exactly. Every multi-core result recorded
before this commit carries the old bias, so compare multi-core results only against runs from
the same side of it.
