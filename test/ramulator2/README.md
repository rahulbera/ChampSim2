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
