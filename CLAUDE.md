# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

ChampSim is a trace-based, cycle-level microarchitecture simulator (C++17). On this
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
    -w 20000000 -i 50000000 --toml stats.toml trace.champsimtrace.xz
```

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
    -w 50000000 -i 200000000 --toml stats.toml trace.champsim2.zst
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

**Every parameter the deleted `champsim_config.json` carried is still settable** — 143
of its 147 leaf keys are runtime knobs, and the other four are compile-time by design
(`block_size`, `page_size`, `num_cores` in `inc/defs.h`; `executable_name` in the
makefile). `501-static-environment.cc` pins that list, so a parameter cannot quietly
stop being configurable; two had, and that test is why they were found.

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
construction. `basic_btb` is the exemplar (direct-predictor geometry). BLBP's
configure adoption is deferred: its tuned surface (`transfer`, `intervals`) is
vector-valued and the store is scalar-only. ITTAGE/CBP6 internals are unreachable by
construction (constexpr policy classes, vendored macros); CBP6's env toggles stay
`getenv` so that results already recorded with them stay comparable.

### Tests

```bash
make test                    # build + run the full Catch2 suite (test/bin/000-test-main)
make test TEST_NUM=091       # run only tests from files whose name starts with 091 (Catch2 filename tag)
make pytest                  # run the Python module-discovery unit tests (test/python/, unittest)
test/bin/000-test-main --order rand --warn NoAssertions --invisibles   # how CI invokes it directly
```

C++ tests live in `test/cpp/src/NNN-name.cc`; the `NNN` prefix encodes the subsystem
(see `test/cpp/README.txt`: 100s=front-end, 200s=OoO core, 400s=caches, 700s=DRAM, etc.).
`test/cpp/src/501-static-environment.cc` pins the hand-written machine: its component
set, the cache order (which is the per-cycle `operate()` order), and the channel-count
formula. There are no compile-only configs any more — the configuration layer they
guarded is gone.

### Cleaning / regenerating

- `make clean` — remove object and dep files. It also deletes
  `inc/champsim_constants.h` and `inc/cache_modules.h`; **those paths are vestigial**,
  nothing has written them for two migrations, and looking for what does is a dead end.
- `make configclean` — also remove `_configuration.mk`, `compile_commands.json`, and
  the two `.csconfig/registry*.inc` files. Note it also deletes every `.cache/` directory,
  so clangd needs `make compile_commands` afterwards.
- `make compile_commands` — regenerate `compile_commands.json` (per module/src/test) for clangd.

## Architecture

### Module discovery, and the machine that is not generated

There is no code generator and no hand-written `main()` wiring either. The machine is
`champsim::static_environment` (`inc/static_environment.h`, `src/static_environment.cc`):
one `operate()`-ordered component list built in a loop over `defs::num_cpus`, and a
**named channel graph** — one channel per edge, its queue geometry taken from the
*lower* component, which is why three separate channels feed the STLB. There are
`num_cpus * 12 + 1` channels: twelve per-core edges plus **one shared LLC→DRAM edge**
(shared because there is one LLC and one memory controller however many cores there
are — getting this wrong is invisible at one core and wrong at two).

Construction order is load-bearing and fixed by member declaration order: channels →
DRAM → vmem → PTWs → caches → cores, with every vector fully `reserve`d and filled
before a pointer into it is handed out. Cache order is the per-cycle `operate()` order:
`LLC` first, then each core's caches alphabetically (`DTLB, ITLB, L1D, L1I, L2C, STLB`).

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
  `#include "ooo_cpu.h"`; do `#undef CHAMPSIM_MODULE` / include / `#define` at the
  *outermost* level. No shipped module includes `ooo_cpu.h`, so the path is untested
  upstream.
- **`CHAMPSIM_TRACE_MEMORY_VALUES=1` changes `ooo_model_instr`'s size in every
  translation unit.** It is build-wide, not per-target: mixing objects across the two
  settings is an ODR violation that produces wrong stats rather than a link error.
  Always `make clean` when switching it. It roughly doubles wall-clock, so it is off by
  default.
- **A perfect cache reporting a 100% hit rate proves nothing**, for the same reason
  the perfect-predictor entry below gives: it follows structurally from the flag. The
  load-bearing checks are that the level below goes quiet (every `*_fill` for the
  perfect cache is zero) and that IPC actually moves. `test/cpp/src/446-perfect-cache.cc`
  pins both, and carries a non-perfect control scenario so the file would fail if the
  flag did nothing.
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
- **DRAM timings are memory-controller CYCLES, so `pmem.frequency` scales them.**
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
  dead headers. `DEPFLAGS` now carries `-MP`, which emits a phony target per header and
  makes the translation unit rebuild instead; `./config.sh && make` is therefore
  sufficient on its own. `make configclean` remains the belt-and-braces recovery.
- **Vendored ITTAGE has undefined behaviour in its RNG.** `inc/ittage/ittage.hpp:246,248`
  left-shift a negative `int` in `MYRANDOM`; UBSan flags it on any `ittage_64kb` run, so
  any results already recorded with that module were produced with it. GCC emits the
  expected two's-complement result, and casting through `unsigned` would be bit-identical, but it
  is vendored competition code — do not change it without deciding what happens to the
  recorded results. It is the *only* UB the simulator reports: ASan+UBSan across the
  shipped module set is otherwise clean.

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
  appear there, correctly: the user did supply them.

Tests are `test/cpp/src/099-toml-printer.cc`, which pin exact output via the
static `format()` seam — the seam `json_printer` lacks, which is why it never
had tests. `test/cpp/src/098-runtime-config.cc` covers the store, including the
statistics-document round trip.

## Conventions

- C++17, warnings-heavy (`global.options`: `-Wall -Wextra -Wshadow -Wpedantic -Wconversion -O3`).
  Modules additionally get `-Wno-unused-parameter -DCHAMPSIM_MODULE` (`module.options`).
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
