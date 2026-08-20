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
substitution stream also works (see the `stats_output` CI job). Warmup/sim counts are
*instructions retired*; reported stats cover the simulation phase only.

Two record formats. v1 is the 64-byte `input_instr`; **v2 is a 512-byte
`input_instr_v2`** carrying physical addresses, access widths, an explicit branch type,
and the data value of every memory operand. Neither format is self-describing, so the
version is asserted on the command line:

```bash
bin/champsim --trace-version 2 --heartbeat-frequency 1000000 \
    -w 50000000 -i 200000000 --toml stats.toml trace.champsim2.zst
```

`--toml` writes the machine-readable statistics document (see below). Without it, only
the plain-text report goes to stdout.

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
reads the JSON and **generates** the concrete instantiation into **`.csconfig/`**
(`core_inst.inc`, `core_inst.cc.inc`, `module_decl.inc`, `legacy_bridge.*` — see
`config/filewrite.py`), plus `_configuration.mk` at the root (which defines
`executable_name`, per-object flags, and module object lists that the top-level
`Makefile` `include`s). `.csconfig/` is gitignored, so no generated source is ever a
staging hazard. (`make clean` still deletes `inc/champsim_constants.h` and friends;
those paths are vestigial — nothing writes them any more.) Multiple JSON files can be merged
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

## This Branch (`rbdev`): branch-prediction research

Four campaigns, each with a full write-up (every number, every figure, and the
disclosures) under `docs/research-log/`. Read the write-up before touching a campaign's
code — the constants in it were tuned, and the reasoning is not reconstructible from
the source.

| Campaign | Code | Write-up |
|---|---|---|
| CBP2025 conditional predictors | `inc/cbp6/`, `branch/cbp6_*` | `docs/research-log/CBP6/` |
| ITTAGE indirect target predictor | `inc/ittage/`, `btb/ittage_{32,64}kb` | `docs/research-log/CBP6/` |
| BLBP bit-level perceptron (clean-room) | `inc/blbp/`, `btb/blbp_64kb{,_tuned}` | `docs/research-log/BLBP/` |
| Headroom oracles | `inc/perfect_group/`, `btb/{perfect,ideal}_*` | both |

The oracle BTBs decompose target-prediction headroom by class (direct / indirect /
return), so a predictor's capture can be stated as a fraction of what is attainable
rather than as a raw MPKI. `cluster_configs/` holds one-binary-per-config JSONs for
cluster batch runs.

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

`tools/blbp_tune/` (trace-stream extractor, the `blbp_eval` objective binary, and the
hill-climbing tuner — plus `TUNING_NOTES.md`, which is how a paused campaign is resumed),
`tools/cbp6_replay/`, `tools/ittage_equiv/`.

Each has its own Makefile that compiles with **nothing but `g++ -I../../inc`** — no
vcpkg — so they build on a bare cluster login node. The consequence is a real
constraint: if a header they include ever starts pulling a vcpkg dependency, those
builds break on the machine where it is hardest to notice. Keep such code in `src/`.

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
reported as **CycWPKI**, DIB lookup/hit/miss counters, `--trace-version`, and a
configurable, flushed `--heartbeat-frequency`.

DIB accounting lives in `cpu_stats` (`inc/core_stats.h`) and is charged in
`O3_CPU::do_check_dib`, which runs once per instruction, gated by `dib_checked`. Only
`dib_hits`/`dib_misses` are stored; `dib_lookups()` is their sum, so the total cannot
drift from its parts. Two things about the numbers are easy to get wrong:

- **A hit means the window was already decoded, not that a neighbour shares it.** The
  DIB is filled at *decode* (`do_dib_update`), many cycles after `check_dib` has
  classified the whole fetch group, so a cold fetch group misses in its entirety —
  four instructions in one 16-byte window are four misses on the first pass, and hit
  only when that code runs again. `dib_misses` counts *instructions*, not distinct
  windows; it is not a code-footprint proxy. `test/cpp/src/141-dib-stats.cc` pins this.
- **The lookup count is not the retired instruction count.** The pipeline is *not*
  drained between phases (`do_phase` in `src/champsim.cc`), so a phase's lookups equal
  its retired count adjusted by the in-flight delta at both boundaries — measured at
  +72 on 400.perlbench and −405 on 657.xz. Close to `instructions`, but neither equal
  to nor an upper bound on it.

### The machine-readable stats document is TOML (`--toml`), not JSON

`src/toml_printer.cc` emits the statistics document; `--json` is **rejected at
startup** with an error pointing at `--toml`. `src/json_printer.cc` is still
compiled and linked so it cannot rot silently, but it is unreachable at run time
and therefore reports zero coverage. An unwritable `--toml` path is also
rejected at startup, and a failed write exits non-zero rather than reporting
success. The format differs from the old JSON in ways that matter to a parser:

