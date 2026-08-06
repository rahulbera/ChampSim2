# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

ChampSim is a trace-based, cycle-level microarchitecture simulator (C++17) driven by a
Python configuration layer. A JSON config is compiled into generated C++ that is linked
against the fixed simulator core and pluggable modules (branch predictors, BTBs,
prefetchers, replacement policies).

## Build, configure, run

The build is **two-stage**: `config.sh` (Python) reads a JSON config and emits generated
sources + a makefile fragment; `make` then compiles. **You must re-run `config.sh` after
any change to a JSON config, or after adding/renaming a module**, before `make` will
reflect it.

```bash
git submodule update --init          # first-time only: fetch vcpkg
vcpkg/bootstrap-vcpkg.sh && vcpkg/vcpkg install   # first-time only: build deps

./config.sh champsim_config.json     # or no arg = default config; generates _configuration.mk + generated sources
make                                 # builds bin/<executable_name> (e.g. bin/champsim)

bin/champsim --warmup-instructions 200000000 --simulation-instructions 500000000 trace.champsimtrace.xz
```

Traces are `.champsimtrace` optionally `.xz`/`.gz`/`.bz2`/`.zst` compressed; a `-`/process
substitution stream also works (see the `json_output` CI job). Warmup/sim counts are
*instructions retired*; reported stats cover the simulation phase only.

Two record formats. v1 is the 64-byte `input_instr`; **v2 is a 512-byte
`input_instr_v2`** carrying physical addresses, access widths, an explicit branch type,
and the data value of every memory operand. Neither format is self-describing, so the
version is asserted on the command line:

```bash
bin/champsim --trace-version 2 --heartbeat-frequency 1000000 \
    -w 50000000 -i 200000000 trace.champsim2.zst
```

### Tests

```bash
make test                    # build + run the full Catch2 suite (test/bin/000-test-main)
make test TEST_NUM=091       # run only tests from files whose name starts with 091 (Catch2 filename tag)
make pytest                  # run the Python config-layer unit tests (test/python/, unittest)
test/bin/000-test-main --order rand --warn NoAssertions --invisibles   # how CI invokes it directly
```

C++ tests live in `test/cpp/src/NNN-name.cc`; the `NNN` prefix encodes the subsystem
(see `test/cpp/README.txt`: 100s=front-end, 200s=OoO core, 400s=caches, 700s=DRAM, etc.).
`test/config/compile-only/*.json` are configs that CI only builds, to catch config-layer
regressions.

### Cleaning / regenerating

- `make clean` — remove object/dep files and generated headers.
- `make configclean` — also remove `_configuration.mk`, legacy bridge files, and
  `compile_commands.json`.
- `make compile_commands` — regenerate `compile_commands.json` (per module/src/test) for clangd.

## Architecture

### Config-driven code generation (the non-obvious part)

There is no hand-written `main()` wiring of components. `config.sh` → `config/` package
(`parse.py`, `filewrite.py`, `instantiation_file.py`, `makefile.py`, `defaults.py`)
reads the JSON and **generates** the concrete instantiation: `src/core_inst.cc`,
`inc/champsim_constants.h`, `inc/cache_modules.h`, `inc/ooo_cpu_modules.h`, and
`_configuration.mk` (which defines `executable_name`, per-object flags, and module
object lists that the top-level `Makefile` `include`s). Multiple JSON files can be merged
(`--join product|chain`) to produce several executables in one pass. To change the
simulated machine (cache sizes, core width, number of cores, which module is active),
edit JSON and re-run `config.sh` — do **not** edit generated files.

By default `config.sh` compiles *all* modules found in the search paths
(`--compile-all-modules`, default on); the JSON only selects which one is *active* per
component. Extra module search roots: `--module-dir` / `--branch-dir` / `--btb-dir` /
`--prefetcher-dir` / `--replacement-dir`.

### Simulation model: `champsim::operable` + global clock

Every clocked component (`O3_CPU`, `CACHE`, `PageTableWalker`, `DRAM_CHANNEL`) derives
from `champsim::operable` (`inc/operable.h`) and implements `operate()`. `src/champsim.cc`
drives simulation: `main()` runs each `phase_info` (warmup / simulation), and `do_phase`
ticks a shared `champsim::chrono::clock` by the min clock period across all operables,
calling `operate_on(clock)` on each. Components communicate through `channel`s
(`inc/channel.h`) — bounded request/response queues — rather than direct calls, so the
memory hierarchy is a graph of operables wired by the generated instantiation.

Progress is monitored: `DEADLOCK_CYCLE` consecutive no-progress cycles trigger
`print_deadlock()` on every operable then `abort()`; a periodic livelock check warns/dies
on low IPC. When adding a component that can stall, implement `print_deadlock()`.

