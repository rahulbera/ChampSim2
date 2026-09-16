#!/usr/bin/env python3
"""Pinned native revision: 72427a1bba3771564c4fb0e494ba02242fd1eaa7.

Export one Ramulator 2 YAML per DDR4-3200 channel count for a ChampSim sweep.

One controller is one channel. The driver requires each controller's org.count[0]
(channel) to be 1 and reads the channel count off the controller list, which must
be a power of two with identical capacity, tCK and transaction size across every
entry (src/ramulator2_driver.cc:211, :374, :396). So a channel sweep repeats the
whole controller block; it never touches the org.

    PYTHONPATH=/path/to/ramulator2/python python3 configs/ramulator2/channels.py \\
        --channels 1 2 4 --out-dir sweep/ddr4-3200

DDR4-3200 is selected by TIMING PRESET, never synthesised by overriding rate or
tCK_ps on a 2400 preset: the exporter derives nRFC/nREFI/nRRDL/nFAW before it
applies overrides, so a faked 3200 keeps 2400's refresh and ACT windows
(421/9363 instead of 560/12480, 6/26 instead of 8/34). Ramulator ships three
rate-3200 bins, W/AA/AC, differing only in nCL/nRCD/nRP and the dependent nRC.

Capacity is the thing to decide before running a sweep. Per channel the shipped
DDR4_8Gb_x8 org is 8 GiB and total capacity is channels x that, so 1/2/4 channels
is 8/16/32 GiB and the physical address space, vmem frame pool and
out_of_range_prefetches all move with the channel count. --iso-capacity instead
holds the total at the one-channel figure by choosing a lower-density part per
channel (8Gb/4Gb/2Gb), which keeps the address space fixed but changes tRFC with
the density. Neither is a pure control; pick the confound you can account for.

Next to the YAMLs it writes manifest.csv with the per-channel and aggregate
nominal bandwidth, the capacities and the ChampSim --set arguments. The nominal
figure is the data bus, not an achievable rate: at nBL 4 and tCK 625 ps one
64-bit channel is 25,600 MB/s, while the ACT window (nFAW 34 at 625 ps) caps
random 64-byte reads near 12 GB/s per channel.

configs/ramulator2/bandwidth.py is the sibling that sweeps nBL for bandwidth
points on DDR4-2400R; this script reuses its exporter plumbing.
"""
import argparse
import csv
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bandwidth  # noqa: E402  (sibling module, same directory)

TIMING_PRESETS = ("DDR4_3200W", "DDR4_3200AA", "DDR4_3200AC")
DEFAULT_TIMING_PRESET = "DDR4_3200AA"  # 22-22-22, the middle of the three shipped bins
DEFAULT_ORG_PRESET = "DDR4_8Gb_x8"  # what every shipped DDR4 config uses: 8 GiB per channel
DEFAULT_CHANNELS = (1, 2, 4)

# Lower-density x8 parts, for --iso-capacity. Keyed by the divisor applied to the
# one-channel capacity, so N channels of ORG_BY_DIVISOR[N] total what 1 channel of
# the 8 Gb part does. Only x8 is listed: dq moves capacity and the ACT window too.
ORG_BY_DIVISOR = {1: "DDR4_8Gb_x8", 2: "DDR4_4Gb_x8", 4: "DDR4_2Gb_x8"}

# A private STANDARDS entry per (org, timing) pair, so bandwidth.py's own table and
# its --standard choices are untouched. Only the three keys its exporter helpers
# read are needed; the nBL calibration fields are DDR4-2400R data at tCK 833 and
# deliberately absent, because nothing here sweeps nBL.
def register_standard(org_preset, timing_preset):
    key = f"_channels:{org_preset}:{timing_preset}"
    bandwidth.STANDARDS[key] = {
        "dram": "DDR4",
        "org_preset": org_preset,
        "timing_preset": timing_preset,
        "availability": None,
        "calibrated": {},
        "measured_nbl": (None, None),
        "random_read_floor_nbl": None,
    }
    return key


def power_of_two(value):
    return value >= 1 and value & (value - 1) == 0


