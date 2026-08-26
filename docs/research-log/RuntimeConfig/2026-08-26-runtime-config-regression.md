# The Runtime-Configuration Migration Is Behaviourally Identical To The JSON Machine: 4,740 Statistic Leaves, Zero Differences

**Date:** 2026-08-26 · **Branch:** `rbdev-runtime-config` · **Authors:** Rahul Bera and Claude Opus 5

---

## 1. Key Idea

`rbdev-runtime-config` replaces ChampSim's JSON configuration layer and its code
generator with a hand-written machine (`src/static_environment.cc`) configured at
run time from TOML. The machine is no longer *generated* from the configuration;
it is C++, and the configuration only supplies its parameters. That is a large
change to make on faith. Before merging it back into `rbdev`, we wanted evidence
that the new machine simulates *the same thing* the generated one did.

Unit tests cannot settle this. `test/cpp/src/501-static-environment.cc` pins the
component set, the cache order and the list of configurable keys, but pinning the
list of knobs is not the same as showing that each knob still has the same effect
on a real workload. The question is empirical, so we answered it empirically.

## 2. Methodology

**Branches compared.** `rbdev` (`6b9603e7`, the JSON generator, built in a
separate worktree) against `rbdev-runtime-config` (`5ca0f7f6`). `rbdev` is a
strict ancestor — 39 commits behind, zero ahead — so the two differ only by the
migration.

**How both were configured.** The baseline is `champsim_config.json`, 147 leaves,
compiled into the `rbdev` binary by `./config.sh champsim_config.json`. To
configure the other branch identically we did **not** hand-write a TOML. We ran
the `rbdev` binary, took its own `[config]` record — the effective configuration
it reports in its statistics document — and kept the 159 keys that are runtime
knobs on the new branch. Both machines are therefore set from *the same numbers*,
not from each side's defaults.

The 34 keys we dropped are the ones the new branch has no knob for, and they
divide cleanly: 29 are structural (component names, `lower_level` and
`lower_translate` wiring — the channel graph, which is now code), 4 are
compile-time or build-time (`block_size`, `page_size` and `num_cores`, confirmed
to match `inc/defs.h`, plus `executable_name`), and `heartbeat_frequency` was
renamed to `sim.heartbeat_frequency` with the same value. The new branch's post-construction validation makes a silent
mismatch impossible: any supplied key that nothing consults is fatal, so a
mistyped or renamed key aborts rather than falling back.

Two configurations were run:

- **Config 1** — the baseline `champsim_config.json` unchanged.
- **Config 2** — every one of the four module kinds moved off its `inc/defs.h`
  default (`gshare`, `ittage_64kb`, `ip_stride`/`next_line` prefetchers,
  `srrip`/`drrip`/`ship` replacements), prefetching active at every cache level,
  a non-default `prefetch_activate` set, changed geometry (ROB, DIB, L1D, L2C and
  LLC), non-uniform clocks (core 3800 MHz against 4000 elsewhere) and an integer
  `vmem.randomization` seed. Config 1 alone leaves too much dark: 124 of 237
  statistic leaves are structurally zero when every prefetcher is `no`, and three
  of the four module-registry paths are skipped because their selection *is* the
  default. Config 2 exists to light those up.

**Workloads.** Ten distinct traces from
`qemu-tracing/images/champsim_out` (nine SWE-agent application traces plus a
second ripgrep window), 10M warmup and 50M simulation instructions each, v2 trace
format. 600M instructions per branch per configuration.

**How results were compared.** Both branches emit the same TOML statistics
document, so the comparison is leaf-by-leaf over the `[phase.*]` section — 237
leaves per run — plus a byte-for-byte hash of that whole section. The comparator
is numeric rather than textual (the new branch writes `4000.0` where the old
writes `4000`) and treats `nan == nan` as equal, because an undefined ratio is a
real value in this format and naive comparison reports it as a difference.
`[meta]` is excluded: `build_id` and `command_line` must differ.

### Commits Covered

- `f8182329` — delete the JSON configuration layer
- `f94cb58d` — restore the two parameters the migration dropped
- `2563bdd4` — recover from a branch switch that changes the module set
- `5ca0f7f6` — initialise `drrip`'s `brrip_counter` before it is read
- Full range: `git log rbdev..rbdev-runtime-config` (39 commits)