### Memory hierarchy & core

`O3_CPU` (`inc/ooo_cpu.h`, `src/ooo_cpu.cc`) models the OoO pipeline
(fetch→decode→dispatch→schedule→execute→retire, with ROB/LQ/SQ, a DIB decoded-instruction
buffer, and register renaming via `register_allocator`). Instructions come from
`tracereader` (`src/tracereader.cc`) filling `cpu.input_queue`. `CACHE` (`inc/cache.h`,
`src/cache.cc`) is the generic cache used for every level (L1I/L1D/L2C/LLC and TLBs);
`PageTableWalker` (`ptw.cc`) + `VirtualMemory` (`vmem.cc`) handle address translation;
`DRAM_CONTROLLER`/`DRAM_CHANNEL` (`dram_controller.cc`) model main memory. Stats are split
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

To add a module: create `branch/mypred/mypred.{h,cc}` (or the relevant kind), reference it
by name in the JSON (`"branch_predictor": "mypred"`), then `./config.sh <json> && make`.

**Legacy modules** (older free-function style, marked by a `__legacy__` file in the module
dir) are supported through generated `legacy_bridge.*` shims — the Makefile's `maybe_legacy_file`
machinery. New modules should use the class-based interface above; none of the shipped
modules currently use the legacy path.

### Support libraries

- `inc/msl/` — Module Support Library, safe to use from modules: `fwcounter` (saturating
  counter), `lru_table`, `bits`, `stat_methods`.
- `inc/util/` — internal helpers: `span`, `algorithm`, `bit_enum`, `units`, `detect` (SFINAE), etc.
- `champsim::address` (`inc/address.h`) — strongly-typed address type; prefer it over raw
  `uint64_t`. Many hooks have both `champsim::address` and legacy `uint64_t` overloads;
  the `uint64_t` ones are `[[deprecated]]`.

## This Branch (`rbdev`): CBP2025 Branch-Predictor Evaluation

Work specific to evaluating the CBP2025 (6th Championship Branch Prediction) submissions
inside ChampSim. Full write-up, with all numbers and figures, in
`docs/research-log/CBP6/`.

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

### Gotchas that cost real time here

- **`SET_ASIDE_CHAMPSIM_MODULE` is not re-entrant.** A module cannot simply
  `#include "ooo_cpu.h"`; do `#undef CHAMPSIM_MODULE` / include / `#define` at the
  *outermost* level. No shipped module includes `ooo_cpu.h`, so the path is untested
  upstream.
- **`CHAMPSIM_TRACE_MEMORY_VALUES=1` changes `ooo_model_instr`'s size in every
  translation unit.** It is build-wide, not per-target: mixing objects across the two
  settings is an ODR violation that produces wrong stats rather than a link error.
  Always `make clean` when switching it. It roughly doubles wall-clock, so it is off by
  default.
- **A "perfect predictor" reporting 0 MPKI proves nothing.** `branch/perfect_branch` +
  `btb/perfect_btb` read the outcome out of the trace at prediction time — the same field
  ChampSim's mispredict rule compares — so 0 MPKI follows structurally even from a corrupt
  trace. Validate trace metadata independently (a not-taken branch must be followed by its
  fall-through address).
- **Beware the arithmetic mean of per-trace percentage reductions.** Several SPEC traces
  have near-zero MPKI; one scored −286% off a 0.009 MPKI baseline and inverted a ranking.
  Pool the counts and take one ratio instead.

### Added to the core

Four branch-predictor hooks (`branch_predictor_final_stats`, `branch_execute_resolve`,
`branch_decode_notify`, `branch_execute_notify`), the `cycles_on_wrong_path` statistic
reported as **CycWPKI**, `--trace-version`, and a configurable, flushed
`--heartbeat-frequency`.

## Conventions

- C++17, warnings-heavy (`global.options`: `-Wall -Wextra -Wshadow -Wpedantic -Wconversion -O3`).
  Modules additionally get `-Wno-unused-parameter -DCHAMPSIM_MODULE` (`module.options`).
- Formatting is enforced by `.clang-format` (LLVM base, 160 col); the `lint` CI workflow
  reformats `src inc prefetcher branch replacement btb tracer` on push. `.clang-tidy`
  configures the enabled checks.
- Dependencies are vendored via vcpkg (`vcpkg.json`): CLI11, nlohmann-json, fmt, catch2,
  and the compression libs (bzip2, liblzma, zlib, zstd). Use `fmt` for output, not
  iostreams/printf.
- CI (`.github/workflows/`) builds across many GCC/Clang versions and macOS, runs the
  compile-only configs, and produces/validates JSON stat output (`--json=`).
