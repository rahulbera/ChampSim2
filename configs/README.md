# configs

Runtime configuration files, applied at startup with `--config <file>`. These
are the only configuration ChampSim has: there is no JSON, and `config.sh`
does nothing but discover modules.

- One `--config` may be followed by more, and by `--set key=value`; sources
  apply strictly in command-line order, and the last definition of a key wins.
- `bin/<executable> --knobs` lists every key that binary accepts with its
  baked default. Unknown keys are fatal at startup.
- Every simulation parameter is runtime-configurable: the
  `branch_predictor`/`btb`/`prefetcher`/`replacement` keys select any compiled
  module by directory name (one per kind), and a module's internal knobs live
  in a sibling table named after it (`[ooo_cpu.cpu0.basic_btb]`). The
  machine's shape is C++ (`src/static_environment.cc`) and
  `NUM_CPUS`/`BLOCK_SIZE`/`PAGE_SIZE` are in `inc/defs.h`; both are code edits
  plus a rebuild.
- A run's statistics document records the loaded files in
  `[meta].config_files` and every applied key under `[config_override]`.

`sample.toml` is a commented example and `lnc.toml` models Intel's Lion Cove;
both target the standard single-core component names (`ooo_cpu.cpu0`,
`cache.cpu0_l1d`, ...).

A run's statistics document is itself a configuration source: `--config
run.toml` on a previous run's `--toml` output reproduces that run's machine.
The loader recognises the document by `[meta].schema_version` and reads its
`[config]` table, ignoring the results. It reproduces only what is
configurable -- `[meta].build_id` must match too, or the binaries are
different machines.
