#!/usr/bin/env python3
"""BLBP tuning campaign driver. Runs ON the cluster login node under nohup.

Parallel hill-climbing over the BLBP configuration space, sized to ~1400
simultaneous single-core Slurm jobs:

  - 8 independent chains, seeded from the paper's published constants with
    different RNGs (chain 0 mutates conservatively, later chains more wildly).
  - Per generation: each chain proposes MUTATIONS_PER_CHAIN single-knob
    mutations (one interval endpoint, or one transfer value kept monotone, or
    theta_init). All ~1280 candidates are evaluated in ONE sbatch array, each
    task = blbp_eval over all training streams (~90 s).
  - Each chain adopts its best strictly-improving candidate; every
    RESTART_PERIOD generations the two worst chains restart from the global
    best ("elite restart").
  - Stop: no global improvement for STOP_AFTER consecutive generations, or
    MAX_GENERATIONS.

The objective is POOLED indirect mispredicts over the TRAINING streams only
(counts, never a mean of per-trace percentages). Test traces are never
extracted into streams at all -- the train/test firewall is physical.

State is checkpointed after every generation to tuner_state.json; the full
propose/accept log lives in gens/gen*/summary.json, so the tuning trajectory
is auditable for overfitting after the fact.

Usage:
  python3 tuner.py --base DIR         # DIR holds streams/, blbp_eval
  python3 tuner.py --base DIR --resume
"""

import argparse
import copy
import glob
import json
import os
import random
import subprocess
import time

CHAINS = 8
MUTATIONS_PER_CHAIN = 124  # 8 chains x 124 = 992, under Slurm MaxArraySize=1001
RESTART_PERIOD = 5
STOP_AFTER = 3
MAX_GENERATIONS = 40
GHIST_BITS = 630
SBATCH_PARTITION = "cpu_part"

PAPER = {
    "M": 952,
    "theta_init": 14,
    "adaptive": 1,
    "selective": 1,
    "transfer": [2, 4, 6, 8, 11, 14, 18, 24],
    "intervals": [[0, 13], [1, 33], [23, 49], [44, 85], [77, 149], [159, 270], [252, 630]],
}


def write_cfg(cfg, path):
    with open(path, "w") as f:
        f.write(f"M {cfg['M']}\n")
        f.write(f"theta_init {cfg['theta_init']}\n")
        f.write(f"adaptive {cfg['adaptive']}\n")
        f.write(f"selective {cfg['selective']}\n")
        f.write("transfer " + " ".join(map(str, cfg["transfer"])) + "\n")
        for lo, hi in cfg["intervals"]:
            f.write(f"interval {lo} {hi}\n")


def mutate(cfg, rng, wildness):
    """One single-knob mutation. wildness scales step sizes per chain."""
    c = copy.deepcopy(cfg)
    kind = rng.choices(["interval", "transfer", "theta"], weights=[70, 20, 10])[0]
    if kind == "interval":
        i = rng.randrange(len(c["intervals"]))
        end = rng.randrange(2)  # 0 = lo, 1 = hi
        step = rng.choice([1, 2, 4, 8, 16, 32])
        step = max(1, int(step * wildness)) * rng.choice([-1, 1])
        lo, hi = c["intervals"][i]
        if end == 0:
            lo = max(0, min(lo + step, hi - 1))
        else:
            hi = max(lo + 1, min(hi + step, GHIST_BITS))
        c["intervals"][i] = [lo, hi]
    elif kind == "transfer":
        i = rng.randrange(len(c["transfer"]))
        c["transfer"][i] += rng.choice([-1, 1, 2])
        # keep monotone non-decreasing, positive, and bounded
        for j in range(len(c["transfer"])):
            c["transfer"][j] = max(1, min(c["transfer"][j], 63))
            if j > 0:
                c["transfer"][j] = max(c["transfer"][j], c["transfer"][j - 1])
    else:
        c["theta_init"] = max(1, min(c["theta_init"] + rng.choice([-2, -1, 1, 2]), 63))
    return c


def submit_generation(base, gen_dir, n_tasks, streams):
    """One sbatch array evaluating every candidate config; returns job id."""
    script = os.path.join(gen_dir, "eval.sbatch")
    with open(script, "w") as f:
        f.write(f"""#!/bin/bash
#SBATCH --partition={SBATCH_PARTITION}
#SBATCH --ntasks=1 --cpus-per-task=1 --mem=2G
#SBATCH --time=01:00:00
#SBATCH --array=0-{n_tasks - 1}
#SBATCH --output={gen_dir}/task_%a.log
cfg={gen_dir}/cand_${{SLURM_ARRAY_TASK_ID}}.cfg
out={gen_dir}/cand_${{SLURM_ARRAY_TASK_ID}}.out
exec {base}/blbp_eval "$cfg" "$out" {' '.join(streams)}
""")
    r = subprocess.run(["sbatch", "--parsable", script], capture_output=True, text=True, check=True)
    return r.stdout.strip().split(";")[0]


