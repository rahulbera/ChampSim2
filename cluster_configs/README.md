# cluster_configs

One ChampSim config per file, each producing exactly ONE binary.

`cluster_run.py` resolves the binary a batch will run as the newest entry in
`bin/` after the build, so a config file that declares several executables is
ambiguous — the batch would silently run whichever landed last. One file, one
binary, one batch.

Every file is derived from `cbp6-runs/sweep_c.json` and differs from it only in
`branch_predictor` / `btb`; the microarchitecture is asserted identical at
generation time so a cluster run is comparable with the local campaign.
