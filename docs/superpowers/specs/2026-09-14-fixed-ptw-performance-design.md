# Fixed PTW latency: first performance experiment

The user requested an optional fixed-latency replacement for detailed page walks,
followed by a KIPS comparison with Hermes. This is the first step of the broader
performance investigation, not a behavior-preserving optimization of detailed PTW.
Implementation and benchmarking are authorized by that request.

## Scope and configuration

Work in the separate clone `/home/rbera/work/alakazam/champsim-perf-fix`, branch
`feat/perf-fix`, based on `feat/ramulator` at `74159f1e`. All experiments use
legacy DRAM; build ChampSim with `WITH_RAMULATOR2=0`.

Each `[ptw.cpuN_ptw]` table gains `model = "detailed"` (default) or `"fixed"`.
`fixed_latency = 200` selects the delay in that CPU's cycles, converted to
picoseconds using its configured frequency. Reject unknown model names, negative
or unrepresentable delays, and a fixed-latency key supplied in detailed mode.
Zero is allowed for diagnostic experiments. Existing PTW geometry keys remain
accepted so a complete configuration can switch models by an override; PSCL
geometry has no effect in fixed mode. Keep the existing PTW clock, admission and
completion bandwidth settings, and request channel.

## Fixed-mode behavior

Keep ITLB, DTLB and STLB modeling. At the STLB miss interface, accept requests
into a FIFO delayed-response queue and map pages with `VirtualMemory::va_to_pa`.
Do not construct PSCLs, allocate a CR3 page, call `get_pte_pa`, or issue any
page-table cache/DRAM traffic. The fixed delay replaces all page-walk and minor
fault latency; allocation still supplies deterministic, per-CPU physical pages.
Data-page placement can differ from detailed mode because page-table pages no
longer consume the allocator's free list.

Limit outstanding requests to `mshr_size`, admissions to `max_read`, and responses
to `max_write`. Require positive capacity and bandwidth in fixed mode. Latency
starts on admission, excluding upstream queueing, and completion is observed on
the first walker tick at or after its deadline. Service ready requests before new
admissions, so zero-latency and warmup requests return on the next walker tick.
Warmup admissions bypass the fixed delay; previously accepted requests retain
their original deadlines and mappings across phase boundaries. Complete requests
once, retain virtual addresses, metadata and dependencies, and honor
`response_requested`. A retained delayed request is not fake progress.

Detailed mode retains its existing algorithm, allocations and scheduling. The
effective configuration records the selected mode and, when selected, fixed
latency. Replay must reproduce the selected machine.

## Evidence and speed comparison

First prove exact detailed-mode phase-statistics parity with the saved baseline,
then prove deterministic fixed-mode replay and absence of page-table traffic.
Unit tests exercise delay boundaries, CPU-versus-PTW clock scaling, warmup and
phase carry-over, bounded backpressure, completion bandwidth, metadata, no-response
requests, page reuse and per-CPU mapping. Use real PTW/channel/vmem components.

Build a fresh Hermes reference from
`/home/rbera/work/hermes-uncore/Hermes` at
`0701249f4656b76a9bd36b5c3a63ee7b831274df` in a separate scratch clone. Disable
its optional prediction/prefetch features. Use the same compiler, trace bytes,
single-core CPU affinity, warmup and ROI counts, and record effective model
differences. Hermes is a historical speed reference, not a correctness oracle.

Measure three variants: current detailed, current fixed-200, and Hermes. Run a
pilot to size the first campaign; use at least three representative supplied
traces and three interleaved repetitions. Do not build or run other experiments
concurrently with timing. Keep input files read-only and put every output in a
dedicated directory. Preserve hashes, commands, binaries, build logs, timings,
host information and parsed statistics.

Primary KIPS is actual retired instructions (warmup plus ROI) divided by 1,000
times externally measured process wall seconds, including startup and final
reporting. Also report CPU seconds, simulated cycles, median/range across runs,
and absolute and relative KIPS gaps. If reliable phase-only timing is available,
label it separately; do not divide ROI-only work by whole-process time. Different
cycle counts and modeling fidelity limit attribution of a KIPS gap to host code.

The first report stops at measured gaps and recommendations for the next
profiling step. It does not implement unrelated cycle-loop or trace optimizations.
