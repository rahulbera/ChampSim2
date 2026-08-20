# configs

Runtime configuration files, applied at startup with `--config <file>` --
unlike the JSON files at the repo root and in `cluster_configs/`, which
`config.sh` compiles into a binary, these change a binary's parameters
*without rebuilding*.

- One `--config` may be followed by more, and by `--set key=value`; sources
  apply strictly in command-line order, and the last definition of a key wins.
- `bin/<executable> --knobs` lists every key that binary accepts with its
  baked default. Unknown keys are fatal at startup.
- Only tier-1 scalars are runtime-configurable (see
  `docs/runtime-config-map.md`): the topology, the module selection
  (branch predictor / BTB / prefetcher / replacement), and
  `NUM_CPUS`/`BLOCK_SIZE`/`PAGE_SIZE` are still fixed at `config.sh` time.
- A run's statistics document records the loaded files in
  `[meta].config_files` and every applied key under `[config_override]`.

`sample.toml` is a commented example; it targets the standard single-core
component names (`ooo_cpu.cpu0`, `cache.cpu0_l1d`, ...).
