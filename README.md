# ChampSim

![GitHub](https://img.shields.io/github/license/ChampSim/ChampSim)
![GitHub Workflow Status](https://img.shields.io/github/actions/workflow/status/ChampSim/ChampSim/test.yml)
![GitHub forks](https://img.shields.io/github/forks/ChampSim/ChampSim)
[![Coverage Status](https://coveralls.io/repos/github/ChampSim/ChampSim/badge.svg?branch=develop)](https://coveralls.io/github/ChampSim/ChampSim?branch=develop)

ChampSim is a trace-based simulator for a microarchitecture study. If you have questions about how to use ChampSim, we encourage you to search the threads in the Discussions tab or start your own thread. If you are aware of a bug or have a feature request, open a new Issue.

# Using ChampSim

ChampSim is the result of academic research. If you use this software in your work, please cite it using the following reference:

    Gober, N., Chacon, G., Wang, L., Gratz, P. V., Jimenez, D. A., Teran, E., Pugsley, S., & Kim, J. (2022). The Championship Simulator: Architectural Simulation for Education and Competition. https://doi.org/10.48550/arXiv.2210.14324

If you use ChampSim in your work, you may submit a pull request modifying `PUBLICATIONS_USING_CHAMPSIM.bib` to have it featured in [the documentation](https://champsim.github.io/ChampSim/master/Publications-using-champsim.html).

# Download dependencies

ChampSim uses [vcpkg](https://vcpkg.io) to manage its dependencies. In this repository, vcpkg is included as a submodule. You can download the dependencies with
```
git submodule update --init
vcpkg/bootstrap-vcpkg.sh
vcpkg/vcpkg install
```

# Configure and build

`config.sh` **discovers the modules** present on disk -- which is all it does --
and emits the registry naming them plus the makefile fragment listing their
objects. Run it once per checkout, and again whenever a module is added, renamed, or
removed -- including when git removes one for you, which is what checking out a branch
with a different module set does.

```
$ ./config.sh
$ make
```

There is no JSON configuration script. The simulated machine's *shape* -- which
caches exist and how they are wired -- is C++ in `src/static_environment.cc`,
and `NUM_CPUS`, `BLOCK_SIZE` and `PAGE_SIZE` are in `inc/defs.h`. Changing
either is a code edit and a rebuild; multi-core is a separate binary by design.

# Download SPEC CPU 2026 traces

32 SimPoint slices of 14 SPEC CPU 2026 workloads, traced in the v2 record
format, are hosted in a public Cloudflare R2 bucket at
`https://traces.rbera.com/champsim2/spec26/`, taking 38 GiB in compressed form.
Each slice is 300 million instructions long. Run them with `--trace-version 2`.

The bucket cannot be listed by opening its base URL in a browser. Instead,
[`manifest.txt`](https://traces.rbera.com/champsim2/spec26/manifest.txt) is the
index of the dataset: it lists every file with its path, size, last-modified
time, upload etag and SHA-256. Downloading needs no prior knowledge of the
layout -- the manifest supplies it.

To download a single trace, append its `path` column to the base URL:

```
$ wget https://traces.rbera.com/champsim2/spec26/708.sqlite_r.sp0.champsim2.zst
$ bin/champsim --trace-version 2 -w 50000000 -i 200000000 -- 708.sqlite_r.sp0.champsim2.zst
```

To download everything, turn the manifest into a URL list:

```bash
BASE=https://traces.rbera.com/champsim2/spec26

# 1. Fetch the index
curl -sO $BASE/manifest.txt

# 2. Build a URL list (skip the '#' preamble and the header row)
grep -v '^#' manifest.txt | tail -n +2 | cut -f1 | sed "s|^|$BASE/|" > urls.txt

# 3. Download. --cut-dirs=2 drops the leading champsim2/spec26/, so traces land
#    in the current directory and SimPoint files in simpoints/. -c resumes an
#    interrupted run, so re-running the same command picks up where it left off.
wget -x -nH --cut-dirs=2 -c -i urls.txt

# 4. Check every file against its SHA-256
sha256sum -c SHA256SUMS
```

To fetch one workload, filter the `path` column first -- e.g. the three
`708.sqlite_r` slices and their SimPoint file:

```bash
grep -v '^#' manifest.txt | tail -n +2 | cut -f1 | grep -E '^(simpoints/)?708\.sqlite_r\.' \
  | sed "s|^|$BASE/|" > sqlite.txt
wget -x -nH --cut-dirs=2 -c -i sqlite.txt
```

The workloads are `706.stockfish_r`, `707.ntest_r`, `708.sqlite_r`,
`710.omnetpp_r`, `714.cpython_r`, `721.gcc_r`, `723.llvm_r`, `727.cppcheck_r`,
`729.abc_r`, `734.vpr_r`, `735.gem5_r`, `750.sealcrypto_r`, `753.ns3_r` and
`777.zstd_r`. A trace is named `<workload>.sp<N>.champsim2.zst`, where `N` is its
SimPoint cluster. Each workload was clustered into at most three clusters of
300M-instruction intervals, and clusters covering less than 5% of the execution
were dropped, which is why a workload has one to three slices.

**SimPoint weights live in `simpoints/<workload>.simpoints.json`, not in the
file name.** Each kept slice carries its `weight` and where it starts
(`skip_instructions`); `dropped` lists the clusters left out, and `kept_weight`
is the sum of the kept weights -- between 0.92 and 1.0, since dropped weight is
not redistributed. Aggregate in two levels: combine a workload's slices with
weights `weight / kept_weight` (as a weighted mean of a per-instruction metric
such as CPI or MPKI), then average across workloads unweighted. Never average
all slices flat: that counts a workload with three slices three times as much as
one with a single slice.

# How to create traces

Program traces are available in a variety of locations, however, many ChampSim users wish to trace their own programs for research purposes.
Example tracing utilities are provided in the `tracer/` directory.

# Run

Cache geometry, core widths, latencies, the DRAM backend, and branch predictor,
BTB, prefetcher and replacement choices are set at **run time** from a TOML file:

```
$ bin/champsim --config configs/sample.toml -w 20000000 -i 50000000 trace.champsimtrace.xz
```

`--config <file>` and `--set key=value` are both repeatable and apply strictly
in command-line order, so the last definition of a key wins whichever source it
came from:

```
$ bin/champsim --config configs/lnc.toml --set ooo_cpu.cpu0.btb=ittage_64kb trace.champsimtrace.xz
```

Keys name a component and a parameter (`ooo_cpu.cpu0.rob_size`,
`cache.cpu0_l1d.sets`, `pmem.tcas`). Four of them select modules by directory
name -- `branch_predictor`, `btb`, `prefetcher`, `replacement` -- so every
compiled module is reachable without rebuilding. A key nothing consumes is a
fatal error at startup, never a silent no-op.

`--knobs` lists every key a binary accepts with the value the current
invocation would use. Its output is a valid TOML document, so it also gives you
a complete starting configuration:

```
$ bin/champsim --knobs > my.toml
```

`configs/sample.toml` is a commented example; `configs/lnc.toml` models Intel's
Lion Cove, tagging each value as disclosed, derived, or default. `lnc.toml` covers
the core and caches only and sets no memory key, so it pairs with whichever DRAM
model you want:

```
$ bin/champsim --config configs/lnc.toml --config configs/dram-legacy.toml -- trace.xz
$ bin/champsim --config configs/lnc.toml --config configs/ramulator2.toml    -- trace.xz
```

Used alone it runs the default legacy DRAM rather than the LPDDR5X-8533 data rate.

Warmup (`-w`) and simulation (`-i`) counts are **instructions retired**, and the
reported statistics cover the simulation phase only. Traces may be plain or
`.xz`/`.gz`/`.bz2`/`.zst` compressed. Two record formats exist and neither is
self-describing, so the version is asserted on the command line -- v1 (64-byte)
is the default, v2 (512-byte, with physical addresses and explicit branch types)
needs `--trace-version 2`.

# Evaluate simulation

ChampSim measures IPC (Instructions Per Cycle) as its performance metric, and
prints a plain-text report to stdout with many more statistics.

`--toml <file>` additionally writes a machine-readable statistics document.
Besides the measurements it records *what produced them*: `[meta]` carries the
command line, the trace version and a content hash of the machine, and
`[config]` is the effective configuration -- every parameter the run actually
consulted, with the value it used. The file is checked at startup and written
in place only after the run succeeds; an existing file that is neither empty
nor a statistics document is refused rather than overwritten. That makes a result
file replayable:

```
$ bin/champsim --toml run.toml -- trace.champsimtrace.xz
$ bin/champsim --config run.toml -- trace.champsimtrace.xz   # same machine, same numbers
```

# Optional Ramulator2 memory

The default is `dram-model = "legacy"`, including in a native-enabled binary.
Legacy-only builds need no Ramulator dependency. The optional backend is verified
on Linux with GCC 13 and the pinned Ramulator 2.1 source revision below:

```bash
git clone https://github.com/CMU-SAFARI/ramulator2.git ../ramulator2
git -C ../ramulator2 checkout 72427a1bba3771564c4fb0e494ba02242fd1eaa7
# Install CMake, GCC 13, Python 3.11+ and PyYAML in your development environment.
python3 -m pip install PyYAML
./config.sh
env -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u CFLAGS make -j6 CXX=/usr/bin/g++-13 \
  WITH_RAMULATOR2=1 RAMULATOR2_ROOT="$(realpath ../ramulator2)"
bin/champsim --config configs/ramulator2.toml --trace-version 2 \
  -w 100000 -i 500000 --toml run.toml -- trace.champsim2.zst
```

The build helper prepares a pure C++ shared library with Python bindings off,
checks the pinned source/dependencies and host ABI, and records library provenance.
Only the private native driver translation unit uses C++20; the simulator's public
interfaces and other sources stay C++17. Python reads source metadata during build
and exports configuration; it is not embedded in simulation. Keep the native
checkout/library available at runtime. Serialize builds and runs sharing a native
root: Ramulator writes `libramulator.so` into its source root even with an
out-of-tree build. `make WITH_RAMULATOR2=0` returns to a legacy-only build.

`configs/ramulator2.toml` selects the exported DDR4 YAML; change
`ramulator2.config` to `configs/ramulator2/lpddr5.yaml` for LPDDR5. Paths are relative
to the process working directory. Native geometry/timing/policies come from the
fully expanded YAML. Native mode rejects every explicit `pmem.*` key, and legacy
mode rejects every `ramulator2.*` key. Start native configuration from this example
or native `--knobs`, rather than overlaying a complete legacy configuration.

Native results use schema 2, with separate adapter/native counters and exact YAML,
hash, revision and build metadata. Replay checks the current YAML contents against
the recorded hash. Fast warmup bypasses demand requests while native clocks tick;
phase changes preserve pending requests, and finalization adds no drain cycles.
The default native no-progress allowance is 10 µs in actual simulator ticks;
explicit `sim.deadlock_cycle` values remain authoritative, with a stderr warning
when one allows less than 10 µs. Legacy `--knobs` dumps and statistics documents
record the value they used (500 by default), so remove the key along with
`pmem.*` when converting one. See
[configuration examples](configs/README.md) and the
[validation record](docs/ramulator-integration/ramulator2-validation.md) for counter units, transaction
sizes, reproducibility limits, and the completed evidence.

The [integration writeup](docs/ramulator-integration/ramulator2-integration.md) explains the design,
test coverage, known limits, and recommended stress tests before a mainline merge.

# Test

```
$ make test      # the Catch2 suite
$ make pytest    # the Python module-discovery tests
```

Good luck and be a champion! <br>
