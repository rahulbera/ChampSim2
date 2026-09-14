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
