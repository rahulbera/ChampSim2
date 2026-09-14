# Ramulator2 regressions

These checks generate their own inputs. They do not need downloaded benchmark
traces, a second CPU model, or checked-in binary fixtures. Use Python 3.11 or
later with PyYAML, a C++20 compiler, and an already prepared native-enabled
ChampSim build. The currently validated native toolchain is Linux/GCC 13.

From the repository root, after the normal enabled build:

```sh
python3 -m unittest discover -s test/ramulator2 -p test_tools.py -v
python3 test/ramulator2/run_oracle.py \
  --native-root /path/to/ramulator2 \
  --build-dir .csconfig \
  --dependency-dir vcpkg_installed/x64-linux \
  --cxx /usr/bin/g++ \
  --output-dir /tmp/champsim-oracle-results
python3 test/ramulator2/run_integration.py \
  --binary bin/champsim \
  --output-dir /tmp/champsim-integration-results
```

Each output directory must be new. The scripts refuse to overwrite an existing
directory. They save received commands, stderr/stdout, statistics and a JSON
summary inside it. The oracle compiles only its two small runners, using the
existing `ramulator2_driver.o`, `runtime_config.o` and native library; it does
not invoke the native build helper or change native source. Use a one-core
ChampSim object build for the oracle: the direct native External frontend
reports one core. The simulator integration runner detects the binary's core
count and works with one-core and two-core binaries.

Serialize builds and executions that use a native source root: upstream writes
`libramulator.so` at that root even with out-of-tree CMake. For concurrent work,
use separate native checkouts/libraries. The oracle sets its subprocess library
search path to the supplied native root. The integration runner inherits the
caller environment, so a stable copied simulator/library pair can be selected
with `LD_LIBRARY_PATH` when other builds are running.

## Differential oracle

`direct_replay.cc` calls the native External frontend API directly and never
includes ChampSim driver code. `driver_replay.cc` uses the public C++17 driver
interface. Both consume `stream.csv`: 22 transactions with explicit address,
source, byte size, release tick, and parent/fragment identity. Small queue
capacities force real read/write backpressure and retries; duplicate writes
complete synchronously; seven read parents have two adjacent 32-byte fragments.
The same stream exercises native 64-byte DDR4 and 32-byte LPDDR5 transactions.
This tests the driver's split-transaction input stream. Adapter parent ownership
and admission splitting are separately covered by C++ test 705 and the complete
simulator integration run.

At tick N, each runner attempts all released, never-accepted rows in CSV order,
then advances one native tick if completion is still pending. Successful rows
are never resubmitted. Immediate callbacks precede their successful SEND record.
The native loop is bounded to 10,000 ticks and subprocesses to 120 seconds.

The comparator requires byte-identical attempt/result/callback streams, not just
matching totals. It checks real rejects of both types, exactly one acceptance
and callback per transaction, two synchronous callbacks, unchanged payload and
release ordering, and split-parent geometry. All native memory scalar leaves
are compared by individually preserved path components, type and exact value;
integer comparisons never round through floating point. Native memory YAML must
also match. The driver runner checks counters before/after finalize without an
extra drain. ConfigNode's earlier erasure of scalar types/float precision cannot
be reversed; the oracle compares its actual scalar strings to the driver's
reported typed interpretation.

`test_tools.py` exercises the comparator with changed callback timing, duplicate
acceptance, missing leaves, wrong types and a one-bit integer error above 2^53.
Those cases must fail even when aggregate request counts would still agree.

## Simulator integration

The runner creates a 32,768-record, 16 MiB uncompressed v2 trace, makes it read-only
and checks its SHA256 after simulation. Every fourth instruction stores; others
load over a 32 MiB virtual footprint. Small data caches force LLC reads and
writebacks. Each core warms 2,000 instructions and measures at least 10,000.
Every simulation subprocess is bounded to 180 seconds.

Cases cover DDR4, LPDDR5 fragmentation, two native channels, and independent
core/LLC/L2 frequency changes. Every case replays its own named TOML output and
compares all effective configuration and phase leaves, including NaNs. Checks
require real reads and writebacks, active native counters for every core,
normalized channel identities, correct native
fragment counts, and nonzero outstanding work at a retirement boundary in at
least one case. The preserved pending-work snapshot complements C++ test 705's
explicit finalization-without-ticking assertion. A YAML mutation at the same
pathname must cause a replay digest diagnostic; the YAML is restored afterward.

The generator can also be used independently:

```sh
python3 test/ramulator2/generate_trace.py \
  --output /tmp/local-memory.champsim2 --instructions 32768
bin/champsim --trace-version 2 --set dram-model=ramulator2 \
  --set ramulator2.config=configs/ramulator2/ddr4.yaml \
  -w 2000 -i 10000 --toml /tmp/local-memory.toml -- /tmp/local-memory.champsim2
```

