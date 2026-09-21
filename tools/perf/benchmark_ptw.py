#!/usr/bin/env python3
"""Sequential single-core, legacy-only PTW/Hermes comparison (Python 3.11+)."""

import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import resource
import statistics
import subprocess
import threading
import time
import tomllib


def sha256(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def build_info(path):
    """Query once before timing; retained predecessor binaries may lack the CLI."""
    try:
        result = subprocess.run([str(Path(path).resolve()), "--build-info"], text=True,
                                capture_output=True, timeout=15)
    except (OSError, subprocess.TimeoutExpired) as error:
        return {"available": False, "reason": str(error)}
    if result.returncode:
        return {"available": False, "returncode": result.returncode, "diagnostic": result.stderr.strip()}
    try:
        data = json.loads(result.stdout)
    except json.JSONDecodeError:
        return {"available": False, "reason": "binary did not emit build-info JSON"}
    return {"available": True, "provenance": data}


def parse_counts(variant, text):
    prefixes = ("Warmup complete", "Finished") if variant == "hermes" else ("Warmup finished", "Simulation finished")
    counts = {}
    for phase, prefix in zip(("warmup", "roi"), prefixes):
        matches = re.findall(rf"^{prefix} CPU 0 instructions: (\d+) cycles: (\d+)", text, re.MULTILINE)
        if len(matches) != 1:
            raise ValueError(f"{variant}: expected exactly one {phase} completion, found {len(matches)}")
        counts[f"{phase}_instructions"], counts[f"{phase}_cycles"] = map(int, matches[0])
    return counts


def throughput(counts, wall_seconds):
    return (counts["warmup_instructions"] + counts["roi_instructions"]) / (1000.0 * wall_seconds)


def command(args, variant, trace, run_dir):
    if variant == "hermes":
        return [str(args.hermes), f"--warmup_instructions={args.warmup}", f"--simulation_instructions={args.instructions}",
                f"--trace_version={trace['version']}", "--offchip_pred_type=none", "--enable_ddrp=false",
                "--measure_dram_bw=false", "--measure_cache_acc=false", "-traces", trace["path"]]
    binary = args.baseline if variant == "baseline" else args.champsim
    argv = [str(binary)]
    for config in args.config:
        argv += ["--config", str(config)]
    for setting in getattr(args, "settings", []):
        argv += ["--set", setting]
    argv += ["--set", "dram-model=legacy"]
    if variant != "baseline":
        argv += ["--set", f"ptw.cpu0_ptw.model={'fixed' if variant == 'fixed' else 'detailed'}"]
    if variant == "fixed":
        argv += ["--set", f"ptw.cpu0_ptw.fixed_latency={args.fixed_latency}"]
    return argv + ["--trace-version", str(trace["version"]), "--hide-heartbeat", "-w", str(args.warmup), "-i", str(args.instructions),
                   "--toml", str(run_dir / "stats.toml"), "--", trace["path"]]


def measure(args, variant, trace, repetition):
    run_dir = args.output / f"{trace['name']}-{variant}-{repetition}"
    run_dir.mkdir()
    argv = command(args, variant, trace, run_dir)
    record = {"trace": trace["name"], "variant": variant, "repetition": repetition,
              "argv": argv, "cwd": str(run_dir), "load_before": os.getloadavg()}
    (run_dir / "command.json").write_text(json.dumps(record, indent=2) + "\n")
    with (run_dir / "stdout.txt").open("w") as out, (run_dir / "stderr.txt").open("w") as err:
        before = resource.getrusage(resource.RUSAGE_CHILDREN)
        start = time.perf_counter_ns()
        # wait(timeout=...) polls on POSIX and can add tens of milliseconds to
        # observed completion. A watchdog keeps the limit while waitpid gives
        # an immediate completion timestamp for the benchmark itself.
        process = subprocess.Popen(argv, cwd=run_dir, stdout=out, stderr=err)
        timed_out = threading.Event()

        def kill_on_timeout():
            timed_out.set()
            process.kill()

        watchdog = threading.Timer(args.timeout, kill_on_timeout)
        watchdog.start()
        returncode = process.wait()
        wall_seconds = (time.perf_counter_ns() - start) / 1e9
        watchdog.cancel()
        watchdog.join()
        after = resource.getrusage(resource.RUSAGE_CHILDREN)
    record.update(wall_seconds=wall_seconds, user_seconds=after.ru_utime - before.ru_utime,
                  system_seconds=after.ru_stime - before.ru_stime, returncode=returncode,
                  timed_out=timed_out.is_set(),
                  load_after=os.getloadavg())
    if returncode or timed_out.is_set():
        (run_dir / "result.json").write_text(json.dumps(record, indent=2) + "\n")
        raise RuntimeError(f"{run_dir}: simulator exited {returncode}, timeout={timed_out.is_set()}; see stderr.txt")
    text = (run_dir / "stdout.txt").read_text()
    counts = parse_counts(variant, text)
    if counts["warmup_instructions"] < args.warmup or counts["roi_instructions"] < args.instructions:
        raise RuntimeError(f"{run_dir}: simulation stopped before the requested instruction count")
    record.update(counts)
    record["kips"] = throughput(counts, wall_seconds)
    record["host_cpu_seconds"] = record["user_seconds"] + record["system_seconds"]
    record["simulated_cycles_per_second"] = (counts["warmup_cycles"] + counts["roi_cycles"]) / wall_seconds
    if variant != "hermes":
        with (run_dir / "stats.toml").open("rb") as stream:
            stats = tomllib.load(stream)
        if stats["config"]["dram-model"] != "legacy":
            raise RuntimeError("This campaign must use legacy DRAM")
        if variant == "fixed":
            translation_events = 0
            for phase in stats["phase"].values():
                for scope in ("roi", "sim"):
                    for name, cache in phase.get(scope, {}).get("cache", {}).items():
                        if "tlb" in name.lower():
                            continue
                        for owner, counters in cache.items():
                            if owner.startswith("cpu") and isinstance(counters, dict):
                                translation_events += sum(value for key, value in counters.items() if key.startswith("translation_"))
            if translation_events:
                raise RuntimeError(f"Fixed PTW generated data-cache translation events: {translation_events}")
            record["data_cache_translation_events"] = translation_events
        record["phase_sha256"] = hashlib.sha256(json.dumps(stats["phase"], sort_keys=True).encode()).hexdigest()
    # All output hashes are evidence, not a determinism assertion: stdout contains elapsed time.
    record["stdout_sha256"] = sha256(run_dir / "stdout.txt")
    (run_dir / "result.json").write_text(json.dumps(record, indent=2) + "\n")
    print(f"{trace['name']:8} {variant:8} r{repetition}: {record['kips']:.2f} KIPS, {wall_seconds:.3f} s", flush=True)
    return record


def summarize(records, output):
    groups = {}
    for record in records:
        groups.setdefault((record["trace"], record["variant"]), []).append(record)
    rows = []
    for (trace, variant), runs in sorted(groups.items()):
        fingerprints = {(r["warmup_instructions"], r["roi_instructions"], r["warmup_cycles"], r["roi_cycles"], r.get("phase_sha256")) for r in runs}
        if len(fingerprints) != 1:
            raise RuntimeError(f"Non-deterministic counts/statistics: {trace}/{variant}")
        rows.append(dict(trace=trace, variant=variant, runs=len(runs),
                         median_kips=statistics.median(r["kips"] for r in runs),
                         min_kips=min(r["kips"] for r in runs), max_kips=max(r["kips"] for r in runs),
                         median_wall_seconds=statistics.median(r["wall_seconds"] for r in runs),
                         median_cpu_seconds=statistics.median(r["host_cpu_seconds"] for r in runs),
                         warmup_instructions=runs[0]["warmup_instructions"], roi_instructions=runs[0]["roi_instructions"],
                         warmup_cycles=runs[0]["warmup_cycles"], roi_cycles=runs[0]["roi_cycles"]))
    for trace, variant in groups:
        if variant == "baseline" and (trace, "detailed") in groups:
            if groups[(trace, "baseline")][0]["phase_sha256"] != groups[(trace, "detailed")][0]["phase_sha256"]:
                raise RuntimeError(f"Detailed-mode phase statistics changed from baseline: {trace}")
    with (output / "summary.csv").open("w") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--champsim", type=Path, required=True)
    parser.add_argument("--hermes", type=Path, required=True)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--config", type=Path, action="append", default=[])
    parser.add_argument("--traces", type=Path, required=True, help="JSON list of name, path, version and sha256")
    parser.add_argument("--output", type=Path, required=True, help="New directory; existing directories are rejected")
    parser.add_argument("--cpu", type=int, required=True)
    parser.add_argument("--warmup", type=int, default=1_000_000)
    parser.add_argument("--instructions", type=int, default=5_000_000)
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--fixed-latency", type=int, default=200)
    parser.add_argument("--timeout", type=float, default=600)
    args = parser.parse_args()
    if min(args.warmup, args.instructions, args.repetitions, args.timeout) <= 0 or args.fixed_latency < 0:
        parser.error("counts, repetitions and timeout must be positive; latency must be nonnegative")
    if args.cpu not in os.sched_getaffinity(0):
        parser.error("requested CPU is outside the available affinity set")
    for name in ("champsim", "hermes", "baseline", "traces"):
        path = getattr(args, name)
        if path is not None:
            setattr(args, name, path.resolve(strict=True))
    args.config = [p.resolve(strict=True) for p in args.config]
    args.output = args.output.resolve()
    traces = json.loads(args.traces.read_text())
    if not traces or len({tr["name"] for tr in traces}) != len(traces):
        parser.error("trace list must be nonempty and names must be unique")
    for trace in traces:
        if not re.fullmatch(r"[A-Za-z0-9_-]+", trace["name"]) or trace["version"] not in (1, 2):
            parser.error("trace names must be simple identifiers and versions must be 1 or 2")
        trace["path"] = str(Path(trace["path"]).resolve(strict=True))
        if sha256(trace["path"]) != trace["sha256"]:
            raise RuntimeError(f"Trace hash mismatch: {trace['path']}")
    args.output.mkdir(parents=True, exist_ok=False)
    os.sched_setaffinity(0, {args.cpu})
    manifest = {"arguments": {k: str(v) if isinstance(v, Path) else [str(x) for x in v] if k == "config" else v for k, v in vars(args).items()},
                "traces": traces, "platform": platform.platform(), "python": platform.python_version(),
                "cpu_affinity": sorted(os.sched_getaffinity(0)), "metric": "actual warmup+ROI retired instructions / (1000 * process wall seconds)",
                "binaries": {key: {"path": str(getattr(args, key)), "sha256": sha256(getattr(args, key)), "build_info": build_info(getattr(args, key))}
                             for key in ("champsim", "hermes", "baseline") if getattr(args, key) is not None},
                "configs": [{"path": str(p), "sha256": sha256(p), "text": p.read_text()} for p in args.config]}
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    (args.output / "lscpu.txt").write_text(subprocess.check_output(["lscpu"], text=True))
    variants = (["baseline"] if args.baseline else []) + ["detailed", "fixed", "hermes"]
    records = []
    for repetition in range(args.repetitions):
        # Rotate variants so the same executable is not always first or last.
        order = variants[repetition % len(variants):] + variants[:repetition % len(variants)]
        for trace in traces:
            for variant in order:
                records.append(measure(args, variant, trace, repetition))
                (args.output / "runs.json").write_text(json.dumps(records, indent=2) + "\n")
    summarize(records, args.output)
    for trace in traces:
        if sha256(trace["path"]) != trace["sha256"]:
            raise RuntimeError(f"Input changed during campaign: {trace['path']}")
    print(f"Results: {args.output / 'summary.csv'}", flush=True)


if __name__ == "__main__":
    main()
