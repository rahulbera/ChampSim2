#!/usr/bin/env python3
"""Pinned native revision: 72427a1bba3771564c4fb0e494ba02242fd1eaa7.

Export one Ramulator 2 YAML per DRAM bandwidth point for a ChampSim sweep.

Each point is the shipped single-channel configuration (configs/ramulator2/ddr4.py)
with the data-bus burst length nBL lengthened, so the channel sustains about
64 B / (nBL x tCK) of transfers and every read gains nBL x tCK of latency, as
legacy pmem.data_rate does. The pinned exporter resolves derived timings before
it applies overrides; this script recomputes them from the overridden values
(on DDR5 that sets nRTW with nBL) and refuses to write a YAML whose timings do
not match the recomputation.

    PYTHONPATH=/path/to/ramulator2/python python3 configs/ramulator2/bandwidth.py \\
        --standard ddr4 --mbps 200 800 1600 6400 --stock --out-dir sweep/ddr4

Next to the YAMLs it writes manifest.csv: nBL, expected bandwidth, whether
the point is calibrated, the suggested guard and legacy settings, and the
ChampSim --set arguments. configs/ramulator2/make_bandwidth_sweep.sh drives it;
docs/ramulator-integration/ramulator2-bandwidth-sweeps.md explains the numbers.
"""
import argparse
import csv
import math
from pathlib import Path
import sys

PAYLOAD_BYTES = 64  # one cache block per native transaction on both presets
LIVELOCK_PERIOD = 1_000_000_000_000
NATIVE_STALL_ALLOWANCE_PS = 10_000_000  # the native default no-progress guard

STANDARDS = {
    "ddr4": {
        "dram": "DDR4",
        "org_preset": "DDR4_8Gb_x8",
        "timing_preset": "DDR4_2400R",
        # Share of time refresh leaves the channel available, measured in the
        # random-read calibration at 3cbb6e2b; linear between points.
        "availability": ((8, 0.95), (32, 0.95), (64, 0.958), (256, 0.978), (512, 1.0)),
        # Target MB/s -> (nBL, legacy pmem.data_rate), both measured to match.
        "calibrated": {
            100: (768, 13.035),
            200: (381, 26.072),
            400: (186, 52.036),
            800: (92, 104.069),
            1600: (46, 209.864),
            3200: (23, 419.968),
            6400: (11, 856.524),
        },
        "measured_nbl": (8, 65536),
        "random_read_floor_nbl": 6,  # at or below, random reads stop near 11.2 GB/s
    },
    "ddr5": {
        "dram": "DDR5",
        "org_preset": "DDR5_16Gb_x8",
        "timing_preset": "DDR5_5600B",
        "availability": None,  # not calibrated: expected = nominal
        "calibrated": {},
        "measured_nbl": (256, 4096),  # nRTW-corrected mcf windows, 89-96% of nominal
        "random_read_floor_nbl": None,
    },
}


def availability(standard, nbl):
    points = STANDARDS[standard]["availability"]
    if points is None:
        return 1.0
    if nbl <= points[0][0]:
        return points[0][1]
    for (n0, a0), (n1, a1) in zip(points, points[1:]):
        if nbl <= n1:
            return a0 + (a1 - a0) * (nbl - n0) / (n1 - n0)
    return points[-1][1]


def nominal_mbps(nbl, tck_ps):
    return PAYLOAD_BYTES * 1e12 / (nbl * tck_ps) / 1e6


def expected_mbps(standard, nbl, tck_ps):
    return nominal_mbps(nbl, tck_ps) * availability(standard, nbl)


def pick_nbl(standard, target_mbps, tck_ps):
    """Return (nBL, calibrated) for a target bandwidth in MB/s (1e6 B/s)."""
    calibrated = STANDARDS[standard]["calibrated"]
    if target_mbps in calibrated:
        return calibrated[target_mbps][0], True
    guess = nominal_mbps(1, tck_ps) / target_mbps
    candidates = range(max(2, math.floor(guess * 0.9)), math.ceil(guess * 1.1) + 2)
    return min(candidates, key=lambda n: abs(expected_mbps(standard, n, tck_ps) - target_mbps)), False


