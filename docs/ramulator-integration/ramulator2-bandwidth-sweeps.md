# DRAM bandwidth sweeps with the Ramulator 2 backend

This report covers how to sweep memory bandwidth with the native backend, what
the bandwidth studies observed while working that out, and the resulting best
practices.

The evidence comes from one Linux host, using GCC 13 and one- and four-core
`-O3` builds of `fix/ramulator2-review` at `3cbb6e2b`: enabled builds, plus a
legacy-only build for some legacy runs. Native results are unchanged through
`4e1bf028`. The refresh figures come from an earlier study on a `74159f1e`
binary. The configuration is the DDR4 fixture
(DDR4_8Gb_x8, DDR4_2400R, one rank, one controller, 64-byte transactions,
8 GiB) unless a section says otherwise. Full numbers and their limits are in the
integration writeup's
[bandwidth subsection](ramulator2-integration.md#modelling-memory-bandwidth-with-the-native-backend)
and [section 4](ramulator2-integration.md#4-weak-points-and-the-stress-tests-they-need),
and in the validation record's
[close-out rows](ramulator2-validation.md#review-and-close-out-evidence).

## Recommended workflow

1. **Export the sweep points** with the committed generator. Do not hand-edit
   the YAMLs:

   ```sh
   RAMULATOR2_ROOT=/path/to/ramulator2 configs/ramulator2/make_bandwidth_sweep.sh sweep
   # or choose the points yourself:
   PYTHONPATH=/path/to/ramulator2/python python3 configs/ramulator2/bandwidth.py \
       --standard ddr4 --stock --mbps 200 800 1600 6400 --out-dir sweep/ddr4
   ```

   [`bandwidth.py`](../../configs/ramulator2/bandwidth.py) writes one YAML per
   point plus `manifest.csv`. The manifest records each point's nBL, expected
   bandwidth, whether the point is calibrated, the suggested guard, legacy
   equivalents and the exact `--set` arguments.
   [`make_bandwidth_sweep.sh`](../../configs/ramulator2/make_bandwidth_sweep.sh)
   exports the default sweep:
   - DDR4: the seven calibrated points from 100 to 6,400 MB/s, plus stock.
   - DDR5: 50-700 MB/s, plus stock.
2. **Run every point** with its manifest `champsim_set` arguments. For example,
   the DDR4 200 MB/s point:

   ```sh
   bin/champsim --set dram-model=ramulator2 \
       --set ramulator2.config=$PWD/sweep/ddr4/ddr4_nbl381.yaml \
       --set sim.livelock_period=1000000000000 \
       -w 10000000 -i 10000000 --toml nbl381.toml -- trace.xz
   ```

3. **Check each document** before using its IPC. Every path below is under
   `phase.<name>.roi.ramulator2`:
   - Channel utilization: `native.memory_system.controller.channel0.total_throughput_MBps`
     divided by the manifest's `expected_mbps`.
   - Saturation: `adapter.rejected_submissions`, and
     `channel0.read_queue_len_avg` near the 32-entry native read buffer.
     `queue_len_avg` also counts the write and priority buffers; the sweep's
     32.8-33.4 is that sum.
4. **Interpret within one backend.** Back claims about shared channels,
   workload mixes or fairness with N-core runs (see
   [Per-core bandwidth](#per-core-bandwidth)).

## Observations

### nBL is the knob, and it behaves like legacy `pmem.data_rate`

- **What it sets.** nBL is the burst length of one data transfer: the channel's
  RD-to-RD and WR-to-WR spacing. Bandwidth is about 64 B / (nBL x tCK) x A.
  - On DDR4-2400R that is 76,831 x A / nBL MB/s.
  - A is the share of time refresh leaves the channel available: 0.95 at nBL
    8-32, 0.958 at 64, 0.978 at 256, and 1.000 from 512.
- **Accuracy.**
  - The calibration harness, which drives each backend alone with 64 pending
    reads, matched 76,831 / nBL within 0.95-1.00 from nBL 8 (9.1 GB/s) to
    nBL 2,048 (37.5 MB/s).
  - End to end, a saturated 605.mcf_s window reached 95% of that at nBL 256,
    99% at 1,024, and 100% from 4,096 to 65,536 (1.17 MB/s).
- **Same meaning as legacy.** Every read gains nBL x tCK of latency, as legacy
  gains one transfer time. `pmem.data_rate` ~= 9,604 / nBL gives the same bus
  time per 64-byte block.
- **Side effects.** Constraints that contain nBL also grow, such as read-write
  turnaround and WR-to-PRE. Other base timings and refresh stay fixed in cycles.
- **Granularity and ceiling.**
  - nBL is an integer, so steps near the top are coarse: nBL 12 to 11 is +9%.
  - At nBL 6 and below, random reads stop near 11.2 GB/s. The ACT window and
    one command per tick limit them, not nBL.
  - 12.8 GB/s needs nBL 6 with a non-JEDEC nFAW of 24, which measured
    12,158.6 MB/s.
  - Beyond that, add controllers with 64-byte transactions: two gave 1.76x and
    four gave 2.63x on 605.mcf_s at nBL 256.

Calibrated DDR4 points, random reads in the harness. The generator uses these
values for these targets:

| Target MB/s | Native nBL (measured MB/s) | Legacy `pmem.data_rate`, `pmem.bankgroups=4` (measured MB/s) |
| ---: | --- | --- |
| 100 | 768 (100.0) | 13.035 (100) |
| 200 | 381 (199.9) | 26.072 (200) |
| 400 | 186 (400.9) | 52.036 (400) |
| 800 | 92 (802.1) | 104.069 (801) |
| 1,600 | 46 (1,596) | 209.864 (1,605.4) |
| 3,200 | 23 (3,183.9) | 419.968 (3,159.3) |
| 6,400 | 11 (6,648.2) | 856.524 (6,348.5) |

### The Python-to-YAML export can leave timings stale

The pinned exporter resolves derived timings from the preset *before* it applies
overrides:

- **DDR5.** An nBL-only DDR5_5600B export keeps `nRTW` at 16. Measured
  throughput was 112%, 121% and 125% of nominal at nBL 1,024, 4,096 and 16,384.
  IPC was 8%, 19% and 27% higher than with `nRTW = nBL + 8` at nBL 256, 1,024
  and 4,096.
- **tCK.** A `tCK_ps` override keeps nREFI, nRFC and the rate-keyed tables at
  the preset's cycle counts.
- **DDR4** derives nothing from nBL, so its nBL overrides are consistent.

`bandwidth.py` recomputes every derived timing from the overridden values and
passes the changed ones explicitly. On DDR5 that sets `nRTW` together with nBL.
It refuses to write a YAML whose exported timings differ from the recomputation.

### DDR5 is not calibrated

With `nRTW` corrected, 605.mcf_s windows measured 625, 164.7 and 42.1 MB/s at
nBL 256, 1,024 and 4,096: 89%, 94% and 96% of 64 B / (nBL x 357 ps). A
generator smoke run at nBL 896 measured 94.9%. nBL-only runs also covered nBL 8
and 16,384. These windows were not confirmed to saturate the channel, so the
shortfall is not attributed to refresh.

The generator therefore reports nominal bandwidth for DDR5 and marks every DDR5
point uncalibrated. The script's non-stock DDR5 points stay inside the measured
nBL range, 256-4,096. The guard rule below was fitted on DDR4 and is unverified
on DDR5. The longest DDR5 stall measured, at nBL 4,096, was 14,343 ticks,
against the rule's 51,934.

### Default guards stop legitimate low-bandwidth runs

- **Livelock check.** The IPC < 0.01 abort fired on 605.mcf_s in both backends
  at 100 MB/s and natively at 75 MB/s, and on 621.wrf_s at 300 MB/s. A longer
  finite period only delayed it. Set `sim.livelock_period=1000000000000` on
  every point.
- **Native no-progress guard.** The default (40,000 ticks at a 250 ps quantum)
  sufficed at 200 MB/s and above.
  - With writes in the window, the longest silent stall was about
    2 x nBL + 900 memory cycles. nBL 5,500 (14 MB/s) finished at 39,660 of its
    40,000 ticks, and nBL 5,800 aborted.
  - The rule `max(40000, ceil(4 x (2 x nBL + 900) x tCK_ps / quantum_ps))` was
    validated only at nBL 5,800 (166,600 ticks).
  - The manifest's `native_deadlock_cycle` column always holds the rule's
    value. `champsim_set` includes it only when it exceeds the default: above
    nBL 1,050 on DDR4 at 250 ps. Pass `--quantum-ps` for other core
    frequencies.
- **Legacy.** The default of 500 ticks aborted at `pmem.data_rate` 64 and
  below. The sweep and overload runs used `sim.deadlock_cycle=400000`.

### Knobs that look equivalent but are worse

- **Scaling tCK** gives the same throughput, but it rounds every timing, leaves
  refresh timings stale, and gave 6.5-10.7% lower IPC than nBL at equal
  bandwidth.
- **A smaller transaction payload** moves in powers of two and changes capacity
  and per-block latency. It gave 4-19% lower IPC near stock bandwidth.
- **Extra controllers with 32-byte transactions** add capacity, not bandwidth:
  core and cache statistics were identical to one controller.
- **Rate-limiting the bus while keeping nBL short** keeps single reads fast. As
  a stand-in for four cores sharing a channel, it gave 44.3% mean absolute
  per-core error on mixed traces at 4.8 GB/s per core, against 25.4% for nBL.

### What a prefetcher sweep looks like

The one-core sweep ran 605.mcf_s, 654.roms_s and 727.cppcheck_r.sp1,
`-w 10000000 -i 10000000`, with and without L2C `spp_dev`, on both backends.
Speedup is IPC with `spp_dev` over IPC without it, legacy / native. "Stock" is
the unthrottled preset and is not bandwidth-matched: random reads top out near
11.2 GB/s native and 18.2 GB/s legacy. Native roms speedups at 200, 6,400 MB/s
and stock come from reruns with a one-line `spp_dev` patch, because the unpatched
module aborted (see below).

| Trace | 200 MB/s | 800 | 1,600 | 6,400 | Stock |
| --- | --- | --- | --- | --- | --- |
| 654.roms_s | 0.993 / 0.983 | 1.033 / 1.011 | 1.109 / 1.052 | 1.773 / 1.811 | 2.485 / 2.583 |
| 727.cppcheck_r | 0.853 / 0.844 | 0.976 / 0.935 | 1.113 / 1.033 | 1.357 / 1.285 | 1.360 / 1.297 |
| 605.mcf_s | 0.935 / 0.959 | 0.960 / 0.989 | 0.950 / 0.982 | 1.001 / 1.002 | 1.002 / 1.001 |

- **Trends.** IPC rose with bandwidth in every series, and the two backends
  agreed on the direction of the prefetcher effect at every level.
- **Prefetching becomes a slowdown.** At 200 MB/s every trace in both backends
  ran slower with `spp_dev`.
  - On roms the prefetches stayed about 99% accurate but kept the native
    buffers full: 11.6M rejected submissions and `queue_len_avg` 32.8.
  - LLC demand miss latency rose from 5,169 to 25,542 cycles.
  - Without a prefetcher, one core drove the channel to 94-98% of its expected
    bandwidth at 200 MB/s on all three traces. At 1,600 MB/s it reached about
    91% on mcf and roms but 70% on cppcheck.
- **Absolute IPC without a prefetcher.** It agreed across backends at
  200 MB/s (native/legacy 0.986-0.993) but not above: 1.033-1.144 at
  800-1,600 MB/s and up to 1.442 at stock. With `spp_dev` at 200 MB/s the ratio
  was 0.976-1.020.
  - Native row hits were 45-52% on roms and 76-88% on cppcheck, against 0-9% in
    legacy. The code suggests why: native's address mapping keeps neighbouring
    blocks in one row, while legacy's rotates them across banks.
  - The row hits were measured, but their contribution was not separated from
    the backends' different core timings or from legacy serving one request per
    bank. Compare within one backend.
- **`spp_dev` has its own bug.** It reads past `confidence_q`
  (`prefetcher/spp_dev/spp_dev.cc:77`), which aborted four native roms runs and
  made results depend on command-line length.

### Cost

- **Host time** scales with simulated cycles, which is instructions / IPC. A
  200 MB/s run cost 7.1-25x its stock run (8.7-15x natively without a
  prefetcher). mcf and roms took 39-52 minutes for 10M warmup plus 10M
  measured instructions, on a host also running other jobs.
- **Backend comparison.** Native was 0-31% faster than legacy in 35 of 36
  matched pairs. The 86-run matrix took 2 h 09 min with six runs in parallel.
- **Memory under saturation** stayed bounded.
  - libquantum at 200 MB/s held 127.2-127.7 MiB over 100M instructions.
  - The LLC's 64-entry outstanding-miss table bounds the reads that expect a
    response; writebacks have no such bound.
  - In a rejection flood, RSS grew from 127.3 to 129.8 MiB over 50M
    instructions, which tracked VirtualMemory's page map.
  - Request accounting was exact in all 75 overload documents, which included
    1.69 billion rejected submissions.

### Per-core bandwidth

A single-core sweep models one core with a dedicated channel. It stands in for
N cores sharing N times that bandwidth only when the shared channel is saturated
and every core runs the same workload. Against four cores sharing a 4B channel,
with the LLC scaled per core:

- **Within 2%:** 605.mcf_s at 1.2 and 0.3 GB/s per core, and 462.libquantum at
  0.3 GB/s.
- **Off:**
  - by 9.6% for libquantum at 1.2 GB/s (its single-core channel only 84% used)
  - by 33.5% for mcf at 4.8 GB/s (the shared channel at 43% of capacity)
  - by 8-124% per core for a four-benchmark mix, whose shared channel was split
    by demand, not evenly
- **Prefetcher geomean:** an L2C `ip_stride` geomean speedup reversed, 0.938
  shared against 1.062 emulated.
- **LLC scaling:** leaving the shared LLC unscaled added about 12%.

## Best practices

1. **Throttle with nBL only.** Hold the preset, controller count, payload,
   capacity and refresh fixed across the sweep, and generate every point with
   `bandwidth.py`.
2. **Prefer the calibrated DDR4-2400R points.** For DDR5 or any other preset,
   measure saturated throughput at two or three points before trusting the
   nominal value.
3. **Report the bandwidth actually modelled**, meaning the expected or measured
   MB/s rather than the target, next to channel utilization. On one DDR4
   channel nBL tracks the formula up to about 9 GB/s (nBL 8), and random reads
   cap near 11.2 GB/s at nBL 6 and below; add controllers beyond that.
4. **Set the guards on every point:**
   - `sim.livelock_period=1000000000000`
   - `sim.deadlock_cycle` wherever the manifest's `champsim_set` includes it
   - `sim.deadlock_cycle=400000` for legacy runs
   - a `timeout` around each run
5. **Record per point:** IPC, LLC miss latency, total throughput and
   utilization, `rejected_submissions`, `read_queue_len_avg`, adapter read
   latency, and prefetch useful/useless counts.
6. **Compare speedups within one backend.** When crossing backends:
   - match capacity (`pmem.bankgroups=4` for 8 GiB)
   - say which legacy `data_rate` mapping you used (bus-time matched or
     calibrated)
   - state legacy's truncated 3 µs refresh interval
   - expect absolute IPC to differ above about 200 MB/s
7. **Describe a single-core sweep as a dedicated channel.** Scale the LLC per
   core, validate one point against an N-core shared-channel run, and use N-core
   runs for mixes, fairness and prefetcher claims. Suggested wording:

   > We model a per-core memory bandwidth B_core by attaching one simulated core
   > to a DDR4-2400R channel whose data-bus burst per 64-byte transfer is
   > lengthened (Ramulator 2 nBL ~= 64 B / (B_core x tCK)), so that the
   > channel's nominal sustained transfer rate is B_core; the measured rate is
   > 95-100% of nominal because of refresh. Timing constraints that include the
   > burst length grow with it, while the other DDR4 base timings, refresh and
   > the 8 GiB capacity are unchanged. This models a core with a dedicated
   > B_core channel, not contention among cores sharing a channel.

8. **Include levels at or below 1,600 MB/s in prefetcher studies**, where
   memory-heavy traces already drive one channel close to capacity from a
   single core. Budget their host time by 1 / IPC.
9. **Keep refresh at the preset.** Refresh sets A and the latency tail:
   refreshing a thousand times less often raised IPC by up to 5.45% and roughly
   halved p99 read latency.
10. **Do not use unpatched `spp_dev`** as a baseline. The later campaigns used
    `next_line`, `ip_stride` and `va_ampm_lite` instead.

## Generator reference

```
bandwidth.py [--standard {ddr4,ddr5}] [--mbps MBPS ...] [--nbl N ...] [--stock]
             --out-dir DIR [--quantum-ps PS] [--force]
```

- **Presets.** `ddr4` is DDR4_8Gb_x8 / DDR4_2400R, the shipped fixture;
  `--stock` reproduces `configs/ramulator2/ddr4.yaml` byte for byte. `ddr5` is
  DDR5_16Gb_x8 / DDR5_5600B, also 8 GiB. Both use one GenericDDR controller with
  FRFCFS, AllBank refresh, the Open row policy and RoBaRaCoCh, like
  `configs/ramulator2/ddr4.py`.
- **Choosing nBL.** A calibrated DDR4 target uses the measured nBL. Any other
  target uses the nBL whose expected bandwidth, with A interpolated on DDR4 and
  A = 1 on DDR5, is closest. `--standard` defaults to `ddr4`, and `--mbps`
  takes integers.
- **Manifest columns.**
  - `nominal_mbps`, `expected_mbps` (blank where other timings cap random reads)
  - `derived_overrides`, such as `nRTW=1032`
  - `native_deadlock_cycle`, `legacy_data_rate_bus_matched`,
    `legacy_data_rate_calibrated`, `legacy_bankgroups`
  - `champsim_set`, with an absolute YAML path
  - `notes`: uncalibrated, outside the measured range, or capped
- **Overwrites.** Existing files are never overwritten without `--force`.
- **Checks.** `test/ramulator2/test_tools.py` checks:
  - the calibrated and nearest-nBL choices
  - the guard and legacy arithmetic
  - the refusal to write timings that differ from their recomputation, with a
    stand-in exporter
  - with `RAMULATOR2_ROOT` naming a native checkout: the byte-identical stock
    export, the derived DDR5 `nRTW`, and the refusal to overwrite

## Limits of this evidence

- **Coverage.**
  - One host.
  - The one-core sweep covered three traces for 10M instructions each.
  - The per-core comparison covered one mix of v1 traces at 5M instructions per
    core.
  - Corrected DDR5 runs covered nBL 256, 896, 1,024 and 4,096, all on
    605.mcf_s.
  - The guard rule was validated at one point.
- **Retention.** The calibration harness, sweep scripts and raw outputs were not
  committed and live only in temporary session directories. Their results
  survive in this report, the writeup, the validation record's bandwidth rows
  and the generator's constants.
- **Unexplained gaps.** Neither the cause of the native/legacy absolute-IPC
  gap nor DDR5's shortfall from nominal was separated experimentally.
