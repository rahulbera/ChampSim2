# cluster_configs

The campaign arms, as runtime configurations against **one** binary.

`arms.toml` maps each arm to its BTB module; every arm shares
`cbp6_tagescl64` as its conditional predictor. A sweep is therefore one build
and eight `--set` values:

```bash
bin/champsim --set ooo_cpu.cpu0.branch_predictor=cbp6_tagescl64 \
             --set ooo_cpu.cpu0.btb=<arm> trace.champsimtrace.xz
```

This replaced eight per-arm JSON files and the eight binaries they built.
Before the JSONs were removed, each arm was verified to reproduce its
dedicated binary's statistics exactly through `--set` — all 237 leaves, on
400.perlbench, including the oracle BTBs and the ITTAGE and BLBP modules.
