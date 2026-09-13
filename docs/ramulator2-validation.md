# Ramulator2 integration validation

This record covers the optional backend added on `feat/ramulator`. The default
remains legacy; the same enabled executable can select either backend at runtime.
The completed task reviews approved the driver/build boundary, request adapter,
native default guard, reporting/replay, and CLI input protection. Final portable
Task 5 tooling and whole-branch checks are listed separately below; they are not
claimed complete merely because earlier task suites passed.

## Reproduce locally

Use the normal vcpkg dependencies and `./config.sh`, plus Linux/GCC 13, CMake and
Python 3.11+ with PyYAML. Clone Ramulator into a separate checkout and retain it
for runtime shared-library loading:

```bash
git clone https://github.com/CMU-SAFARI/ramulator2.git ../ramulator2
git -C ../ramulator2 checkout 72427a1bba3771564c4fb0e494ba02242fd1eaa7
python3 -m pip install PyYAML
./config.sh
env -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u CFLAGS make -j6 CXX=/usr/bin/g++-13 \
  WITH_RAMULATOR2=1 RAMULATOR2_ROOT="$(realpath ../ramulator2)" all test
make pytest
python3 -m unittest discover -s test/ramulator2 -p test_tools.py -v
python3 test/ramulator2/run_oracle.py --native-root "$(realpath ../ramulator2)" \
  --build-dir .csconfig --dependency-dir vcpkg_installed/x64-linux \
  --output-dir /tmp/champsim-native-oracle --cxx /usr/bin/g++-13
python3 test/ramulator2/run_integration.py --binary bin/champsim \
  --output-dir /tmp/champsim-native-integration
```

Use new, dedicated output directories. The portable tools generate bounded local
inputs; they do not depend on private benchmark paths. The oracle compiles small
runners against the already-built driver/runtime configuration objects and native
library. The integration runner discovers compiled core count and exercises
DDR4/LPDDR5, multiple channels, clock changes, and result replay. A standalone
trace generator is also available:

```bash
python3 test/ramulator2/generate_trace.py --output /tmp/local.champsim2 --instructions 32768
bin/champsim --config configs/ramulator2.toml --trace-version 2 -w 0 -i 10000 \
  --toml /tmp/native-run.toml -- /tmp/local.champsim2
bin/champsim --config /tmp/native-run.toml --trace-version 2 -w 0 -i 10000 \
  --toml /tmp/native-replay.toml -- /tmp/local.champsim2
```

For legacy-only mode, `make WITH_RAMULATOR2=0` drops the dependency; pass the same
mode when rebuilding tests. Build mode/compiler/root stamps handle invalidation.
Do not build or run concurrently against a native root that another build may
replace: Ramulator emits `libramulator.so` into its source directory even when
CMake's build directory is elsewhere. Independent roots are required for such
parallel work. Named `--toml FILE -- TRACE...` commands keep output and input
arguments unambiguous. Existing direct/canonical/symlink/hardlink trace aliases
are rejected before destructive output opens.

## Native boundary and reproducibility

The native source revision is
`72427a1bba3771564c4fb0e494ba02242fd1eaa7` (Ramulator 2.1). Validation used Ubuntu
GCC 13.3.0 and CMake 3.28.3. The helper verifies fmt
`e69e5f977d458f2650bb346dadf2ad30c5320281` (10.2.1) and yaml-cpp
`56e3bb550c91fd7005566f19c079cb7a503223cf` (0.9.0), with a pure C++ Release build
and `RAMULATOR_PYTHON_BINDINGS=OFF`. Python reads source metadata at build time and
exports fully expanded YAML; no Python extension or embedded interpreter is used
at simulation time. Native includes/C++20 are confined to the private driver;
public interfaces, the adapter and legacy mode remain C++17.

The helper checks effective host ABI settings, including response/forced-include
files and public config/request/base/spec type layouts and identities. It rejects
incompatible ABI settings instead of silently mixing them with a default native
library. Source, compiler, mode, root and dependency identity are recorded; stale
or substituted libraries are rebuilt or rejected. Runtime verifies the loaded
shared object against recorded library provenance. The regression checks include
mode/root changes and dependency edges in `make -n`, `-q` and `-t` without remake
execution. These checks establish the tested Linux/GCC configuration, not every
compiler/platform combination in the legacy CI matrix.