The generator opens its output exclusively, so it cannot replace existing input.
Simulation commands always use a named output and `--` before trace paths.

## Sanitizer builds

`RAMULATOR2_SANITIZE=1` instruments the native library and ChampSim together
with AddressSanitizer and UndefinedBehaviorSanitizer (LeakSanitizer runs at
exit):

```sh
make -j6 WITH_RAMULATOR2=1 RAMULATOR2_ROOT=/path/to/ramulator2-sanitize \
  RAMULATOR2_SANITIZE=1 all test/bin/000-test-main
```

The build helper configures the pinned native checkout as `RelWithDebInfo`
with `-fsanitize=address,undefined -fno-omit-frame-pointer` for compilation
and the shared-library link; fmt and yaml-cpp inherit those flags through
FetchContent. The Makefile adds the same options, plus `-g`, to every host
compile and link, for both `bin/champsim` and the test binary. Release builds
are unaffected when the variable is unset or 0. It is rejected without
`WITH_RAMULATOR2=1`: an instrumented host with an uninstrumented native library
would leave native allocations and callbacks unchecked.

Provenance records the mode. The native manifest's inputs carry the build
type, flags and sanitizers; the compiler stamp gains `"sanitizers"`; and the
build string in `meta.ramulator2.build` names the mode, for example
`RelWithDebInfo C++20 Python=OFF Sanitizers=address,undefined` instead of
`Release C++20 Python=OFF`. The runtime library check fingerprints whichever
library the helper built, so no manual step is involved.

Changing the variable in either direction changes the compiler stamp and the
manifest inputs, so the next build rebuilds the native library, every host
object and both executables; one object root never mixes instrumented and
uninstrumented objects. Switching back to a release build rebuilds the
byte-identical release library. `make -n`/`-q`/`-t` still execute nothing, and,
as for any change of flags, mode, compiler or root, they do not refresh the
stamps and so do not show that rebuild. Because the library is written into the
native source root, a sanitized build and a release build that must both exist
at once need separate native roots, and separate checkouts: the test binary's
path does not follow `OBJ_ROOT` or `BIN_ROOT`.

Run instrumented binaries with these options, using absolute suppression paths
so that subprocesses started elsewhere find them:

```sh
export ASAN_OPTIONS=halt_on_error=1:detect_leaks=1
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:suppressions=$PWD/test/ramulator2/sanitizers/ubsan.supp
export LSAN_OPTIONS=suppressions=$PWD/test/ramulator2/sanitizers/lsan.supp
CHAMPSIM_EXPECT_RAMULATOR2=1 test/bin/000-test-main
```

The two suppression files cover only the known, pre-existing issues in
vendored ITTAGE (`inc/ittage/ittage.hpp`): the shift in `MYRANDOM()` and the
tables it never frees. They must not name ChampSim integration code or native
Ramulator sources. LeakSanitizer lists the suppressions it used at exit; any
other report is a failure.

The pinned native revision has one such report of its own. With a controller
whose `addr_mapper` is `RITAddrMapper`, `RITAddrMapper::create_base_mapper()`
creates the nested mapper through `Factory::create_implementation` and never
adds it as a child, so LeakSanitizer reports it (about 500 bytes in five
allocations per controller) at exit. Test 704's case for admitted row-indirection
mappers constructs two, so an instrumented `test/bin/000-test-main` exits
non-zero after every test has passed. It is deliberately not suppressed.

C++ test 708 exercises repeated driver and adapter construction, and teardown
with live native requests, parent contexts and callbacks, with and without
`finalize()`; it runs in every enabled build and is meant to be run
instrumented. vcpkg's static libraries and libstdc++ are not instrumented.

The instrumented library is much larger than the release one, and every driver
construction fingerprints the loaded library, so tests that construct many
drivers are noticeably slower in this mode.

## Long native pauses and the no-progress guard

`ddr4_nbl16384.py` is the DDR4 fixture with the burst length raised to 16384
cycles; `ddr4_nbl16384.yaml` is its export from the pinned native revision
(export it as the fixtures above are). A closed-row read then takes
nRCD + nCL + nBL = 16416 cycles, 13.7 us at 833 ps, which is longer than the
10 us that native mode's default `sim.deadlock_cycle` allows. The burst length
is not a JEDEC timing.

`test/python/test_ramulator2_cli.py` rewrites a generated trace so that the
measured phase issues one page walk and read, and requires that the default
guard aborts the run, that an explicit guard of four read latencies in the
machine's ticks lets it finish, and that a guard a hundred times larger
produces identical statistics. It runs in a few seconds and skips without a
native build.

The C++ test `707-ramulator2-clocks.cc` checks the global clock itself: an
independent scheduling reference predicts the tick and time of every operation,
native tick, submission, completion and response under dividing, non-dividing,
equal and extreme core/cache/native period ratios, across empty,
one-instruction and later warmup phases, with a fake driver and with the real
DDR4 and LPDDR5 drivers.
