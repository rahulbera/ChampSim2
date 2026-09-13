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
Lion Cove, tagging each value as disclosed, derived, or default.

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
consulted, with the value it used. That makes a result file replayable:

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
explicit `sim.deadlock_cycle` values remain authoritative. See
[configuration examples](configs/README.md) and the
[validation record](docs/ramulator2-validation.md) for counter units, transaction
sizes, reproducibility limits, and the completed evidence.

The [integration writeup](docs/ramulator2-integration.md) explains the design,
test coverage, known limits, and recommended stress tests before a mainline merge.

# Test

```
$ make test      # the Catch2 suite
$ make pytest    # the Python module-discovery tests
```

Good luck and be a champion! <br>
