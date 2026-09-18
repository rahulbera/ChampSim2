#!/bin/sh
# Export the DDR4-3200 channel sweep for the native backend.
#
#   RAMULATOR2_ROOT=/path/to/ramulator2 configs/ramulator2/make_channel_sweep.sh [OUT_DIR]
#
# Writes OUT_DIR/scaled (1/2/4 channels of DDR4_8Gb_x8, so 8/16/32 GiB total, the
# way adding DIMMs behaves) and OUT_DIR/iso (1/2/4 channels holding 8 GiB total by
# lowering the per-channel density, which fixes the address space but moves tRFC),
# each with manifest.csv. Run both and compare: neither is a pure control for
# channel count on its own.
# See docs/ramulator-integration/ramulator2-reference.md.
set -eu

: "${RAMULATOR2_ROOT:?set RAMULATOR2_ROOT to the pinned Ramulator 2.1 checkout}"
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-channel-sweep}
python=${PYTHON:-python3}

PYTHONPATH="$RAMULATOR2_ROOT/python${PYTHONPATH:+:$PYTHONPATH}"
PYTHONDONTWRITEBYTECODE=1
export PYTHONPATH PYTHONDONTWRITEBYTECODE

"$python" "$here/channels.py" --channels 1 2 4 --out-dir "$out/scaled"
"$python" "$here/channels.py" --channels 1 2 4 --iso-capacity --out-dir "$out/iso"
