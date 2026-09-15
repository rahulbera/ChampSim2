# Ramulator2 integration validation

This record covers the optional backend added on `feat/ramulator`. The default
remains legacy; the same enabled executable can select either backend at runtime.
The completed task reviews approved the driver/build boundary, request adapter,
native default guard, reporting/replay, and CLI input protection. Portable Task 5
oracle and one-/two-core integration checks and the final branch review also
passed. An independent review of `74159f1e` and a pre-merge close-out followed;
their evidence is in [Review and close-out evidence](#review-and-close-out-evidence).
Hosted CI execution remains pending.

For the architecture, interpretation of this evidence, and proposed stress tests
before mainline, read the [integration writeup](ramulator2-integration.md).

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
arguments unambiguous. Direct/canonical/symlink/hardlink trace aliases of the
output, and an existing non-empty regular file that is not a statistics document,
are rejected at startup; nothing is written to the output until the run succeeds.

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
with stalls longer than 10 µs may require an explicit larger threshold. Because
legacy `--knobs` dumps and statistics documents record the `sim.deadlock_cycle`
their run used (500 unless set), a native run with an explicit value below the
10 µs allowance now warns on stderr.

## Verified evidence

Artifacts were retained under `/tmp/champsim-ramulator-validation`; they include
full argv, logs, TOML documents, snapshot observers and comparisons. Generated
results are not committed, and that directory is local temporary storage, not an
archive. Historical comparison baseline:
`79cb5fdb2abd7e754b529eb235c6a69f61e45ea8`.

| Check | Recorded result | Scope |
| --- | --- | --- |
| Final production enabled C++ | 17,414 assertions; 859 passed, one intentional skip | `b5addb74` |
| Final production disabled C++ | 16,988 assertions; 853 passed, seven native skips | `b5addb74` |
| Final production Python | Enabled: all 61 methods passed. Disabled: 58 passed and three native-only methods skipped, which unittest summarizes as `Ran 61 tests ... OK (skipped=4)` because one of them skips two subtests (not 57 passed and four skips) | `b5addb74`, configured tree with `bin/champsim` built |
| Native-job environment Python | 61 tests passed | Native mode/root/compiler exported as the `native` job exports them, in a configured tree with an enabled `bin/champsim`. This is not the hosted `python` job, which never runs `config.sh` or builds |
| Python after the fresh-checkout test fix | Unconfigured, unbuilt checkout: `Ran 61 tests ... OK (skipped=6)`, 55 passed and the six methods that need `bin/champsim` skipped. Configured disabled tree with `bin/champsim`: `OK (skipped=4)`, 58 passed, three native-only methods skipped. Configured enabled tree with an enabled `bin/champsim` and the native mode/root/compiler exported: `Ran 61 tests ... OK`, all 61 passed | Test-only change on `74159f1e`; local Linux runs, not hosted CI. The unconfigured run used the `python` job's discovery command under GNU Make 4.3 and 4.4.1, and its `coverage run`/`coverage lcov` step in a virtual environment without PyYAML. Before the change, the same command failed one test on the missing `_configuration.mk` |
| Suites after the review fixes | Disabled C++: 868 cases, 859 passed, nine native skips, 17,211 assertions. Enabled C++: 868 cases, 867 passed, one intentional skip, 17,663 assertions. Python: configured disabled tree `Ran 73 tests ... OK (skipped=6)` (68 passed, five native-only methods skipped); configured enabled tree `Ran 73 tests ... OK`; unconfigured, unbuilt checkout with the `python` job's discovery command `Ran 73 tests ... OK (skipped=18)` (55 passed, 18 methods that need `bin/champsim` skipped). Native tools: 11 `test_tools.py` tests pass; the oracle again gives identical streams and 44 DDR4 / 48 LPDDR5 typed leaves; the four integration cases match replay in 164 config leaves and 568 / 576 / 648 / 568 phase leaves, two more per case than above because of the new `out_of_range_prefetches` roi/sim keys. The native job's non-PIE provenance step, run locally, builds an `EXEC` binary whose `--knobs` passes | The out-of-range prefetch, native component admission, loader library lookup, `--toml` replacement and guard warning fixes, with the Python test fix, applied to `74159f1e`. Local Linux runs with GCC 13.3, GNU Make 4.4.1 and Python 3.12.3 (no `coverage` step), against a private copy of the pinned native root; not hosted CI |
| Output comparison after the review fixes | `710.omnetpp_r.sp0` and `708.sqlite_r.sp0`, 200,000 warmup / 2,000,000 ROI instructions, pinned `hashed_perceptron`/`basic_btb`, `--toml-sim-stats`. Legacy output of the disabled and of the enabled build matches the `74159f1e` legacy binary in all 472 phase, 178 config and 2 override leaves, and in plain stdout apart from simulation time. Native DDR4 and LPDDR5 output matches the `74159f1e` native binary in all 566 / 574 common phase leaves, all 164 config and 4 override leaves, and `build_id`; so does LPDDR5 with `next_line` on L2C and LLC (574 phase, 164 config, 6 override leaves). The only other differences are `meta.command_line`, the two new `out_of_range_prefetches` leaves (both 0) and that key's one added plain-output line | Two short windows of supplied traces. No prefetch in them reached past capacity (the new counter is 0 in every run), so this shows the fixes leave these outputs unchanged, not that out-of-range handling is exercised |
| Suites after the second review | Disabled C++: 877 cases, 868 passed, nine native skips, 17,309 assertions. Enabled C++: 877 cases, 876 passed, one intentional skip, 17,766 assertions. Python: configured disabled tree `Ran 82 tests ... OK (skipped=6)` (77 passed, five native-only methods skipped); configured enabled tree with the native variables passed to `make pytest`, `Ran 82 tests ... OK`; unconfigured, unbuilt checkout with the `python` job's discovery command `Ran 82 tests ... OK (skipped=27)` (55 passed, 27 methods that need `bin/champsim` skipped). Native tools: 11 `test_tools.py` tests pass; the oracle gives identical streams and 44 DDR4 / 48 LPDDR5 typed leaves; the four integration cases match replay in 164 config leaves and 568 / 576 / 648 / 568 phase leaves | The second review's fixes applied to `d3604b51`: `--toml` resolution through the filesystem, in-place and standard-stream write modes and device probes (`test_output_paths.py`, now 18 methods, and `097-output-target.cc`), deadlock diagnostics that survive a throwing printer, reworded native component rejections, and one more 705 case. Local Linux runs with GCC 13.3 and GNU Make 4.4.1; Python 3.14.4 for `make pytest`, 3.12.3 for the unconfigured checkout and native tools; against a private copy of the pinned native root; not hosted CI |
| Deadlock diagnostics on a supplied trace | `706.stockfish_r.sp0`, `-w 0 -i 1000000`, stdout redirected to a file. Legacy with `sim.deadlock_cycle=3`: the `d3604b51` binary left 0 bytes of stdout, stderr ending in `vector::_M_range_check: __n (which is 155) >= this->size() (which is 128)`; after the fix, 37,124 bytes ending with the legacy DRAM channel's last `[WQ] entry` line. Native, `configs/ramulator2.toml` with `sim.deadlock_cycle=20`: 0 bytes before; 34,659 bytes after, ending with the `Ramulator2 at 13328 ps` block. Every run exits through `abort()` with status 134 | One supplied trace at two stall lengths; its checksum was unchanged. `test_ramulator2_cli.py` reproduces the mechanism with generated source register IDs 155 and 255 |
| `--toml` written in place after the third verification round | The rename-based replacement was removed. Every verification round had found something new the rename changed or lost that writing the file in place keeps: hard links, 234 to 255-byte names, read-only directories, a document's group and access ACL, permission windows, `fs.protected_regular`, stale temporaries. The third found that a new name in a directory with a default ACL got 0666 less the umask instead of the directory's policy, that a target written in place lost the document when it was removed during the run, and that a document without an ACL gained the default ACL's named entries. New `test_output_paths.py` tests failed on `0763f21f` for exactly those three (`0644` and `0600` where a new file there gets `0660`; exit 1 with "No such file or directory" for a hard-linked document; an inherited access ACL) and pass after the change. Suites after it: disabled C++ 877 cases, 868 passed, nine native skips, 17,317 assertions; enabled C++ 877 cases, 876 passed, one intentional skip, 17,778 assertions; Python `Ran 89 tests ... OK (skipped=6)` disabled and `Ran 89 tests ... OK` enabled, all 25 output-path methods passing in both. The full C++ suite with `-s` redirected to a file now ends with its summary | Two guarantees are given up: a failure during the final write itself, such as a full disk, can leave the target empty or partial (the error says so), and the write is not atomic for concurrent writers or readers of the same name; a failed startup still leaves it untouched. Local Linux runs with GCC 13.3 and Python 3.14.4, against a private copy of the pinned native root; not hosted CI |
| Adapter deterministic + ASan/UBSan/leaks | 195 assertions / 16 cases, both pass | Task 3 core; callbacks, retries, epochs, teardown |
| Native default guard | Four CLI cases and 195 adapter assertions pass; protected cold DDR4/LPDDR5 retire 10k | `c6e7997b`; explicit 500 reproduces premature abort |
| Portable direct native versus driver oracle | Identical attempt/callback streams and all 44 DDR4 / 48 LPDDR5 typed memory leaves | 22 transactions; 1,065 / 705 attempts; 1,043 / 683 rejects; 22 callbacks and seven split parents each |
| Portable one-core integration | All 164 config leaves and 566 / 574 / 646 / 566 phase leaves match replay; digest/default-guard checks pass | DDR4, LPDDR5, multichannel and independent frequency changes |
| Independent portable two-core integration | All 301 config leaves; 1,487 / 1,495 / 1,573 / 1,487 phase leaves match replay | Same four cases, actual CPU1 activity and both native channels verified |
| Final one-core legacy comparison | Six comparisons: SQLite warmed/cold and omnetpp warmed × omitted/explicit; all 237 phase / 177 prior config leaves match | `b5addb74` |
| Final two-core legacy comparison | Four comparisons: mixed warmed/cold × omitted/explicit; all 695 phase / 314 prior config leaves match | `b5addb74` |
| Real two-core environment | 732 assertions / 14 cases | `c6e7997b`; source 0/1 served by DDR4 and LPDDR5 |
| Native reporting run/replay | DDR4 566 phase / 164 config leaves; LPDDR5 574 / 164, all equal including NaNs | Task 4, explicit guard 10,000; 586 reads, 586 / 1,172 fragments |
| Native two-core reporting replay | DDR4 745 phase / 301 config leaves; LPDDR5 749 / 301, all equal including NaNs | Final `b5addb74`, default guard 40,000 |
| Legacy reporting replay | 472 phase / 178 config leaves equal; 235 ROI leaves match cold reference | Task 4 enabled binary, schema 1 |
| CLI input preservation | Two tests / six meaningful subcases pass | Ambiguous output arguments and filesystem trace aliases |
| Standalone harness builds | Forced C++17 rebuilds of blbp_tune, cbp6_replay and ittage_equiv all exit 0 | Project includes only; no native or vcpkg dependency |
| Published native TOML example | Actual `--knobs` construction/parsing passes; guard 40,000 and no `pmem` table | `configs/ramulator2.toml` |

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

Portable oracle evidence is retained in `task5-oracle-final/{summary,commands}.json`;
one-core generated integration in `task5-tooling-onecore/{summary,commands}.json`.
The one-core runs include writes, 12 LPDDR5 rejections, and outstanding parents
at retirement in all four cases.

Portable two-core generated-input evidence also demonstrates actual writebacks,
retries and live requests at retirement:

| Case | Accepted writes | Rejected submissions | Outstanding parents | Outstanding fragments |
| --- | ---: | ---: | ---: | ---: |
| DDR4 | 5,187 | 0 | 39 | 39 |
| LPDDR5 | 5,131 | 159,684 | 34 | 65 |
| Two native channels | 5,145 | 0 | 49 | 49 |
| Independent core/cache frequency change | 5,176 | 0 | 43 | 43 |

All four two-core runs and replays exit successfully. CPU1 native counters are
active, and the multichannel case has activity in channel0 and channel1. The
nonzero outstanding values demonstrate retirement without a post-run drain.
Generated trace SHA256 is
`bd30cd675aec8aa50a343481ecff24c8c9c20784861bcb3d7376a7f139c67516` and stays unchanged.
Evidence: `task5-validation/twocore-deterministic-integration/{summary,commands,independent-cpu1-checks}.json`.

Standalone compatibility was checked with forced builds, not up-to-date no-ops:
`env -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u CFLAGS make -B CXX=/usr/bin/g++ -j2`
was run independently in `tools/blbp_tune`, `tools/cbp6_replay` and
`tools/ittage_equiv`. All exited 0; logs contain actual `-std=c++17` compiler
commands using project includes only, without native/vcpkg headers or libraries.
Evidence: `task5-standalone-{blbp_tune,cbp6_replay,ittage_equiv}.log`. The published
`bin/champsim --config configs/ramulator2.toml --knobs` example also constructed
the native backend and parsed successfully, reporting `dram-model=ramulator2`,
`sim.deadlock_cycle=40000` and no `pmem` table (`task5-example-knobs.toml`/`.stderr`).

The complete branch review approved the implementation with no Critical or
Important findings. Its one replay-comparator test improvement was committed as
`db25b8cf` and independently approved: all 11 tooling tests and all eight saved
one-/two-core replay pairs pass with exact scalar types and unchanged leaf counts.
See the [final review and correction](reviews/2026-09-13-ramulator2.md).

Hosted GitHub Actions execution remains pending. Local validation does not claim
that the hosted compiler matrix has run.

## Review and close-out evidence

An independent first-wave review of `74159f1e` (2026-09-13) and a pre-merge
close-out (2026-09-14 and 15, code changes ending at `4e1bf028`) ran on the same
kind of setup: one shared 32-core Linux x86-64 host, Ubuntu GCC 13.3.0 at
`/usr/bin/g++` with inherited compiler variables removed, CMake 3.28.3, and a
private copy of the pinned native root for every native build. Simulations pinned
`hashed_perceptron` and `basic_btb` on every core unless a row says otherwise.
No run used hosted CI. Raw outputs are in temporary session directories (see the
[integration writeup](ramulator2-integration.md#6-what-must-survive-for-posterity)),
not here.

| Check | Recorded result | Scope |
| --- | --- | --- |
| First-wave adapter differential oracle | An untracked version of test 706 over the real driver, with an independent model, over up to 12 runs (10 YAML variants, two of them also with two feeders): 10 seeds x 10,000 parents, 10 x 100,000, 50 more seeds x 5,000, a 3,000-packet backlog with bursts of 200, counters after every operate, finalization without drain, and 1- and 2-core 10 x 10,000 campaigns. 1,530 seed runs over eleven campaigns, 24,225,863 parents and 625,647,736 native send attempts, no discrepancy. Mutants: 13 of 14, then 17 of 18 detected; M12 is equivalent | `74159f1e`, detached 1- and 2-core worktrees; synthetic traffic |
| First-wave native sanitizers | An instrumented `RelWithDebInfo` `libramulator.so` swapped into private roots with a patched manifest; host `-O1 -fsanitize=address,undefined`. With ITTAGE suppressed: 1-core suite 860 cases, 859 passed, one skip, 17,414 assertions; 2-core suite 862, 861 passed, one skip, 17,988 assertions (with two lifecycle stress cases); direct oracle 1,065 / 705 attempts; both integration runners with the recorded leaf counts; v2 710.omnetpp_r and 723.llvm_r at `-w 100000 -i 2000000` on DDR4 and LPDDR5, plus small-cache, tiny-buffer, two-controller and CommandCounter/CmdTraceRecorder runs, all leaf-equal to unsanitized builds. No integration or native report. Positive control: upstream `External` with a source-1 read gives a native heap-buffer-overflow | `74159f1e`; not a supported build path, so `meta.ramulator2.build` still said Release; the stress harness's callback-copy check was vacuous |
| First-wave 4- and 8-core runs | 4-core `-O3` enabled build, v2 mix 753.ns3_r, 777.zstd_r, 729.abc_r and 710.omnetpp_r at `-w 500000 -i 3000000` in up to five core permutations: legacy, DDR4 with 1, 2 and 4 controllers, LPDDR5 and LPDDR5 with 4-entry buffers, plus prefetcher and 15M / 10M runs. All exit 0; the 28 native documents satisfy every accounting invariant; a rerun and a replay give byte-identical phase sections and 575 equal config leaves; `run_integration.py` passes at four cores (575 config leaves; LPDDR5 409,295 rejected submissions). 8-core `-w 200000 -i 1000000` legacy, DDR4, DDR4 two-controller, LPDDR5 and LPDDR5 4-entry runs all finish with every invariant. Latin-square per-trace IPC spread: DDR4 1.78%, LPDDR5 1.82%, legacy 2.95% | `74159f1e`; one mix; queue-wait probes uncommitted |
| First-wave real traces | Main checkout's enabled 1-core binary. 708.sqlite_r.sp0, 735.gem5_r.sp1, 777.zstd_r.sp1, 723.llvm_r.sp2 (v2) and 462.libquantum (v1) on legacy, DDR4 and LPDDR5 at `-w 1000000 -i 10000000 --toml-sim-stats`: 15 of 15 exit 0; accounting invariants in every document; LLC issued reads equal accepted reads within one in flight (17 documents); three identical zstd LPDDR5 runs; replay from another directory (574 phase / 164 config leaves). 735.gem5_r.sp1 `-w 5000000 -i 100000000` DDR4: RSS 65,016 -> 65,356 KiB over 70 samples; 47,226 reads and 24,269 writes, all completed. 1-entry native buffers: 137,702-458,681 rejected submissions per run | `74159f1e`; one core |
| First-wave legacy regression | Legacy-only `-O3` builds of `79cb5fdb` and `74159f1e` at 1 and 2 cores, plus enabled builds: 13 one-core and 7 two-core configurations (non-default `pmem` with two channels, memory as the fastest clock, `-w 0`, `configs/lnc.toml` with its own `cbp6_tagescl64` and `ittage_64kb` predictors, vmem variations, replay of baseline documents). 89 comparisons over 58,718 phase leaves, 18,245 config leaves and 13,794 stdout lines: no undocumented difference. 58 documents byte-identical apart from their `build_id`, `command_line`, `config_files` and `dram-model` lines. Controls: a `pmem` change gives 164 differences, a fault-penalty change 140 | `-w 1000000 -i 10000000` (1 core), `-w 500000 -i 5000000` (2 cores) |
| Suites at the integrated close-out | Disabled: 891 cases, 873 passed, 18 skipped, 53,018 assertions; `make pytest` `Ran 100 tests ... OK (skipped=8)`. Enabled: 891 cases, 890 passed, one intentional skip, 67,974 assertions (67,975 with `CHAMPSIM_EXPECT_RAMULATOR2=1` and the `cpp` job's flags); `make pytest` `Ran 100 tests ... OK`. Unconfigured, unbuilt checkout: `Ran 100 tests ... OK (skipped=38)`. `test_tools.py`: 23 tests OK. Enabled per file: 704 15 cases / 200 assertions, 705 22 / 436, 706 two passed and four hidden skipped / 27, 707 4 / 49,766, 708 2 / 303. Direct oracle 1,065 / 705 attempts and 44 / 48 leaves; `run_integration.py` 164 config and 568 / 576 / 648 / 568 phase leaves | `63c44f38` |
| Suites after the review's fixes | Disabled: 893 cases, 873 passed, 20 skipped, 53,038 assertions; `make pytest` `OK (skipped=8)`. Enabled: 893 cases, 892 passed, one skip, 68,000 assertions; `make pytest` `OK`. `test_tools.py` 23 OK in both trees. A short oracle campaign (ddr4, lpddr5 and their tiny variants, 3 seeds x 3,000 parents, differential and recovery) reproduced the `63c44f38` totals | Tree identical to `4e1bf028`; `make pytest` with `TMPDIR` set to a short path |
| Adapter differential oracle (test 706) | 1 core, 12 runs x 10 seeds x 10,000 parents: 120 seeds, 1,305,584 parents, 30,979,923 attempts, 1,633,679 accepted fragments, 637,406 responses, 53,142 out-of-range prefetches. 2 cores with `--require-all-cores`: identical totals, 816,956 / 816,723 fragments by core. 4 cores, 6 runs: 60 seeds, 652,187 parents, 19,471,302 attempts. Recovery at `DIFF_CYCLES=40`: 9,600 / 2,400 / 1,200 cycles at 1 / 2 / 4 cores. Further 1-core campaigns: finalization without drain (120 seeds), counters after every operate (1,439,043 comparisons), seeds 11-60, and 10 x 100,000 parents (309,404,766 attempts). Ten campaigns: 1,530 seeds, 33,476,840 parents, 893,828,614 attempts, 4,554 invalid-request cases, zero discrepancies. `run_mutants.py` (2 cores): 31 mutants, 30 detected, M12 equivalent; a 1-core control confirms M19 is equivalent there | Binaries at `6c3e409d` on the oracle branch (production sources of `3cbb6e2b`); mutation run on a copy of `f2ca5f96` |
| `RAMULATOR2_SANITIZE` build path | Instrumented build in 3 min 23 s at `-j6`, 0 warnings: 302 instrumented objects (193 host, 109 native), a 207,008,248-byte library, and `meta.ramulator2.build` `RelWithDebInfo C++20 Python=OFF Sanitizers=address,undefined`. Full suite: 879 cases, 878 passed, one skip, 18,082 assertions, no ASan or UBSan report, exit 1 on 1,014 bytes in 10 allocations from native `RITAddrMapper`. 777.zstd_r.sp1 `-w 100000 -i 500000` on DDR4 equals the release build in all 285 phase leaves. Flipping 0 -> 1 -> 0 rebuilt all 193 host objects each way and restored release library SHA-256 `00df468bdd4386fd6951584f3612a6fa07d8c5db9bf415000bca051383bda157`; leaving the variable unset afterwards rebuilt nothing. Release `make -n` output is byte-identical with the old and new Makefile. Controls: a driver leaking native state unless finalized fails 708 in release; an adapter use-after-free is reported only by instrumented 705 and 708 | Implementer branch at `14f7e43f` |
| Heavy instrumented traffic | 1-, 2- and 4-core `RAMULATOR2_SANITIZE=1` builds with the CI job's `ASAN_OPTIONS`, `UBSAN_OPTIONS` and `LSAN_OPTIONS`: 24 simulations (15 / 5 / 4), 44,700,055 measured instructions, 2,023,106 accepted reads, 459,824 writes, 3,536,932 fragments, 53,664,906 rejected submissions, up to 42 live parents at exit. No sanitizer report; every phase and config leaf equals an unsanitized build (285 / 289 one-core DDR4 / LPDDR5, 329 and 413 with two and four controllers, 237 legacy, 746 / 750 two-core, 2,388 four-core). Suite: 891 cases, 890 passed, one skip, 67,975 assertions (1 core) and 68,267 assertions (2 cores), exit 1 only on 2,028 bytes in 20 allocations under `RITAddrMapper::create_base_mapper()`. Instrumented oracle: 10 x 10,000 (120 / 120 seeds) and 40-cycle recovery (30 / 30) at 1 and 2 cores. Forced no-progress aborts with 1 and 6 live parents, SIGTERM and SIGINT: no report and no leak check; `handle_abort=1` turns the abort into an ASan ABRT report | `63c44f38`; at 200 MB/s the multi-core ROI was 250,000 or 100,000 per core (2 cores) and 50,000 or 100,000 (4 cores); mixes split by trace version; 20-30 times release wall time |
| `native_sanitize` steps, run locally | Each step run with the job's `bash -eo pipefail` shell and environment: tests 704-708 without `[rit-addr-mapper]`, 50,742 assertions in 45 cases, exit 0 in 141 s; the instrumented generated-trace simulation, exit 0 in 29 s; `[rit-addr-mapper]~[.]`, 16 assertions in 2 cases, then 2,028 bytes in 20 allocations, exit 1; with `lsan-rit.supp` (added after the close-out) the same step exits 0 and lists those 20 allocations and 2,028 bytes as suppressed. Before the split, a deliberate 32-byte leak added to test 705 left that step's exit status unchanged | Tree identical to `4e1bf028`; not hosted; the `vm.mmap_rnd_bits` step was not exercised |
| Clock reference and guard tests | Test 707: 4 cases, 49,766 assertions enabled (disabled 3 passed, one skip, 35,692); seven source mutations each fail it. `test_ramulator2_cli.py`: the default guard on 9 machines x 3 explicit values, and the `ddr4_nbl16384.yaml` pause, which aborts by default and finishes with 218,580 and 21,858,000 ticks with identical leaves. Bisection: 54,698 ticks aborts, 54,699 passes. Enabled suite 881 cases, 880 passed, one skip, 67,544 assertions; `make pytest` 93 OK (disabled `OK (skipped=8)`). Test 707 has 49,786 assertions after `cf9eb618` | Clocks branch at `de53e50d` (production sources of `3cbb6e2b`) |
| Real-trace clock sweep | 708.sqlite_r.sp0 and 777.zstd_r.sp1, `-w 1000000 -i 5000000 --hide-heartbeat`, DDR4 and LPDDR5, nine core/cache/LLC period sets each: 36 runs, 36 self-replays and 2 repeats exit 0; accounting invariants; replay equal in 285 / 289 phase and 164 config leaves, type-exact. `-i 0` and `-i 1` each run exactly one global tick. 708.sqlite_r.sp0 with `ddr4_nbl16384.yaml`, `-w 1000000 -i 20000`: the default guard aborts after warmup; 218,580 ticks finishes (20,002 instructions, 110 reads). On the generated trace, native `clock_ratio` 3 and 4 leave all 285 phase leaves unchanged | `de53e50d`, one core; trace SHA-256 unchanged |
| Native counter bounds | Test 704 at `fbe9032b`: 15 cases, 199 assertions; with only the limits seam and no checks, 4 of 14 cases fail. Enabled suite 883 cases, 882 passed, one skip, 17,879 assertions; disabled 883, 869 passed, 14 skipped, 17,320. Against `3cbb6e2b` on 708.sqlite_r.sp0 `-w 500000 -i 5000000` DDR4: 9 pairs equal in 285 phase and 164 config leaves; `perf stat` user-mode instructions +34,458,590 (+0.0088%); task-clock unchanged within noise. Test 704 has 17 cases and 206 assertions after `e49bf310` and the `[rit-addr-mapper]` split | Counter branch at `fbe9032b`, integrated as `20430228` and `f3dca235`; lowered limits only |
| Sustained overload | Native 1- and 4-core and legacy 1-core `-O3` builds, `-w 1000000`, `sim.livelock_period=1000000000000`; native nBL 381 and 92, stock, and 1-, 2- and 4-entry buffers; legacy `pmem.data_rate=25.207349`, `pmem.bankgroups=4`, `sim.deadlock_cycle=400000`. 76 simulations: 75 documents (69 native pass 14 / 14 invariants, 6 legacy 2 / 2), one stopped for time at 43.0M ROI instructions. 25,317,170 accepted fragments and 1,687,148,249 rejected submissions. libquantum 100M: VmRSS 127.2 -> 127.7 MiB. Rejection flood 50M: 127.3 -> 129.8 MiB. 14 instrumented drains end empty. 7 probe / unmodified pairs equal in 285 phase and 164 config leaves | `3cbb6e2b`; up to eight concurrent runs at load 6-26; 64-byte transactions only |
| Regression across the close-out | `3cbb6e2b` against `63c44f38`, 708.sqlite_r.sp0 and 777.zstd_r.sp1, `-w 200000 -i 2000000 --toml-sim-stats`: legacy on disabled and enabled binaries and native DDR4 and LPDDR5 from one absolute YAML copy, 16 runs. Legacy 472 phase / 178 config leaves, DDR4 568 / 164, LPDDR5 576 / 164: all equal; `meta` differs only in `command_line` | Two supplied traces, one core |
| Bandwidth calibration | Harness `bwcal.cc` over `champsim::make_memory_backend`, 64 pending 64-byte reads. Native `nBL` 1 (rejected) to 2,048 plus payload, controller, rank, tCK and spacing variants; legacy `pmem.data_rate` 12.5-12,800 at `pmem.bankgroups=4`. Measured / predicted native throughput 0.950-1.000 from nBL 8 to 2,048; the calibrated mapping in the integration writeup. End to end on 605.mcf_s: default guards abort legacy at 400 and 100 MB/s and native at 100 MB/s; with `sim.livelock_period=1000000000000`, and legacy `sim.deadlock_cycle=400000`, both complete | `3cbb6e2b`, 1-core enabled build |
| Bandwidth sweep | 605.mcf_s, 654.roms_s and 727.cppcheck_r.sp1 at 200, 800, 1,600, 6,400 and 12,800 MB/s and stock, with and without L2C `spp_dev`, on both backends, `-w 10000000 -i 10000000`: 86 runs. Native/legacy IPC without a prefetcher 0.986-0.993 at 200 MB/s, 1.033-1.144 at 800-1,600, 1.101-1.388 at 6,400, 1.050-1.442 at stock. Row hits: native roms 45-52%, cppcheck 76-88%, mcf 5-7%; legacy 0-9%. `spp_dev` aborted four native roms runs (heap-buffer-overflow at `spp_dev.cc:77`; rerun with a one-line patch). `out_of_range_prefetches` 0 in all six `next_line` runs | `3cbb6e2b`, one core |
| Bandwidth stress | 605.mcf_s read-only window `-w 100000 -i 100000`: native read+write throughput 95% of 76,830 / nBL at nBL 256, 99% at 1,024 and 100% at 4,096, 16,384 and 65,536. Guards: native nBL 5,500 finishes at 39,660 of 40,000 ticks and 5,800 aborts (`-w 1000000 -i 100000`); read-only windows abort from nBL 12,000; legacy `pmem.data_rate` 100 finishes and 64 aborts; nBL 5,800 finishes with 166,600 ticks. DDR5_5600B: 112-125% of nominal throughput at nBL 1,024-16,384 without `nRTW = nBL + 8`. Two controllers with 32-byte transactions equal one with 64-byte ones in every core and cache leaf | `3cbb6e2b`, one core |
| Per-core bandwidth emulation | 1- and 4-core builds, `-w 2000000 -i 5000000` per core, LLC 8,192 sets at four cores; native nBL 4 / 16 / 64 / 256 against legacy 2,400 / 600 / 150 / 37.5 MT/s: 99 runs. Same trace on four cores, single-core error at 4.8 / 1.2 / 0.3 GB/s per core: 605.mcf_s +33.5 / -1.5 / +1.2%, 462.libquantum -7.7 / -9.6 / -1.9%. Mix of mcf, libquantum, roms and cactuBSSN: native per-core errors 8.4-123.5%, mean absolute 25.4 / 25.6 / 41.7%. `ip_stride` geomean 0.938 shared against 1.062 emulated at 1.2 GB/s. Shared 2 MiB LLC: +11.9% | `3cbb6e2b`; one mix of v1 traces; one run per configuration |
| Refresh study | JESD79-4, Micron and ISSI datasheets and the Ramulator source history: native nREFI 7.8 µs (DDR4) and 3.906 µs (LPDDR5) are correct. 605.mcf_s, 708.sqlite_r.sp0 and 710.omnetpp_r.sp0 at `-w 10000000 -i 50000000`, exported fixtures against nREFI x 1,000: IPC +0.07% to +5.45%, adapter read latency -3.6% to -10.1%, p99 read latency roughly halved. With `sim.deadlock_cycle=500` both abort at their first refresh after warmup. Legacy on 605.mcf_s: 15,991 refreshes in 47.97 ms, one per 3.0 µs | `74159f1e` 1-core `-O3` binary; one rank, AllBank |

### Reproducing the close-out checks

Use separate checkouts and native roots for release and instrumented builds, and
remove inherited compiler variables first (`env -u CXX -u CC -u CXXFLAGS -u
CPPFLAGS -u LDFLAGS -u CFLAGS CXX=/usr/bin/g++ make ...`). A multi-core check
needs its own checkout with `num_cpus` changed in `inc/defs.h`. Build
`bin/champsim` in its own `make` invocation before `test/bin/000-test-main`
when timing it: shared objects first built for the test binary (after `make
test`, or with `test` named before `all`) keep the test target's `-g3 -Og` and
are linked into `bin/champsim`, which made one simulator 2.3 times slower with
identical results.

```bash
# Oracle campaigns (enabled test binary; each output directory must be new)
python3 test/ramulator2/oracle_variants.py --output-dir "$OUT/variants"
python3 test/ramulator2/run_differential.py --binary test/bin/000-test-main \
  --manifest "$OUT/variants/manifest.json" --output-dir "$OUT/diff" \
  --seeds 10 --parents 10000            # add --require-all-cores on 2+ cores
python3 test/ramulator2/run_differential.py --binary test/bin/000-test-main \
  --manifest "$OUT/variants/manifest.json" --output-dir "$OUT/recovery" \
  --campaign recovery --runs ddr4-tiny,lpddr5-tiny,ddr4-tiny-2feeders --env DIFF_CYCLES=40
python3 test/ramulator2/run_mutants.py --native-root "$PRIVATE_ROOT" --output-dir "$OUT/mutants"

# Instrumented build and the native_sanitize test selection
make -j6 WITH_RAMULATOR2=1 RAMULATOR2_ROOT="$SAN_ROOT" RAMULATOR2_SANITIZE=1 all
make -j6 WITH_RAMULATOR2=1 RAMULATOR2_ROOT="$SAN_ROOT" RAMULATOR2_SANITIZE=1 test/bin/000-test-main
export ASAN_OPTIONS=halt_on_error=1:detect_leaks=1
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:suppressions=$PWD/test/ramulator2/sanitizers/ubsan.supp
export LSAN_OPTIONS=suppressions=$PWD/test/ramulator2/sanitizers/lsan.supp
CHAMPSIM_EXPECT_RAMULATOR2=1 test/bin/000-test-main -# --order rand \
  "[#704-ramulator2-driver]~[.]~[rit-addr-mapper],[#705-ramulator2-backend]~[.]~[rit-addr-mapper],[#706-ramulator2-differential]~[.]~[rit-addr-mapper],[#707-ramulator2-clocks]~[.]~[rit-addr-mapper],[#708-ramulator2-lifecycle]~[.]~[rit-addr-mapper]"
export LSAN_OPTIONS=suppressions=$PWD/test/ramulator2/sanitizers/lsan-rit.supp   # the known native RITAddrMapper leak only
CHAMPSIM_EXPECT_RAMULATOR2=1 test/bin/000-test-main --order rand "[rit-addr-mapper]~[.]"

# Clock, guard and counter tests (enabled build)
test/bin/000-test-main -# "[#707-ramulator2-clocks]"
test/bin/000-test-main -# "[#704-ramulator2-driver]"
PYTHONPATH=$PWD python3 -m unittest discover -v --start-directory=test/python -p test_ramulator2_cli.py
```

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

The remedial CLI commit (`1d741c50` in main, isolated `ee18d4cc`) validated trace
count before output opening, skipped output probes for `--knobs`, and rejected
filesystem aliases of traces. All resumed validation used named outputs, `--`,
protected scratch copies and checksum guards. That commit did not close every
ordinary accident. With one trace path more than the binary has cores, the
optional `--toml` value still consumed the first trace, the count check passed, and
the startup probe truncated the trace before the run overwrote it with statistics
and exited 0. The probe also truncated a `--config` source or an earlier results
document before a startup error such as a replay `config_hash` mismatch.

The CLI no longer writes to a named output at startup, and every output check
applies to the file the kernel reaches through the name. The first version of
this protection read the name as typed but renamed over a textual fold of it:
`missing/../machine.toml`, `machine.toml/../notes.txt` and a dangling link to
`missing/../notes.txt` skipped the signature check and replaced the file the fold
produced. Links are now followed one at a time and directories canonicalized by
the kernel, so those names are "cannot open", as they were at `74159f1e`.

It refuses an input trace, a directory, and an existing non-empty regular file
that does not begin with `# ChampSim statistics.`; an existing regular file must
also open for writing, and a non-empty one for reading (an empty one that cannot
be read is accepted; a non-empty one is refused as "cannot read"), and a new
name must be creatable (the file created to show it is removed again). The file
behind stdout or stderr (`/dev/stdout` redirected to a log, or the log's own
name) receives the document on that stream after the plain report, keeping the
log. A FIFO or process substitution is not opened at startup; a device or socket
must open at startup. After a successful run the checks are repeated and the
document is written in place: an existing file is truncated and rewritten, so it
keeps its inode, links, owner, group, ACLs and permissions, and a missing one,
including one removed during the run, is created with default permissions. A
failure during that final write can leave the target empty or partial, and the
error says so. The write is not atomic, so concurrent runs writing one name, or
readers of it, can see an empty or mixed document; runs launched together at one
new name are not refused at startup, since a check repeats when the name appears
or vanishes under it. An existing statistics document named by mistake is still
overwritten, and none of this protects against concurrent path renames.

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
