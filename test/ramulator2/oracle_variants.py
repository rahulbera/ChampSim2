#!/usr/bin/env python3
"""Write the native YAML variants that the adapter differential oracle runs over.

Every variant starts from a committed fixture in configs/ramulator2 and changes
only controller buffers, controller count, channel interleaving or the native
transaction payload. A manifest records, for each campaign run, the YAML, the
transaction bytes and clock period the driver must report (test 706 checks
both), and the number of feeder channels.
"""
import argparse
import copy
from dataclasses import dataclass, field
import json
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parents[2]
FIXTURES = REPO / "configs" / "ramulator2"
# Native transaction bytes and tCK_ps of the committed fixtures.
FIXTURE_GEOMETRY = {"ddr4": (64, 833), "lpddr5": (32, 1453)}


@dataclass(frozen=True)
class Variant:
    name: str
    fixture: str
    # (read, write, priority) buffer sizes, one tuple per controller or one for all.
    buffers: tuple = ()
    controllers: int = 1
    interleave_bits: int = 0
    payload: int = 0
    notes: str = ""


VARIANTS = (
    Variant("ddr4", "ddr4", notes="DDR4 fixture as committed"),
    Variant("lpddr5", "lpddr5", notes="LPDDR5 fixture: two 32-byte fragments per cache block"),
    Variant("ddr4-tiny", "ddr4", buffers=((1, 2, 1),), notes="first-fragment rejections and long backlogs"),
    Variant("lpddr5-tiny", "lpddr5", buffers=((1, 1, 1),), notes="partial heads on nearly every parent"),
    Variant("ddr4-2ch", "ddr4", controllers=2),
    Variant("ddr4-2ch-tiny", "ddr4", buffers=((2, 2, 2),), controllers=2),
    Variant("lpddr5-2ch-tiny", "lpddr5", buffers=((2, 1, 2),), controllers=2),
    Variant("lpddr5-2ch-asym", "lpddr5", buffers=((1, 1, 2), (4, 3, 2)), controllers=2,
            notes="asymmetric buffers split one parent's acceptance across channels"),
    Variant("ddr4-4ch-interleave2-tiny", "ddr4", buffers=((2, 2, 2),), controllers=4, interleave_bits=2),
    Variant("ddr4-tx128-tiny", "ddr4", buffers=((2, 2, 2),), payload=128,
            notes="real 128-byte native transactions: one 64-byte request per block"),
)


@dataclass(frozen=True)
class Run:
    name: str
    variant: str
    feeders: int = 1


RUNS = tuple(Run(v.name, v.name) for v in VARIANTS) + (
    Run("ddr4-tiny-2feeders", "ddr4-tiny", feeders=2),
    Run("lpddr5-2ch-tiny-2feeders", "lpddr5-2ch-tiny", feeders=2),
)


def build(variant, fixtures=FIXTURES):
    """Return (document, transaction bytes, period ps) for one variant."""
    document = yaml.safe_load((Path(fixtures) / f"{variant.fixture}.yaml").read_text())
    template = document["memory_system"]["controllers"][0]
    controllers = []
    for index in range(variant.controllers):
        controller = copy.deepcopy(template)
        if variant.buffers:
            sizes = variant.buffers[index] if len(variant.buffers) > 1 else variant.buffers[0]
            controller["read_buffer_size"], controller["write_buffer_size"], controller["priority_buffer_size"] = sizes
        if variant.payload:
            controller["dram"]["data_payload_bytes"] = variant.payload
        controllers.append(controller)
    if variant.buffers and len(variant.buffers) not in (1, variant.controllers):
        raise ValueError(f"{variant.name}: {len(variant.buffers)} buffer tuples for {variant.controllers} controllers")
    document["memory_system"]["controllers"] = controllers
    document["memory_system"]["channel_mapper"]["interleave_bits"] = variant.interleave_bits
    tx, period = FIXTURE_GEOMETRY[variant.fixture]
    return document, (variant.payload or tx), period


def generate(output_dir, fixtures=FIXTURES):
    """Write every variant and manifest.json into a new directory; return the manifest."""
    output = Path(output_dir)
    output.mkdir(parents=True, exist_ok=False)
    by_name = {}
    for variant in VARIANTS:
        document, tx, period = build(variant, fixtures)
        path = output / f"{variant.name}.yaml"
        path.write_text(yaml.safe_dump(document, sort_keys=False, default_flow_style=None, width=1000))
        by_name[variant.name] = {"yaml": str(path.resolve()), "tx": tx, "period": period, "controllers": variant.controllers,
                                 "notes": variant.notes}
    manifest = {"runs": [dict(name=run.name, variant=run.variant, feeders=run.feeders, **by_name[run.variant]) for run in RUNS]}
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", required=True, help="new directory to create")
    parser.add_argument("--fixtures", default=str(FIXTURES), help="directory holding ddr4.yaml and lpddr5.yaml")
    args = parser.parse_args()
    manifest = generate(args.output_dir, args.fixtures)
    for run in manifest["runs"]:
        print(f"{run['name']}: tx={run['tx']} period={run['period']} feeders={run['feeders']} {run['yaml']}")


if __name__ == "__main__":
    main()