def plan_points(channels, org_preset, iso_capacity):
    """Return [(channels, org_preset)] in the given order, without duplicates."""
    points, seen = [], set()
    for n in channels:
        if n in seen:
            print(f"channels.py: ignoring duplicate channel count {n}", file=sys.stderr)
            continue
        seen.add(n)
        points.append((n, ORG_BY_DIVISOR[n] if iso_capacity else org_preset))
    return points


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--channels", type=int, nargs="*", default=list(DEFAULT_CHANNELS),
                        help="channel counts to export (default: 1 2 4); each must be a power of two")
    parser.add_argument("--timing-preset", choices=TIMING_PRESETS, default=DEFAULT_TIMING_PRESET,
                        help=f"DDR4-3200 speed bin (default: {DEFAULT_TIMING_PRESET})")
    parser.add_argument("--org-preset", default=DEFAULT_ORG_PRESET,
                        help=f"per-channel device (default: {DEFAULT_ORG_PRESET}); ignored with --iso-capacity")
    parser.add_argument("--iso-capacity", action="store_true",
                        help="hold total capacity at the one-channel figure by lowering the per-channel density")
    parser.add_argument("--interleave-bits", type=int, default=0,
                        help="CacheLineInterleave shift above the transaction offset (default: 0, cache-line round robin)")
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--quantum-ps", type=float, default=250.0, help="simulation tick used for the deadlock guard")
    parser.add_argument("--force", action="store_true", help="overwrite existing outputs")
    args = parser.parse_args(argv)

    try:
        import ramulator
    except ImportError:
        parser.error("cannot import ramulator: set PYTHONPATH to the pinned checkout's python/ directory")

    bad = [n for n in args.channels if not power_of_two(n)]
    if bad:
        parser.error(f"channel counts must be powers of two (the driver refuses anything else): {bad}")
    if not args.channels:
        parser.error("no channel counts requested")
    if args.interleave_bits < 0:
        parser.error("--interleave-bits must not be negative")
    if args.iso_capacity:
        unsupported = [n for n in args.channels if n not in ORG_BY_DIVISOR]
        if unsupported:
            parser.error(f"--iso-capacity has no x8 part for {unsupported}; supported: {sorted(ORG_BY_DIVISOR)}")

    points = plan_points(args.channels, args.org_preset, args.iso_capacity)
    bin_label = args.timing_preset.replace("DDR4_", "").lower()

    args.out_dir.mkdir(parents=True, exist_ok=True)
    files = [args.out_dir / f"ddr4_{bin_label}_{n}ch.yaml" for n, _ in points] + [args.out_dir / "manifest.csv"]
    existing = [str(f) for f in files if f.exists()]
    if existing and not args.force:
        parser.error(f"refusing to overwrite {', '.join(existing)} (use --force)")

    rows = []
    for (channels, org_preset), path in zip(points, files):
        key = register_standard(org_preset, args.timing_preset)
        # No nBL override, so nothing derived can go stale -- but run the same check
        # bandwidth.py does, so a preset whose secondaries disagree still refuses.
        overrides, derived, org, timing = bandwidth.resolve_with_derived_timings(ramulator, key, {})
        path.write_text(bandwidth.export_yaml(ramulator, key, overrides, channels=channels,
                                              interleave_bits=args.interleave_bits))

        tck_ps = timing["tCK_ps"]
        nbl = timing["nBL"]
        channel_gib = bandwidth.capacity_bytes(org) / 2**30
        guard = bandwidth.suggested_deadlock_cycle(nbl, tck_ps, args.quantum_ps)
        champsim_set = ["dram-model=ramulator2", f"ramulator2.config={path.resolve()}"]
        if guard > bandwidth.default_deadlock_cycle(args.quantum_ps):
            champsim_set.append(f"sim.deadlock_cycle={guard}")

        notes = []
        if args.iso_capacity and channels != 1:
            notes.append(f"iso-capacity: {org_preset} changes density, and so tRFC, against the 1-channel point")
        elif channels != 1:
            notes.append("capacity scales with channel count; address space differs from the 1-channel point")
        if args.interleave_bits:
            notes.append(f"interleaving {2 ** args.interleave_bits} blocks per channel, not one")

        rows.append({
            "file": path.name,
            "org_preset": org_preset,
            "timing_preset": args.timing_preset,
            "tck_ps": tck_ps,
            "nbl": nbl,
            "channels": channels,
            "interleave_bits": args.interleave_bits,
            "channel_capacity_gib": f"{channel_gib:.3f}",
            "total_capacity_gib": f"{channel_gib * channels:.3f}",
            "nominal_mbps_per_channel": f"{bandwidth.nominal_mbps(nbl, tck_ps):.1f}",
            "nominal_mbps_total": f"{bandwidth.nominal_mbps(nbl, tck_ps) * channels:.1f}",
            "native_deadlock_cycle": guard,
            "champsim_set": " ".join(f"--set {entry}" for entry in champsim_set),
            "notes": "; ".join(notes),
        })
        print(f"{path}: {channels} channel(s), {channel_gib * channels:.0f} GiB total, "
              f"{bandwidth.nominal_mbps(nbl, tck_ps) * channels:.0f} MB/s nominal")

    manifest = files[-1]
    with manifest.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    print(f"{manifest}: {len(rows)} points")


if __name__ == "__main__":
    main()
