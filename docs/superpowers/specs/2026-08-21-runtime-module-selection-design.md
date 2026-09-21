# Runtime module selection — phase B

Approved scope 2026-08-21 (single-module selection; registry + `configure()`
hook; prefetchers grow list support in a later phase). Builds on phase 1
(`2026-08-20-runtime-config-design.md`).

## User-visible behavior

```
bin/champsim --set ooo_cpu.cpu0.branch_predictor=hashed_perceptron \
             --set ooo_cpu.cpu0.btb=ittage_64kb \
             --set cache.cpu0_l1d.prefetcher=next_line trace.xz
```

- Four selection keys per component: `ooo_cpu.<cpu>.branch_predictor`,
  `ooo_cpu.<cpu>.btb`, `cache.<name>.prefetcher`, `cache.<name>.replacement`.
  The configure-time JSON choice is the baked default; every compiled module
  (`--compile-all-modules`, default on) is selectable by its directory name.
- **Single module only**: a comma in the value is a fatal error naming the
  rule. A baked multi-module pack stays intact unless overridden; overriding
  it replaces the whole pack with the one named module. (Prefetcher list
  support is the planned extension; the registry shape must not preclude it.)
- An unknown module name is fatal before simulation, listing the valid names.
- Module-internal knobs live in a **sibling table named after the module**
  (the selection key itself is a scalar, so TOML forbids using it as a table
  header): `[ooo_cpu.cpu0.basic_btb]` `sets = 2048`. A module receives them
  through an optional `configure()` hook.
- `--knobs` now probes against the *actual* runtime store, so it lists the
  selected modules' knobs, and it appends the registered module names per
  kind. A run with no `--config`/`--set` lists exactly what it did before.
- No-override runs stay bit-identical: the baked `model<Pack...>` pimpl is
  never touched unless a selection key differs from its default.

## Mechanism

**Post-construction pimpl swap — not a factory through the builders.** The
four concept types are nested in `O3_CPU`/`CACHE` and unnameable in the
builder headers, but the pimpl members are public, the model templates
construct tenants from the bare owner pointer, and no module hook fires
between environment construction and `op.initialize()` (`champsim::main`).
So the generated environment constructor body, after the member init-list has
built every component (addresses final: `build<>()` reserves then emplaces),
does per component:

```cpp
if (auto sel = cfg.value<std::string>("ooo_cpu.cpu0.branch_predictor", "bimodal"); sel != "bimodal") {
  cores.at(0).install_branch_module(champsim::configured::make_branch_module<CHAMPSIM_BUILD>(sel, &cores.at(0)));
}
```

`install_*_module` are four trivial named setters (self-documenting; the only
O3_CPU/CACHE API addition). Discarding the default pimpl is side-effect-free:
wrapper destructors are trivial, and the CBP6/BLBP/ITTAGE function-local
statics are constructed only by hook calls, which have not happened yet.

**The generated registry** (`.csconfig/registry.cc.inc`, compiled by a new
fixed TU `src/generated_registry.cc` on the `generated_environment.cc`
`__has_include` pattern): per build id, four factory functions
(`make_branch_module<ID>` etc., declared alongside `config_record<ID>` in
`core_inst.inc`) mapping every compiled module's name to
`std::make_unique<O3_CPU::branch_module_model<X>>(owner)`. An unknown name
throws listing the valid names; the throw surfaces through the construction
try/catch as a clean exit 1. Names are directory basenames, which equal the
class names for every shipped module (verified: 31/31, no cross-kind
collisions). Legacy (`__legacy__`) modules are skipped with a comment.
`Fragment.join` keys everything by build id, so `--join` builds get N
registries in one file. This TU includes every module header — a collision
surface that exists nowhere else; `perfect_indirect` already namespaces its
helpers after a prior collision, and CI's compile-only sweep is the guard.

**The `configure()` hook**: a module may implement
`void configure(const champsim::runtime_config& cfg, std::string_view prefix)`.
One new SFINAE trait per kind (probing exactly this one signature — the
existing double-probe traits show why only one), one virtual
`impl_configure` per concept, implemented in each model. The generated
constructor calls it after the pimpl is decided (swapped or baked), with
`prefix = <component>.<module name>`. A STANDING composed pack gets no
configure() — a shared prefix across the pack would collide knobs — but a
single module runtime-selected over one does, inside the swap branch (the
post-audit fix; the knob table would otherwise be fatally unconsumed). Hook
runs before
`initialize()` and before any prediction, so function-local-static state has
not latched. `runtime_config.h` includes only standard headers, so
referencing it from `modules.h` keeps the `tools/` bare-g++ constraint.

