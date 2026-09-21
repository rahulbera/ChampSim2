#!/usr/bin/env python3
"""Compare bounded direct-native and ChampSim-driver replay, using an existing native build."""
import argparse
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import re
import subprocess

import yaml

REPO = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent


def check_leaves(raw, typed):
    original = {tuple(item["path"]): item["text"] for item in raw}
    actual = {tuple(item["path"]): item for item in typed if item["path"][0] == "memory_system"}
    assert len(original) == len(raw), "duplicate direct scalar path"
    assert len(actual) == sum(item["path"][0] == "memory_system" for item in typed), "duplicate driver scalar path"
    assert original.keys() == actual.keys(), "native scalar leaf paths differ"
    for path, text in original.items():
        leaf = actual[path]
        if path[-1] in ("impl", "id"):
            kind, expected, value = 4, text, leaf["text"]
        elif text in ("true", "false"):
            kind, expected, value = 3, text == "true", leaf["text"] == "1"
        elif re.fullmatch(r"-?[0-9]+", text):
            integer = int(text)
            if -(2**63) <= integer <= 2**64 - 1:
                kind, expected, value = int(integer > 2**63 - 1), integer, int(leaf["text"])
            else:
                kind, expected, value = 4, text, leaf["text"]
        else:
            try:
                expected, value = float(text), float(leaf["text"])
                kind = 2
            except ValueError:
                kind, expected, value = 4, text, leaf["text"]
        assert leaf["kind"] == kind, (path, "scalar type", leaf["kind"], kind)
        assert value == expected or isinstance(value, float) and math.isnan(value) and math.isnan(expected), (path, expected, value)
    return len(original)


def check_events(text, transactions):
    rows = list(csv.DictReader(text.split("SUMMARY", 1)[0].splitlines()))
    sends = [row for row in rows if row["EVENT"] == "SEND"]
    callbacks = [row for row in rows if row["EVENT"] == "CALLBACK"]
    accepted = set()
    rejected_types = set()
    for row in sends:
        index = int(row["id"])
        original = transactions[index]
        assert index not in accepted, f"accepted transaction {index} resubmitted"
        assert int(row["tick"]) >= int(original["release_tick"]), "submission before release"
        for key in ("type", "address", "source", "size", "parent", "fragment"):
            assert row[key] == original[key], (index, key, row[key], original[key])
        if row["accepted"] == "1":
            accepted.add(index)
        else:
            assert row["accepted"] == "0"
            rejected_types.add(row["type"])
    assert accepted == set(range(len(transactions)))
    assert sorted(int(row["id"]) for row in callbacks) == list(range(len(transactions))), "missing/duplicate callback"
    assert rejected_types == {"0", "1"}, "fixture did not exercise read and write backpressure"
    assert sum(row["synchronous"] == "1" for row in callbacks) == 2, "duplicate writes did not complete synchronously"
    parents = {}
    for row in transactions:
        parents.setdefault(row["parent"], []).append(row)
    split = [parts for parts in parents.values() if len(parts) == 2]
    assert split and all([int(row["fragment"]) for row in parts] == [0, 1] and int(parts[1]["address"]) - int(parts[0]["address"]) == 32 for parts in split)
    return {"transactions": len(transactions), "attempts": len(sends), "rejections": len(sends) - len(accepted), "callbacks": len(callbacks), "split_parents": len(split)}


def compare_events(direct, driver, transactions):
    assert direct == driver, "attempted submission/callback streams differ"
    return check_events(driver, transactions)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native-root", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, default=REPO / ".csconfig")
    parser.add_argument("--dependency-dir", type=Path, default=REPO / "vcpkg_installed/x64-linux")
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    native, build, deps, out = (path.resolve() for path in (args.native_root, args.build_dir, args.dependency_dir, args.output_dir))
    out.mkdir(parents=True, exist_ok=False)
    commands = []

    def run(argv, label, capture=False):
        commands.append([str(arg) for arg in argv])
        environment = dict(os.environ)
        environment["LD_LIBRARY_PATH"] = str(native) + os.pathsep + environment.get("LD_LIBRARY_PATH", "")
        result = subprocess.run(argv, capture_output=True, text=True, timeout=120, env=environment)
        (out / f"{label}.stderr").write_text(result.stderr)
        (out / f"{label}.stdout").write_text(result.stdout)
        if result.returncode:
            raise RuntimeError(f"{label} failed ({result.returncode}); see {out / (label + '.stderr')}")
        return result.stdout if capture else None

    common = [args.cxx, "-O2", "-Wall", "-Wextra", "-Werror", "-isystem", deps / "include"]
    library = native / "libramulator.so"
    run([*common, "-std=c++20", "-isystem", native / "src", HERE / "direct_replay.cc", library,
         f"-Wl,-rpath,{native}", "-o", out / "direct_replay"], "build-direct")
    run([*common, "-std=c++17", "-I", REPO / "inc", HERE / "driver_replay.cc", build / "ramulator2_driver.o",
         build / "runtime_config.o", library, f"-Wl,-rpath,{native}", "-L", deps / "lib", "-lfmt", "-ldl", "-o", out / "driver_replay"], "build-driver")
    with (HERE / "stream.csv").open() as stream:
        transactions = list(csv.DictReader(stream))
    results = {}
    for fixture in ("ddr4", "lpddr5"):
        config = yaml.safe_load((REPO / "configs/ramulator2" / f"{fixture}.yaml").read_text())
        for controller in config["memory_system"]["controllers"]:
            controller["read_buffer_size"] = controller["write_buffer_size"] = 2
        config_file = out / f"{fixture}.yaml"
        config_file.write_text(yaml.safe_dump(config, sort_keys=False))
        events = {}
        for runner in ("direct", "driver"):
            events[runner] = run([out / f"{runner}_replay", config_file, HERE / "stream.csv", out / f"{fixture}-{runner}.yaml"], f"{fixture}-{runner}", True)
        event_counts = compare_events(events["direct"], events["driver"], transactions)
        raw = json.loads((out / f"{fixture}-direct.yaml.raw.json").read_text())
        typed = json.loads((out / f"{fixture}-driver.yaml.typed.json").read_text())
        leaves = check_leaves(raw, typed)
        native_yaml = (out / f"{fixture}-direct.yaml").read_text()
        driver_yaml = (out / f"{fixture}-driver.yaml").read_text()
        assert native_yaml == "memory_system:\n" + driver_yaml.split("memory_system:\n", 1)[1], "raw native memory counters differ"
        results[fixture] = dict(event_counts, typed_counter_leaves=leaves)
    results["library_sha256"] = hashlib.sha256(library.read_bytes()).hexdigest()
    (out / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
    (out / "summary.json").write_text(json.dumps(results, indent=2) + "\n")
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
