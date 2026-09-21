# Fixed PTW and simulator performance investigation — 2026-09-14

Replacing detailed page walks with a fixed 200-cycle delay does **not** explain
most of the speed gap to Hermes. It improves KIPS by 3–9% on three tested windows,
but reduces KIPS by 15% on mcf because the simplified model simulates more cycles.
Hermes remains 2.35–5.01 times faster than current fixed-PTW ChampSim. The first
optimization targets are repeated allocations, small bandwidth helper calls,
core queue scans, and legacy DRAM address decoding.

This is a first controlled investigation of single-simulation elapsed time. It
implements the requested model switch and identifies optimization candidates;
it does not yet claim a behavior-preserving speedup of detailed ChampSim.
All experiments use **`dram-model=legacy`** and release builds with
`WITH_RAMULATOR2=0`.

## Workspace and source provenance

Work is in `/home/rbera/work/alakazam/champsim-perf-fix`, branch `feat/perf-fix`,
created by a separate `--no-hardlinks` clone of `feat/ramulator`. The original
`champsim-ramulator` checkout was not modified.

| Role | Revision |
|---|---|
| Original detailed ChampSim baseline | `74159f1e5dcb7cea1d27869c2ad8c51d19d0a5a4` |
| Fixed-PTW candidate used for all reported timings and profiles | `25959793` |
| Hermes reference | `0701249f4656b76a9bd36b5c3a63ee7b831274df` |

The watchdog correction is `17bd7fd5`; analysis tools and this report follow on
the same branch. The archived `champsim-candidate` binary was deliberately retained: all
performance results refer to its recorded hash, not an executable overwritten
by later builds. The followup changes default watchdog selection at startup;
normal 200-cycle experiments retain the existing 500-tick guard.

Raw evidence is outside Git:

```
/home/rbera/work/alakazam/champsim-perf-results/2026-09-14-ptw/
```

It contains original/candidate executables, the isolated Hermes build and source
selection patch, trace hashes and protected scratch copies, compiler/test logs,
host controls, per-run commands, stdout/stderr, statistics, timing records and
profiler data. The main measured campaign is `campaign/`; short compatibility
checks are `pilot/`. See [the tooling README](../../../tools/perf/README.md) for
commands and [the design](../../superpowers/specs/2026-09-14-fixed-ptw-performance-design.md)
for the model contract.

## Fixed PTW behavior

An overlay on the user's normal machine configuration selects the simplified
model:

```toml
dram-model = "legacy"

[ptw.cpu0_ptw]
model = "fixed"
fixed_latency = 200
```

`model = "detailed"` is the default. `fixed_latency` is measured in the owning
CPU's cycles, converted using `ooo_cpu.cpu0.frequency`. It is not a duration in
DRAM cycles. The fixed mode still models ITLB, DTLB and STLB lookup behavior;
only a miss reaching the walker uses the replacement service.

The builder carries an optional duration. Fixed mode skips CR3/PTE allocation
and PSCL construction, and dispatches to a bounded FIFO in the existing walker
operable. It uses `VirtualMemory::va_to_pa` for stable physical-page mapping and
sends no page-table requests to L1D. Admissions obey `max_read`, completions obey
`max_write`, and pending requests obey `mshr_size`.

The delay begins at admission and completion occurs at a PTW service tick. The
walker completes ready requests before accepting new ones, so zero-delay requests
return on the next tick. Warmup admissions bypass delay; requests already queued
keep their deadlines across phase boundaries. Responses retain virtual address,
physical page, metadata, CPU mapping identity and instruction dependencies.
No-response requests still release capacity. Waiting for a deadline is not
reported as simulator progress.

Strict configuration validation rejects unknown modes, negative/fractional or
overflowing delays, invalid clocks, and zero fixed-mode service limits.
`fixed_latency` supplied in detailed mode is an unused-key error. PSCL keys
remain accepted in fixed mode to permit overlays on full existing TOML files.

Review exposed a real edge case: a long fixed delay could exceed the default
500-tick no-progress watchdog. The followup derives the minimum default guard as
`ceil(delay / quantum) + ceil(walker_period / quantum) + 1`, where `quantum` is the
minimum operable clock period. It retains the larger existing guard and respects
an explicit `sim.deadlock_cycle`. An unrepresentable default is rejected. The
separate low-IPC livelock detector still applies to extreme configurations.

