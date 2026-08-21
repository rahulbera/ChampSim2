# configs

Runtime configuration files, applied at startup with `--config <file>` --
unlike the JSON files at the repo root and in `cluster_configs/`, which
`config.sh` compiles into a binary, these change a binary's parameters
*without rebuilding*.

- One `--config` may be followed by more, and by `--set key=value`; sources
  apply strictly in command-line order, and the last definition of a key wins.
- `bin/<executable> --knobs` lists every key that binary accepts with its
  baked default. Unknown keys are fatal at startup.
- Tier-1 scalars AND module selection are runtime-configurable: the
  `branch_predictor`/`btb`/`prefetcher`/`replacement` keys select any compiled
  module by directory name (one per kind), and a module's internal knobs live
  in a sibling table named after it (`[ooo_cpu.cpu0.basic_btb]`). The
  topology and `NUM_CPUS`/`BLOCK_SIZE`/`PAGE_SIZE` are still fixed at
  `config.sh` time (see `docs/runtime-config-map.md`).
- A run's statistics document records the loaded files in
  `[meta].config_files` and every applied key under `[config_override]`.

`sample.toml` is a commented example; it targets the standard single-core
component names (`ooo_cpu.cpu0`, `cache.cpu0_l1d`, ...).