**Exemplar adoption — `basic_btb`**: the direct predictor's `sets`/`ways`
constants feed a runtime-sized `lru_table`; its `configure()` rebuilds the
table from `<prefix>.sets`/`<prefix>.ways` before first use. This is the
tier-2 "already runtime-shaped" item and makes BTB geometry sweeps
rebuild-free. BLBP adoption is deliberately deferred: its principal tuned
surface (`transfer`, `intervals`) is vector-valued and the store is
scalar-only — recorded as the follow-up that needs list values. ITTAGE and
CBP6 internals are unreachable by construction (constexpr policy classes,
vendored macro arrays); CBP6's getenv toggles stay getenv for comparability
with prior campaign runs.

## Validation

Three categories of user key, checked in `main`:

1. Generated manifest + `sim.*` — as phase 1, before construction.
2. Selection keys — in the manifest automatically (the regex harvests
   `value<std::string>` lookups); the *value* is validated by the registry
   during construction.
3. Module-knob tables — `<component>.<registered module name>.<knob>`:
   accepted before construction when the middle segment is a registered name
   of a kind valid for that component (generated per-kind name arrays);
   then, **after construction and before simulation**, any such key never
   consulted is fatal ("module X did not consume key Y — is it selected on
   that component?"). The hook contract therefore requires a module to
   consult every knob it owns unconditionally. This catches knobs aimed at a
   non-selected module and typo'd knob names, at the cost that per-knob
   typos surface after construction rather than before — the phase-1
   "before construction" wording is amended to "before simulation".

`champsim::main` is wrapped in the same try/catch as construction, so a
module throwing from `initialize()` (e.g. CBP6's `require_single_core`, a
`std::runtime_error`, when selected onto cpu>0) exits 1 instead of
`std::terminate`. Defensive: unreachable on the shipped single-core configs,
so it is verified by inspection, not by a test.

## Provenance

Selection and knob keys flow through the existing machinery: `[config]`
records the baked modules, `[config_override]` the runtime selections and
knobs that applied. Effective modules = `[config]` overlaid with
`[config_override]`, unchanged rule.

## Testing

- Python: registry emission (contents, per-kind name arrays, build-id
  keying, legacy skip), selection lookup + swap + configure emission shapes,
  manifest inclusion of selection keys (the old "branch_predictor absent"
  pin inverts), single-member-pack gating of configure emission.
- C++: `modules.h` configure dispatch (module with/without the hook);
  `basic_btb` configure geometry; store string-value semantics (done);
  swap-preserves-default behavior at the unit level where reachable.
- End-to-end on a real build: no-override bit-identity vs baseline;
  `--set` of each kind changes the expected stats signature (branch MPKI
  moves when the predictor changes); unknown module name lists valid names
  and exits 1; comma value rejected; unconsumed module-knob key fatal;
  `basic_btb` sets/ways override visibly changes BTB behavior; `--knobs`
  lists selected-module knobs and module names.
- CI: a `stats_output` step selecting a different predictor and asserting
  `[config_override]` plus a moved stat.

## Post-audit hardening (2026-08-21)

The registry fragment no longer includes `core_inst.inc` (guard-less; N joined
fragments made every multi-executable configure a redefinition error — the
fixed TU includes it once). A module runtime-selected over a composed baked
pack now receives `configure()` inside the swap branch. The store is
non-copyable, so a `configure(runtime_config, …)` taking the store BY VALUE
fails the trait instead of recording consultation on a discarded copy — the
unconsumed-knob error then names the module. `basic_btb`'s geometry knobs use
`positive_value` (`lru_table` validates sets but never ways; a zero-way table
silently never hits). The `--knobs` sim reads moved inside the try/catch.

## Out of scope

Prefetcher module lists (registry composes N pimpls — needs the composite
fold-parity work), BLBP `configure()` adoption (vector-valued store), any
`[config]`-section rewrite, upstream's `feature/runtime_config2` approach
(deletes the Python layer and SFINAE dispatch; incompatible with the CBP6
adapter and all four campaigns).
