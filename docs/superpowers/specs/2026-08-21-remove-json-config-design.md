# Remove the JSON configuration layer — phase C

Approved 2026-08-21. Ends the two-config-file split left by phases 1 and B:
after this, a TOML file is the only configuration a user writes, and the
compile-time constants live in a header they edit like any other code.

Decided by the user, not open for redesign here:

- **No configurable topology.** The standard hierarchy is written in C++;
  changing it is a code edit. This is preferred to a script deriving it.
- **`NUM_CPUS` stays compile-time**, so multi-core is a separate binary.
  Development iterates on single core first, then moves to multi-core.
- **Upstream divergence is acceptable.** This fork is maintained privately.

## End state

```
inc/defs.h              NUM_CPUS / BLOCK_SIZE / PAGE_SIZE, and the runtime defaults
src/environment.cc      hand-written; loops over NUM_CPUS; ~150 lines
configs/*.toml          the only configuration file a user writes
config.sh               module discovery only: registry + Makefile object list
```

Deleted: `champsim_config.json`, `cluster_configs/*.json` (converted to TOML),
`test/config/compile-only/`, and the parameter/topology half of `config/` —
`parse.py`, `instantiation_file.py`, `filewrite.py`, `config_record.py`,
`defaults.py`, plus their Python tests. `modules.py`, `makefile.py`, `cxx.py`,
`util.py` survive; `module_registry.py` survives and becomes the main emitter.

## What the hand-written environment must reproduce

The generated constructor is 371 lines but only 16 component constructions;
the rest is repetition a loop collapses. The load-bearing part is the channel
graph — **one channel per edge**, its queue geometry taken from the *lower*
component, which is why identical-looking channels repeat (three separate
channels feed `cpu0_STLB`). For the standard single-core hierarchy, the
generator produces 13 channels:

| ch | lower ← upper | ch | lower ← upper |
|----|---------------|----|---------------|
| 0 | `cpu0_L1D` ← `cpu0_PTW` | 7 | `cpu0_PTW` ← `cpu0_STLB` |
| 1 | `DRAM` ← `LLC` | 8 | `cpu0_DTLB` ← `cpu0_L1D` |
| 2 | `cpu0_STLB` ← `cpu0_DTLB` | 9 | `cpu0_ITLB` ← `cpu0_L1I` |
| 3 | `cpu0_STLB` ← `cpu0_ITLB` | 10 | `cpu0_STLB` ← `cpu0_L2C` |
| 4 | `cpu0_L2C` ← `cpu0_L1D` | 11 | `cpu0_L1I` ← `cpu0` |
| 5 | `cpu0_L2C` ← `cpu0_L1I` | 12 | `cpu0_L1D` ← `cpu0` |
| 6 | `LLC` ← `cpu0_L2C` | | |

Fan-in per component: `cpu0_L1D` ← {0, 12}, `cpu0_L2C` ← {4, 5},
`cpu0_STLB` ← {2, 3, 10}; every other component has exactly one upper edge.
The DRAM feeder (ch 1) is unbounded (`numeric_limits<size_t>::max()` on all
three queues) with `lg2(BLOCK_SIZE)` offset bits; TLB-facing channels use
`lg2(PAGE_SIZE)`; `match_offset` is set on the L1s and the DTLB/ITLB edges.

The hand-written version expresses this as a named graph rather than index
arithmetic — the indices above are an artifact of generation, not a contract.
Construction order is fixed by member declaration order and must stay:
channels → DRAM → vmem → PTWs → caches → cores, with every vector fully built
before pointers into it are handed out (reallocation would dangle them).

**What is lost:** any configuration whose component set differs from the
standard hierarchy — an extra cache level (LNC's real L1.5, for instance), a
cache with two lower levels, or a differing per-core hierarchy in a multi-core
build. Accepted: such a change becomes a C++ edit.

## Validation and provenance

**Validation flips to post-construction.** There is no generated manifest, so
the rule becomes: construct, collect `consulted()`, and any user key never
consulted is unknown. This is uniform for scalars, module selections and
module knobs — today the first is checked before construction and the last
after, an asymmetry that disappears. Errors surface after construction rather
than before; already accepted for module knobs in phase B.

**`[config]` becomes the effective configuration.** With no JSON there is no
"baked" config to record. Instead the store records each consulted key's
*effective* value (the override if one applied, else the fallback), and that
is emitted as `[config]`. This is strictly more useful than phase 1's
baked-plus-overlay: one table, no overlay arithmetic, and — the thing the
phase-1 spec had to admit was false — a run's `[config]` becomes directly
feedable back as `--config`. `[config_override]` stays as the record of what
the user changed, which is now a proper subset.

This needs one store addition: `value<T>` currently records the *fallback*
in `consulted_`; it must record the value actually returned.

## Sequence

Each step is independently verifiable and committable.

**C1 — hand-written environment.** Add `inc/defs.h` and `src/environment.cc`;
keep the generated path alive and selectable so the two can be compared.
*Proof:* build both, run ≥2 traces (400.perlbench, 657.xz), and require the
`phase` section of the TOML and the plain-text report to be identical. This
is the same equivalence test that caught real bugs in phases 1 and B.

**C2 — validation and provenance.** Flip validation to post-construction;
make `[config]` the effective configuration. *Proof:* every failure path
still exits 1 with a message naming the offender (unknown key, unknown
module, comma value, unconsumed module knob, bad type, non-positive knob);
`[config]` of a no-override run equals the old `[config]`, and of an
overridden run equals old `[config]` overlaid with `[config_override]`.

**C3 — delete the JSON layer.** Remove the generated environment path, the
JSON files, and the dead half of `config/`; shrink `config.sh` to discovery.
*Proof:* full C++ and Python suites, `tools/` still builds with bare `g++`,
CI green, `--knobs` unchanged.

**C4 — convert the configs.** `cluster_configs/*.json` → TOML against one
binary; `configs/lnc.json` disappears and `configs/lnc.toml` stands alone.
*Proof:* each converted config reproduces its old binary's numbers on a
fixed trace.

## Risks

1. **A silently wrong channel parameter.** A wrong queue size, `offset_bits`,
   or `match_offset` changes timing without failing anything. The equivalence
   test in C1 is the only real guard; it must run before C3 removes the
   generated path to compare against.
2. **Construction/static-initialization order.** `inc/defaults.hpp`'s
   namespace-scope `const auto` objects read `LOG2_BLOCK_SIZE` during static
   init. Phase C makes this *better* — `defs.h` constants are
   constant-initialized and a hand-written environment can build defaults
   lazily — but the ordering must be re-verified, not assumed.
3. **Losing a knob silently.** Hand-writing ~150 lookups risks omitting one.
   Mitigation: a test asserting the hand-written environment's consulted-key
   set is a superset of the generated one's, run while both paths exist.

## Out of scope

Runtime topology, runtime `NUM_CPUS`/`BLOCK_SIZE`/`PAGE_SIZE`, prefetcher
module lists, BLBP's vector-valued knobs.
