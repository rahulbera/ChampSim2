# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

ChampSim is a trace-based, cycle-level microarchitecture simulator (C++17, with an
optional private C++20 Ramulator2 driver). On this
branch it is **configured at run time from a TOML file**: the simulated machine is
hand-written C++ linked against the simulator core and every compiled module (branch
predictors, BTBs, prefetchers, replacement policies), and a run selects modules and
sets every parameter from `--config`/`--set`. The upstream JSON configuration layer and
its code generator are gone; `config.sh` only discovers modules.

## Build, configure, run

Modules are added by creating a directory and no header can know that, which is the
whole of what `config.sh` still generates: the registry naming them, plus the makefile
fragment listing their objects.

```bash
git submodule update --init          # first-time only: fetch vcpkg
vcpkg/bootstrap-vcpkg.sh && vcpkg/vcpkg install   # first-time only: build deps

./config.sh                          # discover modules; emits .csconfig/registry*.inc + _configuration.mk
make                                 # builds bin/champsim

bin/champsim --config configs/lnc.toml --set ooo_cpu.cpu0.btb=ittage_64kb \
    -w 20000000 -i 50000000 --toml stats.toml -- trace.champsimtrace.xz
```

Named portable builds use Python 3.10+ and the selected compiler's target:

```bash
make debug release fast              # retain all three canonical binaries
make BUILD_MODE=fast all test         # same mode, isolated production/test objects
make release X86_ISA=x86-64           # explicit v1 fallback; default is x86-64-v2
make print-build-paths BUILD_MODE=release  # JSON: obj, dep, binary
bin/champsim --build-info             # standalone compiler/dependency provenance JSON
```

Plain `make` is `BUILD_MODE=release BUILD_FLAVOR=sim` (`Makefile:3-4`), so the
default binary is `-O3 -g3` with assertions on. Release and fast use `-O3 -g3`;
debug uses `-O0 -g3` and keeps frame pointers. ChampSim assertions are enabled in
debug/release and disabled in fast, which is the only intended difference between
those two and is worth 0.67-1.56% median KIPS, so time `fast`, not the default
build. Fast does not define global `NDEBUG`; Catch2 and dependency assertions
retain their own policies. Every standard mode uses generic tuning. GCC 9/10 use
the explicit v2 extension expansion when their driver lacks the named
architecture. Linux x64, little-endian AArch64 and the existing Darwin target
routes are selected from the compiler target. ARM/Darwin routing does not
establish hardware validation.

`X86_ISA` accepts only `x86-64` and `x86-64-v2` (`config/build_config.py:262`), and
`-march=native` in `CXXFLAGS` is rejected like any other `-m` flag
(`config/build_config.py:105`). Both have already been tried and rejected, so do
not re-run the experiment: the explicit v3 profile (`bedfcb89`) measured
**4.13-5.41% median KIPS slower** than v2 fast on the four protected traces, with
all twelve paired runs favouring v2, and was removed again (`8d8aad28`); the ETH
headnode's native target resolves architecture *and* tuning to `skylake-avx512`,
whose AVX-512 features are v4, which kratos10's EPYC 7742 cannot execute, so it
was rejected on compatibility before any timing run. The codegen cause of the v3
regression was never isolated. Sections 5 and 6 of
`docs/research-log/Performance/2026-09-15-build-optimization.md` hold the tables
and receipts; note its commit hashes predate the rebase and no longer resolve.

`OBJ_ROOT`, `DEP_ROOT`, and `BIN_ROOT` are container roots. Their resolved leaves
include compiler target, ISA, mode, compiler/options/dependency fingerprint, and
production/test flavor. Named targets retain only canonical binaries; ordinary
`all` atomically publishes the configured binary alias (default `bin/champsim`).
Ordinary `test` also publishes `test/bin/000-test-main`; `test_main_name` overrides
that publication path. Distinct policies may build concurrently; simultaneous
writers of the identical selection and concurrent `config.sh`/cleanup/source
changes are unsupported. A user-specified shared alias is last-writer-wins.
Do not combine named and ordinary goals (`make fast test`); use `BUILD_MODE`.

Dependencies are selected explicitly with `VCPKG_INSTALLED_DIR` and
`VCPKG_TARGET_TRIPLET` (target-matched x64/arm64 Linux or OSX triplets). Make never
installs dependencies implicitly. Provenance records selected libraries, package
receipts and available vcpkg identity; external dependency ISA requirements remain
**unknown** unless established by a separate controlled build/deployment receipt.
The default library directories are vcpkg Release directories in every mode.

The build rejects conflicting optimization, assertion, ISA or tuning flags from
user flags, compiler argv and response files. Transparent argv compiler wrappers
and quoted flag values work in legacy mode; shell programs in `CXX`, opaque
compiler escape channels, static/stateful/exact linker library selection, and
special Make graph characters in output paths are unsupported. User-supplied
positional objects, archives and linker scripts are rejected, including forwarded
and nested-response forms. Use driver `-L`/`-l` selection for extra libraries:
resolved ELF/Mach-O libraries and ordinary archives are content-fingerprinted;
implicit scripts and thin archives are rejected regardless of filename suffix.
Compiler/system search results that cannot be resolved remain explicitly unknown.
Forwarded linker options use a bounded grammar: ordinary rpath, soname, init,
symbol-preserving switches and their operands are supported; unknown opaque
switches are rejected. An operand must stay in its own option channel. Transparent
wrapper chains may name GCC/Clang drivers and ccache/sccache/distcc/icecc; custom
wrappers can also serve as the single `CXX` executable.
Ambient Conda flags can conflict: use a controlled environment, for
example `env -u CFLAGS -u CPPFLAGS -u CXXFLAGS -u LDFLAGS make release CXX=/usr/bin/g++`.
Compiler/response/known forced-include contents participate in build identity.
`--build-info` is standalone and does not construct the simulated machine. Its
policy identity is separate from the statistics configuration `build_id` and the
binary SHA256. Mixed build-info/simulation invocations are rejected by CLI parsing.

Re-run `config.sh` after **adding, renaming, or removing a module** — including when
git removes one for you, which is what checking out a branch with a different module
set does. No `make clean` is needed for that; see the branch-switch gotcha for why.
Changing the machine's shape (which caches exist, how they are wired) is a code edit in
`src/static_environment.cc`; changing `NUM_CPUS`, `BLOCK_SIZE` or `PAGE_SIZE` is an edit
to `inc/defs.h` and a rebuild — multi-core is a separate binary by design.

Traces are `.champsimtrace` optionally `.xz`/`.gz`/`.bz2`/`.zst` compressed; a `-`/process
substitution stream also works (see the `stats_output` CI job). Warmup/sim counts are
*instructions retired*; reported stats cover the simulation phase only.

Two record formats. v1 is the 64-byte `input_instr`; **v2 is a 512-byte
`input_instr_v2`** carrying physical addresses, access widths, an explicit branch type,
and the data value of every memory operand. Neither format is self-describing, so the
version is asserted on the command line:

```bash
bin/champsim --trace-version 2 --heartbeat-frequency 1000000 \
    -w 50000000 -i 200000000 --toml stats.toml -- trace.champsim2.zst
```

`--toml` writes the machine-readable statistics document (see below). Without it, only
the plain-text report goes to stdout.

### Runtime configuration

Every simulation parameter is set at run time. `--config <toml-file>` and
`--set key=value` are both repeatable and apply **strictly in command-line order**, so
the last definition of a key wins whichever source it came from. Keys use the
statistics document's `[config]` language — `ooo_cpu.cpu0.rob_size`,
`cache.cpu0_l1d.sets`, `ptw.cpu0_ptw.mshr_size`, `pmem.tcas`, `vmem.num_levels`,
`sim.deadlock_cycle` — with component names lower-cased. `--knobs` lists every key a
binary accepts, with the value the current invocation would use, plus the selectable
modules per kind; its output is valid TOML (the module list is commented), so
`--knobs > my.toml` is how you get a complete starting configuration. Note that
`--knobs` builds the machine to probe it, which is why ChampSim's construction-time
warnings had to move to stderr — anything on stdout that is not the report corrupts
both this listing and the statistics report itself.

