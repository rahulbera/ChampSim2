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

Production and test objects are isolated automatically by mode and flavor.
Use a controlled compiler environment and resolve candidate paths explicitly:

```bash
./config.sh
env -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u CFLAGS \
  make CXX=/usr/bin/g++ -j4 WITH_RAMULATOR2=0 release
env -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u CFLAGS \
  make CXX=/usr/bin/g++ -j4 WITH_RAMULATOR2=0 BUILD_MODE=release test
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


## Behavior-neutral optimization comparisons

`compare_optimization.py --before BINARY --after BINARY --traces MANIFEST
--config configs/champsim_config.toml --config configs/perf-hermes.toml
--output NEW_DIRECTORY` runs paired detailed-PTW timings (1M/3M, three repetitions,
CPU 8), rejecting any difference in complete phase statistics or effective config.
`--regression --warmup 100000 --instructions 500000 --repetitions 1` additionally
covers both PTW modes, changed seeds/clocks, prefetching and DRAM geometry; its
short-run timings are not performance claims. Build and run these sequentially.

`malloc_counts.c` is an optional Linux/glibc diagnostic interposer, compiled with
`gcc -std=c11 -O2 -fPIC -shared tools/perf/malloc_counts.c -o /absolute/counter.so`.
Set `LD_PRELOAD=/absolute/counter.so` and
`CHAMPSIM_MALLOC_COUNTS=/absolute/new-file.json` for a single simulation. It counts
malloc/calloc/realloc calls, including startup, and writes only a new output file.
It uses glibc's internal allocation entry points, so it is not portable and is
never loaded for reported KIPS measurements. Check phase parity with the
uninstrumented run. The running optimization log records code revisions and each
incremental before/after result.

## Named build provenance

Use `make release`, `make debug`, or `make fast` for separate canonical binaries.
Release is `-O3 -g3` with ChampSim assertions; fast uses the same optimization with
ChampSim assertions disabled; debug uses `-O0 -g3 -fno-omit-frame-pointer`.
The x64 default is `X86_ISA=x86-64-v2`; `X86_ISA=x86-64` retains a baseline control.
Never infer a speed benefit from an ISA level or attribute v1/v2 differences to
assertion removal. Keep each comparison's toolchain and dependencies fixed.

`make print-build-paths BUILD_MODE=release` prints the canonical binary and
object/dependency directories as JSON. `OBJ_ROOT`, `DEP_ROOT`, and `BIN_ROOT` are
containers, with distinct policy/flavor leaves. Ordinary `make` publishes
`bin/champsim` atomically; named targets leave that alias alone. Use isolated
containers for candidates and preserve predecessor binaries before publishing.
Do not mix named and ordinary goals; `make BUILD_MODE=fast all test` selects fast
for both, with isolated test objects. Distinct policies may build concurrently;
identical-selection concurrent writers are unsupported. Never overlap builds or
provenance hashing with reported timing runs.

`<binary> --build-info` emits JSON without simulation construction. The paired
benchmark manifests query it once before timing and retain it beside the binary
SHA256. Historical binaries that lack the command are recorded as unavailable.
Compiler policy identity does not change statistics `meta.build_id`, which still
identifies simulated configuration. Existing statistics comparators are unchanged.
The legacy build path supports Python 3.10+; these performance tools retain their
Python 3.11+ requirement (`tomllib` and `hashlib.file_digest`).

Provenance selects and fingerprints the target-matched installed vcpkg dependencies.
External libraries' ISA requirements are explicitly unknown without independent
build receipts; successful startup is not portability certification. Hardware and
codec validation must be recorded per deployment. ARM/Darwin policy routing has
no implied runtime validation. Optional native Ramulator retains Release/C++20,
receives the selected ISA flags, and uses an immutable shared source-root manifest:
changed/unknown occupied roots require a fresh isolated clone.
