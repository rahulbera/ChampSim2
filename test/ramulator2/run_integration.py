#!/usr/bin/env python3
"""Run portable native simulator regressions using only a generated, protected v2 trace."""
import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import subprocess
import tomllib

import yaml

from generate_trace import generate

REPO = Path(__file__).resolve().parents[2]


def equal_leaves(left, right, path=""):
    if isinstance(left, dict):
        assert isinstance(right, dict) and left.keys() == right.keys(), f"table shape changed at {path}"
        return sum(equal_leaves(value, right[key], path + "/" + key) for key, value in left.items())
    assert type(left) is type(right), (path, "scalar type changed", type(left).__name__, type(right).__name__)
    assert left == right or isinstance(left, float) and isinstance(right, float) and math.isnan(left) and math.isnan(right), (path, left, right)
    return 1


def check_cpu_traffic(channels, cores):
    for cpu in range(cores):
        activity = sum(channel.get(f"read_row_{kind}_core_{cpu}", 0)
                       for channel in channels.values() for kind in ("hits", "misses", "conflicts"))
        assert activity > 0, f"source {cpu} never reached native memory"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    binary, out = args.binary.resolve(), args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=False)
    commands = []

    def execute(label, arguments, success=True):
        argv = [str(binary), *map(str, arguments)]
        commands.append(argv)
        result = subprocess.run(argv, capture_output=True, text=True, timeout=180)
        (out / f"{label}.stdout").write_text(result.stdout)
        (out / f"{label}.stderr").write_text(result.stderr)
        assert (result.returncode == 0) == success, (label, result.returncode, result.stderr[-2000:])
        return result

    probe = execute("knobs", ["--knobs"])
    assert "# DRAM backends (dram-model): legacy, ramulator2" in probe.stdout, "binary has no native support"
    cores = len(tomllib.loads(probe.stdout)["ooo_cpu"])
    trace = out / "memory.champsim2"
    generate(trace, 32768)
    trace_hash = hashlib.sha256(trace.read_bytes()).hexdigest()
    common = ["--trace-version", "2", "-w", "2000", "-i", "10000", "--toml-sim-stats"]
    for cpu in range(cores):
        for key, value in {"branch_predictor": "hashed_perceptron", "btb": "basic_btb"}.items():
            common += ["--set", f"ooo_cpu.cpu{cpu}.{key}={value}"]
        for cache in (f"cpu{cpu}_l1d", f"cpu{cpu}_l2c"):
            common += ["--set", f"cache.{cache}.sets=16", "--set", f"cache.{cache}.ways=4"]
    common += ["--set", "cache.llc.sets=32", "--set", "cache.llc.ways=4"]
    configs = {}
    for name in ("ddr4", "lpddr5"):
        configs[name] = out / f"{name}.yaml"
        configs[name].write_bytes((REPO / "configs/ramulator2" / f"{name}.yaml").read_bytes())
    multichannel = yaml.safe_load(configs["ddr4"].read_text())
    multichannel["memory_system"]["controllers"].append(copy.deepcopy(multichannel["memory_system"]["controllers"][0]))
    configs["multichannel"] = out / "multichannel.yaml"
    configs["multichannel"].write_text(yaml.safe_dump(multichannel, sort_keys=False))
    frequency = ["--set", "cache.llc.frequency=1000"]
    for cpu in range(cores):
        frequency += ["--set", f"ooo_cpu.cpu{cpu}.frequency=2500", "--set", f"cache.cpu{cpu}_l2c.frequency=1500"]
    cases = [("ddr4", configs["ddr4"], []), ("lpddr5", configs["lpddr5"], []),
             ("multichannel", configs["multichannel"], []), ("frequency", configs["ddr4"], frequency)]
    results, documents = {}, {}
    for name, config, overrides in cases:
        output, replay = out / f"{name}.toml", out / f"{name}-replay.toml"
        execute(name, [*common, *overrides, "--set", "dram-model=ramulator2", "--set", f"ramulator2.config={config}",
                       "--toml", output, "--", *([trace] * cores)])
        doc = tomllib.loads(output.read_text())
        assert doc["meta"]["schema_version"] == 2
        assert doc["meta"]["dram_model"] == "ramulator2"
        assert doc["meta"]["ramulator2"]["yaml"] == config.read_text()
        roi = doc["phase"]["simulation"]["roi"]
        assert "dram" not in roi
        assert all(roi["core"][f"cpu{cpu}"]["instructions"] >= 10000 for cpu in range(cores))
        adapter = roi["ramulator2"]["adapter"]
        assert adapter["accepted_reads"] > 0 and adapter["completed_reads"] > 0
        assert adapter["accepted_writes"] > 0 and adapter["completed_writes"] > 0
        scale = 2 if name == "lpddr5" else 1
        # Parents still partly admitted at the boundary can contribute one fewer
        # fragment than a fully submitted block; the native queue is not drained.
        parents = adapter["accepted_reads"] + adapter["accepted_writes"]
        assert parents <= adapter["accepted_fragments"] <= scale * parents
        if scale == 2:
            assert adapter["accepted_fragments"] > parents
        channels = roi["ramulator2"]["native"]["memory_system"]["controller"]
        assert set(channels) == ({"channel0", "channel1"} if name == "multichannel" else {"channel0"})
        check_cpu_traffic(channels, cores)
        execute(name + "-replay", ["--trace-version", "2", "-w", "2000", "-i", "10000", "--toml-sim-stats", "--config", output,
                                  "--toml", replay, "--", *([trace] * cores)])
        again = tomllib.loads(replay.read_text())
        results[name] = {"config_leaves_equal": equal_leaves(doc["config"], again["config"]),
                         "phase_leaves_equal": equal_leaves(doc["phase"], again["phase"]), "adapter": adapter}
        assert doc["meta"]["build_id"] == again["meta"]["build_id"]
        documents[name] = doc
    assert documents["frequency"]["config"]["ramulator2"] == documents["ddr4"]["config"]["ramulator2"], "core/cache frequency changed native configuration"
    assert documents["frequency"]["config"]["cache"]["llc"]["frequency"] == 1000
    assert documents["frequency"]["config"]["ooo_cpu"]["cpu0"]["frequency"] == 2500
    assert documents["frequency"]["phase"]["simulation"]["roi"]["core"]["cpu0"]["cycles"] != documents["ddr4"]["phase"]["simulation"]["roi"]["core"]["cpu0"]["cycles"]
    assert any(case["adapter"]["outstanding_parents"] > 0 for case in results.values()), "fixture did not expose in-flight state at retirement"
    original = configs["ddr4"].read_bytes()
    try:
        configs["ddr4"].write_bytes(original + b"\n# changed at the same pathname\n")
        rejected = execute("digest-rejection", ["--config", out / "ddr4.toml", "--knobs"], False)
        assert "ramulator2.config_hash" in rejected.stderr and not rejected.stdout
    finally:
        configs["ddr4"].write_bytes(original)
    assert hashlib.sha256(trace.read_bytes()).hexdigest() == trace_hash, "input trace changed"
    results["cores"] = cores
    results["trace_sha256"] = trace_hash
    (out / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
    (out / "summary.json").write_text(json.dumps(results, indent=2) + "\n")
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