The machinery is a flat `champsim::runtime_config` store (`inc/runtime_config.h`,
toml++ for *reading* only — it alphabetizes on output, which is why the hand-written
writer stays). `src/static_environment.cc` consults it for every builder argument, with
the previous JSON value as the fallback (`.rob_size(cfg.value<std::size_t>(...
, 352))`). Two things follow from there being no generated manifest any more:

- **Validation is post-construction.** The store records which keys were *consulted*;
  after construction, any user key never consulted is fatal, naming the offender. This
  is uniform across scalars, module selections and module knobs. It also means a
  key naming a component that does not exist (`ooo_cpu.cpu2.*` in a 1-core binary) is
  caught, but only after the machine is built.
- **Values are checked during construction, always, and the message names the key.**
  Separate from the unconsulted-key rule above: `configured_bandwidth`,
  `configured_pte_page_size` and `configured_register_file_size`
  (`src/static_environment.cc:146,155,165`) and `power_of_two_dimension`/
  `channel_width` (`src/legacy_memory_backend.cc:18,27`) throw
  `runtime config: <key> …` for a negative width, a `pte_page_size` that is not a
  power of two in (1 KiB, PAGE_SIZE], a `register_file_size` outside 1..32767
  (`PHYSICAL_REGISTER_ID` is `int16_t`), or a non-power-of-two
  `pmem.channels`/`ranks`/`bankgroups`/`banks`/`bank_rows`/`bank_columns`. They are
  throws, not assertions, so `BUILD_MODE=fast` does not remove them. The deeper
  guards behind them — `src/dram_controller.cc:36,44,49`, `src/vmem.cc:33,41` —
  throw `std::invalid_argument` and name no key; `main()` catches `std::exception`
  (`src/main.cc:288`), prints `ERROR: <what>` on stderr and exits 1. It caught only
  `std::runtime_error` until `87aa5ba2`, so an unrepresentable aggregate DRAM
  capacity used to terminate the process instead. `--knobs` builds the machine, so
  it reports these errors too and exits 1. `501-static-environment.cc`'s "Invalid
  nonzero operational geometry" case and `test/python/test_operational_config_errors.py`
  pin the accepted and rejected boundaries.
- **`[config]` is the effective configuration** — every consulted key with the value
  actually used, not a baked record. So a run's own statistics document is a valid
  configuration source: `--config run.toml` on a previous run's `--toml` output
  reproduces that run's machine. The loader recognises the document by
  `[meta].schema_version` and reads its `[config]` table, ignoring the results;
  an ordinary file has no such key and is read whole. `[meta].build_id` (an FNV-1a
  hash of the effective configuration) still has to match, or the binaries are
  different machines.

The environment is constructed **after** `CLI11_PARSE` for this reason, and
`--config`/`--set` use CLI11 `->trigger_on_parse()` so their callbacks fire in argv
order. `configs/sample.toml` is a commented example; `configs/lnc.toml` models Intel
Lion Cove, tagging each value `disclosed`/`derived`/`default` with numbered references.
`lnc.toml` is **core and caches only, deliberately**: it sets no memory key, so it
composes with either backend — pair it with `configs/dram-legacy.toml` for the
LPDDR5X-8533 platform figure or with `configs/ramulator2.toml` for native. On its own
it runs the default legacy DRAM (`data_rate` 3200, not 5600), which is the one value
that differs. A config that sets any `pmem.*` key cannot be combined with
`dram-model = "ramulator2"`: the check is a prefix match with no allowlist
(`src/memory_backend.cc:23`), so no subset survives the switch.

**Every parameter the deleted `champsim_config.json` carried is still settable** — 143
of its 147 leaf keys are runtime knobs, and the other four are compile-time by design
(`block_size`, `page_size`, `num_cores` in `inc/defs.h`; `executable_name` in the
makefile). `501-static-environment.cc` pins 142 of them against the machine's
consulted set, so a parameter cannot quietly stop being configurable; two had, and
that test is why they were found. The 143rd, `sim.heartbeat_frequency`, is outside
that list because `main()` reads it rather than the environment (`src/main.cc:213`)
— nothing pins it, so it is the one knob that can still be dropped silently.

The two that needed more than a scalar lookup:

- **`cache.<name>.prefetch_activate`** is a *set* of access types, held as the JSON's
  own comma-separated string (`"LOAD,PREFETCH"`) and parsed against
  `access_type_names`. An unrecognised name is fatal, so a typo cannot silently
  disable prefetching on a cache.
- **`vmem.randomization`**'s meaning depends on its *type*, which is the JSON's rule
  preserved exactly: `false` disables page-frame shuffling, any integer is the seed,
  and `true` means seed 1. `runtime_config::holds<T>` is what lets the environment
  tell those apart.

`wq_check_full_addr` is the one thing that never was a JSON key: it is a per-edge
property of the channel graph (`match_offset`), not a per-component scalar, so it lives
in `src/static_environment.cc` with the rest of the wiring.

**Module selection is also runtime** (phase B): `ooo_cpu.<cpu>.branch_predictor`,
`ooo_cpu.<cpu>.btb`, `cache.<name>.prefetcher`, `cache.<name>.replacement` select any
compiled module by directory name, one per kind (a comma is rejected; prefetchers are
the planned list case). Mechanism: a discovered name→factory registry
(`champsim::configured::make_*_module`, declared in `.csconfig/registry.inc` and
defined in `registry.cc.inc` via the fixed TU `src/generated_registry.cc`) whose
products replace the default type-erased pimpls **after construction, before any hook
fires** — the one window where the swap is free (`install_*_module`). The
`inc/defs.h` defaults apply when no key selects otherwise. Modules may implement
`configure(const champsim::runtime_config&, std::string_view prefix)` (SFINAE hook,
`bound_to::has_configure`) to accept knobs from a sibling table named after the module
(`[ooo_cpu.cpu0.basic_btb] sets = 2048`); the contract is that a module consults
every knob it owns unconditionally — an unconsumed knob key is fatal after
construction. `basic_btb` is the exemplar: `[ooo_cpu.cpu0.basic_btb] sets`/`ways`
resize its direct predictor (`btb/basic_btb/basic_btb.cc:50`), default 1024x8.
`blbp_64kb`, `blbp_64kb_tuned`, `ittage_64kb` and `ittage_32kb` implement
`configure` too, but only over the basic_btb direct path each of them copied, and
under a different key — `.direct.sets`/`.direct.ways`, not `.sets`/`.ways`
(`inc/blbp/blbp_btb.h:184`, `inc/ittage/ittage_btb.h:224`, same 1024x8 default) —
because a bare `sets` there would name the vendored tables that cannot be sized. A
comparison between BTB arms is honest only while every arm's direct geometry is
identical, so move these on all arms or none. Past the direct path nothing is
reachable: BLBP's tuned surface (`transfer`, `intervals`) is vector-valued and the
store is scalar-only, and ITTAGE/CBP6 internals are constexpr policy classes and
vendored macros; CBP6's env toggles stay `getenv` so that results already recorded
with them stay comparable.

### Optional native DRAM backend

The runtime root key `dram-model` defaults to `legacy`, including in a
native-enabled binary; `ramulator2` requires a build with
`WITH_RAMULATOR2=1 RAMULATOR2_ROOT=/absolute/native/root` against the exact
Ramulator 2.1 revision `72427a1bba3771564c4fb0e494ba02242fd1eaa7`, with Linux/GCC
13, CMake, Python 3.11+ and PyYAML. `config/ramulator2_build.py` pins that revision
and the fmt/yaml-cpp ones, verifies the clean source, the host/native ABI and the
library fingerprint, and records provenance the runtime re-checks against the
library the loader actually opened. Only the private C++20 driver translation unit
sees native headers; the public interface, adapter, legacy mode and standalone
harnesses stay C++17. `configs/ramulator2.toml` is the example;
`ramulator2.config` names a fully expanded YAML export, relative to the process
working directory.