- **`lower_snake_case` keys throughout**, including lower-cased component names
  (`cpu0_l1d`, `llc`). A configured name that is not a bare TOML key is quoted,
  never rewritten, so two distinct names can never collide onto one table.
- **No arrays.** Every key holds a single scalar; what were per-CPU arrays are
  now per-CPU tables (`…roi.cache.cpu0_l1d.cpu0`), so key names do not change
  with the core count. The document root is `[meta]` plus `[phase.<name>]`.
- **Only the region of interest is emitted by default.** With a single ROI the
  whole-run section is a byte-identical copy of it, so `sim` is opt-in via
  `--toml-sim-stats`; `[meta].sim_stats` records which, so its absence is never
  ambiguous. (This is *not* `plain_printer`'s rule, which keys the same
  decision on `NUM_CPUS > 1`.)
- **Every ratio is rounded to two decimals with its exact integer operands beside
  it** (`total_miss_latency_cycles` next to `miss_latency`, `total_branches` and
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

- `[meta]` carries `build_id`, `warmup_instructions`, `simulation_instructions`,
  `trace_version`, and `command_line`. `build_id` is `0x` plus the 16-hex-digit
  shake_128 digest that `config/filewrite.py` derives from the *whole* parsed
  config — so it identifies the configuration exactly, and is printed
  zero-padded because a digest may begin with a zero. `command_line` is `argv`
  joined verbatim; the shell has already expanded process substitution and
  globs, so it is a record of what the process received, **not** a re-runnable
  command.
- `[config]` is the parsed configuration the binary was generated from, rendered
  by `config/config_record.py` and embedded as a `champsim::configured::config_record<ID>`
  specialization in `.csconfig/core_inst.inc`. Component tables are lower-cased
  the same way the stats tables are, so `[config.cache.cpu0_l1d]` joins directly
  to `[phase.<name>.roi.cache.cpu0_l1d]` — CI asserts the two key sets are equal.

Three things about `[config]` are not obvious:

- **Module names are recovered from the config layer's internal `_*_data` keys,
  not the plain ones.** `cpu0_L1I` has no `replacement` key at all in the shipped
  config — the policy comes from `champsim::defaults::default_l1i` — so reading
  only the plain key would silently omit the most useful field. Other
  `_`-prefixed keys are dropped: `_offset_bits` is the C++ expression
  `champsim::lg2(64)` and `_defaults` names a C++ object.
- **It records what was *requested*, so C++-side defaults are absent.** The
  generated instantiation starts from `champsim::defaults::default_core` and
  overrides only what the config layer produced, so anything supplied purely by
  `inc/defaults.hpp` (e.g. the PTW's PSCL geometry) never appears.
- **The record may only be named inside `#ifndef CHAMPSIM_TEST_BUILD`.**
  `src/main.cc` *is* linked into the test binary (`$(call get_base_objs,TEST)`),
  where `CHAMPSIM_BUILD` expands to the non-literal `0xTEST`. `main.cc` therefore
  reads the blob and passes it to the printer as a `toml_printer::run_info`;
  `toml_printer.cc` names no generated symbol, which is what keeps it linkable
  into the tests and preserves the static `format()` seam.

Tests are `test/cpp/src/099-toml-printer.cc`, which pin exact output via the
static `format()` seam — the seam `json_printer` lacks, which is why it never
had tests — and `test/python/test_config_record.py`, which pins the rendering
rules and round-trips the shipped config through `tomllib`.

## Conventions

- C++17, warnings-heavy (`global.options`: `-Wall -Wextra -Wshadow -Wpedantic -Wconversion -O3`).
  Modules additionally get `-Wno-unused-parameter -DCHAMPSIM_MODULE` (`module.options`).
- Formatting is enforced by `.clang-format` (LLVM base, 160 col); the `lint` CI workflow
  reformats `champsim_config.json vcpkg.json src inc prefetcher branch replacement btb
  test tracer` **in place on push** — note `test/` is included, so a new test file gets
  reformatted by CI unless you run clang-format first. `.clang-tidy` configures the checks.
- `.commit-profile` at the repo root records this branch's commit conventions for the
  `git-commit` skill: `<component>: imperative summary` subjects, `make test` to verify,
  and the clang-format invocation above. Never add AI co-author or tool-attribution
  trailers to a commit message.
- Dependencies are vendored via vcpkg (`vcpkg.json`): CLI11, nlohmann-json, fmt, catch2,
  and the compression libs (bzip2, liblzma, zlib, zstd). Use `fmt` for output, not
  iostreams/printf.
- CI (`.github/workflows/`) builds across many GCC/Clang versions and macOS, runs the
  compile-only configs, and produces/validates the TOML stat document (`--toml=`).