def default_deadlock_cycle(quantum_ps):
    return max(500, math.ceil(NATIVE_STALL_ALLOWANCE_PS / quantum_ps))


def suggested_deadlock_cycle(nbl, tck_ps, quantum_ps):
    """The bandwidth study's rule: four times the longest measured silent stall,
    about 2 x nBL + 900 memory cycles. It was validated only at DDR4 nBL 5,800."""
    return max(default_deadlock_cycle(quantum_ps), math.ceil(4 * (2 * nbl + 900) * tck_ps / quantum_ps))


def legacy_data_rate(nbl, tck_ps):
    """pmem.data_rate with the same data-bus time per 64-byte block (9,604/nBL on DDR4-2400)."""
    return 8e6 / (nbl * tck_ps)


def capacity_bytes(org):
    return org["density"] * 2**20 * (org["channel_width"] // org["dq"]) * org["rank"] // 8


def dram_component(ramulator, standard, timing_overrides):
    spec = STANDARDS[standard]
    cls = getattr(ramulator.dram, spec["dram"])
    return cls(org_preset=spec["org_preset"], timing_preset=spec["timing_preset"], rank=1, **timing_overrides)


def resolve_with_derived_timings(ramulator, standard, overrides):
    """Add every derived timing the exporter would leave stale, then check the result."""
    spec = STANDARDS[standard]
    cls = getattr(ramulator.dram, spec["dram"])
    overrides = dict(overrides)
    org, exported = dram_component(ramulator, standard, overrides).resolve()
    intended = dict(cls.timing_presets[spec["timing_preset"]])
    intended.update(overrides)
    cls.resolve_secondary_timings(intended, org)
    intended.update(overrides)
    derived = {name: value for name, value in intended.items() if exported.get(name) != value}
    overrides.update(derived)
    org, exported = dram_component(ramulator, standard, overrides).resolve()
    mismatched = sorted(name for name, value in intended.items() if exported.get(name) != value)
    if mismatched:
        raise SystemExit(f"{standard}: exported timings {mismatched} differ from their recomputation; not writing")
    return overrides, derived, org, exported


def export_yaml(ramulator, standard, overrides):
    from ramulator.export import dict_to_yaml

    frontend = ramulator.frontend.External(clock_ratio=1)
    controller = ramulator.controller.GenericDDR(
        dram=dram_component(ramulator, standard, overrides),
        scheduler=ramulator.scheduler.FRFCFS(),
        refresh_manager=ramulator.refresh_manager.AllBank(),
        row_policy=ramulator.row_policy.Open(),
        addr_mapper=ramulator.addr_mapper.RoBaRaCoCh(),
    )
    memory = ramulator.memory_system.GenericDRAM(
        clock_ratio=1,
        controllers=[controller],
        channel_mapper=ramulator.channel_mapper.CacheLineInterleave(),
    )
    return dict_to_yaml({"frontend": frontend.to_config(), "memory_system": memory.to_config()})


def plan_points(standard, mbps, nbls, stock, tck_ps):
    """Return [(label, nBL or None, target MB/s or None, calibrated)] without duplicates."""
    points, seen = [], {}
    if stock:
        points.append(("stock", None, None, False))
    for target in mbps:
        nbl, calibrated = pick_nbl(standard, target, tck_ps)
        points.append((f"nbl{nbl}", nbl, target, calibrated))
    for nbl in nbls:
        points.append((f"nbl{nbl}", nbl, None, False))
    unique = []
    for point in points:
        if point[0] in seen:
            print(f"warning: {point[0]} requested twice (targets {seen[point[0]]} and {point[2]}); writing it once", file=sys.stderr)
            continue
        seen[point[0]] = point[2]
        unique.append(point)
    return unique


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[1], formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--standard", choices=sorted(STANDARDS), default="ddr4")
    parser.add_argument("--mbps", type=int, nargs="*", default=[], help="target bandwidths in MB/s (1e6 B/s)")
    parser.add_argument("--nbl", type=int, nargs="*", default=[], help="explicit nBL values")
    parser.add_argument("--stock", action="store_true", help="also export the preset without an override")
    parser.add_argument("--out-dir", required=True, type=Path)
    parser.add_argument("--quantum-ps", type=float, default=250.0, help="ChampSim's global tick: the smallest operable period (default 250 ps, 4 GHz cores)")
    parser.add_argument("--force", action="store_true", help="overwrite existing files")
    args = parser.parse_args(argv)

    try:
        import ramulator
    except ImportError:
        parser.error("cannot import ramulator: set PYTHONPATH to the pinned Ramulator 2.1 checkout's python directory")

    if any(n < 2 for n in args.nbl) or any(m <= 0 for m in args.mbps):
        parser.error("nBL must be at least 2 (the driver rejects 1) and targets positive")
    if not (args.mbps or args.nbl or args.stock):
        parser.error("nothing to export: give --mbps, --nbl or --stock")

    spec = STANDARDS[args.standard]
    stock_org, stock_timing = dram_component(ramulator, args.standard, {}).resolve()
    tck_ps = stock_timing["tCK_ps"]
    points = plan_points(args.standard, args.mbps, args.nbl, args.stock, tck_ps)

    args.out_dir.mkdir(parents=True, exist_ok=True)
    files = [args.out_dir / f"{args.standard}_{label}.yaml" for label, *_ in points] + [args.out_dir / "manifest.csv"]
    existing = [str(f) for f in files if f.exists()]
    if existing and not args.force:
        parser.error(f"refusing to overwrite {', '.join(existing)} (use --force)")

    rows = []
    for (label, nbl, target, calibrated), path in zip(points, files):
        overrides, derived, org, timing = resolve_with_derived_timings(ramulator, args.standard, {} if nbl is None else {"nBL": nbl})
        path.write_text(export_yaml(ramulator, args.standard, overrides))
        effective_nbl = timing["nBL"]
        guard = suggested_deadlock_cycle(effective_nbl, tck_ps, args.quantum_ps)
        champsim_set = ["dram-model=ramulator2", f"ramulator2.config={path.resolve()}", f"sim.livelock_period={LIVELOCK_PERIOD}"]
        if guard > default_deadlock_cycle(args.quantum_ps):
            champsim_set.append(f"sim.deadlock_cycle={guard}")
        capacity_gib = capacity_bytes(org) / 2**30
        legacy_calibrated = spec["calibrated"].get(target, (None, None))[1] if calibrated else None
        low, high = spec["measured_nbl"]
        notes = []
        capped = bool(spec["random_read_floor_nbl"]) and effective_nbl <= spec["random_read_floor_nbl"]
        if capped:
            notes.append("other timings cap random reads near 11.2 GB/s")
        elif not low <= effective_nbl <= high:
            notes.append(f"outside the measured nBL range {low}-{high}")
        if spec["availability"] is None:
            notes.append("uncalibrated: measure throughput before use")
        rows.append({
            "file": path.name,
            "standard": args.standard,
            "org_preset": spec["org_preset"],
            "timing_preset": spec["timing_preset"],
            "tck_ps": tck_ps,
            "nbl": effective_nbl,
            "derived_overrides": " ".join(f"{k}={v}" for k, v in sorted(derived.items())),
            "target_mbps": "" if target is None else target,
            "nominal_mbps": round(nominal_mbps(effective_nbl, tck_ps), 1),
            "expected_mbps": "" if capped else round(expected_mbps(args.standard, effective_nbl, tck_ps), 1),
            "calibrated": "yes" if calibrated else "no",
            "capacity_gib": capacity_gib,
            "native_deadlock_cycle": guard,
            "legacy_data_rate_bus_matched": round(legacy_data_rate(effective_nbl, tck_ps), 6),
            "legacy_data_rate_calibrated": "" if legacy_calibrated is None else legacy_calibrated,
            "legacy_bankgroups": int(capacity_gib / 2) if capacity_gib % 2 == 0 else "",
            "champsim_set": " ".join(f"--set {s}" for s in champsim_set),
            "notes": "; ".join(notes),
        })
        expected = rows[-1]["expected_mbps"]
        print(f"{path}: nBL {effective_nbl}" + (f", expected {expected} MB/s" if expected != "" else "") + (f" ({rows[-1]['notes']})" if notes else ""))

    with open(files[-1], "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    print(f"{files[-1]}: {len(rows)} points")


if __name__ == "__main__":
    main()