## Measurement controls and comparability

The host was an AMD Ryzen AI Max+ 395, 16 cores / 32 threads, about 64 GB RAM,
running Linux 7.0.0-30-generic. Each simulation and its children were pinned to
logical CPU 8; its SMT sibling 24 was not used by this campaign. Runs were
sequential, with no overlapping builds or other campaign simulations. Turbo was
enabled; the governor was `powersave` with `balance_performance` energy preference.
We recorded these settings without changing host policy. Frequency was not fixed,
and this was not an exclusively reserved machine.

Both source trees were rebuilt with system GCC 13.3 and `-O3`. Current ChampSim
uses C++17 and Hermes C++11. Inherited compiler/linker flags were unset. Catch2
objects were built separately so the test target's `-Og` flags could not contaminate
the timed release executable. Existing dependency libraries were reused; their
builds and linkage are not equivalent across the two simulators.

Hermes was built in an isolated clone with GLC, perceptron, no prefetchers and LRU.
Optional off-chip prediction, DDRP, DRAM-bandwidth measurement and cache-accuracy
measurement were explicitly disabled. Current ChampSim used
`configs/champsim_config.toml` plus `configs/perf-hermes.toml` to match available
GLC parameters: ROB/scheduler 512, execution/retirement width 6, mispredict penalty
17, cache/TLB geometry, and available memory timings. All command lines and
resolved current configurations are archived.

The models remain materially different:

- Core pipeline, register allocation, DIB and BTB algorithms are different.
- Hermes' 100-cycle page-table penalty is applied differently from a delay on
  every admitted STLB miss in current fixed mode.
- Fixed mode replaces the minor-page-fault penalty and allocates no page-table
  pages, changing physical-page assignments even with the same random seed.
- Current fixed mode enforces its default five pending walkers. Current detailed
  PTW does not enforce its `MSHR_SIZE` member. This concurrency difference was
  retained and disclosed rather than tuned after seeing mcf's result.
- DRAM scheduling, address mapping and refresh behavior differ.
- Trace decoding/branch reconstruction, statistics work and dependencies differ.

Thus Hermes is a useful historical speed reference, not a correctness oracle or
an isolated experiment on PTW overhead.

Four trace windows were used. Original inputs were hashed, copied, rehashed and
made read-only in the scratch results directory; originals were left untouched.

| Name | Trace |
|---|---|
| SQLite | v2 `708.sqlite_r.sp0.champsim2.zst` |
| omnetpp | v2 `710.omnetpp_r.sp0.champsim2.zst` |
| GCC | v2 `721.gcc_r.sp1.champsim2.zst` |
| mcf | v1 `605.mcf_s-1536B.champsimtrace.xz` |

The primary campaign uses **1 million warmup + 3 million ROI instructions**, three
repetitions of each model on each trace: 36 runs. Variant order rotates across
repetitions. Trace order is fixed. The Python harness measures high-resolution
whole-process wall time with a blocking process wait and a separate timeout
watchdog, and separately records child user/system CPU time.

**KIPS = (actual warmup retired + actual ROI retired) / (1,000 × process wall
seconds).** Actual retirement-width overshoot is included. The denominator
includes startup, trace reading/decompression, simulation and final reporting.
This is not ROI-only KIPS. Do not divide only ROI instructions by these timings.
CPU time was essentially equal to wall time in the primary campaign, consistent
with CPU-bound execution on this host and these warmed filesystem inputs.

## Measured speed gap

Values are median KIPS, with minimum–maximum across three runs in parentheses.

| Trace | Detailed ChampSim | Fixed 200 ChampSim | Hermes | Fixed vs detailed | Hermes / fixed |
|---|---:|---:|---:|---:|---:|
| SQLite | 206.93 (206.32–208.49) | 224.54 (224.26–225.69) | 681.84 (676.89–686.66) | +8.51% | 3.04× |
| omnetpp | 227.23 (224.93–230.07) | 234.39 (232.61–236.08) | 840.43 (826.64–846.57) | +3.15% | 3.59× |
| GCC | 281.41 (279.69–282.06) | 293.80 (291.62–295.08) | 691.12 (687.30–696.94) | +4.40% | 2.35× |
| mcf | 74.57 (74.41–75.29) | 63.29 (63.25–63.37) | 316.91 (315.91–317.82) | −15.12% | 5.01× |