Runtime input must be fully expanded YAML with External, GenericDRAM and
CacheLineInterleave, with homogeneous controller capacity/period/transaction size.
The driver uses exact integer picoseconds and payload-aware capacity, including
native payload overrides. It validates source IDs, signed native address range,
capacity/page reservations, and mapper shifts before using the native graph.
There is one native graph and one memory operable; discovery creates no dummy
legacy controller or second native simulator. The LLC feeder remains shared and
unbounded. Native mode rejects explicit `pmem.*`; legacy rejects `ramulator2.*`.

The shipped DDR4 preset has 64-byte transactions, 833 ps tCK and 8 GiB capacity;
LPDDR5 has 32-byte transactions, 1,453 ps tCK and 1 GiB capacity. Their readable
Python sources and exported YAML are under `configs/ramulator2/`. YAML paths are
relative to the process working directory. Output records the canonical absolute
path while retaining original overrides. Schema 2 stores exact YAML, content hash,
pinned native revision, library identity and compiler/build information.
`ramulator2.config_hash` contributes to the existing effective-configuration
fingerprint (the project's historical FNV convention is preserved). Replaying
`--config run.toml` re-reads current YAML and rejects hash/revision mismatches;
it does not automatically restore the YAML archive or command-line run lengths.
The hashes are reproducibility identifiers, not cryptographic attestations.

## Timing and counter semantics

Fast warmup bypasses fresh demand submissions while native clocks and refresh
continue. Phase changes reset event counters without discarding native queues,
clocks, partial feeder heads or callback contexts. Native parent acceptance and
read latency start at the first accepted fragment. A partially admitted parent
is outstanding even if its accepted fragments have already completed; a head
with no accepted fragment is not yet outstanding. Responses wait for every
fragment and retain original metadata. Rejected fragments retry in FIFO order;
accepted fragments are never repeated. Synchronous native callbacks are deferred
until acceptance bookkeeping is safe.

Schema 2 uses `phase.<name>.<roi|sim>.ramulator2.adapter` and `.native`, plus raw
`native_yaml`. Counts are independent 64-bit events; outstanding fields are live
gauges. `total_read_latency_ps` is a sum in picoseconds and `read_latency_samples`
is its denominator, including response-suppressed and carry-over reads. Carry-over
completions can exceed current-phase acceptances. Native counters retain their
native definitions/units; write completion means command issue/coalescing, not a
drained data bus. Each finishing CPU captures an owned shared-memory ROI snapshot;
the last finisher supplies the final shared snapshot. Finalization runs once after
all phases without extra drain ticks, so outstanding work can remain at retirement.

Native doubles remain unrounded, paths are escaped per component, and native
controller identities become `channel0`, `channel1`, etc. TOML integers fit signed
64-bit range; larger unsigned values use exact decimal strings with a schema
comment. JSON retains uint64 integers and represents nonfinite native values as
`nan`/`inf`/`-inf` strings. Legacy output remains schema 1 with existing formatter
bytes and legacy units, without native tables or fabricated native-era bus zeros.

The legacy no-progress default remains 500 global ticks. An initial valid DDR4
refresh stalled demand for 421 × 833 ps = 350,693 ps, exceeding the old 500 × 250 ps
= 125,000 ps guard. Commit `c6e7997b` keeps real request-based progress and selects
`max(500, ceil(10 microseconds / minimum actual positive operable period))` only
when native mode omits `sim.deadlock_cycle`. The default is 40,000 at 250 ps and
60,241 at 166 ps. Explicit positive overrides remain unchanged. Custom devices
with stalls longer than 10 µs may require an explicit larger threshold.

## Verified evidence

Artifacts were retained under `/tmp/champsim-ramulator-validation`; they include
full argv, logs, TOML documents, snapshot observers and comparisons. Generated
results are not committed. Historical comparison baseline:
`79cb5fdb2abd7e754b529eb235c6a69f61e45ea8`.

| Check | Recorded result | Scope |
| --- | --- | --- |
| Final production enabled C++ | 17,414 assertions; 859 passed, one intentional skip | `b5addb74` |
| Final production disabled C++ | 16,988 assertions; 853 passed, seven native skips | `b5addb74` |
| Final production Python | Enabled 61 passed; disabled 57 passed, four native skips | `b5addb74` |
| Exported CI environment Python | 61 tests passed | Native mode/root/compiler inherited as environment variables, matching CI |
| Adapter deterministic + ASan/UBSan/leaks | 195 assertions / 16 cases, both pass | Task 3 core; callbacks, retries, epochs, teardown |
| Native default guard | Four CLI cases and 195 adapter assertions pass; protected cold DDR4/LPDDR5 retire 10k | `c6e7997b`; explicit 500 reproduces premature abort |
| Direct native versus driver prototype | Identical attempt/callback streams and all 44 DDR4 / 48 LPDDR5 typed memory leaves | 22 requests; 1,043 / 683 real rejections; two synchronous writes each |
| Final one-core legacy comparison | Six comparisons: SQLite warmed/cold and omnetpp warmed × omitted/explicit; all 237 phase / 177 prior config leaves match | `b5addb74` |
| Final two-core legacy comparison | Four comparisons: mixed warmed/cold × omitted/explicit; all 695 phase / 314 prior config leaves match | `b5addb74` |
| Real two-core environment | 732 assertions / 14 cases | `c6e7997b`; source 0/1 served by DDR4 and LPDDR5 |
| Native reporting run/replay | DDR4 566 phase / 164 config leaves; LPDDR5 574 / 164, all equal including NaNs | Task 4, explicit guard 10,000; 586 reads, 586 / 1,172 fragments |
| Native two-core reporting replay | DDR4 745 phase / 301 config leaves; LPDDR5 749 / 301, all equal including NaNs | Final `b5addb74`, default guard 40,000 |
| Legacy reporting replay | 472 phase / 178 config leaves equal; 235 ROI leaves match cold reference | Task 4 enabled binary, schema 1 |
| CLI input preservation | Two tests / six meaningful subcases pass | Ambiguous output arguments and filesystem trace aliases |

Supplied v2 SQLite/omnetpp trace results at `c6e7997b`, with 100,000 warmup
instructions and effective native guard 40,000:

| Run | Core | ROI instructions | ROI cycles |
| --- | --- | ---: | ---: |
| One-core SQLite DDR4 | CPU0 | 500,000 | 835,214 |
| One-core SQLite LPDDR5 | CPU0 | 500,000 | 1,082,629 |
| Two-core mixed DDR4 | CPU0 SQLite | 100,001 | 176,371 |
| Two-core mixed DDR4 | CPU1 omnetpp | 100,003 | 130,083 |
| Two-core mixed LPDDR5 | CPU0 SQLite | 100,001 | 233,026 |
| Two-core mixed LPDDR5 | CPU1 omnetpp | 100,001 | 133,252 |

The final integrated two-core runs at `b5addb74` retain those native instruction/
cycle counts and replay all config/phase leaves exactly. CPU1 native row counts
are DDR4 conflicts=6/hits=22/misses=6 and LPDDR5 conflicts=7/hits=67/misses=10.
DDR4 accepts/completes 814 reads and 814 fragments; LPDDR5 822 reads and 1,644
fragments. These supplied windows have zero writebacks and zero outstanding work
at retirement; they do not establish retry, synchronous write or nondraining
behavior by themselves. Earlier deterministic adapter and direct-native replay
checks cover those distinctions. Phase/counter snapshots precede finalization.

Final legacy parity covers ten comparisons with the baseline at both one and two
cores, warmed (`-w 100000 -i 500000`) and cold (`-w 0 -i 10000`). Every numeric
phase leaf matches exactly, with NaN-aware comparisons. Only verified protected
trace path spellings are normalized; no numeric value is normalized. The sole
new effective configuration leaf is `dram-model = "legacy"`, producing the
expected historical fingerprint changes: one-core `642614b070025965` to
`48f0cac4a69838dc`, two-core `a0ee15fb39a729b3` to `379b788d75c38566`. Omitted and
explicit legacy share the same effective config/hash. Command-line metadata
records the changed executable/output/input names and explicit selector. Detailed
final evidence is in `task5-validation/report.md`, `legacy-{1,2}core-results.json`
and `native-twocore-{ddr4,lpddr5}*-comparison.json` under the artifact directory. Replay checks changed YAML
at the same pathname, required a digest diagnostic, and restored scratch input.
Named TOML and native `--knobs` parse; the unnamed stdout TOML tail parses. Missing
output directories and `/dev/full` return nonzero. Full-suite VMEM capacity warnings
are existing diagnostic noise, not pristine logs or unexplained test failures.

Still pending when this record was written: portable Task 5 runner verification,
multi-channel/frequency generated-input comparisons, standalone harness checks,
CI execution on hosted runners, and whole-branch review. Earlier passing
checks above do not substitute for these. The responsible validators will update
this section when their evidence is available.

## Validation input incident and recovery

A scratch Task 4 command placed the original SQLite trace after optional
`--toml=`. CLI11 consumed it as the output filename, and the old output probe
truncated the trace before validating positional inputs. The user was informed;
subsequent empty-input segfaults were excluded from native regression evidence.
No other trace was changed.

`708.sqlite_r.sp0.champsim2.zst` was recovered from the kratos2 mirror into scratch,
verified against the pre-existing catalog, and atomically restored after confirming
the damaged local file was still empty. Restored size: **327,181,771 bytes**.
SHA256: **`9655873004cefe5a5d2d8f501dbf3a6c5503bc0f83782166354b1e8ba4262556`**.
`zstd -t` passed with **153,600,000,000 decoded bytes**. Post-recovery simulation
checks reverified both canonical and protected scratch hashes. Evidence:
`task4-recovery.json`, `task4-recovery-zstd.log`, and the native protected-trace
manifest. The recovery was exact, not a replacement trace of similar provenance.

The remedial CLI commit (`1d741c50` in main, isolated `ee18d4cc`) validates trace
count before output opening, skips output probes for `--knobs`, and rejects
filesystem aliases of traces. All resumed validation uses named outputs, `--`,
protected scratch copies and checksum guards. This prevents ordinary accidental
aliasing; it does not claim protection from concurrent malicious path renames.

## Archived implementation rulings

The following decision lines are preserved verbatim from the implementation
ledger so their rationale and tradeoffs survive removal of the scratch workspace.

> Ruling: Use the existing dedicated feat/ramulator checkout for implementation, with detached baseline reference worktrees — user requested autonomous implementation in this project and the branch is already dedicated — if unwanted, the commits can be moved to another branch.

> Ruling: Reuse named agent threads with explicit bounded task briefs when fresh spawns hit the four-thread harness limit — no free fresh thread is available — reduced context isolation is offset by independent task diff reviews.

> Ruling: Invoke packaged skill scripts with bash and explicit output paths — installed scripts lack executable bits — no project behavior changes.

> Ruling: Count a parent accepted and start read latency at its first accepted native fragment; outstanding gauges include partially admitted parents — this gives meaningful partial-admission statistics while phase resets retain live requests — choosing full admission instead would change reported counters/latency, not request delivery.

> Ruling: Add default optional config_record() to memory_backend in Task3 and consume it once in Task4 run metadata — exact input YAML otherwise stays inaccessible inside private driver; avoids collecting stats after finalization or copying YAML into each snapshot — costs one additive interface method if a different metadata abstraction is preferred.

> Ruling: Implement independent Task3 adapter core in detached /tmp/champsim-ramulator-adapter against frozen C++17 driver contract while Task2 builds native support — developer parallelism guidance and disjoint checkouts permit useful work without shared edits; finish wiring/native validation only after Task2 review — costs adapter rework if driver contract changes.

> Ruling: Extend isolated Task3 work to factory/availability/diagnostic wiring while Task2 build fixes run; retain native validation/import gate — ABI/Make fixes do not change the frozen driver contract and this avoids idle work without main-tree races — costs rework if a driver API change emerges.

> Ruling: Prepare independent Task4 reporting in detached /tmp/champsim-ramulator-reporting built from73f317fc plus Task3 core/wiring while Task2 build fixes run — snapshot interfaces are fixed and default-ABI behavior is already validated; final main integration remains gated on prior reviews — costs reporting rework if those interfaces change.

> Ruling: Serialize native uint64 values above INT64_MAX as exact decimal strings in schema2, with an explanatory comment; ordinary counters remain integers and native doubles retain precision — TOML signed64 parsers cannot replay larger integer literals losslessly — consumers must parse extremely large counter strings explicitly. JSON retains uint64 and represents nonfinite native doubles as named strings.

> Ruling: Keep native progress tied to accepted/completed requests and preserve legacy default500 and every explicit sim.deadlock_cycle; when omitted for Ramulator derive max(500,ceil(10microseconds/minimum actual positive operable period)) after the single environment construction — native refresh can legitimately exceed the legacy idle window — custom devices with longer pauses still need an explicit override. Reject nonpositive native operable periods.

> Ruling: Add narrow CLI input protection in Task4: validate required traces before opening output (except knobs-only); reject lexical/canonical/symlink/hardlink output aliases of inputs; regress ambiguous optional-output invocations against scratch files — concrete data loss demonstrated missing CLI safety — this slightly extends reporting scope and prevents recurrence. All validation now uses explicit named outputs, -- separators, protected scratch copies and checksum guards.

> Ruling: Prepare portable Task5 differential replay/generator tooling independently in reporting worktree afterd6f89e8b while Task4 review and Task3 validation run — frozen native driver contract and separate native root allow useful disjoint work — tooling may need rework if review changes the contract; final Task5 integration stays gated on prior tasks.
