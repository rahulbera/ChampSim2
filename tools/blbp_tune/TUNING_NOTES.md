# BLBP Tuning Campaign — State, Learnings, And How To Resume

**Status (2026-08-14):** paused at generation 35/40, NOT converged by its own
stop criterion (stall was 0 — still improving ~0.1–0.2% per generation). Paused
because another user queued ~19K jobs and per-generation queue latency exploded.
The generation-35 best is **frozen and committed** as `btb/blbp_64kb_tuned/`
(commit `4e007021`) and is being validated with full ChampSim runs; a resumed
campaign that beats it should produce `blbp_64kb_tuned_v2`, not overwrite it.

## Where Everything Lives

| What | Where |
|---|---|
| Campaign workspace | `kratos2:/home/rahbera/blbp_tune/` |
| Checkpoint (resume from this) | `tuner_state.json` — generation 35, stall 0, all 8 chains |
| Trajectory + accept logs | `tuner.log`, `gens/gen*/summary.json` (763 MB total; the `cand_*.{cfg,out}` under `gens/` can be deleted to save space — `summary.json` and `tuner_state.json` carry everything needed) |
| Training streams | `streams/` — 40 `.stream` + `.meta.json` (5.0 GB). TEST TRACES WERE NEVER EXTRACTED; keep it that way |
| Objective binary | `blbp_eval` (built on the cluster — do NOT rsync a local build over it; glibc mismatch, see Traps) |
| Frozen config | `tuned_best.{json,cfg}`, mirrored in `btb/blbp_64kb_tuned/` |

## How To Resume

```bash
ssh kratos2
cd /home/rahbera/blbp_tune
# 1. raise the generation cap (35 of 40 are used; give it real headroom)
sed -i 's/^MAX_GENERATIONS = 40$/MAX_GENERATIONS = 80/' tuner.py
# 2. resume -- picks up all 8 chains and the stall counter from the checkpoint
setsid nohup python3 tuner.py --base /home/rahbera/blbp_tune --resume \
    > tuner_resume.log 2>&1 < /dev/null &
```

Check queue pressure first: `squeue -p cpu_part -h -o "%u" | sort | uniq -c |
sort -rn | head`. Each generation is one 992-task array (~7 min/task); on an
idle cluster a generation is ~10 min end to end.

## Trajectory (Pooled Train Indirect Mispredicts)

Paper constants: **2,339,594** (4.11% of 56.9M measured indirects).

```
gen  1: 2,171,684   gen  5: 2,069,593   gen 10: 2,022,934   gen 15: 1,995,495
gen 20: 1,958,430   gen 25: 1,950,552   gen 30: 1,943,663   gen 35: 1,929,199
```

−7.2% in the first generation alone, −14.5% by gen 15, **−17.5% at gen 35**.
The curve is still descending; extrapolating the last 10 generations suggests
maybe another 1–2% is available, not more, without widening the search space.

## What The Tuner Learned (Generation-35 Optimum)

```
              paper                          tuned
intervals     (0,13)(1,33)(23,49)(44,85)    (0,9)(1,17)(7,26)(28,45)
              (77,149)(159,270)(252,630)    (54,71)(159,160)(380,486)
transfer      2 4 6 8 11 14 18 24           1 2 5 8 11 14 18 27
theta_init    14                            16
```

1. **Every interval compressed toward recent history.** The paper's spans sum
   to ~600 bits of coverage; the tuned set covers ~200. Our workloads —
   agentic dispatch loops especially — correlate on much shorter conditional
   history than the CBP-5 traces the paper tuned on.
2. **Interval 6 collapsed to a single bit (159,160)** in EVERY chain — all
   eight found this independently, the strongest cross-chain signal in the run.
   A single conditional-history bit at depth ~159 carries real information;
   worth understanding *which* branch that tends to be in our traces before
   trusting it as more than a statistical artifact.
3. **The deep interval split into two basins**: chains landed on either
   (380,486) or (~170,~340) — both beat the paper's (252,630), and the
   (380,486) basin is currently ahead by ~0.5%. A resumed campaign should watch
   whether the (170,340) basin overtakes; if the two stay within noise, the
   choice is arbitrary and should be reported as such.
4. **Transfer curve steepened at both ends** (low weights damped 2→1, top
   amplified 24→27): more contrast between unconfident and confident
   sub-predictors.
5. **Chain agreement is tight**: all 8 finals within 1.0% (1.929–1.944M);
   chains 0/4/6 share the same basin as the winner. This looks like a real
   optimum for THIS search space, not noise.

## What The Search Space Deliberately Excluded

Resuming with the same space yields ≤~2%. The bigger wins, if wanted, require
widening — each of these invalidates iso-storage or the equivalence-to-shell
claim, so treat as a NEW config, not a resume:

- **M, K, N, IBTB geometry** — size-bearing, frozen to keep the 64 KB budget.
- **The GHIST hash itself** (folded-XOR) — a documented assumption, never
  searched.
- **Local-history parameters** (256×10, target bit 3) — untouched.
- **Number of intervals** — fixed at 7; given the collapse of interval 6 to one
  bit, an 8th short interval might be free value.
- **selective/adaptive toggles** — left on throughout (the paper's ablation
  says both help; we never re-verified on our traces).

## Traps Hit (Do Not Rediscover)

1. **Slurm MaxArraySize = 1001** on kratos2 — arrays of 1280 are rejected;
   8×124 = 992 is the fit.
2. **`zstandard` is not installed** for the cluster python3.10 —
   `python3 -m pip install --user zstandard` (NFS home covers compute nodes).
3. **Do not rsync local binaries to the cluster** — local glibc 2.38 vs
   cluster 2.35; `blbp_eval` must be rebuilt there (`make -C tools/blbp_tune`).
4. **`pkill -f <pattern>` self-matches** the invoking SSH shell if the pattern
   appears in its command line. Kill by PID. (Third occurrence this project.)
5. **Completion checks must test content, not existence**: stream files exist
   from `open()`; only the `.meta.json` sidecar marks a finished extraction.
   Default `squeue` output truncates job names — never grep it for a suffix.
6. **Login-node timing lies** — `blbp_eval` over 40 streams took >4 min there
   vs ~7 min for a full compute-node task; always time on a compute node.

## Local Arm Outcome (2026-08-13)

The local-machine exploration arm (tuner_local.py, 4 chains x 24 workers, 36
generations on the 32-core dev box) finished at **1,925,333** -- 0.2% better
than the frozen 1,929,199, then eight consecutive stalled generations. That is
BELOW the 1% v2 bar, so the frozen generation-35 configuration stands and no
v2 shell was minted. Its best config is preserved at
`blbp_tune_local/local_best.json` on the dev machine.

Read together with the cluster arm's tight 8-chain agreement, this is decent
evidence the frozen config sits at a genuine local optimum of THIS search
space. Further gains need a widened space (see the exclusions above), not more
polishing.

## Interaction With The Frozen Evaluation

The generation-35 config is committed and its full-run validation + one-shot
test-set evaluation proceed regardless of any resumed tuning. If a resumed
campaign finds a materially better config, it gets a NEW shell + a NEW frozen
evaluation; the generation-35 numbers stay in the record as-is. Never re-run
the test set for the same config twice.