The absolute Hermes advantage over fixed mode is **457.31, 606.04, 397.33 and
253.62 KIPS**, respectively. Main-campaign process medians range from 4.76 to
63.20 seconds, so these are not subsecond launch-time measurements.

To separate modeled work from host cost, compare total simulated CPU cycles
and throughput in thousands of simulated cycles per wall second:

| Trace | Detailed cycles | Fixed cycles | Detailed Kcycles/s | Fixed Kcycles/s | Hermes Kcycles/s |
|---|---:|---:|---:|---:|---:|
| SQLite | 5,577,087 | 5,251,972 | 288.52 | 294.81 | 1,031.91 |
| omnetpp | 5,671,679 | 5,659,403 | 322.19 | 331.63 | 1,536.20 |
| GCC | 4,121,158 | 3,960,073 | 289.93 | 290.87 | 1,062.60 |
| mcf | 10,735,394 | 14,928,705 | 200.13 | 236.21 | 1,816.69 |

On mcf, fixed mode increases the total cycle count by 39.06%. Host wall time per
simulated cycle falls from 4.997 to 4.233 microseconds, but the extra modeled cycles
more than offset that reduction. The fixed penalty, walker-capacity difference,
missing page-table traffic and changed page placement are all possible
contributors; this experiment does not isolate their individual effects.

## Profiling method and validity

`perf` hardware/software events are blocked by this host's
`perf_event_paranoid=4`. No security policy was changed. GNU gprofng 2.42 initially
appeared usable but reported CPU totals about one tenth of actual CPU time and
warned that its sampling timer had changed. A separate two-second CPU loop and
`strace` reproduced the problem outside either simulator: a 100 ms timer remained
active while the requested 10 ms timer was disarmed. These CPU-time profiles were
**rejected**, retained in `cpu-profiles/` with a warning, and are not the source of
the percentages below.

The accepted profiles use `gdb_sample.py`: GDB starts its own inferior, interrupts
it at deterministic jittered 15–25 ms intervals, unwinds up to 64 frames, and
continues without delivering SIGINT to the simulator. A pidfd identifies the
owned process. These are **wall-stack sample shares**, not hardware counters or
measured CPU seconds. Because the unprofiled runs are CPU-bound, they are useful
for ranking host work. Debugger elapsed time is never used as a KIPS result.

Nine full-window profiles cover SQLite, GCC and mcf, each with detailed, fixed
and Hermes binaries. They contain 283–3,407 samples each. Every profile reproduced
its reference's actual retirement and cycle counts; every current ChampSim
profile also reproduced the complete phase-statistics hash. A separate SQLite
probe collected 950 samples and agreed on the dominant core/cache paths.
Raw stacks, timestamps, program counters and symbols are in `stack-profiles/`;
`component-summary.csv` is generated by `summarize_stacks.py`.

Percentages are rounded sampling estimates, with only one main profile per case.
At 700–1,000 samples, small differences of a few percentage points should not be
ranked confidently; periodic behavior and sample correlation also limit simple
independent-sample error estimates. Function names do not identify every inlined
operation. Unknown symbols and startup/finish frames remain visible in the raw
records.

## Where current ChampSim spends its time

These categories are disjoint by enclosing component call path. “Framework”
includes cycle dispatch, environment views, phase completion checks and sorting.
Allocator work is charged to its caller's component here.

| Window / model | Samples | Core | Cache | PTW | Legacy DRAM | Trace | Framework |
|---|---:|---:|---:|---:|---:|---:|---:|
| SQLite detailed | 964 | 42.3% | 41.9% | 2.3% | 1.7% | 6.8% | 4.8% |
| SQLite fixed | 876 | 44.4% | 40.0% | 0.8% | 1.6% | 7.9% | 5.3% |
| GCC detailed | 703 | 37.0% | 43.2% | 2.3% | 2.3% | 9.2% | 5.7% |
| GCC fixed | 676 | 41.3% | 42.2% | 0.6% | 0.9% | 9.6% | 5.3% |
| mcf detailed | 2,641 | 45.3% | 29.0% | 1.7% | 18.6% | 1.7% | 3.7% |
| mcf fixed | 3,407 | 50.2% | 32.5% | 0.4% | 10.9% | 1.5% | 4.5% |

