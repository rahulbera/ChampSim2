#!/usr/bin/env python3
"""Extract a (pc, next_ip, branch_type, taken) branch stream from a v2 ChampSim trace.

The BLBP tuner needs the conditional-outcome stream (to build GHIST) and the
indirect-target stream (to build local histories and score candidates) in one
sequence, so every branch record is kept, tagged with the v2 trace's explicit
branch type (reserved[0], gated on the feature bit in reserved[1] & 0x01) and
the trace's branch_taken byte. Targets are derived the way ChampSim derives
them -- the next instruction's IP -- so the stream exercises the same
index/tag/history paths a real run would.

Record: little-endian packed (pc: u64, next_ip: u64, branch_type: u8, taken: u8)
= 18 bytes. branch_type values follow ChampSim's enum (inc/instruction.h):
0=direct_jump 1=indirect 2=conditional 3=direct_call 4=indirect_call 5=return 6=other.

Usage: extract_stream.py <trace.zst> <out.stream> <n_records> [warmup_instructions]
       n_records = 0 means the whole trace.
       warmup_instructions (default 50_000_000): a sidecar <out>.meta.json
       records how many BRANCH RECORDS fall inside the warmup window, so the
       tuner can warm the predictor on exactly the instructions a full
       ChampSim run warms on and count mispredicts only after the boundary.
"""
import json
import struct, sys, zstandard

REC, OFF_IP, OFF_ISBR, OFF_TAKEN, OFF_BTYPE, OFF_FEAT = 512, 0, 8, 9, 120, 121
INDIRECT, CONDITIONAL, INDIRECT_CALL = 1, 2, 4  # champsim branch_type enum

def main(src, dst, limit, warmup_instrs=50_000_000):
    d = zstandard.ZstdDecompressor(max_window_size=2**31)
    out = open(dst, "wb"); prev = None; n = nbr = ncond = nind = 0
    warmup_branches = None
    with open(src, "rb") as fh, d.stream_reader(fh) as r:
        buf = b""
        while not limit or n < limit:
            c = r.read(1 << 22)
            if not c: break
            buf += c; u = (len(buf)//REC)*REC
            for off in range(0, u, REC):
                ip = struct.unpack_from("<Q", buf, off)[0]
                if prev is not None:
                    p_ip, p_isbr, p_bt, p_tk = prev
                    if p_isbr:
                        out.write(struct.pack("<QQBB", p_ip, ip, p_bt, p_tk))
                        nbr += 1
                        ncond += p_bt == CONDITIONAL
                        nind += p_bt in (INDIRECT, INDIRECT_CALL)
                if warmup_branches is None and n >= warmup_instrs:
                    warmup_branches = nbr
                assert buf[off+OFF_FEAT] & 0x01, "trace lacks explicit branch type"
                prev = (ip, buf[off+OFF_ISBR], buf[off+OFF_BTYPE], buf[off+OFF_TAKEN])
                n += 1
                if limit and n >= limit: break
            buf = buf[u:]
    out.close()
    if warmup_branches is None:
        warmup_branches = nbr  # trace shorter than the warmup window
    with open(dst + ".meta.json", "w") as mf:
        json.dump({"records": n, "branches": nbr, "conditionals": ncond,
                   "indirects": nind, "warmup_instructions": warmup_instrs,
                   "warmup_branches": warmup_branches}, mf)
    print(f"records={n} branches={nbr} conditionals={ncond} indirects={nind} warmup_branches={warmup_branches}")

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], int(sys.argv[3]),
         int(sys.argv[4]) if len(sys.argv) > 4 else 50_000_000)