Five things cost a day each if you do not know them:

- **Never build in, modify, or run out-of-tree CMake against a native root another
  build or run may be using** -- Ramulator writes `libramulator.so` into its own
  source root. Concurrent *ChampSim* builds against one root are fine: an exclusive
  `flock` serializes preparation and the rest reuse the manifest. A different
  compiler, `X86_ISA` expansion or `RAMULATOR2_SANITIZE` setting is rejected rather
  than overwriting, so each of those needs a fresh isolated clone.
- **Native mode rejects every `pmem.*` key and legacy rejects every `ramulator2.*`
  key**, and native's `sim.deadlock_cycle` default is
  `max(500, ceil(10 us / minimum actual operable period))` rather than 500. Start
  from `configs/ramulator2.toml` or native `--knobs`, never by overlaying a
  complete legacy configuration.
- **Only a subset of native components is admitted** -- the rest are refused before
  native construction, each with a reason. Controller/DRAM family pairing is *not*
  validated: a mismatched pair can be refused, stall until the no-progress guard
  aborts, or silently run with the generic controller's semantics.
- **`nBL` on DDR4_2400R is the bandwidth knob** (about 76,831 x A / nBL MB/s, A
  0.95-1.00), exported by `configs/ramulator2/bandwidth.py`. Low-bandwidth runs
  need `sim.livelock_period` raised and, below about 13 MB/s, `sim.deadlock_cycle`.
- **Channels are controllers, not an org field.** `org.count[0]` must be 1; the
  channel count is the length of the `controllers` list and must be a power of two
  with identical entries. `configs/ramulator2/channels.py` exports DDR4-3200 at 1/2/4
  channels, and decide capacity deliberately — the default scales it with the channel
  count, `--iso-capacity` holds it by lowering the per-channel density.
- **`RAMULATOR2_SANITIZE=1` needs its own native root.** Tests 704-708 are the
  native suite, and what the `native_sanitize` CI job runs.

[The native backend reference](docs/ramulator-integration/ramulator2-reference.md)
is the full operating manual: build, ABI and provenance rules, the admitted
component lists with every rejection reason, the adapter's submission and
completion semantics, the enforced INT_MAX request and tick limits, the sanitizer
mode and its pinned `RITAddrMapper` leak, the schema 2 statistics layout, and the
bandwidth procedure. Read it before changing anything native.

See [the validation record](docs/ramulator-integration/ramulator2-validation.md)
for evidence, limitations, the corrected default guard, and the recovered
validation input incident, and
[the integration writeup](docs/ramulator-integration/ramulator2-integration.md)
for the architecture, the review and pre-merge close-out evidence with its limits,
the status of every known weak point, and what remains before mainline integration
(among it hosted CI and a clean-host reproduction). Portable regressions live in
`test/ramulator2`; the enabled CI job uses generated local traces and the pinned
native root, preserving the legacy compiler matrix.

### Optional fixed-latency translation

For performance experiments, `[ptw.cpu0_ptw] model = "fixed"` replaces detailed
page walks with `fixed_latency = 200` **CPU cycles** per admitted STLB miss.
`model = "detailed"` is the default. Use `configs/fixed-ptw.toml` as an overlay;
repeat the table for each core in a multicore binary. This experiment uses
`dram-model = "legacy"` exclusively.

ITLB/DTLB/STLB remain active. Fixed mode performs no PSCL lookup, CR3/PTE
allocation, or page-table cache/DRAM request. `VirtualMemory::va_to_pa` still
allocates deterministic per-CPU data pages, but its minor-fault delay is replaced
by the fixed delay. Removing PTE allocations can change data-page placement.
The existing PTW operable services a FIFO: `mshr_size` bounds pending requests,
`max_read` bounds admissions, and `max_write` bounds completions per PTW tick.
The detailed implementation does not currently enforce its `MSHR_SIZE` member;
that difference is another modeling variable when interpreting speed results.

Latency starts on admission, excluding upstream queueing, and is rounded up to
a PTW service tick. Completion precedes admission, so even zero-latency requests
return on the next tick. Warmup admissions bypass latency; pending requests keep
their deadlines across phase resets. Existing PSCL configuration keys stay
accepted for easy configuration overlays but have no effect in fixed mode.
`fixed_latency` is an unused-key error in detailed mode. Negative/overflowing
delays, unknown models and zero fixed-mode queue/bandwidth limits are rejected.
The default `sim.deadlock_cycle` grows when necessary to cover the fixed delay
and rounding to the next PTW tick, measured in global simulation ticks. Explicit
user overrides remain authoritative; pending timers do not invent progress.
An unrepresentable default allowance is rejected. The separate livelock check
still applies to simulations with extremely low IPC.

The fixed-mode tests are `test/cpp/src/601-fixed-ptw.cc` and
`test/python/test_fixed_ptw_cli.py`. See
[the benchmark tooling](tools/perf/README.md) for reproducible KIPS measurements.
The [performance investigation](docs/research-log/Performance/2026-09-14-fixed-ptw-hotspots.md)
records the legacy-only Hermes comparison, validated stack/allocation profiles,
modeling differences, and the proposed order of behavior-preserving optimizations.
**Its KIPS predate the optimization pass it proposed.** Retaining optimizations 1-8
and 11 made the simulator 2.18-2.37x faster on the four protected traces at 1M/3M
(sqlite 196.79 → 453.02 KIPS, omnetpp 217.68 → 515.41, gcc 269.74 → 611.58, mcf
71.91 → 156.70, all twelve pairs favouring the new binary with complete phase and
`[config]` parity), so any throughput figure quoted from before that pass is not a
current baseline. The
[running optimization log](docs/research-log/Performance/2026-09-14-performance-optimization.md)
records each retained optimization with its commit, regression verdict and paired
KIPS. The other four reports in
[`docs/research-log/Performance/`](docs/research-log/Performance/) are the Linux
perf studies that chose the second pass's targets and rank what is left (ROB
schedule/execute/complete at 21-32% of sampled cycles), the build optimization
campaign, and the rebase record mapping all 61 performance commits onto this
branch.

**A behavior-preserving optimization is gated by
`tools/perf/compare_optimization.py`, not by `make test`.** It refuses any
difference in the complete `[phase]` statistics, the effective `[config]`, or the
warmup/ROI retirement and cycle counts, so a non-inert change fails there rather
than reaching a reader; `--regression` adds a 16-case control matrix. A change to
DRAM address mapping or ROB traversal needs the long gate too — 50M simulation
across the 14 SPEC26 workloads — and `--timeout` exists for that window. Never
overlap a build, a test campaign or a profile with a timing run.
[The benchmark tooling](tools/perf/README.md) has the procedure, the manifest
hashing rule, and the harness's own tests, which `make pytest` does not reach.

### Tests

```bash
make test                    # build + run the full Catch2 suite (test/bin/000-test-main)
make test TEST_NUM=091       # tests from files starting 091 — exactly one file must match
make pytest                  # the Python suite (test/python/, unittest); much of it skips silently
python3 -m unittest discover -s tools/perf -v   # perf-harness tests; `make pytest` does NOT reach them
test/bin/000-test-main --order rand --warn NoAssertions --invisibles   # how CI invokes it directly
```

`TEST_NUM` is a prefix filter that must resolve to **one** file.
`config/build_rules.mk:210` folds every match into a single bracketed Catch2 spec, so
`TEST_NUM=601` becomes the one tag `[#601-fixed-ptw #601-multiple-inflight-walks]`;
Catch2 3.12 ends a tag only at `[` or `]`, so the space stays inside the tag name,
nothing matches, and the binary exits 2 (`NoTestsRunExitCode`) — `make test` fails
having run nothing. Two prefixes are ambiguous today,
`601-fixed-ptw`/`601-multiple-inflight-walks` and
`703-dram-address-mapping`/`703-memory-backend`, so name enough of the file:
`TEST_NUM=601-fixed`.

