#!/bin/sh
# Export the recommended DRAM bandwidth sweep for the native backend.
#
#   RAMULATOR2_ROOT=/path/to/ramulator2 configs/ramulator2/make_bandwidth_sweep.sh [OUT_DIR]
#
# Writes OUT_DIR/ddr4 (calibrated points plus the stock fixture) and OUT_DIR/ddr5
# (uncalibrated points inside the measured nBL range, with nRTW set together with
# nBL, plus the stock preset), each with manifest.csv.
# See docs/ramulator-integration/ramulator2-bandwidth-sweeps.md.
set -eu

: "${RAMULATOR2_ROOT:?set RAMULATOR2_ROOT to the pinned Ramulator 2.1 checkout}"
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-bandwidth-sweep}
python=${PYTHON:-python3}

PYTHONPATH="$RAMULATOR2_ROOT/python${PYTHONPATH:+:$PYTHONPATH}"
PYTHONDONTWRITEBYTECODE=1
export PYTHONPATH PYTHONDONTWRITEBYTECODE

"$python" "$here/bandwidth.py" --standard ddr4 --stock --mbps 100 200 400 800 1600 3200 6400 --out-dir "$out/ddr4"
"$python" "$here/bandwidth.py" --standard ddr5 --stock --mbps 50 100 200 400 700 --out-dir "$out/ddr5"
