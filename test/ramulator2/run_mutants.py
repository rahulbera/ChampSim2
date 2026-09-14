#!/usr/bin/env python3
"""Check that test 706's differential oracle detects each adapter mutant.

The repository's tracked and untracked (not ignored) files are copied to a
scratch directory, so the working tree is never modified: the script records
the bytes of every file a mutant touches, and inc/defs.h, before it starts
and fails if any changed. The copy is configured once; its test binary is
built into a separate OBJ_ROOT/BIN_ROOT, first unmutated (whose short campaign
must pass), then once per mutant with the mutant's textual replacements
applied to the copy's source (normally src/ramulator2_memory_backend.cc) and
removed again afterwards. A mutant is
detected when any campaign run reports a discrepancy (see run_differential.py).

Native builds: a new OBJ_ROOT makes the build helper rebuild libramulator.so
inside --native-root unless --seed-native-obj names an object root whose
ramulator2-native/manifest.json already matches that root. Use a private
native checkout that no other build or run is using.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
from types import SimpleNamespace

import oracle_mutants
import oracle_variants
import run_differential

REPO = Path(__file__).resolve().parents[2]
INHERITED_BUILD_VARIABLES = ("CXX", "CC", "CXXFLAGS", "CPPFLAGS", "LDFLAGS", "CFLAGS")
DEFAULT_RUNS = "ddr4,lpddr5,lpddr5-tiny,lpddr5-2ch-asym,ddr4-tiny-2feeders"
DEFAULT_RECOVERY_RUNS = "ddr4-tiny,lpddr5-tiny,lpddr5-2ch-tiny-2feeders"


def build_environment(cxx):
    env = {key: value for key, value in os.environ.items() if key not in INHERITED_BUILD_VARIABLES}
    env["CXX"] = cxx
    return env


def repository_files(repo):
    listing = subprocess.check_output(["git", "-C", str(repo), "ls-files", "-z", "--cached", "--others", "--exclude-standard"])
    return sorted({path for path in listing.decode().split("\0") if path})


def copy_repository(repo, destination):
    """Copy tracked and untracked, not ignored, regular files and symlinks. Returns the count."""
    repo, destination = Path(repo), Path(destination)
    destination.mkdir(parents=True, exist_ok=False)
    copied = 0
    for relative in repository_files(repo):
        source = repo / relative
        if not (source.is_symlink() or source.is_file()):
            continue  # a submodule gitlink or a deleted tracked file
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        if source.is_symlink():
            os.symlink(os.readlink(source), target)
        else:
            shutil.copy2(source, target)
        copied += 1
    return copied


# The files this script changes in its scratch copy. Their bytes in the
# repository are recorded before the run and must be unchanged after it.
GUARDED = tuple(sorted({mutant.path for mutant in oracle_mutants.MUTANTS} | {"inc/defs.h"}))


def working_tree_state(repo):
    return {path: hashlib.sha256((Path(repo) / path).read_bytes()).hexdigest() for path in GUARDED}


def set_num_cpus(text, cores):
    replaced, count = re.subn(r"(inline constexpr std::size_t num_cpus = )\d+;", rf"\g<1>{cores};", text)
    if count != 1:
        raise ValueError(f"inc/defs.h: num_cpus definition occurs {count} times")
    return replaced


def run_logged(command, cwd, env, log_path):
    with open(log_path, "w") as log:
        return subprocess.run(command, cwd=cwd, env=env, stdout=log, stderr=subprocess.STDOUT).returncode


def prepare(args, out):
    scratch, obj, bin_dir = out / "scratch", out / "obj", out / "bin"
    copied = copy_repository(args.repo, scratch)
    vcpkg = Path(args.repo) / "vcpkg_installed"
    if vcpkg.exists():
        os.symlink(vcpkg.resolve(), scratch / "vcpkg_installed")
    defs = scratch / "inc" / "defs.h"
    defs.write_text(set_num_cpus(defs.read_text(), args.cores))
    env = build_environment(args.cxx)
    if run_logged([sys.executable, "config.sh"], scratch, env, out / "config.log") != 0:
        raise RuntimeError(f"config.sh failed; see {out / 'config.log'}")
    obj.mkdir()
    for name in ("registry.inc", "registry.cc.inc"):
        shutil.copy2(scratch / ".csconfig" / name, obj / name)
    if args.seed_native_obj:
        manifest = Path(args.seed_native_obj) / "ramulator2-native" / "manifest.json"
        (obj / "ramulator2-native").mkdir()
        shutil.copy2(manifest, obj / "ramulator2-native" / "manifest.json")
    return SimpleNamespace(scratch=scratch, obj=obj, bin=bin_dir, env=env, copied=copied)


def build(args, tree, log_path):
    target = tree.bin / "000-test-main"
    command = ["make", f"-j{args.make_jobs}", "WITH_RAMULATOR2=1", f"RAMULATOR2_ROOT={Path(args.native_root).resolve()}", f"OBJ_ROOT={tree.obj}",
               f"BIN_ROOT={tree.bin}", f"test_main_name={target}", str(target)]
    return target if run_logged(command, tree.scratch, tree.env, log_path) == 0 else None


def short_campaign(args, binary, manifest, log_dir):
    log_dir.mkdir(parents=True)
    options = SimpleNamespace(seeds=args.seeds, seed0=args.seed0, parents=args.parents, jobs=args.jobs, timeout=args.timeout,
                              require_all_cores=False, cwd=str(args.repo))
    results = {"ok": True, "runs": [], "totals": {}}
    if args.runs:
        runs = run_differential.load_runs(manifest, args.runs)
        results = run_differential.campaign(binary, runs, ["differential"], options, log_dir, {})
    if args.recovery_runs:
        recovery = run_differential.load_runs(manifest, args.recovery_runs)
        extra = dict(item.split("=", 1) for item in args.recovery_env)
        more = run_differential.campaign(binary, recovery, ["recovery"], options, log_dir, extra)
        results = {"ok": results["ok"] and more["ok"], "runs": results["runs"] + more["runs"], "totals": {**results["totals"], **more["totals"]}}
    return results


def first_failure(results):
    for run in results["runs"]:
        if not run["ok"]:
            return f"{run['campaign']} {run['name']}: {run['discrepancies'][0]}"
    return ""


def judge(mutant, cores, detected):
    """Classify a completed mutant run. Returns (status, acceptable)."""
    expected_equivalent = bool(mutant.equivalent) or cores < mutant.min_cores
    if expected_equivalent:
        return ("equivalent, survived", True) if not detected else ("equivalent but detected (false positive)", False)
    return ("detected", True) if detected else ("survived", False)


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--native-root", required=True, help="private pinned Ramulator 2.1 checkout")
    p.add_argument("--output-dir", required=True, help="new directory for the scratch copy, builds, logs and summary.json")
    p.add_argument("--repo", default=str(REPO), help="repository to copy (default: this checkout)")
    p.add_argument("--cxx", default="/usr/bin/g++")
    p.add_argument("--cores", type=int, default=2, help="num_cpus for the scratch build; M19 needs 2 or more")
    p.add_argument("--mutants", default="", help="comma-separated names or ids such as M01,M20 (default: all)")
    p.add_argument("--runs", default=DEFAULT_RUNS, help="manifest runs for the differential campaign ('' to skip)")
    p.add_argument("--recovery-runs", default=DEFAULT_RECOVERY_RUNS, help="manifest runs for the recovery campaign ('' to skip)")
    p.add_argument("--recovery-env", action="append", default=["DIFF_CYCLES=6"], help="KEY=VALUE for recovery runs")
    p.add_argument("--seeds", type=int, default=3)
    p.add_argument("--seed0", type=int, default=1)
    p.add_argument("--parents", type=int, default=2000)
    p.add_argument("--jobs", type=int, default=6, help="concurrent campaign subprocesses")
    p.add_argument("--make-jobs", type=int, default=6)
    p.add_argument("--timeout", type=float, default=600)
    p.add_argument("--seed-native-obj", default="", help="object root whose native manifest matches --native-root (avoids a rebuild)")
    p.add_argument("--keep-scratch", action="store_true", help="keep the scratch copy, objects and binary")
    return p


def main(argv=None):
    args = parser().parse_args(argv)
    args.repo = Path(args.repo).resolve()
    out = Path(args.output_dir).resolve()
    out.mkdir(parents=True, exist_ok=False)
    if not args.runs and not args.recovery_runs:
        raise SystemExit("nothing to run: --runs and --recovery-runs are both empty")
    selected = oracle_mutants.by_name([name for name in args.mutants.split(",") if name])
    before = working_tree_state(args.repo)
    summary = {"arguments": {k: str(v) for k, v in vars(args).items()}, "working_tree": before, "mutants": []}
    tree = prepare(args, out)
    manifest = out / "variants" / "manifest.json"
    oracle_variants.generate(manifest.parent, tree.scratch / "configs" / "ramulator2")
    pristine = {path: (tree.scratch / path).read_text() for path in GUARDED}
    status = 0
    try:
        started = time.monotonic()
        binary = build(args, tree, out / "build-baseline.log")
        if binary is None:
            raise RuntimeError(f"baseline build failed; see {out / 'build-baseline.log'}")
        baseline = short_campaign(args, binary, manifest, out / "logs" / "baseline")
        summary["baseline"] = {"ok": baseline["ok"], "first_failure": first_failure(baseline), "totals": baseline["totals"],
                               "seconds": round(time.monotonic() - started, 1)}
        print(f"baseline: {'pass' if baseline['ok'] else 'FAIL ' + first_failure(baseline)}", flush=True)
        if not baseline["ok"]:
            status = 2
            selected = []  # mutants cannot be judged against a failing baseline
        for mutant in selected:
            started = time.monotonic()
            record = {"name": mutant.name, "description": mutant.description, "equivalent": mutant.equivalent, "min_cores": mutant.min_cores}
            source = tree.scratch / mutant.path
            try:
                source.write_text(oracle_mutants.apply(pristine[mutant.path], mutant))
            except ValueError as error:
                record.update(status="stale", acceptable=False, first_failure=str(error))
            else:
                try:
                    binary = build(args, tree, out / f"build-{mutant.name}.log")
                    if binary is None:
                        record.update(status="build failed", acceptable=False, first_failure=f"see {out / f'build-{mutant.name}.log'}")
                    else:
                        results = short_campaign(args, binary, manifest, out / "logs" / mutant.name)
                        detected = not results["ok"]
                        record["status"], record["acceptable"] = judge(mutant, args.cores, detected)
                        record["first_failure"] = first_failure(results)
                        record["runs"] = [{"campaign": r["campaign"], "name": r["name"], "ok": r["ok"], "discrepancies": r["discrepancies"][:3]}
                                          for r in results["runs"]]
                finally:
                    source.write_text(pristine[mutant.path])
            record["seconds"] = round(time.monotonic() - started, 1)
            summary["mutants"].append(record)
            print(f"{mutant.name}: {record['status']}: {record['first_failure'][:200]}", flush=True)
            if not record["acceptable"]:
                status = 1
    finally:
        for path, text in pristine.items():
            (tree.scratch / path).write_text(text)
        after = working_tree_state(args.repo)
        summary["working_tree_unchanged"] = after == before
        if after != before:
            print("ERROR: the working tree changed during the mutation run", file=sys.stderr)
            status = 3
        summary["exit_status"] = status
        (out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        if not args.keep_scratch:
            for path in (tree.scratch, tree.obj, tree.bin):
                shutil.rmtree(path, ignore_errors=True)
    return status


if __name__ == "__main__":
    sys.exit(main())