def wait_for(job_id, n_tasks, gen_dir, poll=30):
    while True:
        time.sleep(poll)
        q = subprocess.run(["squeue", "-j", job_id, "-h"], capture_output=True, text=True)
        done = len(glob.glob(os.path.join(gen_dir, "cand_*.out")))
        if not q.stdout.strip():
            return done
        print(f"  [{time.strftime('%H:%M:%S')}] {done}/{n_tasks} results in", flush=True)


def score_of(out_path):
    """Pooled mispredicts from a blbp_eval output; None if missing/corrupt."""
    try:
        with open(out_path) as f:
            for line in f:
                if line.startswith("TOTAL"):
                    return int(line.split()[2])
    except OSError:
        pass
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True)
    ap.add_argument("--resume", action="store_true")
    args = ap.parse_args()
    base = os.path.abspath(args.base)
    state_path = os.path.join(base, "tuner_state.json")

    streams = sorted(glob.glob(os.path.join(base, "streams", "*.stream")))
    assert streams, "no training streams found"
    print(f"training streams: {len(streams)}", flush=True)

    if args.resume and os.path.exists(state_path):
        state = json.load(open(state_path))
        print(f"resuming at generation {state['generation']}", flush=True)
    else:
        state = {
            "generation": 0,
            "stall": 0,
            "chains": [{"config": copy.deepcopy(PAPER), "score": None} for _ in range(CHAINS)],
            "best": {"config": copy.deepcopy(PAPER), "score": None},
        }

    while state["generation"] < MAX_GENERATIONS and state["stall"] < STOP_AFTER:
        gen = state["generation"]
        gen_dir = os.path.join(base, "gens", f"gen{gen:03d}")
        os.makedirs(gen_dir, exist_ok=True)

        # Candidate 0 of each chain is the chain's INCUMBENT (scored fresh in
        # gen 0 only; afterwards its score is retained and the slot reused for
        # a mutation).
        cands = []
        for ci, chain in enumerate(state["chains"]):
            rng = random.Random(hash((ci, gen, 0xB1B9)))
            wildness = 1.0 + ci * 0.5
            if chain["score"] is None:
                cands.append((ci, chain["config"]))
            for _ in range(MUTATIONS_PER_CHAIN - (1 if chain["score"] is None else 0)):
                cands.append((ci, mutate(chain["config"], rng, wildness)))

        for idx, (_, cfg) in enumerate(cands):
            write_cfg(cfg, os.path.join(gen_dir, f"cand_{idx}.cfg"))

        print(f"generation {gen}: {len(cands)} candidates", flush=True)
        job = submit_generation(base, gen_dir, len(cands), streams)
        got = wait_for(job, len(cands), gen_dir)
        print(f"  results: {got}/{len(cands)}", flush=True)

        improved_any = False
        accepts = []
        for ci, chain in enumerate(state["chains"]):
            best_idx, best_score = None, chain["score"]
            for idx, (owner, _) in enumerate(cands):
                if owner != ci:
                    continue
                sc = score_of(os.path.join(gen_dir, f"cand_{idx}.out"))
                if sc is None:
                    continue
                if best_score is None or sc < best_score:
                    best_idx, best_score = idx, sc
            if best_idx is not None:
                chain["config"] = cands[best_idx][1]
                chain["score"] = best_score
                accepts.append({"chain": ci, "cand": best_idx, "score": best_score})
                if state["best"]["score"] is None or best_score < state["best"]["score"]:
                    state["best"] = {"config": copy.deepcopy(chain["config"]), "score": best_score}
                    improved_any = True
            elif chain["score"] is not None:
                accepts.append({"chain": ci, "cand": None, "score": chain["score"]})

        # Elite restart: every RESTART_PERIOD generations, the two worst chains
        # restart from the global best.
        if gen > 0 and gen % RESTART_PERIOD == 0:
            ranked = sorted(range(CHAINS), key=lambda i: state["chains"][i]["score"] or 1 << 60, reverse=True)
            for i in ranked[:2]:
                state["chains"][i] = {"config": copy.deepcopy(state["best"]["config"]), "score": state["best"]["score"]}

        state["stall"] = 0 if improved_any else state["stall"] + 1
        state["generation"] = gen + 1
        json.dump(accepts, open(os.path.join(gen_dir, "summary.json"), "w"), indent=1)
        json.dump(state, open(state_path, "w"), indent=1)
        print(f"  best={state['best']['score']} stall={state['stall']}", flush=True)

    print(f"DONE: best score {state['best']['score']}", flush=True)
    json.dump(state["best"], open(os.path.join(base, "tuned_best.json"), "w"), indent=1)
    write_cfg(state["best"]["config"], os.path.join(base, "tuned_best.cfg"))


if __name__ == "__main__":
    main()