Startup/finish/other accounts for the remaining 0.0–0.3%, before rounding.
Detailed PTW itself occupies about 2% of stacks, but this is not its total cost:
its generated requests also cause cache, DRAM and later core work.

Additional overlapping views of those same samples identify useful targets:

| Window / model | Allocator call-path union | Bandwidth helper exclusive leaves | Schedule/execute/complete ROB-scan call-path union |
|---|---:|---:|---:|
| SQLite detailed | 8.8% | 11.9% | 21.4% |
| SQLite fixed | 10.2% | 10.3% | 21.3% |
| GCC detailed | 11.9% | 10.7% | 16.5% |
| GCC fixed | 12.9% | 9.3% | 20.0% |
| mcf detailed | 5.5% | 12.8% | 31.0% |
| mcf fixed | 5.9% | 15.0% | 34.0% |

Do not add these to the component percentages or to one another. For example,
`execute_instruction()` includes bandwidth checks and child execution work;
removing its scans cannot recover its entire inclusive percentage. Allocator
stacks include time below allocation/free entry points, not all object-copying
or cache-locality cost.

Hermes showed only 0–0.35% allocator stacks in its three smaller profiles; absence
of samples is not proof of zero allocations. Its work is concentrated in
`reg_dependency`, `schedule_memory_instruction` and `update_rob`. Different
algorithms and much lower absolute time mean that a similar percentage does not
mean similar host cost per instruction. Its trace path also remains visible
(roughly 4–12% inclusive, before separating virtual-address allocation).

## Concrete hotspots and proposed order of optimization

1. **Remove per-cycle debug-only allocations.** In
   `src/cache.cc`, `CACHE::operate` constructs `channels_bandwidth_consumed`,
   appends one value for each upstream WQ/RQ/PQ, and uses it only inside
   `if constexpr (champsim::debug_print)`. Release allocation paths remain visible.
   The matched one-core topology performs **25 allocations per global cycle**
   for these vectors: 3 each in LLC/ITLB/DTLB/L1I, 4 each in L1D/L2C, and 5 in
   STLB, as their upstream counts grow capacities through powers of two.
   Put the vector and updates behind that compile-time condition. This is the
   narrowest first change: preserve all bandwidth accounting and the existing
   debug output. Measure the actual resulting speedup separately.

2. **Stop rebuilding temporary environment vectors every global tick.**
   `src/champsim.cc::do_cycle` receives `trace_index` by value, reconstructs
   `operable_view`, sorts it, and constructs `cpu_view`. `do_phase` copies a
   `vector<bool>` and constructs CPU views twice more each tick.
   `src/static_environment.cc::operable_view` grows a vector without reserving
   capacity. Together these temporary vectors account for another **10
   allocations per global cycle** in this build: five for operables, three CPU
   views, one trace-index copy and one phase-completion copy.
   Pass invariant data by const reference and reuse phase-local storage.
   Preserve the original canonical input order before each `std::sort`: retaining
   the previous sorted order can change equal-time component ordering and results.
   Avoid changing the sort algorithm or event scheduling in this first patch.

3. **Inline the small bandwidth helpers, then evaluate other build changes
   separately.** `has_remaining`, `amount_remaining`, the constructor and
   `consume` live out of line in `src/bandwidth.cc`; helpers collectively account
   for 9–15% of exclusive samples. Their callers include cache and ROB loops.
   Disassembly confirms `has_remaining` is an out-of-line `cmp`/`setg`/`ret`
   sequence (plus `endbr64`), not substantial computation.
   An inline definition can remove call boundaries and expose loop simplification.
   Preserve signed accounting and the `consume` underflow exception; this is not
   permission to remove bounds checks. A separate LTO experiment may be useful,
   but should not be mixed into the first source change or credited twice.