`make pytest` is `unittest discover --start-directory=test/python` (`Makefile:42`) and
is long past module discovery: 150 tests covering build-mode and compiler-flag policy
(`test_build_modes.py` drives real Make/g++ fixture builds), the assertion policy,
output paths, trace-decoder errors, construction-time configuration errors, and the
Ramulator build and CLI. 48 of them are `@unittest.skipUnless` on `bin/champsim`
(`CHAMPSIM_BINARY` overrides it) and `test_build_info` skips unless the differently
named `CHAMPSIM_TEST_BINARY` is set. `make release`, `make debug` and `make fast`
publish no alias, so on an unbuilt or named-build-only tree the run reports success
having executed little more than `test_filewrite`, `test_module_registry`, `test_util`
and `test_defaults_agree` — point `CHAMPSIM_BINARY` at the canonical path from
`make print-build-paths`.

Neither `make pytest` nor CI discovers `tools/perf`, so its four `test_*.py` files are
run by nothing automatic; run them yourself after touching any script there.

C++ tests live in `test/cpp/src/NNN-name.cc`; the `NNN` prefix encodes the subsystem
(see `test/cpp/README.txt`: 100s=front-end, 200s=OoO core, 300s=retirement,
400s=caches, 500s=environment/phases, 600s=page table walkers, 700s=DRAM,
800s=virtual memory, 900s=assorted bugs).
`test/cpp/src/501-static-environment.cc` pins the hand-written machine: its component
set, the cache order (which is the per-cycle `operate()` order), and the channel-count
formula. There are no compile-only configs any more — the configuration layer they
guarded is gone.

### Cleaning / regenerating

- `make clean` — remove only the resolved selection's objects, dependencies and
  canonical binaries. Use the same mode/options that selected the build, and
  `BUILD_FLAVOR=test` for the test flavor: it is a separate policy leaf with its own
  fingerprint, so an ordinary `make clean` does not touch it. The published aliases
  sit outside the leaf and are absolute symlinks into it, so `clean` leaves
  `bin/champsim` (or `test/bin/000-test-main`) behind as a *dangling* link — `ls`
  still shows it and `Path.is_file()` does not, which silently turns the `make pytest`
  binary-driven suites into skips rather than failures until the next build
  republishes it.
- `make configclean` — also remove `_configuration.mk` and the configured registry
  inputs. Configuration cleanup must not run concurrently with any build.
- `make compile_commands` — write the selected flavor's exact compiler argv to
  `<resolved obj>/compile_commands.json`. Use `BUILD_FLAVOR=test` for tests. Editor
  publication is explicit: point clangd at that database or copy/symlink it yourself.

## Architecture

### Module discovery, and the machine that is not generated

There is no code generator and no hand-written `main()` wiring either. The machine is
`champsim::static_environment` (`inc/static_environment.h`, `src/static_environment.cc`):
one `operate()`-ordered component list built in a loop over `defs::num_cpus`, and a
**named channel graph** — one channel per edge, its queue geometry taken from the
*lower* component, which is why three separate channels feed the STLB. There are
`num_cpus * 12 + 1` channels: twelve per-core edges plus **one shared LLC→DRAM edge**
(shared because there is one LLC and one selected memory backend however many cores
there are — getting this wrong is invisible at one core and wrong at two).

Construction order is load-bearing and fixed by member declaration order: channels →
DRAM → vmem → PTWs → caches → cores, with every vector fully `reserve`d and filled
before a pointer into it is handed out. Cache order is the per-cycle `operate()` order:
`LLC` first, then each core's caches alphabetically (`DTLB, ITLB, L1D, L1I, L2C, STLB`).
That holds among operables due at the same time only while there are at most 16:
`do_cycle` orders them with `std::sort`, which libstdc++ does not keep stable above
16 elements, and a machine with two or more cores has at least 18.

`config.sh` → `config/` package (`modules.py`, `module_registry.py`, `makefile.py`,
`filewrite.py`, `util.py`) walks the four module directories and emits only what a
header cannot know: `.csconfig/registry.inc` + `registry.cc.inc` (the name→factory
tables) and `_configuration.mk` (module object lists the top-level `Makefile`
includes). `.csconfig/` is gitignored, so no generated source is a staging hazard.
Re-run it after **adding, renaming, or removing a module** — a branch switch that
changes the module set counts, because git does the removing.

A module is selected by its **directory basename**, so two modules of one kind cannot
share one — `config.sh` rejects that, naming both paths, because the registry would
otherwise emit the name twice and the first factory silently win. A `__legacy__` marker
is rejected outright for the same reason: the bridge generator is gone, so the sources
would compile and the class would never be registered.

Extra module search roots: `--module-dir` / `--branch-dir` / `--btb-dir` /
`--prefetcher-dir` / `--replacement-dir`. Every discovered module is compiled into every
binary; which one is *active* is a run-time key.

**What this trades away:** any machine whose component set differs from the standard
hierarchy — an extra cache level, a cache with two lower levels, a non-uniform per-core
hierarchy — is now a C++ edit rather than a JSON edit. That was the deliberate choice:
the shape is code, the parameters are configuration.

**This machine was proved equivalent to the generated one it replaced**, at one core and
at two: 281 `[config]` leaves and 695 statistic leaves, zero differences. Both paths
coexist only at `225930b8`, behind `-DCHAMPSIM_STATIC_ENV`, so that is where to rebuild
the reference if the channel graph is ever suspected. Two traps make the comparison lie
before it tells the truth: configure the reference from `champsim_config.json`, not a
minimal JSON (otherwise its channel queues fall back to `_queue_factor` — L1D rq 32/pq 32
instead of 64/8), and pin the branch predictor on both sides, because the
baked default changed (next paragraph).

**The default branch predictor changed with the migration.** The deleted
`champsim_config.json` selected `bimodal`, overriding the `hashed_perceptron` that
`inc/defaults.hpp` bakes; `inc/defs.h` now names the C++ default, so a
configuration-free run predicts better than it used to — 4.46 MPKI against bimodal's
8.38 on 400.perlbench. A run that pins its predictor is unaffected, but a default-build
baseline is not, and any comparison against a pre-migration run must pin
`ooo_cpu.<cpu>.branch_predictor`.

### Simulation model: `champsim::operable` + global clock

Every clocked component (`O3_CPU`, `CACHE`, `PageTableWalker`, `DRAM_CHANNEL`) derives
from `champsim::operable` (`inc/operable.h`) and implements `operate()`. `src/champsim.cc`
drives simulation: `main()` runs each `phase_info` (warmup / simulation), and `do_phase`
ticks a shared `champsim::chrono::clock` by the min clock period across all operables,
calling `operate_on(clock)` on each. Components communicate through `channel`s
(`inc/channel.h`) — bounded request/response queues — rather than direct calls, so the
memory hierarchy is a graph of operables wired by the static environment.

Progress is monitored through `sim.deadlock_cycle` consecutive global simulation
ticks without progress, followed by per-operable diagnostics and `abort()`. Legacy
defaults to 500 ticks. Native mode, only when the key is omitted, defaults to
`max(500, ceil(10 us / minimum actual operable period))` after constructing the
selected environment once. Explicit values retain their meaning, but in native
mode one allowing less than 10 us prints a stderr warning: legacy `--knobs` dumps
and statistics documents record the value they used (500 by default), so a
converted configuration should drop `sim.deadlock_cycle` along with `pmem.*`.
Nonpositive native operable periods are errors. The diagnostics are flushed before
`abort()`, so a redirected stdout keeps the memory backend's, which print last.
Each operable prints inside `try`/`catch` (`print_deadlock_diagnostics`): one that
throws gets a one-line note and the rest still print. The core's printer used to
throw on real v2 traces, counting dependencies of unrenamed entries whose register
IDs lie past the 128-entry physical register file. A periodic livelock check
warns/dies on low IPC.

### Memory hierarchy & core

