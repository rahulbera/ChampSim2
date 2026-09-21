#!/usr/bin/env python3
"""Local-machine BLBP tuning: the v2 EXPLORATION arm.

Same hill-climbing as tuner.py but candidates run as local processes instead of
a Slurm array -- for polishing while the cluster is contended. Narrow
generations (one candidate per core), many of them.

DISCIPLINE: this arm never touches the frozen generation-35 configuration or
its evaluation. Its output is a CANDIDATE for blbp_64kb_tuned_v2, minted only
if it beats the frozen score by more than 1% -- below that, tuning noise and
test-set churn are not worth a second frozen evaluation.

Seeds from the cluster's tuner_state.json (all 8 chains), collapsed to 4 local
chains (best 4 of 8) to fit narrow generations.

Usage: python3 tuner_local.py --base DIR [--workers N] [--resume]
"""

import argparse
import copy
import glob
import json
import os
import random
import subprocess
import time

sys_path = os.path.dirname(os.path.abspath(__file__))
import sys

sys.path.insert(0, sys_path)
from tuner import PAPER, mutate, write_cfg, score_of  # noqa: E402  same search space, same formats

CHAINS = 4
STOP_AFTER = 8       # narrow generations accept less often; stall longer before stopping
MAX_GENERATIONS = 200
FROZEN_SCORE = 1_929_199  # generation-35 committed config; the bar is 1% below
V2_BAR = int(FROZEN_SCORE * 0.99)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True)
    ap.add_argument("--workers", type=int, default=26)
    ap.add_argument("--resume", action="store_true")
    args = ap.parse_args()
    base = os.path.abspath(args.base)
    state_path = os.path.join(base, "local_state.json")

    streams = sorted(glob.glob(os.path.join(base, "streams", "*.stream")))
    assert len(streams) == 40, f"expected 40 training streams, found {len(streams)}"

    if args.resume and os.path.exists(state_path):
        state = json.load(open(state_path))
        print(f"resuming local arm at generation {state['generation']}", flush=True)
    else:
        cluster = json.load(open(os.path.join(base, "tuner_state.json")))
        seeds = sorted(cluster["chains"], key=lambda c: c["score"] or 1 << 60)[:CHAINS]
        state = {
            "generation": 0,
            "stall": 0,
            "chains": [{"config": copy.deepcopy(c["config"]), "score": c["score"]} for c in seeds],
            "best": copy.deepcopy(cluster["best"]),
        }
        print(f"seeded from cluster gen {cluster['generation']}: best {state['best']['score']:,}", flush=True)

    per_chain = max(1, args.workers // CHAINS)
    while state["generation"] < MAX_GENERATIONS and state["stall"] < STOP_AFTER:
        gen = state["generation"]
        gen_dir = os.path.join(base, "gens", f"local{gen:04d}")
        os.makedirs(gen_dir, exist_ok=True)

        cands = []
        for ci, chain in enumerate(state["chains"]):
            rng = random.Random(hash((ci, gen, 0x10CA1)))
            wildness = 0.5 + ci * 0.5  # local arm polishes: gentler than the cluster arm
            for _ in range(per_chain):
                cands.append((ci, mutate(chain["config"], rng, wildness)))

        procs = []
        for idx, (_, cfg) in enumerate(cands):
            cfg_p = os.path.join(gen_dir, f"cand_{idx}.cfg")
            out_p = os.path.join(gen_dir, f"cand_{idx}.out")
            write_cfg(cfg, cfg_p)
            procs.append(subprocess.Popen(["nice", "-n", "10", os.path.join(base, "blbp_eval"), cfg_p, out_p] + streams,
                                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        for p in procs:
            p.wait()

        improved = False
        for ci, chain in enumerate(state["chains"]):
            best_idx, best_score = None, chain["score"]
            for idx, (owner, _) in enumerate(cands):
                if owner != ci:
                    continue
                sc = score_of(os.path.join(gen_dir, f"cand_{idx}.out"))
                if sc is not None and (best_score is None or sc < best_score):
                    best_idx, best_score = idx, sc
            if best_idx is not None:
                chain["config"], chain["score"] = cands[best_idx][1], best_score
                if best_score < state["best"]["score"]:
                    state["best"] = {"config": copy.deepcopy(chain["config"]), "score": best_score}
                    improved = True

        state["stall"] = 0 if improved else state["stall"] + 1
        state["generation"] = gen + 1
        json.dump(state, open(state_path, "w"), indent=1)
        marker = " *** V2 BAR CLEARED ***" if state["best"]["score"] <= V2_BAR else ""
        print(f"local gen {gen}: best={state['best']['score']:,} stall={state['stall']}{marker}", flush=True)

        # Keep the gens dir from growing unboundedly: candidates are cheap to
        # regenerate, the state file carries everything that matters.
        for f in glob.glob(os.path.join(gen_dir, "cand_*")):
            os.remove(f)
        os.rmdir(gen_dir)

    print(f"LOCAL ARM DONE: best {state['best']['score']:,} "
          f"({'v2 candidate' if state['best']['score'] <= V2_BAR else 'below the v2 bar; frozen config stands'})", flush=True)
    json.dump(state["best"], open(os.path.join(base, "local_best.json"), "w"), indent=1)


if __name__ == "__main__":
    main()