4. **Avoid repeated legacy DRAM address decoding.** On detailed mcf,
   `bank_request_index` is on 17.1% of stacks; `swizzle_bits` alone has 7.6%
   exclusive samples and `get_bankgroup` another 5.9%. In
   `src/dram_controller.cc::schedule_packet`, the minimum-element comparator
   decodes both candidates repeatedly for each queue scan. Cache bank/rank/group
   indices when admitting an immutable-address request. Precompute geometry
   masks/offsets and consider a zero-width-field fast path: this campaign has one
   bank group, yet generic swizzling still runs. Keep address mapping and packet
   priority/tie behavior identical. Test multiple channels/ranks/bank groups as
   well as the one-group benchmark before accepting the change.

5. **Reduce repeated ROB and LSQ scans while preserving ordering.**
   `schedule_instruction`, `execute_instruction` and
   `complete_inflight_instruction` repeatedly traverse ROB entries, especially
   when memory blocks progress. `do_execution` scans the entire LQ and SQ even for
   instructions without memory operations. First examine cheap guards and cached
   immutable facts. Ready queues, direct LSQ references and incremental scheduling
   are larger changes: they must preserve issue width, program-order priority,
   register dependencies, store forwarding, completion and phase transitions.
   An idealized ready-queue rewrite should not be the first performance patch.

6. **Eliminate trace instruction copies before tuning I/O buffers.**
   In `inc/tracereader.h`, consuming the deque copies its front instruction;
   reverse `adjacent_difference` for branch targets and `apply_branch_target`
   also copy vector-owning instruction objects. Investigate moving a consumed
   front and an in-place target update. Preserve lookahead across the 127-record
   refill boundary, final records, trace repeat behavior, branch metadata and both
   trace formats. Trace/decompression is 7–10% on SQLite/GCC and about 1.5–1.7% on
   mcf; it cannot alone explain the latter's large speed gap. Hermes' larger
   compressed-stream buffers suggest a later controlled buffer-size experiment,
   not evidence that storage is currently the main bottleneck.

These percentages are bounds on where to investigate, not additive promised
speedups. No optimization in this list has yet been benchmarked as a candidate.

## Allocation event evidence

GNU gprofng heap events were calibrated separately from its broken CPU timer:
the known-count probe reports exactly 1,000 allocations and 64,000 bytes. The
same timer warning is retained with each heap run and accepted only for allocation
events. No gprofng CPU seconds are used. Heap tracing is extremely intrusive:
the first 20k/100k window was stopped as too expensive and excluded; accepted
probes use **1k warmup + 4k ROI** with separately recorded reference statistics.
These tiny windows are not throughput measurements or representative estimates
of whole-program allocation rates. Their short reference runs overlapped the
end of the abandoned heap pilot, so their recorded KIPS must not be used.

The completed SQLite detailed probe retired 5,004 instructions over 38,305 CPU
cycles and recorded **1,519,972 allocations**, including startup and warmup. Of
those, **957,625 = 25 × 38,305** came from the debug-only cache vector. Operable
views caused 191,540 allocations (five per cycle plus phase/startup calls), and
CPU views another 114,926 (three per cycle plus initialization/reporting).
Source and archived release disassembly confirm the two other per-cycle
temporary copies also allocate; they are not separately attributed heap counters.
The 35 combined recurring allocations per cycle are specific to this topology,
clocking and libstdc++ vector growth; they are not a universal ChampSim constant.
Periodic livelock checks add another CPU-view allocation on longer runs. A second
read-only review verified both the arithmetic and the emitted allocation calls.

The same probe attributes 53,747 allocations to the trace reader, including
23,932 beneath branch-target reconstruction. Across the simulator, instruction
copy constructors account for 44,614 allocations. These inclusive counts overlap:
they must not be added to the trace-reader total. Some optimized ancestor frames
are absent in gprofng's unwinds, so we rely on identifiable allocation paths and
code inspection rather than treating its complete parent tree as exact.
Cumulative allocated bytes are not peak live memory or resident-set size.

The SQLite fixed probe also retired 5,004 instructions and recorded 1,171,993
allocations, of which 712,675 were the same 25-per-cycle debug-vector pattern.
Hermes retired 5,005 instructions and recorded only 7,357 total allocations in its
short SQLite probe. These are startup-inclusive diagnostic windows, not a claim
that the corresponding whole-program allocation-rate ratio is constant.

