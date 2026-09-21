#!/usr/bin/env python3
"""Extract a (PC, target, is_indirect) branch stream from a v2 ChampSim trace.

The equivalence harness needs both predictor copies driven by an identical
sequence. Targets are derived the way ChampSim derives them -- the next
instruction's IP -- so the stream exercises the same index/tag/history paths a
real run would.
"""
import struct, sys, zstandard

REC, OFF_IP, OFF_ISBR, OFF_BTYPE, OFF_FEAT = 512, 0, 8, 120, 121
INDIRECT, INDIRECT_CALL = 1, 4          # champsim branch_type enum

def main(src, dst, limit):
    d = zstandard.ZstdDecompressor(max_window_size=2**31)
    out = open(dst, "wb"); prev = None; n = nbr = nind = 0
    with open(src, "rb") as fh, d.stream_reader(fh) as r:
        buf = b""
        while n < limit:
            c = r.read(1 << 22)
            if not c: break
            buf += c; u = (len(buf)//REC)*REC
            for off in range(0, u, REC):
                ip = struct.unpack_from("<Q", buf, off)[0]
                if prev is not None:
                    p_ip, p_isbr, p_bt = prev
                    if p_isbr:
                        ind = 1 if p_bt in (INDIRECT, INDIRECT_CALL) else 0
                        out.write(struct.pack("<QQB", p_ip, ip, ind))
                        nbr += 1; nind += ind
                assert buf[off+OFF_FEAT] & 0x01, "trace lacks explicit branch type"
                prev = (ip, buf[off+OFF_ISBR], buf[off+OFF_BTYPE])
                n += 1
                if n >= limit: break
            buf = buf[u:]
    out.close()
    print(f"records={n} branches={nbr} indirect={nind}")

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], int(sys.argv[3]))
