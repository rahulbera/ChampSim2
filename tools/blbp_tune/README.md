# BLBP tuning streams

Stream tooling for the BLBP tuning campaign (design:
`docs/superpowers/specs/2026-08-13-blbp-design.md`, section 4). A stream is
the branch-only projection of a v2 ChampSim trace, small enough to replay
millions of times during hill-climbing without touching the simulator. The
tuner itself lives elsewhere; this directory is extraction + sanity only.

## Record format

One record per **branch** instruction, in trace order. 18 bytes, little-endian,
packed (`struct` format `<QQBB`), no header, no padding:

| Offset | Size | Field         | Meaning |
|-------:|-----:|---------------|---------|
| 0      | 8    | `pc`          | u64, IP of the branch |
| 8      | 8    | `next_ip`     | u64, IP of the next instruction in the trace — the target if taken, the fall-through if not. This is exactly how ChampSim derives targets. |
| 16     | 1    | `branch_type` | u8, ChampSim `branch_type` enum (`inc/instruction.h`): 0=direct_jump, 1=indirect, 2=conditional, 3=direct_call, 4=indirect_call, 5=return, 6=other |
| 17     | 1    | `taken`       | u8, the trace's `branch_taken` byte (0/1) |

`branch_type` is the v2 trace's **explicit** type from `reserved[0]` (byte 120
of the 512-byte record); the extractor asserts the feature bit
(`reserved[1] & 0x01`, byte 121) on every record, so a pre-v2 trace fails loudly
instead of silently reading zeros. `taken` is the trace's byte at offset 9.

The last instruction of the window is never emitted (its `next_ip` is unknown),
matching `tools/ittage_equiv/extract_stream.py`, which this extends.

## Extracting

```bash
python3 tools/blbp_tune/extract_stream.py <trace.champsim2.zst> <out.stream> <n_records>
```

`n_records` counts trace **instructions** read (not branches emitted);
`0` means the whole trace. Requires the `zstandard` Python package. Prints:

```
records=3000000 branches=734004 conditionals=715786 indirects=5142
```

(`records` = instructions read; `indirects` = types 1 and 4.) For the campaign:
each of the 40 training traces extracted once, full 250M window
(`n_records=0` on a 250M-instruction trace), stored beside the traces on the
cluster. Rough size: ~18 B x ~20-25% branch density ≈ 1-1.2 GB per 250M window.

## Sanity-checking

```bash
python3 tools/blbp_tune/stream_stats.py <out.stream> [more.stream ...]
```

Prints the branch-type histogram with per-type taken rates. What to check:

- **Every non-conditional type must be 100% taken** (jumps, calls, returns,
  indirects are unconditional). Anything else means the taken byte is
  misattributed.
- **Conditional taken rate strictly inside (0, 100)** — 0 or 100 means a
  broken taken byte, not a real workload.
- **Indirect share matches the workload's known character** — e.g. SPEC
  omnetpp ≈ 1.7 indirect branches per kilo-instruction (~2.0 indirect-MPKI
  territory); agentic traces several times that. Per-kilo-instruction rates
  need the extractor's `records` count, since the stream holds only branches.
- **File size must be exactly 18 x branches** — `stream_stats.py` asserts
  size % 18 == 0 and a manual `ls -l` cross-check against the extractor's
  `branches` count catches truncation.

Smoke reference (first 3M instructions):

| Trace | branches | cond taken | indirect share |
|---|---:|---:|---:|
| 710.omnetpp_r.sp0 | 734,004 | 29.05% | 0.70% (1.71/KI) |
| swe_agent_w00001 | 579,729 | 34.40% | 6.66% (12.9/KI) |
