# Fixed PTW Performance Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan
> task by task in the authorized separate clone. Steps use checkboxes.

**Goal:** Add optional fixed PTW latency and measure the legacy-DRAM KIPS gap
between detailed ChampSim, simplified ChampSim and Hermes.

**Architecture:** Add one optional duration to the existing PTW builder and a
separate FIFO service path to PageTableWalker. The environment converts runtime
CPU-cycle latency to time; the detailed path remains intact.

**Tech Stack:** C++17, Catch2, runtime TOML, Python 3.11, GNU Make, system GCC.

**Spec:** [Fixed PTW design](../specs/2026-09-14-fixed-ptw-performance-design.md)

## Global constraints

- All experiments use legacy DRAM and `WITH_RAMULATOR2=0`.
- Work only on `feat/perf-fix` in the separate clone.
- Preserve detailed-mode phase statistics; fixed mode deliberately changes modeling.
- Fixed latency is in the owning CPU's cycles, default 200 when selected.
- Preserve trace files; pass explicit output paths and `--` to ChampSim.
- Keep test objects separate from release objects; unset inherited compiler flags.
- Run timing sequentially on one pinned CPU after all compilation has finished.

## Task 1: Fixed-latency service and configuration

**Files:** `inc/ptw{,_builder}.h`, `src/ptw{,_builder}.cc`,
`src/static_environment.cc`, new `test/cpp/src/601-fixed-ptw.cc`.

**Interface:** `ptw_builder::fixed_latency(std::optional<chrono::picoseconds>)`
selects the service path. Existing channel requests/responses remain unchanged.

- [x] Save a clean release baseline and run existing C++ tests in separate objects.

```bash
env -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u CFLAGS make CXX=/usr/bin/g++ -j6 WITH_RAMULATOR2=0
env -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u CFLAGS make CXX=/usr/bin/g++ CPPFLAGS=-I.csconfig -j6 WITH_RAMULATOR2=0 OBJ_ROOT=.csconfig-test DEP_ROOT=.csconfig-test test
```

- [x] Write a failing runtime-boundary test that sets fixed mode, admits an STLB
  request, requires no lower-level traffic and requires a response after 200 CPU
  cycles. Use unrandomized physical pages and literal expected mappings.

```cpp
cfg.set("ptw.cpu0_ptw.model=fixed");
cfg.set("ptw.cpu0_ptw.fixed_latency=200");
cfg.set("vmem.randomization=false");
// Send through the STLB's real lower channel; operate the real PTW.
// Check empty lower queues before and after the response deadline.
```

- [x] Run `[fixed-ptw]` against the baseline and retain the expected failing log.
- [x] Add optional builder duration; skip PSCL/CR3 construction when present.
  At `operate()` entry dispatch to `operate_fixed()`. Complete ready FIFO entries,
  then accept bounded RQ prefixes and set `ready_at = current_time + delay`.
  Preserve response fields through the existing completed-entry representation.
- [x] Wire `.model` and `.fixed_latency` to runtime config; validate model name,
  multiplication bounds, positive queue/service limits and builder duration.
- [x] Add tests for zero delay, nonuniform clocks, queue saturation, bandwidth,
  no-response requests, phase carry-over, duplicate-page reuse and CPU identity.
- [x] Run focused and full suites, format touched C++, and review the diff.
- [x] Document runtime usage in `configs/fixed-ptw.toml` and `CLAUDE.md`; commit.

## Task 2: Controlled legacy-only experiment

**Files:** new `tools/perf/benchmark_ptw.py`, `tools/perf/README.md`, benchmark
configuration; durable external results directory for binaries and raw evidence.

**Interface:** the harness accepts binary/config/trace paths, CPU affinity,
warmup/ROI counts and repetitions, and writes raw run records plus summary CSV.

- [x] Prepare an isolated Hermes clone, select GLC/perceptron/no prefetch/LRU,
  build with `/usr/bin/g++`, and record configuration and source differences.
- [x] Rebuild candidate release objects separately from tests. Run a short
  detailed-baseline parity check and fixed replay before measuring performance.
- [x] Implement a small subprocess timing harness with `perf_counter`, child CPU
  accounting, explicit output directories, actual retired-count parsing, and
  error/timeout checks. Record argv as JSON arrays, not shell strings.

```python
kips = actual_retired / (1000.0 * wall_seconds)
```

- [x] Pilot all three variants; size three real-trace runs to exceed startup noise.
- [x] Run at least three interleaved repetitions per trace and variant with no
  concurrent builds. Preserve results, input hashes, output hashes and host state.
- [x] Check model-specific deterministic results across repetitions and verify
  detailed-phase parity and fixed-mode absence of translation traffic.
- [x] Write the report with KIPS gaps, variability, configurations, limitations
  and the next profiling recommendation; commit tooling and documentation.

## Task 3: Profile and rank hotspots (user-authorized continuation)

The user subsequently asked to continue thorough performance analysis while away,
identifying hotspots to optimize. This extends the first experiment to profiling;
it does not authorize silently weakening the detailed model for a speed claim.

- [x] Finish the unprofiled timing campaign before running profilers or builds.
- [x] Collect software CPU samples from the same release binaries on SQLite,
  GCC and mcf, in detailed/fixed/Hermes modes. `perf` is blocked by this host's
  `perf_event_paranoid=4`. GNU gprofng CPU timing proved unreliable in a
  controlled timer probe; use GDB jittered stack sampling instead. Preserve
  rejected collector logs and validate all simulator outputs.
- [x] Check every profiled run's retirement/cycle counts and ChampSim phase
  statistics against its corresponding unprofiled run. Separate profiler overhead
  from the baseline KIPS measurements.
- [x] Export exclusive and inclusive function costs and call paths. Attribute
  scheduler, core, cache, translation, trace/decompression, allocator and startup
  work without summing overlapping inclusive percentages.
- [x] Run short heap-tracing probes if allocation is material in CPU samples;
  inspect call paths and event counts. Do not use their perturbed elapsed time as
  simulator throughput. Archive the original commands and samples.
- [x] Tie the measured hotspots to concrete source loops/copies/allocations,
  distinguish evidence from hypotheses, and rank behavior-preserving optimization
  candidates with the checks each future change would require.
- [x] Address the review finding that long fixed delays can exceed the default
  watchdog. First run the new CLI regression against the benchmarked binary;
  then derive the default allowance from fixed delay, walker period and actual
  minimum clock quantum, keeping explicit overrides authoritative. Recheck short
  delay legacy parity after the change.
- [x] Record completed evidence, methodological limits and a proposed next
  optimization sequence in the performance report. Keep all raw artifacts outside
  source control and all code/documentation on `feat/perf-fix`.


## Completed evidence

See [the performance report](../../research-log/Performance/2026-09-14-fixed-ptw-hotspots.md).
The primary campaign contains 36 sequential unprofiled runs, nine accepted full
stack profiles, and six accepted short heap-event collections. Profiled model
outputs match their references. Rejected gprofng CPU profiles and the abandoned
larger heap pilot are explicitly excluded and retained for diagnosis. The fixed
service is commit `25959793`; watchdog followup is `17bd7fd5`. All remaining
optimization candidates are documented as future, separately measured changes.
