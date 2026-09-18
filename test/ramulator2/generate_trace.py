#!/usr/bin/env python3
"""Generate a bounded, deterministic little-endian ChampSim v2 memory trace."""
import argparse
from pathlib import Path
import struct


def generate(output, instructions):
    if not 1 <= instructions <= 1_000_000:
        raise ValueError("instructions must be between 1 and 1000000")
    # Exclusive creation protects an existing trace or any other user input.
    with Path(output).open("xb") as stream:
        for index in range(instructions):
            record = bytearray(512)
            struct.pack_into("<Q", record, 0, 0x400000 + 4 * (index % 512))
            record[10] = 1 + index % 16
            address = 0x800000 + ((index * 67) % 524288) * 64
            store = index % 4 == 0
            struct.pack_into("<Q", record, 16 if store else 32, address)
            record[116 if store else 112] = 8
            stream.write(record)
    Path(output).chmod(0o444)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--instructions", type=int, default=32768)
    args = parser.parse_args()
    generate(args.output, args.instructions)