`O3_CPU` (`inc/ooo_cpu.h`, `src/ooo_cpu.cc`) models the OoO pipeline
(fetch→decode→dispatch→schedule→execute→retire, with ROB/LQ/SQ, a DIB decoded-instruction
buffer, and register renaming via `register_allocator`). Instructions come from
`tracereader` (`src/tracereader.cc`) filling `cpu.input_queue`. `CACHE` (`inc/cache.h`,
`src/cache.cc`) is the generic cache used for every level (L1I/L1D/L2C/LLC and TLBs);
`PageTableWalker` (`ptw.cc`) + `VirtualMemory` (`vmem.cc`) handle address translation;
`memory_backend` owns the selected legacy controller or native adapter;
`DRAM_CONTROLLER`/`DRAM_CHANNEL` (`dram_controller.cc`) implement legacy memory. Stats are split
into `sim_stats` (whole run) and `roi_stats` (region of interest / sim phase) per component.

### The module system (`inc/modules.h`)

Four pluggable module kinds, each a subdirectory of source files:

| Kind | Directory | Base class | Bound to |
|------|-----------|------------|----------|
| Branch direction predictor | `branch/<name>/` | `champsim::modules::branch_predictor` | `O3_CPU*` |
| Branch target buffer | `btb/<name>/` | `champsim::modules::btb` | `O3_CPU*` |
| Prefetcher | `prefetcher/<name>/` | `champsim::modules::prefetcher` | `CACHE*` |
| Replacement policy | `replacement/<name>/` | `champsim::modules::replacement` | `CACHE*` |

A module is a C++ class inheriting the matching base and constructible from its bound
pointer (usually `using Base::Base;`). Hooks are **detected at compile time via SFINAE**
(the `has_*` traits in `modules.h`), so you implement only the hooks you need and the
simulator calls the first matching overload. The base pointer (`intern_`) gives access to
the owning `CACHE`/`O3_CPU` (e.g. `NUM_WAY`, `prefetch_line(...)`). See
`docs/src/Modules.rst` for the full hook signatures; `branch/bimodal/` is the minimal
reference example.

To add a module: create `branch/mypred/mypred.{h,cc}` (or the relevant kind), re-run
`./config.sh` so it is discovered, `make`, then select it at run time with
`--set ooo_cpu.cpu0.branch_predictor=mypred` (or from a TOML file). The directory
basename is the module's name and must equal its class name.

**Legacy modules are gone.** The `__legacy__` free-function style depended on
`config/legacy.py` and the `legacy_bridge.*` generation, both deleted with the
configuration layer; the Makefile machinery that drove them went with it. No shipped
module used the path. `config.sh` now *rejects* a `__legacy__` marker with a message
naming the directory — without that it would compile the sources and leave the class
unregistered, giving a module that builds and can never be selected.

### Support libraries

- `inc/msl/` — Module Support Library, safe to use from modules: `fwcounter` (saturating
  counter), `lru_table`, `bits`, `stat_methods`.
- `inc/util/` — internal helpers: `span`, `algorithm`, `bit_enum`, `units`, `detect` (SFINAE), etc.
- `champsim::address` (`inc/address.h`) — strongly-typed address type; prefer it over raw
  `uint64_t`. Many hooks have both `champsim::address` and legacy `uint64_t` overloads;
  the `uint64_t` ones are `[[deprecated]]`.

## Predictor modules with tuned or vendored constants

Several shipped modules carry constants that cannot be reconstructed from the source
and must not be "cleaned up" on inspection: `btb/blbp_64kb_tuned` was produced by a
parameter search, and the `branch/cbp6_*` predictors plus `inc/ittage/` are vendored
competition code. Read the module's own header comments before changing a constant in
one of them.

The oracle BTBs (`btb/{perfect,ideal}_*`, `inc/perfect_group/`) decompose
target-prediction headroom by class (direct / indirect / return), so a predictor's
capture can be stated as a fraction of what is attainable rather than as a raw MPKI.
Selecting between them is a runtime `--set`, so a sweep over predictors is one binary
and N values, where each variant used to need its own configure-and-build.

### The CBP6 adapter (`inc/cbp6/`)