## 3. Key Results

1. **The two machines are bit-identical on every trace, in both configurations.**
   4,740 statistic leaves compared (10 traces × 237 leaves × 2 configurations),
   **zero differing**; 3,180 shared configuration leaves, zero differing. The
   `[phase.*]` section of each document hashes the same — this is byte equality,
   not agreement to two decimal places.

2. **The study found one real bug, and it was not a regression.** Config 2
   initially differed on all ten traces. Bisecting — first ruling out clocks,
   then module kinds, then individual caches — isolated it to `drrip` on the L2C.
   `drrip::brrip_counter` had no initialiser and was never set by the
   constructor, yet `update_brrip` increments it and tests it against
   `BRRIP_MAX`; the module reads an uninitialised value whose garbage sets the
   phase of BRRIP's one-in-32 insertion. `drrip.h` is byte-identical on both
   branches, so the bug predates the migration. The new branch merely exposes it
   by constructing the module later (in `install_replacement_module`) on
   differently conditioned memory. Zeroing the member took the isolated case from
   73 differing leaves to 0 and the ten-trace sweep to byte-identical.

3. **The runtime path costs about 1% of wall clock.** Measured sequentially on an
   idle machine, alternating branches, three repetitions: 101.33 s mean against
   100.24 s, a 1.1% penalty. The ranges overlap — the new branch was faster in one
   of the three pairs — so 1% is an upper bound, not a measured slowdown. The
   parallel sweep timings are contended and must not be read as a performance
   comparison.

4. **Config 2 raised live coverage from 113 to 152 of 237 leaves** and exercised
   all four registry paths. This matters for how much the all-clear is worth:
   under Config 1 only the branch-predictor registry path is taken, because
   `basic_btb`, `no` and `lru` *are* the defaults and `select_modules` skips the
   registry for them.

5. **Trace-set warning.** `swe_agent_w0000*` in that directory are hardlinks of
   `prometheus__prometheus-15142_w0000*` — same inode, same bytes. The directory
   holds 36 distinct traces under 40 names. An unwary sweep silently double-counts
   one workload; ours did, until we checked inodes and substituted a real tenth.

## 4. Confidence And Limits

Confidence is high for the configuration space actually swept, and the mechanism
supports it: ChampSim is deterministic given a trace and a configuration, nothing
seeds from the clock, and the only randomness — page-frame shuffling — is seeded
by `vmem.randomization`, which was an explicit integer on both sides. A
difference would therefore have been a defect, not noise. Both configurations
supply all 159 keys explicitly, so both machines were driven from identical
numbers rather than from agreeing defaults.

What this does **not** establish: multi-core (`num_cpus > 1` is a separate
binary; equivalence there was shown separately at `225930b8`), v1 traces, the
`--set` and `--knobs` paths, and error handling. One asymmetry is known and
untested here: when a cache omits `rq_size`/`wq_size`/`pq_size`, the old
generator substitutes a single `_queue_factor` for all three while the new branch
falls back per queue to the builder default. Both configurations supply those
keys explicitly, so the divergence cannot bite — but a comparison run from a
*partial* TOML would hit it and should not be read as a regression.

## 5. Next Steps

1. **Fast-forward `rbdev` to `rbdev-runtime-config`.** The evidence supports it
   and the merge is a fast-forward, so there is no conflict risk.
2. **Consider whether `drrip`'s fix should propagate upstream** — the bug is in
   stock ChampSim, not in this fork's additions.
3. **Re-run this comparison if the channel graph is ever edited.** The method is
   cheap now that the harness exists, and the graph is the part of the machine
   that unit tests pin only structurally.

## 6. References

[1] Study artefacts: `/home/rbera/work/bpeval/regression-2026-08-26/` — both
    sweeps' statistics documents, the derived configurations, `compare.py`, the
    bisect variants and the timing measurements.

[2] `CLAUDE.md`, "The machine-readable stats document is TOML" and "Gotchas that
    cost real time here" — the `nan`, `_queue_factor` and default-predictor traps
    that this methodology had to avoid.

[3] `test/cpp/src/501-static-environment.cc` — pins the component set, the cache
    order and the list of configurable keys; this study supplies the behavioural
    half that the pin cannot.
