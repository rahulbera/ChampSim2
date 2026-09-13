# configs

Runtime configuration files, applied at startup with `--config <file>`. These
select simulator parameters and modules. There is no JSON configuration layer,
and `config.sh` only discovers modules. The optional native DRAM backend reads its
device configuration from an exported YAML file selected by TOML.

- One `--config` may be followed by more, and by `--set key=value`; sources
  apply strictly in command-line order, and the last definition of a key wins.
- `bin/<executable> --knobs` lists every key that binary accepts with the
  value the current invocation would use. Its output is a valid TOML document
  (the module list is commented), so `--knobs > my.toml` gives a complete
  starting configuration. Unknown keys are fatal at startup.
- Every simulation parameter is runtime-configurable: the
  `branch_predictor`/`btb`/`prefetcher`/`replacement` keys select any compiled
  module by directory name (one per kind), and a module's internal knobs live
  in a sibling table named after it (`[ooo_cpu.cpu0.basic_btb]`). The
  machine's shape is C++ (`src/static_environment.cc`) and
  `NUM_CPUS`/`BLOCK_SIZE`/`PAGE_SIZE` are in `inc/defs.h`; both are code edits
  plus a rebuild.
- A run's statistics document records the loaded files in
  `[meta].config_files` and every applied key under `[config_override]`.

`sample.toml` is a commented example, `lnc.toml` models Intel's Lion Cove, and
`champsim_config.toml` is the pre-migration baseline -- the deleted
`champsim_config.json` in the runtime key language, verified to reproduce that
JSON build bit-for-bit. All three target the standard single-core component
names (`ooo_cpu.cpu0`, `cache.cpu0_l1d`, ...).

Use `champsim_config.toml` for anything that must compare against a
pre-migration number. It pins `ooo_cpu.cpu0.branch_predictor = "bimodal"` on
purpose: the baked default changed with the migration, so a run that drops that
key is a different machine.

A run's statistics document is itself a configuration source: `--config
run.toml` on a previous run's `--toml` output reproduces that run's machine.
The loader recognises the document by `[meta].schema_version` and reads its
`[config]` table, ignoring the results. It reproduces only what is
configurable -- `[meta].build_id` must match too, or the binaries are
different machines.


## Native memory example

`ramulator2.toml` selects native memory in a build made with
`WITH_RAMULATOR2=1 RAMULATOR2_ROOT=/absolute/path/to/ramulator2`:

```toml
dram-model = "ramulator2"

[ramulator2]
config = "configs/ramulator2/ddr4.yaml"
```

Use `bin/champsim --config configs/ramulator2.toml --trace-version 2 -w 100000
-i 500000 --toml run.toml -- trace.champsim2.zst` from the repository root. YAML
paths resolve relative to the process working directory, not the TOML file.
`ramulator2/ddr4.py` and `ramulator2/lpddr5.py` are readable configuration sources;
the adjacent YAML files are their fully expanded exports for the pinned native
revision. Re-export from source without installing the native Python extension:

```bash
PYTHONPATH=/absolute/path/to/ramulator2/python python3 -m ramulator export \
  configs/ramulator2/ddr4.py -o configs/ramulator2/ddr4.yaml
```

Export requires Python 3.10+ and PyYAML; the regression tools need Python 3.11+
for `tomllib`. Both fixtures use External, GenericDRAM and CacheLineInterleave:

| Fixture | Native controller | Native transaction | Clock period | Capacity |
| --- | --- | ---: | ---: | ---: |
| DDR4 | GenericDDR | 64 B | 833 ps | 8 GiB |
| LPDDR5 | LPDDR5 | 32 B | 1,453 ps | 1 GiB |

Other exports must keep that frontend, memory system and channel mapper. Each
controller `impl` must be `GenericDDR`, `LPDDR5`, `LPDDR6`, `GDDR7`, `HBM12`,
`HBM34` or `PRAC`. Each controller `addr_mapper` must be `RoBaRaCoCh`,
`ChRaBaRoCo` or `MOP4CLXOR`. `RITAddrMapper` is accepted only when its nested
`addr_mapper` is one of those three and `reserved_rows_per_bank` is absent or 0.
Other components fail with a configuration error naming the rejected `impl`.
The error comes before any native component is constructed. Examples are
`BlockHammer`, which needs Ramulator's own BHO3 CPU frontend, and
`PassThroughAddrMapper`, which expects a frontend to fill the address vector.
An admitted component can still fail the geometry and timing checks.

A 64-byte cache block becomes two LPDDR5 transactions; it returns only after both
complete. A native transaction larger than a block can serve separate block
requests within that transaction. Homogeneous multi-channel configurations are
supported. Mixed capacities, periods, or transaction sizes are rejected.

`dram-model` defaults to `legacy`. Do not combine native selection with `pmem.*`
keys from `sample.toml`/`lnc.toml` or a full legacy `--knobs` dump: inactive keys
are errors. Native `--knobs` reports only the selected backend's keys. For example:

```bash
bin/champsim --config configs/ramulator2.toml --knobs > native-knobs.toml
bin/champsim --config native-knobs.toml --trace-version 2 -w 100000 -i 500000 \
  --toml run.toml -- trace.champsim2.zst
bin/champsim --config run.toml --trace-version 2 -w 100000 -i 500000 \
  --toml replay.toml -- trace.champsim2.zst
```

Schema 2 archives the exact input YAML in `meta.ramulator2.yaml`, with canonical
absolute path, `config_hash`, pinned `revision`, and build provenance. Effective
`ramulator2.config_hash` participates in `meta.build_id`. A replay still reads the
file at its effective path; a missing or changed file fails before simulation.
To relocate an unchanged YAML, override `ramulator2.config` after `--config`.
Original supplied path spelling remains in `config_override`. Run lengths and
trace format are command-line inputs, so repeat them when replaying.

Use a named output followed by `--` before input paths. Output paths equivalent
to an input trace (including symlinks and hardlinks) are rejected at startup, and
so is an existing non-empty file that does not begin like a ChampSim statistics
document: a trace an optional `--toml` value consumed, a `--config` source or the
native YAML. A regular output file is replaced only once the run succeeds, so a
failed in-place replay (for example `--config run.toml --toml run.toml` after a
`config_hash` mismatch) leaves `run.toml` unchanged.
An omitted `--toml` filename appends the TOML document after ordinary stdout;
use a named file when a standalone parseable document is needed. See the
[validation record](../docs/ramulator2-validation.md) for native statistics and
phase semantics.