CBP2025 predictors are driven through nine pipeline-event callbacks; ChampSim uses its
module interface. `champsim::cbp6::host<Tenant>` translates between them so a submission
can be **vendored unmodified** rather than rewritten. Hosted tenants:
`branch/cbp6_tagescl64` (the CBP2016 64KB baseline CBP2025 ships), `cbp6_tagescl192`,
`cbp6_runlts_norv` (the CBP2025 winner), `cbp6_ddtage` (Ros's submission).

- The host **must** have static storage duration — vendored predictors' constructors do
  not initialise every member and rely on zero-initialisation.
- `CBP6_PROTOCOL_CHECK=1` validates the call sequence; `CBP6_DELAYED_UPDATE=1` moves the
  update from fetch to execute; `CBP6_CALL_DUMP` records a replay log.
- Each vendored source documents every deviation from the submission at its site.

### Standalone harnesses (`tools/`)

`tools/blbp_tune/`, `tools/cbp6_replay/`, `tools/ittage_equiv/`. Each has its own
Makefile that compiles with **nothing but `g++ -I../../inc`** — no vcpkg — so they build
on a bare login node. The consequence is a real constraint: if a header they include
ever starts pulling a vcpkg dependency, those builds break on the machine where it is
hardest to notice. Keep such code in `src/`.

**`btb/blbp_64kb_tuned`'s constants came from a search that was paused, not finished,
and resuming it is very likely not worth it.** `tools/blbp_tune/TUNING_NOTES.md` has the
state: stopped at generation 35 of 40 for an operational reason, *not* by its own stop
criterion — the objective was still falling ~0.1–0.2% per generation, having fallen
17.5% overall. Extrapolating the last ten generations suggests another 1–2% is
available from the same search space and no more. The parameters that would actually
move the result — `M`, `K`, `N` and the IBTB geometry — are **size-bearing** and were
frozen to hold the storage budget, so they are outside the space by construction: a
resumed run cannot reach them, and widening to include them is a new configuration
rather than a resume, because it breaks the iso-storage claim. The generation-35 best is
frozen and committed; a later search that beats it should produce a new module, not
overwrite that one.

### Gotchas that cost real time here

- **`SET_ASIDE_CHAMPSIM_MODULE` is not re-entrant.** A module cannot simply
  `#include "ooo_cpu.h"`: `inc/ooo_cpu.h:20-23` sets the flag aside and undefines
  `CHAMPSIM_MODULE`, the `address.h` it pulls in re-defines the macro on its own way
  out (`inc/address.h:671-673`), and the `util/lru_table.h` at `ooo_cpu.h:46` then
  fires its `#error` from outside its own include guard (`inc/util/lru_table.h:17`).
  Do `#undef CHAMPSIM_MODULE` / include / `#define` at the *outermost* level.
  Upstream ships no module that needs this, which is why the bug survives there; on
  this branch sixteen do — every `cbp6_*` predictor, `perfect_branch`, and all the
  BTBs that reach into the core. `branch/cbp6_tagescl64/cbp6_tagescl64.cc:1-15`
  explains it in full and is the pattern to copy.
- **`CHAMPSIM_TRACE_MEMORY_VALUES=1` changes `ooo_model_instr`'s size in every
  translation unit.** It is build-wide, not per-target: mixing objects across the two
  settings is an ODR violation that produces wrong stats rather than a link error.
  Named build isolation accounts for this switch, so changing it selects different
  objects automatically. It roughly doubles wall-clock, so it is off by default.
- **A perfect cache reporting a 100% hit rate proves nothing**, for the same reason
  the perfect-predictor entry below gives: it follows structurally from the flag. The
  load-bearing checks are that the level below goes quiet (every `*_fill` for the
  perfect cache is zero) and that IPC actually moves. `test/cpp/src/446-perfect-cache.cc`
  pins only the first — 64 distinct blocks, all hits, `mock_ll.packet_count() == 0` —
  and carries a non-perfect control scenario so the file would fail if the flag did
  nothing. The IPC half is a whole-run check and is in no test; run it yourself, and
  check the size of the change as well as its direction. A perfect DTLB once lifted
  mcf 11.5x over its baseline because it mapped every page to physical page 0; a
  correct one lifts it 15% (see `cache.<name>.perfect` below).
- **A "perfect predictor" reporting 0 MPKI proves nothing.** `branch/perfect_branch` +
  `btb/perfect_btb` read the outcome out of the trace at prediction time — the same field
  ChampSim's mispredict rule compares — so 0 MPKI follows structurally even from a corrupt
  trace. Validate trace metadata independently (a not-taken branch must be followed by its
  fall-through address).
- **Beware the arithmetic mean of per-trace percentage reductions.** Several SPEC traces
  have near-zero MPKI; one scored −286% off a 0.009 MPKI baseline and inverted a ranking.
  Pool the counts and take one ratio instead.
- **`inc/defs.h`'s module defaults must name what `inc/defaults.hpp` bakes.**
  `select_modules` *skips* the registry when a selection equals the default, trusting the
  builder already installed it. A mismatch compiles, runs, and makes `[config]` report a
  module the run did not use. `test/python/test_defaults_agree.py` is the only thing
  tying the two files together.
- **A duration in *cycles* scales by `static_environment::time_quantum(cfg)`, never a
  literal.** `do_phase` ticks by the smallest `clock_period` among the operables, so the
  quantum depends on every frequency key. vmem's minor-fault penalty used a baked 250 ps
  (`1e6/4000`), which was right only until something overrode a frequency — then every
  page fault was proportionally too expensive, silently. If you add a component with a
  clock, add its frequency key to that sweep; `501-static-environment.cc` pins the
  computed quantum against the constructed machine's actual minimum.
- **Two configuration keys are named for something they are not.**
  `ooo_cpu.<cpu>.scheduler_size` feeds `schedule_width()` — a per-cycle search
  *bandwidth*, not the size of a structure — and `ptw.<name>.max_read`/`max_write`
  feed `tag_bandwidth`/`fill_bandwidth`. Both names came from the JSON and were kept
  so old configurations still load; read them as widths.
- **Geometry knobs read through `positive_value`; queue sizes deliberately do not.**
  Thirteen keys used to kill the process at zero (SIGFPE in the DRAM divisors, SIGABRT in
  the cache asserts) and the two DIB knobs silently built a structure that can never hit.
  `pq_size = 0` is the shipped TLB configuration, which is why queues are exempt.
- **Legacy DRAM timings are memory-controller CYCLES, so `pmem.frequency` scales them.**
  `tCAS`/`tRCD`/`tRP`/`tRAS` are multiplied by `mc_period` in the `DRAM_CHANNEL`
  constructor, and `mc_period = 1e6 / pmem.frequency`. Raising the frequency without
  rescaling the cycle counts shortens absolute core latency by the same factor —
  4266 MHz against the stock 24-cycle timings gives a 5.6 ns tCAS and drops LLC miss
  latency from 186 to 67 cycles, with nothing to flag it. `pmem.data_rate` is the
  independent one: it sets only the data-bus transfer period, so raising it alone
  models a faster bus with intact core timings, which is what a memory *generation*
  change usually wants. Note the 2:1 data-rate-to-clock ratio is a DDR relation and
  does not hold for LPDDR5.
- **Gitignored generated state does not follow a branch switch, and the hazard is the
  *dep* file, not the orphan objects.** `_configuration.mk` and `.csconfig/` survive a
  checkout, so a branch that adds modules leaves a registry naming directories the new
  branch does not have. The stale `.o` files are inert — both link rules pass `$^` over
  lists that bottom out in `$(wildcard <dir>/*.cc)`, and nothing in the build globs
  `*.o`, so an object whose source is gone is never named. What actually stopped `make`
  was `.csconfig/generated_registry.d`: a dependency file whose translation unit still
  exists, naming five headers git had deleted. `config.sh` cannot clear a `.d` (nothing
  in `config/` removes a file), so `./config.sh && make` alone did not repair it and
  `make clean && make` alone did not either — `registry.cc.inc` still `#include`d the
  dead headers. The dependency recipe (`dep_recipe`, `config/build_rules.mk:164-166`)
  now passes `-MM -MP`, which emits a phony target per header and makes the
  translation unit rebuild instead; `./config.sh && make` is therefore sufficient on
  its own. Since the build-path isolation, that `.d` no longer sits at the root of
  `.csconfig/` — it is `<resolved DEP_ROOT>/generated_registry.d`, one per flavor
  (`build_rules.mk:109`), e.g.
  `.csconfig/x86_64-linux-gnu/x86-64-v2/release/<fingerprint>/{sim,test}/`; a copy
  still at the root is a pre-isolation leftover no build reads. `make configclean`
  remains the belt-and-braces recovery.
- **Vendored ITTAGE has undefined behaviour in its RNG.** `inc/ittage/ittage.hpp:246,248`
  left-shift a negative `int` in `MYRANDOM`; UBSan flags it on any `ittage_64kb` run, so
  any results already recorded with that module were produced with it. GCC emits the
  expected two's-complement result, and casting through `unsigned` would be bit-identical, but it
  is vendored competition code — do not change it without deciding what happens to the
  recorded results. It is the only UB the simulator *core* reports — but read
  "ASan+UBSan across the shipped module set is clean" as a short-run result, not a
  guarantee. That sweep never drove `spp_dev`'s lookahead deep enough: it needs about
  seven iterations to push `pf_q_tail` past the MSHR-sized `confidence_q`, so ASan
  first reported that heap-buffer-overflow (then `prefetcher/spp_dev/spp_dev.cc:77`)
  in the Ramulator bandwidth sweep rather than in the module sweep. Fixed and pinned
  by `test/cpp/src/454-spp-dev-behavior.cc` in `38cf7cb7`; that report's `spp_dev`
  numbers predate it. A sweep that does not reach a module's own deep path proves
  nothing about that module.
- **`make fast` removes the shipped modules' bounds guards, so it corrupts memory
  where release aborts.** The `CHAMPSIM_ASSERT` conversion took the modules'
  `assert(0)` guards with it, and fast compiles them to nothing.
  `prefetcher/spp_dev` is the worked example: `GLOBAL_REGISTER::update_entry` seeded
  its victim search at `min_conf = 100`, but confidence is a percentage that reaches
  exactly 100 for any signature only ever followed by one delta, so once all eight
  GHR entries were maximally confident it found no victim — release printed
  `[GHR] Cannot find a replacement victim!` and aborted (four native 654.roms_s runs
  in the bandwidth sweep), while fast wrote index 8 of five eight-element arrays.
  Reproduce any fast-build anomaly under `release` before believing it, and never
  expect a sanitizer run on a fast binary to trip these guards.
- **ChampSim's own assertions are `CHAMPSIM_ASSERT`, not `<cassert>`'s `assert`.**
  `inc/champsim_assert.h` defines it against `CHAMPSIM_ENABLE_ASSERTIONS`, which
  `config/build_config.py:317` sets to 0 only in `fast`. It is independent of
  `NDEBUG`, which no mode defines and which is rejected outright from user flags
  (`config/build_config.py:103`), so a plain `assert` cannot be compiled out of any
  build here — it would survive into fast and cost exactly what the policy meant to
  remove. Put no side effect inside a `CHAMPSIM_ASSERT`: the disabled form is
  `static_cast<void>(0)` and never evaluates its argument
  (`test/python/test_champsim_assert.py:60`). Modules get the same policy —
  `module_prereqs` carries `$(base_options)` (`config/build_rules.mk:184,129`).

### Added to the core

Four branch-predictor hooks (`branch_predictor_final_stats`, `branch_execute_resolve`,
`branch_decode_notify`, `branch_execute_notify`), the `cycles_on_wrong_path` statistic
reported as **CycWPKI**, DIB lookup/hit/miss counters, `--trace-version`, a
configurable, flushed `--heartbeat-frequency`, and the per-cache `perfect` flag.

**`cache.<name>.perfect`** (default `false`, accepted by every cache including the
three TLBs) makes every lookup hit, *including the first access to a block that was
never filled* — there is no compulsory miss, because the point is headroom: "what is
the performance if every request hits here?". `CACHE::try_hit` short-circuits before
touching the tag array, so the array is never filled or evicted and this cache issues
nothing downward. Four things it deliberately does **not** change: the configured hit
latency and `max_tag_check`/queue limits still apply; requests are still translated,
so a perfect L1D still exercises the DTLB/STLB/PTW (set `perfect` on those to model
perfect translation, which composes); this cache's own prefetcher and replacement
hooks are bypassed, since neither can affect a cache that cannot miss and there is no
way index to hand a replacement policy; and the level below is *not* silenced
outright — it keeps serving its other clients, so an L2C under a perfect L1D still
sees the L1I's misses.

**A perfect TLB returns the real translation.** A TLB's response data *is* the
translation, so the three TLBs are built with `.virtual_memory(&vmem)` and a perfect
one answers with `vmem->va_to_pa(...)`, the call the walker's last step makes, without
the walk and without the minor-fault penalty. A perfect TLB allocates no PTE pages and
takes each data frame at lookup time, so physical placement differs from a walking
run: unrandomized, a perfect DTLB lifted mcf 23%, against 17% under the default seed.
Compare perfect-TLB runs against a baseline with `vmem.randomization` on. Data caches
get no virtual memory and echo the request's data field, which nothing above them
reads. Until this was fixed, perfect TLBs echoed that data field too. The core never
sets it, so every page mapped to physical page 0, and a workload's whole data footprint
collapsed into 64 cache blocks. At `-w 1000000 -i 3000000`, 605.mcf with a perfect
DTLB ran at IPC 3.04; with the real translation it runs at 0.30, against a 0.26
baseline. Perfect-TLB results from before the fix are invalid.
`test/cpp/src/447-perfect-tlb.cc` pins the real translation, and
`501-static-environment.cc` pins that only the TLBs hold the virtual memory.

DIB accounting lives in `cpu_stats` (`inc/core_stats.h`) and is charged in
`O3_CPU::do_check_dib`, which runs once per instruction, gated by `dib_checked`. Only
`dib_hits`/`dib_misses` are stored; `dib_lookups()` is their sum, so the total cannot
drift from its parts.

**There are two front-end routes, not one.** `promote_to_decode` partitions the fetch
window on `decoded`: DIB hits go to `DIB_HIT_BUFFER` charged `dib.hit_latency`, misses
go to `DECODE_BUFFER` charged `decode_latency`, and `decode_instruction` merges the two
back in program order under `dib.inorder_width`. The two routes are NOT symmetric:
`do_dib_hit` sets `ready_time` and nothing else, while `do_decode` also fills the DIB
(`do_dib_update`), fires `branch_decode_notify`, and performs decode-time misprediction
recovery for direct branches. So a DIB hit never notifies the predictor at decode and
never resolves a branch early — and a *perfect* DIB would eliminate both entirely.
Measured inert today: `cbp6_runlts_norv` is the only consumer of the decode hook, and
its mispredict count is identical at 76% and 97% DIB hit rate with `CBP6_PROTOCOL_CHECK`
satisfied in both. It is a trap for the next predictor that needs the hook, not a
current bug. Warmup is free on both routes — it
was charged on the DIB route only, until `test/cpp/src/122-dib-warmup-latency.cc` pinned
the symmetry. `ooo_cpu.<cpu>.dib.hit_latency`, `.dib.inorder_width` and
`.dib.hit_buffer_size` were builder-only until they became runtime keys; the width and
the buffer size refuse zero, `hit_latency` does not, because a zero-cost hit is a
meaningful thing to ask for. A `hit_buffer_size` of zero stalls the *decode* path too —
`promote_to_decode` takes the min of both buffers' free space.

Four things about the numbers are easy to get wrong:

- **A hit means the window was already decoded, not that a neighbour shares it.** The
  DIB is filled at *decode* (`do_dib_update`), many cycles after `check_dib` has
  classified the whole fetch group, so a cold fetch group misses in its entirety —
  four instructions in one 16-byte window are four misses on the first pass, and hit
  only when that code runs again. `dib_misses` counts *instructions*, not distinct
  windows; it is not a code-footprint proxy. `test/cpp/src/141-dib-stats.cc` pins this.
- **The DIB is worth very little at the shipped parameters, and the widths dominate.**
  `dib.hit_latency` and `decode_latency` are both 1, so a hit and a miss reach dispatch
  on the same cycle — `src/ooo_cpu.cc` says so at `// assume DECODE_LATENCY =
  DIB_HIT_LATENCY`. Tripling the hit rate (a 1x1 DIB against the shipped 32x8) moved
  cycles by 0.5%: the DIB removes L1I *hits*, not misses. The width knobs move results
  roughly 7x harder than the latency knob. Intel states the uop-cache path has shorter
  latency than legacy decode but publishes no magnitude for any Cove core;
  `configs/lnc.toml` carries the derivation and the sources.
- **Residency is granted per 16-byte window, which is optimistic at any size.** One
  decoded instruction makes its whole window hit, including instructions never decoded;
  a real uop cache has per-region micro-op limits and drops a region to the decoders
  when it will not fit. So sizing the DIB as though an entry were a micro-op inflates it
  by however many instructions share a window — measured at ~3.8 on our traces, which is
  why `configs/lnc.toml` is 128 sets and not 512.
- **The lookup count is not the retired instruction count.** The pipeline is *not*
  drained between phases (`do_phase` in `src/champsim.cc`), so a phase's lookups equal
  its retired count adjusted by the in-flight delta at both boundaries — measured at
  +72 on 400.perlbench and −405 on 657.xz. Close to `instructions`, but neither equal
  to nor an upper bound on it.

### The machine-readable stats document is TOML (`--toml`), not JSON

`src/toml_printer.cc` emits the statistics document; `--json` is **rejected at
startup** with an error pointing at `--toml`. `src/json_printer.cc` is still compiled
and linked so it cannot rot silently; the CLI rejects it, while focused stream tests
exercise native JSON compatibility. An unwritable `--toml` path is also rejected at
startup, as is an existing non-empty regular file that is not a statistics document or
cannot be read to check, and a failed write exits non-zero rather than reporting
success; how each kind of target is written, and what a failure leaves, is described
with `--toml` under *Optional native DRAM backend*. The format differs from the old
JSON in ways that matter to a parser:

- **`lower_snake_case` core/cache/legacy memory keys**, including lower-cased component names
  (`cpu0_l1d`, `llc`). A configured name that is not a bare TOML key is quoted,
  never rewritten, so two distinct names can never collide onto one table.
- **No arrays.** Every key holds a single scalar; what were per-CPU arrays are
  now per-CPU tables (`…roi.cache.cpu0_l1d.cpu0`), so key names do not change
  with the core count. The document root is `[meta]`, `[config]`,
  `[config_override]` and `[phase.<name>]`; `[config]` is emitted even when it is
  empty, so a consumer can always index it.
- **Only the region of interest is emitted by default.** With a single ROI the
  whole-run section is a byte-identical copy of it, so `sim` is opt-in via
  `--toml-sim-stats`; `[meta].sim_stats` records which, so its absence is never
  ambiguous. (This is *not* `plain_printer`'s rule, which keys the same
  decision on `NUM_CPUS > 1`.)
- **Core/cache/legacy ratios are rounded to two decimals with their exact integer operands beside
  them** (`total_miss_latency_cycles` next to `miss_latency`, `total_branches` and
  `total_mispredicts` next to `mpki`), so rounding never loses information —
  recompute rather than trusting the rounded value.
- **An undefined ratio is `nan`**, a real TOML float, never a dropped key. The
  plain printer writes `-` for the same case; the old JSON collapsed NaN and
  infinity indistinguishably to `null`.
- It carries stats the JSON never did: the per-type **executed** branch census,
  `wq_full`, per-access-type **fill** counts, the DIB census
  (`dib_lookups`/`dib_hits`/`dib_misses`/`dib_hit_rate`), and `ipc`/`mpki`/
  `branch_prediction_accuracy`/`avg_cycles_per_mispredict`.
- `miss_latency` uses a **cache-wide** demand-fill denominator, not
  `plain_printer`'s per-CPU one; the two disagree once `NUM_CPUS > 1`.

It also records **what produced the document**, not just what was measured:

- `[meta]` carries `schema_version`, `num_cpus`, `sim_stats`, `build_id`,
  `warmup_instructions`, `simulation_instructions`, `trace_version`,
  `config_files` and `command_line`. `build_id` is `0x` plus a 16-hex-digit
  **FNV-1a hash of the effective configuration**, computed in
  `src/toml_printer.cc` — not a hash of a generated blob, because there is no
  generator any more. (std::hash was rejected: it is not stable across
  platforms or runs, so two machines could not compare build ids.) It is
  printed zero-padded because a digest may begin with a zero. `command_line` is
  `argv` joined verbatim; the shell has already expanded process substitution
  and globs, so it is a record of what the process received, **not** a
  re-runnable command.
- `[config]` is the **effective configuration**: every key the machine
  consulted during construction, with the value actually used — the override if
  one applied, else the built-in fallback. Component tables are lower-cased the
  same way the stats tables are, so `[config.cache.cpu0_l1d]` joins directly to
  `[phase.<name>.roi.cache.cpu0_l1d]` — CI asserts the two key sets are equal.

Three things about `[config]` are not obvious:

- **It is a configuration source, not just a record.** `--config` on a run's own
  `--toml` output reproduces that run's machine; the loader recognises the
  document by `[meta].schema_version` and reads its `[config]` table, ignoring
  the results. Pinned by `test/cpp/src/098-runtime-config.cc`.
- **It records what was *consulted*, so a key absent from it is a key nothing
  reads.** This is the same set validation uses: a user key never consulted is
  fatal. Conversely, a knob a module only reads when selected appears only when
  it is selected — which is why the hook contract requires a module to consult
  every knob it owns unconditionally.
- **`[config_override]` is what the configuration *sources* set**, not what
  differs from the default. Feed a full `[config]` back in and all ~160 keys
  appear there, correctly: the user did supply them. The exception is a key that
  lost a precedence contest to something outside the store: a
  `sim.heartbeat_frequency` beaten by an explicit `--heartbeat-frequency` is erased
  from the table (`src/main.cc:428`), because it did not take effect. `[config]`
  then reports the winner's value — `runtime_config::override_effective`
  (`src/main.cc:220`) records an effective value that came from neither the store
  nor the fallback, which is also how the native driver records the hash, revision
  and canonical path it read out of the YAML (`src/ramulator2_driver.cc:361-365`).

**Where the document goes is checked twice, and nothing is destroyed before the run
succeeds** — this is what stops a misplaced path from eating a trace.

Use `--toml result.toml -- trace...`. Nothing is written to or truncated in a
named output before the run has succeeded. After the trace count,
`champsim::output::plan` (`src/output_target.cc`) checks the file the kernel
reaches through the name, resolving links one at a time and never folding `..`
by text, so `missing/../x` is "cannot open" as in `open()`. It refuses an input
trace (by inode or canonical path), a directory, and an existing non-empty
regular file that does not begin with `# ChampSim statistics.`
(`toml_printer::document_signature`) or cannot be read to check that (an empty
unreadable file is accepted). That protects a trace the optional value swallowed
when one path too many is given, a `--config` source, and the YAML. What will be
opened after the run must open at startup: an existing regular file for writing
without `O_TRUNC`, a device or socket non-blocking, and a new name by an
`O_CREAT|O_EXCL` create that is unlinked again. A FIFO (or process substitution)
is not opened at startup, since that would consume the reader. The inode behind
stdout or stderr (`/dev/stdout`, `/proc/self/fd/1`, the log the shell redirected
to) receives the document on that stream after the plain report.

After a successful run the document is rendered to memory, and
`champsim::output::write` repeats the checks, since the filesystem may have
changed during the run; if they now refuse, nothing is written. It then writes
in place: an existing file is opened `O_TRUNC` without `O_CREAT` (which
`fs.protected_regular` refuses on another user's file in a sticky directory), and
a missing one, including one removed during the run, is created
`O_CREAT|O_EXCL` with mode 0666. So existing files keep their inode, hard links,
owner, group, ACLs and permissions, and new files get what the umask or a
default ACL gives any new file there. A failed open leaves the target untouched;
a failure after the truncating open can leave it empty or partial, and says so.
Every failure exits 1. The write is not atomic, so concurrent writers or readers
of one name can see an empty or mixed document. A check that sees the name
appear or vanish under it (another run's startup probe) repeats, up to 16
times, instead of refusing. `--knobs` never probes output paths. Full stdout with
unnamed `--toml` still contains progress/plain output before the TOML tail.

Tests are `test/cpp/src/099-toml-printer.cc`, which pin exact output via the
static `format()` seam; native JSON compatibility is also tested through its
stream output in `798-dram-plain-printer.cc`. `test/cpp/src/098-runtime-config.cc`
covers the store, including the statistics-document round trip. The output-target
rules above are `test/cpp/src/097-output-target.cc` (`plan`/`write` over scratch
directories, FIFOs, devices and unreadable files) and
`test/python/test_output_paths.py`, 27 cases that drive the built binary — trace
aliasing, descriptor names, ACLs and hard links, read-only directories, a target
replaced or removed mid-run, concurrent runs on one new name, stream targets. That
one runs under `make pytest`, takes the binary from `CHAMPSIM_BINARY` or
`bin/champsim`, and **skips silently** when neither exists.

## Conventions

- C++17 (only `ramulator2_driver.cc` uses C++20 in enabled builds), warnings-heavy (`global.options`: `-Wall -Wextra -Wshadow -Wpedantic -Wconversion`; optimization comes from `BUILD_MODE`).
  Modules additionally get `-Wno-unused-parameter -DCHAMPSIM_MODULE` (`module.options`).
- Invariant checks are `CHAMPSIM_ASSERT` (`inc/champsim_assert.h`), never `assert`.
  It is gated on `CHAMPSIM_ENABLE_ASSERTIONS` (default 1, so the standalone `tools/`
  harnesses keep it), is independent of `NDEBUG`, is `constexpr`-safe, evaluates its
  expression exactly once, and prints
  `ChampSim assertion failed: <expr> (<file>:<line>)` before `abort()`. `-DNDEBUG`,
  `-DCHAMPSIM_ENABLE_ASSERTIONS=...` and `-DCHAMPSIM_TEST_BUILD=...` are rejected from
  user flags as policy-owned macros (`config/build_config.py:103`), and the build
  re-reads the compiler's effective `-dM` macros to catch a forced include that flips
  one. Anything a *user* can trip — a configuration value, a corrupt or truncated
  compressed trace — must `throw` instead, because `main()` catches `std::exception`
  and prints `ERROR: ...` with exit 1: an unrepresentable `pmem.bank_rows` is a
  diagnostic, not a `terminate`.
- Formatting is enforced by `.clang-format` (LLVM base, 160 col); the `lint` job in
  `.github/workflows/main.yml` reformats `vcpkg.json src inc prefetcher branch
  replacement btb test tracer` **in place on push**. Note the asymmetry: `test` is
  formatted but is *not* in the job's `add:` list, so a test file's reformatting is
  never committed back and recurs on every push — run clang-format on new test files
  yourself. `.clang-tidy` configures the checks.
- `.commit-profile` at the repo root records this branch's commit conventions for the
  `git-commit` skill: `<component>: imperative summary` subjects, `make test` to verify,
  and the clang-format invocation above. Never add AI co-author or tool-attribution
  trailers to a commit message.
- Dependencies are vendored via vcpkg (`vcpkg.json`): CLI11, nlohmann-json, fmt, catch2,
  and the compression libs (bzip2, liblzma, zlib, zstd). Use `fmt` for output, not
  iostreams/printf.
- CI (`.github/workflows/`) builds across many GCC/Clang versions and macOS, runs the
  compile-only configs, and produces/validates the TOML stat document (`--toml=`).
