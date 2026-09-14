# Fixed PTW / Hermes speed comparison

This first performance experiment compares current ChampSim with detailed page
walks, current ChampSim with a fixed translation delay, and the older Hermes
simulator. Every ChampSim invocation explicitly selects legacy DRAM. Hermes uses
its built-in DRAM controller, with optional off-chip prediction and DDRP disabled.

## Metric and controls

KIPS = **actual warmup plus ROI retired instructions / (1,000 × process wall
seconds)**. The denominator includes construction, trace reading/decompression,
simulation, and final reporting. Do not interpret it as phase-only KIPS. Actual
counts include retirement-width overshoot. The harness separately records host
CPU time and simulated cycles per wall second.

Runs are sequential and inherit one pinned CPU. Variant order rotates each
repetition; the harness saves commands, binary/config/trace hashes, full outputs,
return codes, load averages, timings and a median/range summary. Use an idle
machine and do not run builds or other experiments concurrently. Turbo/governor
and library differences should be recorded with the report. A timeout watchdog
allows blocking `waitpid` rather than timeout polling to timestamp termination.

The harness rejects existing output directories, trace hash mismatches, missing
completion records, premature retirement, nonzero exit codes, non-legacy memory,
and non-deterministic counts. It checks full ChampSim phase-statistics hashes
across repetitions. Hermes determinism is checked only for retired counts and
simulated cycles, not its entire statistics document. Fixed runs must have zero
translation events in data caches. An optional original binary also checks
detailed-mode phase-statistics parity.

## Build and run

Build release and test objects separately. This Makefile normally shares objects
between targets, and `make test` adds `-Og`. When using a separate test object
directory, explicitly include the original generated registry directory:

```bash
./config.sh
env -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u CFLAGS \
  make CXX=/usr/bin/g++ -j6 WITH_RAMULATOR2=0
env -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u CFLAGS \
  make CXX=/usr/bin/g++ CPPFLAGS=-I.csconfig -j6 WITH_RAMULATOR2=0 \
  OBJ_ROOT=.csconfig-test DEP_ROOT=.csconfig-test test
python3 -m unittest discover -s tools/perf -v
```

Prepare Hermes in an isolated clone because its build script copies selected
modules over source files. For this study, use revision
`0701249f4656b76a9bd36b5c3a63ee7b831274df`, GLC, perceptron, all four prefetchers
`no`, and LLC replacement `lru`, with system GCC and its release `-O3` flags.
The `libbf` static library is also required. Do not time an unexplained preexisting
binary. The report's results archive includes the exact source-selection patch,
build log, effective configuration output and binary hashes.

Create read-only scratch copies of trace inputs and a JSON manifest:

```json
[
  {"name": "sqlite", "version": 2, "path": "/absolute/scratch/708.sqlite_r.sp0.champsim2.zst", "sha256": "the verified SHA256 of that file"}
]
```

Pass absolute binary/input paths. The output directory must be new. A small
100k/500k, one-repetition pilot can first check compatibility and choose run
lengths; use longer repeated runs for the reported results.

```bash
python3 tools/perf/benchmark_ptw.py \
  --champsim /absolute/champsim-candidate \
  --hermes /absolute/hermes/bin/champsim \
  --config configs/champsim_config.toml --config configs/perf-hermes.toml \
  --traces /absolute/traces.json --output /absolute/new-results \
  --cpu 8 --warmup 1000000 --instructions 3000000 --repetitions 3
```

`--baseline /absolute/original-champsim` adds the original detailed variant and
asserts phase-statistics parity against candidate detailed mode. The KIPS shown
for short pilots are not the final performance claim. `--fixed-latency` defaults
to 200. The fixed variant overrides only PTW model/latency; all remaining
ChampSim configuration is shared with detailed mode.

## Interpretation

