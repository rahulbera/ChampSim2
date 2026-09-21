#!/usr/bin/env python3
"""Sanity-check a BLBP tuning stream (see extract_stream.py for the format).

Prints a branch-type histogram with per-type taken rates, plus the two numbers
worth eyeballing against known workload behaviour: the conditional taken rate
(must be strictly inside (0, 100) on any real trace) and the indirect share.

Usage: stream_stats.py <stream> [<stream> ...]
"""
import struct, sys

REC = 18
NAMES = {0: "direct_jump", 1: "indirect", 2: "conditional", 3: "direct_call",
         4: "indirect_call", 5: "return", 6: "other", 7: "not_branch"}
INDIRECT, CONDITIONAL, INDIRECT_CALL = 1, 2, 4

def stats(path):
    cnt, taken, odd = {}, {}, 0
    with open(path, "rb") as fh:
        while True:
            b = fh.read(REC * 65536)
            if not b: break
            assert len(b) % REC == 0, f"{path}: size not a multiple of {REC}"
            for off in range(0, len(b), REC):
                _, _, bt, tk = struct.unpack_from("<QQBB", b, off)
                cnt[bt] = cnt.get(bt, 0) + 1
                taken[bt] = taken.get(bt, 0) + (tk != 0)
                odd += tk > 1
    total = sum(cnt.values())
    print(f"{path}: branch_records={total}")
    if odd:
        print(f"  WARNING: {odd} records have taken byte > 1")
    for bt in sorted(cnt):
        n, t = cnt[bt], taken[bt]
        print(f"  {NAMES.get(bt, f'type{bt}'):13s} n={n:10d}  share={100*n/total:6.2f}%  taken={100*t/n:6.2f}%")
    ncond, nind = cnt.get(CONDITIONAL, 0), cnt.get(INDIRECT, 0) + cnt.get(INDIRECT_CALL, 0)
    cond_rate = 100 * taken.get(CONDITIONAL, 0) / ncond if ncond else float("nan")
    print(f"  conditional_taken_rate={cond_rate:.2f}%  indirect_share={100*nind/total:.2f}% ({nind} of {total} branches)")

if __name__ == "__main__":
    for p in sys.argv[1:]:
        stats(p)
