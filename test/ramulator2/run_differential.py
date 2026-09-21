#!/usr/bin/env python3
"""Run the hidden adapter differential campaigns of test 706 over YAML variants.

Each manifest run (see oracle_variants.py) is one subprocess of an enabled test
binary with DIFF_* environment variables. Every seed prints one summary line;
this script parses them, totals the counters, writes summary.json and exits
nonzero on any discrepancy: a FAIL line, a missing seed line, a nonzero exit,
a timeout, or (with --require-all-cores) a core that received no accepted
fragments.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

REPO = Path(__file__).resolve().parents[2]
TAGS = {"differential": "[.differential]", "recovery": "[.differential-recovery]"}
FIELD = re.compile(r"(\w+)=(\S*)")
# Keys whose campaign total is a maximum, not a sum.
MAXIMA = ("max_",)
# Per-run constants and text, reported but never totalled.
UNTOTALLED = ("yaml", "seed", "tx", "feeders", "wall_seconds")


def parse_value(text):
    if re.fullmatch(r"\d+", text):
        return int(text)
    if re.fullmatch(r"\d+(/\d+)+", text):
        return [int(part) for part in text.split("/")]
    return text


def parse_log(text):
    """Return one record per seed summary line: {"fields", "verdict", "message"}."""
    records = []
    for line in text.splitlines():
        if not line.startswith("seed="):
            continue
        head, separator, tail = line.partition(" wall=")
        if not separator:
            records.append({"fields": {}, "verdict": "FAIL", "message": f"malformed seed line: {line}"})
            continue
        fields = {key: parse_value(value) for key, value in FIELD.findall(head)}
        wall, _, verdict = tail.partition(" ")
        fields["wall_seconds"] = wall.rstrip("s")
        if verdict == "PASS":
            records.append({"fields": fields, "verdict": "PASS", "message": ""})
        elif verdict.startswith("FAIL"):
            records.append({"fields": fields, "verdict": "FAIL", "message": verdict[len("FAIL"):].strip()})
        else:
            records.append({"fields": fields, "verdict": "FAIL", "message": f"unrecognised verdict: {verdict}"})
    return records


def accumulate(totals, fields):
    for key, value in fields.items():
        if key in UNTOTALLED:
            continue
        if isinstance(value, int):
            totals[key] = max(totals.get(key, 0), value) if key.startswith(MAXIMA) else totals.get(key, 0) + value
        elif isinstance(value, list):
            prior = totals.get(key, [0] * len(value))
            if len(prior) != len(value):
                raise AssertionError(f"{key} changed length from {len(prior)} to {len(value)}")
            totals[key] = [a + b for a, b in zip(prior, value)]
    return totals


def summarize(records, expected_seeds, returncode, timed_out=False, require_all_cores=False):
    """Judge one run. Returns a dict with "ok", "discrepancies" and "totals"."""
    # Seed failures first: they name the broken check, an exit status does not.
    failures = [r for r in records if r["verdict"] != "PASS"]
    discrepancies = [f"seed {record['fields'].get('seed', '?')}: {record['message']}" for record in failures]
    if timed_out:
        discrepancies.append("timed out")
    elif returncode != 0:
        discrepancies.append(f"exit status {returncode}")
    if len(records) != expected_seeds:
        discrepancies.append(f"{len(records)} seed lines for {expected_seeds} seeds")
    totals = {}
    for record in records:
        accumulate(totals, record["fields"])
    if require_all_cores and records:
        by_cpu = totals.get("accepted_by_cpu")
        if not isinstance(by_cpu, list) or len(by_cpu) < 2 or not all(by_cpu):
            discrepancies.append(f"not every core had accepted fragments: accepted_by_cpu={by_cpu}")
    return {"ok": not discrepancies, "discrepancies": discrepancies, "seeds_passed": len(records) - len(failures),
            "seeds_failed": len(failures), "totals": totals}


def run_one(binary, run, campaign, args, output_dir, extra_env):
    log_path = Path(output_dir) / f"{campaign}-{run['name']}.log"
    env = dict(os.environ)
    env.update({"DIFF_YAML": run["yaml"], "DIFF_TX": str(run["tx"]), "DIFF_PERIOD": str(run["period"]), "DIFF_FEEDERS": str(run["feeders"]),
                "DIFF_SEEDS": str(args.seeds), "DIFF_SEED0": str(args.seed0), "DIFF_PARENTS": str(args.parents)})
    env.update(extra_env)
    command = [str(binary), TAGS[campaign]]
    started = time.monotonic()
    timed_out = False
    try:
        result = subprocess.run(command, cwd=args.cwd, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=args.timeout)
        output, returncode = result.stdout, result.returncode
    except subprocess.TimeoutExpired as expired:
        output = expired.stdout if isinstance(expired.stdout, str) else (expired.stdout or b"").decode(errors="replace")
        returncode, timed_out = None, True
    log_path.write_text(output)
    verdict = summarize(parse_log(output), args.seeds, returncode, timed_out, args.require_all_cores)
    verdict.update(name=run["name"], campaign=campaign, log=str(log_path), returncode=returncode, seconds=round(time.monotonic() - started, 2),
                   environment={k: v for k, v in env.items() if k.startswith("DIFF_")})
    return verdict


def load_runs(manifest_path, names):
    runs = json.loads(Path(manifest_path).read_text())["runs"]
    if names:
        wanted = names.split(",")
        unknown = sorted(set(wanted) - {run["name"] for run in runs})
        if unknown:
            raise SystemExit(f"unknown runs: {', '.join(unknown)}")
        runs = [run for run in runs if run["name"] in wanted]
    return runs


def campaign(binary, runs, campaigns, args, output_dir, extra_env):
    jobs = [(run, name) for name in campaigns for run in runs]
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results = list(pool.map(lambda job: run_one(binary, job[0], job[1], args, output_dir, extra_env), jobs))
    totals = {}
    for result in results:
        totals.setdefault(result["campaign"], {})
        accumulate(totals[result["campaign"]], result["totals"])
    return {"ok": all(r["ok"] for r in results), "runs": results, "totals": totals}


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--binary", required=True, help="enabled test binary (test/bin/000-test-main)")
    p.add_argument("--manifest", required=True, help="manifest.json written by oracle_variants.py")
    p.add_argument("--output-dir", required=True, help="new directory for logs and summary.json")
    p.add_argument("--campaign", default="differential", help="differential, recovery, or both separated by a comma")
    p.add_argument("--runs", default="", help="comma-separated manifest run names (default: all)")
    p.add_argument("--seeds", type=int, default=10)
    p.add_argument("--seed0", type=int, default=1)
    p.add_argument("--parents", type=int, default=10000, help="measured parents per seed (differential)")
    p.add_argument("--jobs", type=int, default=6)
    p.add_argument("--timeout", type=float, default=3600, help="seconds per run subprocess")
    p.add_argument("--env", action="append", default=[], help="extra KEY=VALUE for the test binary (e.g. DIFF_CYCLES=40)")
    p.add_argument("--require-all-cores", action="store_true", help="fail unless every core had accepted fragments")
    p.add_argument("--cwd", default=str(REPO), help="working directory for the test binary")
    return p


def main(argv=None):
    args = parser().parse_args(argv)
    campaigns = [name for name in args.campaign.split(",") if name]
    if not campaigns or any(name not in TAGS for name in campaigns):
        raise SystemExit(f"--campaign must name {' or '.join(TAGS)}")
    extra_env = dict(item.split("=", 1) for item in args.env)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=False)
    runs = load_runs(args.manifest, args.runs)
    result = campaign(Path(args.binary).resolve(), runs, campaigns, args, output_dir, extra_env)
    result["arguments"] = vars(args)
    (output_dir / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
    for run in result["runs"]:
        state = "ok" if run["ok"] else "DISCREPANCY: " + "; ".join(run["discrepancies"][:3])
        print(f"{run['campaign']} {run['name']}: {run['seeds_passed']} passed, {run['seeds_failed']} failed, {run['seconds']}s: {state}")
    for name, totals in result["totals"].items():
        print(f"{name} totals: {json.dumps(totals, sort_keys=True)}")
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