`configs/perf-hermes.toml` matches GLC sizes, widths and timings that can be
expressed in current ChampSim. It is an overlay on `champsim_config.toml`, not a
complete machine description. Both use perceptron, no prefetchers and LRU.
Hermes still differs in pipeline/BTB behavior, translation and page placement,
DRAM algorithms, statistics work and trace decoding. Its 100-cycle page-table
penalty applies differently from a delay on every admitted STLB miss. Current
fixed mode also enforces `mshr_size`; current detailed PTW does not enforce that
member. These are speed references with documented differences, not equivalent
microarchitectural models.

Fixed mode retains the PTW service clock and the empty PTW-to-L1D channel in the
graph. It removes detailed walks, not every polling cost of the surrounding
framework. Changes in KIPS mix changes in cost per simulated cycle with changes
in the number of cycles simulated. Compare both metrics before assigning a
speed gap to PTW implementation overhead. Run optimizations that preserve the
detailed model only after measuring the remaining bottlenecks.

## Stack profiling without hardware counter access

`profile_ptw.py` replays the exact commands and release binaries from a completed
campaign, checking input/binary hashes and post-run retirement, cycle and phase
statistics. Its default `--kind stacks` uses GDB with the supplied
`gdb_sample.py`, Python-enabled GDB, and Linux pidfds. It does not require changing
`perf_event_paranoid`. Use a fresh output directory:

```bash
python3 tools/perf/profile_ptw.py \
  --campaign /absolute/new-results --output /absolute/new-stack-profiles \
  --traces sqlite gcc mcf --variants detailed fixed hermes --interval-ms 20
python3 tools/perf/summarize_stacks.py /absolute/new-stack-profiles \
  --output /absolute/new-stack-profiles/component-summary.csv
```

The sampler starts its own inferior, sends jittered SIGINT stops at 75–125% of the
requested interval, records stacks and continues without passing the signal to
the application. Percentages are wall-stack sample shares, useful as approximate
CPU shares for the verified CPU-bound cases. Profiler duration is not KIPS. Raw
stacks retain function names, PCs, libraries and timing intervals for inspection.
One profile is suitable for broad ranking, not precise small-difference claims.

Component categories in `summarize_stacks.py` are disjoint. Allocator, bandwidth
and ROB-scan columns overlap those categories and one another. Hermes has a
different top-level loop; its remaining main-loop samples share the
`startup_finish_other` category. Do not add inclusive call-path percentages or
interpret Hermes and current component implementations as equivalent.

`--kind cpu` and `--kind heap` use GNU gprofng. This host's gprofng 2.42 has a
reproduced timer problem: a requested 10 ms period sampled at about 100 ms and
reported CPU seconds about ten times too small. CPU profiles with collector
warnings are rejected, even if the simulation itself matches its reference.
Use GDB profiles for this investigation's timing attribution.

Heap tracing records allocation events and can be useful independently of the
CPU timer, but requires separate calibration. Here a noinline C function with
1,000 calls to `malloc(64)` followed by `free`, with an inline-assembly use of each
pointer to prevent optimization, yielded exactly 1,000 allocations / 64,000 bytes
at that function in gprofng. The probe source, executable and experiment are
archived with the report. Only after such validation may
`--allow-heap-timer-warning` accept that specific warning for **heap counts**.
Other warnings are rejected; no gprofng CPU-time result is accepted this way.

Use a much shorter, separately benchmarked window for heap tracing. Point the
profiler at that short campaign so it still checks exact reference phase hashes:

```bash
python3 tools/perf/profile_ptw.py \
  --campaign /absolute/short-heap-reference --output /absolute/new-heap-profiles \
  --traces sqlite mcf --variants detailed fixed hermes --kind heap \
  --allow-heap-timer-warning
```

Heap tracing can be hundreds of times slower and generate large files. Its
allocation totals include startup and warmup. Compare normalized counts and call
paths, not its elapsed time. To export useful allocation rankings:

```bash
gprofng display text \
  -metrics i+heapalloccnt:i+heapallocbytes:e+heapalloccnt:name \
  -sort i+heapalloccnt -limit 100 -functions /absolute/run/profile.er
```

The report and optimization order are in
[the performance research log](../../docs/research-log/Performance/2026-09-14-fixed-ptw-hotspots.md).