All six heap collections completed and reproduced reference retirement and
cycle counts; the four ChampSim collections also reproduced full phase hashes.
Total event counts include startup and warmup:

| Window / model | Retired instructions | Simulated cycles | Total allocations | Debug-vector allocations |
|---|---:|---:|---:|---:|
| SQLite detailed | 5,004 | 38,305 | 1,519,972 | 957,625 |
| SQLite fixed | 5,004 | 28,507 | 1,171,993 | 712,675 |
| SQLite Hermes | 5,005 | 29,674 | 7,357 | — |
| mcf detailed | 5,007 | 36,467 | 1,478,260 | 911,675 |
| mcf fixed | 5,007 | 22,909 | 989,906 | 572,725 |
| mcf Hermes | 5,003 | 33,957 | 6,154 | — |

Every current-model probe has exactly 25 debug-vector allocations per simulated
CPU cycle with these equal CPU/cache clocks. This is evidence for recurring host
work, not a comparison of equivalent microarchitectural execution across models.
Accepted event exports and reference checks are stored in `heap-small-profiles/` and
`heap-small-reference/`; `export_heap.py` in the archive produces
`allocation-summary.csv`.

## Validation and remaining stress work

The feature's ten C++ cases / 61 assertions cover fixed timing, clock conversion,
backpressure, completion bandwidth, mapping and response contracts. They failed
against the original implementation and pass with fixed mode. The complete C++
suite passes **17,049 assertions in 863 cases**, with seven native Ramulator tests
skipped in this legacy-only build. A baseline test setup initially omitted the
runtime registry include in its separate object directory; that setup failure
was diagnosed and corrected before the implementation comparison.

Five CLI regression cases exercise watchdog derivation, unequal CPU/global
clocks, explicit overrides, representability and a real long-delay simulation.
Four failed before the followup fix; all five pass afterward. Detailed default
behavior is checked against the saved pre-feature binary on all four pilot
traces. Main-campaign repetitions preserve full phase statistics within each
current model. Fixed runs have zero translation-access counters in data caches.
After the watchdog correction, all eight detailed/fixed pilot replays match the
benchmarked feature binary, and four fixed runs also replay identically when
configured from their own statistics TOML. An initial replay script omitted the
v2 trace-version CLI argument; that setup failure was corrected and retained
in the archive. Trace version remains a CLI input, separate from model TOML.

The full Python suite reports 66 tests, with four native-backend skips, and the five performance
tooling tests pass. The followup code review accepted the watchdog correction
and caught a profiling validation loophole: instruction-window overrides could
bypass reference checks. Those overrides were removed; the final profiling tool
requires a matching reference campaign for every collection. None of the
accepted profiles used the removed override path.

The remaining checks before mainline integration and later optimization claims
are substantial:

- Repeat with the normal ChampSim configuration as well as the matched GLC
  overlay: increasing ROB/scheduler capacities to 512 can magnify scan costs.
- Longer ROI windows and multiple trace offsets, including more translation-heavy
  and dependency-heavy programs. One 3M ROI window is not an entire workload.
- More seeds, low/high fixed delays, different CPU/PTW clocks, queue and bandwidth
  sizes, and sustained saturation. In particular, isolate the fixed/detailed
  walker-capacity difference rather than attributing its effects to host speed.
- Multicore configurations, shared caches, heterogeneous clocks and independent
  per-core PTW selections. Direct mapping tests are not a multicore stress run.
- Multiple legacy DRAM geometries and refresh conditions. The one-bank-group
  comparison is useful but will overrepresent some no-op mapping opportunities.
- Randomized, long-running invariants for admitted/completed requests and phase
  transitions, plus sanitizer coverage where feasible.
- Repeat profiles and timing on another host/compiler; obtain hardware cache,
  branch and instruction counters on a host that permits `perf`.
- Evaluate each behavior-preserving patch separately against complete phase
  statistics, not only IPC or total cycles. Re-run the main timing matrix with
  rotated order, then add longer windows. Keep model simplification and source
  optimization results distinct.

Single-run latency remains the priority. Concurrent throughput, peak process
memory, cold-filesystem behavior and native Ramulator performance were not goals
of this campaign and have not been characterized here.
