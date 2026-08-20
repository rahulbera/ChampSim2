# Runtime TOML configuration — phase 1 (tier-1 scalars)

Approved 2026-08-20. Phase 1 of the migration mapped in
`docs/runtime-config-map.md`: every tier-1 scalar becomes a runtime knob, set
from a TOML file (`--config`) and/or the command line (`--set`), with strict
left-to-right last-defined-wins precedence. `NUM_CPUS` / `BLOCK_SIZE` /
`PAGE_SIZE` stay compile-time; tier 3 (module selection, module-internal
geometry) is untouched; the topology stays configure-time.

## User-visible behavior

```
bin/champsim --config base.toml --set ooo_cpu.cpu0.rob_size=512 \
             --config override.toml --set cache.cpu0_l1d.sets=128 trace.xz
```

- `--config <file>` loads a TOML file into the runtime store; `--set key=value`
  sets one key. Both repeatable. Sources apply **strictly in argv order**; the
  last definition of a key wins, whether it came from a file or the flag.
- Key naming is the stats document's `[config]` language: `ooo_cpu.cpu0.rob_size`,
  `cache.cpu0_l1d.sets`, `ptw.cpu0_ptw.mshr_size`, `pmem.tcas`,
  `vmem.num_levels`, `sim.deadlock_cycle` — lower-cased exactly as
  `config/config_record.py` folds them. The two sections speak one naming
  language, but the emitted `[config]` is NOT directly feedable to `--config`:
  it sits under a `config.` root table and carries non-knob keys (names,
  wiring, module selection) that strict validation rejects. `--knobs` is the
  authoritative list of what a binary accepts.
- With no `--config`/`--set`, behavior is **identical** to today: the
  configure-time JSON values are the baked defaults.
- An unknown key, an unparseable file, or a type mismatch is fatal at startup,
  before construction, naming the offender. No warnings-and-continue.
- `--knobs` prints every runtime-overridable key for this binary with its baked
  default, then exits.

## Mechanism

**The store** (`inc/runtime_config.h`, `src/runtime_config.cc`): a flat
`std::map<std::string, value_type>` where `value_type` is
`std::variant<int64_t, double, bool, std::string>`. Mutators `load_file(path)`
(toml++, flattening nested tables into dotted keys) and `set("key=value")`;
last write wins by construction. Accessor `value<T>(key, fallback)` — `int64 →
double` coercion allowed, everything else strict; a mismatch throws with the
key name and main exits 1. The store names no generated symbol, so it links
into the test binary and is unit-tested directly. It records which keys were
consulted and which were applied, and the ordered file list, for provenance.

**Generated lookups**: `config/instantiation_file.py` emits, for every tier-1
scalar entry, a store lookup with the configure-time value as the default —
`.rob_size(cfg.value<long>("ooo_cpu.cpu0.rob_size", 352))` — instead of a bare
literal. The generated environment's constructor gains a
`const champsim::runtime_config& cfg` parameter. Wiring, module selection,
names, `_offset_bits`, and the three globals stay literal.

**Units rule**: the store holds values in the JSON's units; conversions Python
performs today move into the generated expression so overrides pass through the
same arithmetic — MHz → picoseconds with truncation
(`static_cast<long long>(1000000.0 / f)`, matching Python `int()`),
`refresh_period` ms → µs ×1000, `minor_fault_penalty` × the configure-time
clock period. Caveat, documented at the emission site: the vmem penalty and the
DBUS/MC period conversions use configure-time *derived* constants where the
derivation crosses components; overriding `pmem.frequency` changes the memory
controller clock but nothing recomputes cross-component derivations that
Python performed once.

**Channel queues**: `decorate_queues` already keys each channel's rq/wq/pq by
its lower component; the decoration gains the component's store prefix so the
generated channel constructors emit lookups too (`cache.cpu0_l1d.rq_size`).
The DRAM feeder channel's unbounded sizes stay literal.

**The manifest**: the generator emits, alongside `config_record<ID>`, a
`constexpr` array of every key it emitted a lookup for. Startup validation
checks every store key against it; `--knobs` prints it. A Python test
regex-extracts the keys from generated lookup lines and asserts set-equality
with the manifest, so the two cannot drift.

**Overridability rule (stated, accepted)**: a key is runtime-overridable iff
the generated code emitted a lookup for it, which requires the parsed
configure-time config to have carried a value (JSON or Python defaults). In
practice `parse.py` fills essentially every scalar, so coverage is near-total;
`--knobs` is the source of truth per binary.

**CLI order**: `--config`/`--set` use CLI11 `->trigger_on_parse()` so their
callbacks fire in argv order. Environment construction moves **after**
`CLI11_PARSE` (it currently precedes it); the `--hide-heartbeat` callback,
which walks `cpu_view()`, becomes a deferred flag applied post-construction.
Test build unaffected: every generated-symbol reference stays inside
`#ifndef CHAMPSIM_TEST_BUILD`.

**Sim-level knobs**: `sim.heartbeat_frequency` (default: the generated
constant), `sim.deadlock_cycle` (default 500), `sim.livelock_period` (default
10000000) join the store; `do_phase` receives them as plain values.

**Post-audit hardening** (2026-08-21): out-of-range `--set` numbers are
rejected (`errno` checks) instead of clamping; every store read in `main`,
including the `sim.*` knobs, is inside one try/catch so a bad value is a clean
exit 1, never an abort; frequencies and `pmem.data_rate` use
`positive_value()` so a zero or negative value cannot reach the clock-period
division; and a stored `sim.heartbeat_frequency` that lost to the explicit
`--heartbeat-frequency` flag is excluded from `[config_override]`.

**Two scalar exceptions stay configure-time** (discovered during
implementation): `wq_check_full_addr`, whose value also shapes the generated
channel constructors so a runtime override would desynchronize the two
consumers, and `vmem.randomization`, whose emitted form is an empty
`std::optional` when disabled — not expressible as a scalar lookup.

## Provenance

`[meta]` gains `config_files` (ordered, joined into one string per the
no-arrays rule). A new `[config_override]` table records exactly the keys the
runtime store applied with their effective values, so baked `[config]` +
`[config_override]` is a complete record. `command_line` already captures the
argv.

## Testing

- **C++** (`test/cpp/src/098-runtime-config.cc`): store semantics — last-wins
  across interleaved `load_file`/`set`, type coercion and strictness, dotted
  flattening, malformed input failures, manifest validation, provenance
  tracking. `099-toml-printer.cc`: `[config_override]` and `config_files`
  emission pins.
- **Python** (`test/python/test_instantiation_file.py` + new file): lookup
  emission per parts-dict entry, manifest completeness (regex cross-check),
  ctor signature.
- **End-to-end**: `configs/sample.toml` (checked in, commented) driven through
  a real build: no-config run byte-equivalent to the pre-change baseline on
  the same trace; `--config` changes an observable stat; `--set` after
  `--config` wins; `--config` after `--set` wins; unknown key exits 1.
- **CI**: the `stats_output` job gains a `--config configs/sample.toml --set …`
  run asserting `[config_override]` contents and the last-wins rule.

## Dependencies

`tomlplusplus` (header-only, MIT) added to `vcpkg.json` — used for **reading**
only; the hand-written writer stays (toml++ alphabetizes keys on output, which
is why it was rejected for writing). `tools/` builds are unaffected: nothing
under `tools/` includes the store.

## Out of scope (later phases)

Module selection registry (phase B), module-internal geometry, topology from
TOML, making the three globals runtime, removing config.sh's JSON layer.
