# Known bugs

A running task list of the open defects in this fork, most of them found during the
SMT roadmap review (`docs/smt-integration/2026-09-21-roadmap-critique.md` §10, on the
`feat/smt` branch). Each entry states the symptom, the root cause with `file:line`, how
to reproduce it, and the recommended fix.

Only open bugs are listed. When a bug is fixed, delete its entry in the same commit
as the fix, and put the evidence in the commit message. IDs are never reused, so a gap
in the numbering is a fixed bug: B1, perfect TLBs mapping every page to physical page
0, was fixed in `fcd13089`.

The rules for a fix:

- Start with a test that fails for the right reason.
- State whether the fix is inert or non-inert.
  - An inert fix must pass `tools/perf/compare_optimization.py`, including
    `--regression`.
  - A non-inert fix lands alone, with the difference explained and a new baseline
    recorded.
- User-trippable errors throw, so `main()` prints `ERROR: ...` and exits 1.
  Invariants use `CHAMPSIM_ASSERT`.

Line numbers are at `81275433` unless an entry says otherwise.

## Summary

| ID | Bug | Severity | Status |
|---|---|---|---|
| [B2](#b2-the-schedulers-register-check-runs-on-already-renamed-entries) | The scheduler's register check runs on already-renamed entries: spurious stops, and out-of-bounds reads above 256 | Medium | Open |
| [B3](#b3-empty-sub-record-or-unreadable-traces-segfault) | Empty, sub-record or unreadable traces segfault | Medium | Open |
| [B4](#b4-a-tlb-prefetcher-deadlocks-the-walk-path) | A TLB prefetcher deadlocks the walk path | Medium | Open |
| [B5](#b5-an-mshr-merge-of-two-virtual-addresses-strands-a-waiter) | An MSHR merge of two virtual addresses strands a waiter | Medium (latent) | Open |
| [B6](#b6-exhausting-physical-memory-silently-aliases-pages) | Exhausting physical memory silently aliases pages | Low | Open |
| [B7](#b7-the-v2-branch-type-probe-consumes-pipe-input) | The v2 branch-type probe consumes pipe input | Medium | Open |
| [B8](#b8-trace-end-of-file-handling) | Trace end-of-file handling: FIFO hang, no drain, N−1 laps, `-` | Low–Medium | Open |
| [B9](#b9-small-defects) | Small defects (no-op flag, wrong overload, dead declaration, path parsing) | Low | Open |

---

## B2. The scheduler's register check runs on already-renamed entries

**Status: open.** Severity: medium. Inherited from upstream ChampSim, which fixed it
on `develop` in `f46c1ff0` (2026-07-07), after this fork's base `51588e1d`. The fix
is **non-inert**: it changes when the scheduler stops under register pressure, so it
must land alone and be re-baselined.

**Symptom.** One misplaced check, two effects:

- At every shipped configuration (128 registers by default, 200 in `lnc.toml`) the
  check charges entries that are already renamed for registers they already hold,
  and stops the ROB walk spuriously. Correcting it moves ROI cycles by −0.30% to
  +0.11% (measured below).
- Above 256 registers it reads `frontend_RAT` out of bounds, which is undefined
  behaviour. The validator accepts 1–32767 (`src/static_environment.cc:165-173`) and
  CLAUDE.md documents that range, and nothing crashes.

**Root cause.**

- `O3_CPU::schedule_instruction` computes `count_if(source_registers,
  !isAllocated(src))` plus `destination_registers.size()`, and `break`s when fewer
  registers are free (`src/ooo_cpu.cc:441-447`). It does this for every entry it
  visits, *before* the `!scheduled` test at `:448`. The release binary's machine
  code has the same order.
- `do_scheduling` overwrites a scheduled entry's sources and destinations with
  **physical** IDs in place (`:464-472`), and nothing clears `scheduled`.
- `isAllocated` indexes the 256-entry architectural `frontend_RAT` with them
  (`src/register_allocator.cc:79`, `inc/register_allocator.h:22`). Architectural IDs
  come from `unsigned char` trace fields, so only physical IDs reach 256 or more.
- The walk charges its `SCHEDULER_SIZE` budget only for unexecuted entries
  (`:453-455`), and the scheduled entries always form a prefix of the ROB. So every
  walk runs the check over the whole scheduled prefix, executed or not, before it
  reaches the first entry that actually needs registers.

For a renamed entry the count is "how many of its physical IDs are unmapped
*architectural* registers" plus its destinations. The correct need is 0. The wrong
count is never below the correct one, so from the same state the buggy walk can only
stop earlier.

Where an out-of-bounds index lands (release build; `RegisterAllocator` is 1128 bytes
at `O3_CPU+0x4d8`):

| Index | Memory read |
|---|---|
| 256–511 | `backend_RAT` |
| 512–563 | `free_registers` and `physical_register_file` internals |
| 564–647 | later `O3_CPU` members |
| ≥ 648 | outside the `O3_CPU` object, up to about 64 KiB away at 32766 |

`rename_src_register` also pops the free list without an emptiness check
(`src/register_allocator.cc:40-41`); `rename_dest_register` asserts (`:23`). Today
the check always runs just before the pop, so this is a missing invariant, not
something a user can trip.

**History.**

- Upstream `b51f61bd` (2024-10-21) put the check before the `scheduled` test, with
  the raw operand counts.
- Upstream `d05dd1f2` (2024-11-19) added `isAllocated` and the source term, which
  is what feeds physical IDs to the architectural RAT.
- The fork's runtime key has accepted any size since `d20d00a3` (2026-08-21), so the
  out-of-bounds read has been reachable with `register_file_size` ≥ 257 since then.
  `b80b2a7f` only formalised and documented 1–32767.
- The performance log recorded the out-of-bounds read at 512 registers and deferred
  it (`docs/research-log/Performance/2026-09-14-performance-optimization.md:847-875`).

**Reproduction.** All runs use the release binary built from `81275433` (unmodified
files).

The out-of-bounds read, on 605.mcf (v1) with `--set
ooo_cpu.cpu0.register_file_size=512 -w 0 -i 20000` and a gdb breakpoint on
`RegisterAllocator::isAllocated` when its argument exceeds 255: the first hit is
`archreg=258` from `src/ooo_cpu.cc:444`, which reads `backend_RAT[2]`. At 4000
registers, 3000 instructions make 5,317,843 out-of-bounds calls, the largest index
being 3998.

Detection (checked with GCC 13.3 on a `std::array<short, 256>` read at index 257):

| Build | Catches it |
|---|---|
| `-D_GLIBCXX_ASSERTIONS` | yes, aborts in `std::array::operator[]` |
| `-fsanitize=bounds-strict` | yes |
| `-fsanitize=undefined`, `-fsanitize=bounds` | no |
| `-fsanitize=address` | no: indices below 648 stay inside the object |
| release | no |

So the `native_sanitize` CI job (`address,undefined`) cannot see this class of bug.

How often the check stops the walk on an already-scheduled entry. These counts come
from a non-stopping gdb breakpoint at the loop exit, which leaves the output
byte-identical to a plain run, at `-w 200000 -i 1000000`:

| Run | ROI scheduler calls | Spurious stops | Genuine stops |
|---|---|---|---|
| mcf v1, defaults (128 registers) | 3,224,117 | 3,019,388 (93.7%) | 199,205 |
| 721.gcc_r v2, defaults | 1,452,264 | 548,175 (37.7%) | 44,206 |
| 708.sqlite_r v2, `lnc.toml` (200 registers) | 1,620,425 | 451,155 (27.8%) | 54,392 |

Most spurious stops happen with no register free (88% of mcf's), where a correct
check would usually stop at the next entry anyway. That is why the effect on results
is small; the stop frequency is not a measure of impact.

**Measured effect of the fix.** The fix was emulated under gdb by patching the
in-memory branch so the check is skipped for scheduled entries; it is not a built
fix. Upstream's exact form (check only `!scheduled && ready_time <= now` entries) was
emulated separately and gave byte-identical documents in all four cases compared.
ROI cycles, before → after:

| Case | Window | Before | After | Change |
|---|---|---|---|---|
| mcf, defaults | 200k/1M | 3,224,159 | 3,214,629 | −0.296% |
| gcc, defaults | 200k/1M | 1,507,986 | 1,507,989 | +0.000% |
| sqlite, defaults | 200k/1M | 1,705,097 | 1,705,104 | +0.000% |
| omnetpp, defaults | 200k/1M | 1,198,800 | 1,195,976 | −0.236% |
| mcf, LNC | 200k/1M | 1,756,163 | 1,755,923 | −0.014% |
| gcc, LNC | 200k/1M | 1,513,086 | 1,512,745 | −0.023% |
| sqlite, LNC | 200k/1M | 1,641,083 | 1,642,939 | +0.113% |
| omnetpp, LNC | 200k/1M | 887,467 | 886,925 | −0.061% |
| gcc, defaults | 1M/10M | 10,277,644 | 10,272,246 | −0.053% |
| mcf, defaults | 1M/10M | 38,498,951 | 38,500,066 | +0.003% |
| omnetpp, defaults | 1M/10M | 13,040,318 | 13,022,513 | −0.137% |
| mcf, LNC | 1M/10M | 21,857,448 | 21,839,324 | −0.083% |
| mcf, perf gate | 1M/3M | 10,182,772 | 10,176,029 | −0.066% |
| sqlite, perf gate | 1M/3M | 5,107,015 | 5,107,151 | +0.003% |
| omnetpp, perf gate | 1M/3M | 5,287,547 | 5,285,301 | −0.042% |
| gcc, perf gate | 1M/3M | 3,748,419 | 3,747,525 | −0.024% |

- LNC is `configs/lnc.toml` plus `configs/dram-legacy.toml`. The perf gate is
  `configs/champsim_config.toml` plus `configs/perf-hermes.toml` on the protected
  traces.
- `[config]` is identical in every pair. `[phase]` changes in every pair (62–112
  lines per document), for example mcf's `cycles_on_wrong_path` 31,332 → 32,812. The
  default predictor's mispredict counts did not change.
- The change goes both ways over a run. Renaming earlier moves when later resources
  are taken, so a correct check is not always faster.

Above 256 registers:

| Case | Before → after |
|---|---|
| LNC + 512, mcf | 923,998 → 917,213 cycles (−0.734%) |
| LNC + 512, sqlite / gcc | 16 / 24 `[phase]` lines differ |
| defaults + 512, mcf and sqlite | identical `[phase]` |
| 4000 on defaults or LNC (mcf, gcc) | identical `[phase]` |
| defaults + 256, mcf | +0.082% |

The fix is inert **by construction** only where the free count can never fall below
an instruction's largest possible need: 6 for v1/v2 (4 sources and 2 destinations),
8 for CloudSuite (4 destinations). That holds when `register_file_size` exceeds
roughly 255 + D·`rob_size` + need, with D the destinations per instruction: about 965
at the default ROB of 352 and 1413 at `lnc.toml`'s 576 for v1/v2, and 2567 for
CloudSuite at 576. So the 4000-register results are guaranteed unaffected. The
identical 512-register runs on defaults are below that bound: they were unaffected
on two traces at 200k/1M, which is not a guarantee.

**Recorded results this affects.**

- Every simulation on this branch. The absolute numbers of any run at 128 or 200
  registers shift slightly; before/after comparisons made with one binary stand,
  because both sides had the bug.
- `docs/smt-integration/2026-09-21-roadmap-critique.md` §1 and §9.4: the PRF 4000
  columns stand; the 64/100/128/200 columns shift by up to about 0.3%, and the
  conclusions do not change.
- `2026-09-14-performance-optimization.md:862-865` says no effect on the 128-register
  benchmark configs is inferred. That is true of the out-of-bounds read only; the
  spurious check ran in every campaign. Parity verdicts stand; absolute KIPS and
  statistics are pre-fix.
- `2026-09-15-linux-perf-hotspots.md:196-211` attributes 3.2–4.5% of sampled cycles
  to `isValid`, `isAllocated` and `count_free_registers` together. The fix removes
  most `isAllocated`/`count_free_registers` calls but none of `isValid`'s (called from
  `execute_instruction`). Re-profile before quoting a host speedup.
- `docs/research-log/CBP6/2026-08-06-cbp2025-predictors-in-champsim.md` reports IPC,
  speedup and CycWPKI measured with the bug. Predictors updated at execute
  (`CBP6_DELAYED_UPDATE=1`, and `cbp6_runlts_norv`'s execute-time value delivery) may
  also see their MPKI move (inferred).
- Pre-fix and post-fix statistics documents have the same `build_id` and `[config]`,
  so nothing in a document tells them apart. Record the binary's SHA256 or
  `--build-info` with every re-baselined result.

**Fix (recommended): upstream's, plus two guards, as its own commit.**

- Move the check inside `if (!rob_it->scheduled && rob_it->ready_time <=
  current_time)`, before `do_scheduling`, and keep the `break`. Do not turn it into
  `continue`: that would rename a younger instruction before an older writer and
  corrupt `frontend_RAT`.
  - This is upstream `f46c1ff0`'s `src/ooo_cpu.cc` hunk (+9/−6). The commit is on
    upstream `develop` only, not `master`. This repository has no upstream remote,
    and the commit's test uses `champsim::modules::ModuleBuilder`, which the fork
    does not have. So either add the remote and `git cherry-pick -n f46c1ff0`,
    replacing the test, or apply the hunk by hand. Either way, credit it with a
    `(cherry picked from commit f46c1ff0...)` line.
- Add `CHAMPSIM_ASSERT(!free_registers.empty())` to `rename_src_register`, mirroring
  `rename_dest_register`.
- Bound `isAllocated`'s index. `.at()` matches `isValid` and makes a violation
  observable in Catch2 as `std::out_of_range`; `CHAMPSIM_ASSERT` is what CLAUDE.md
  prescribes for an invariant a user cannot trip, and compiles out of `fast`. Choose
  one and say why in the commit. The perf plan to inline these queries must keep the
  check.
- Correct the two comments that say registers are renamed at dispatch:
  `src/ooo_cpu.cc:371-373` and `inc/cbp6/cbp6_host.h:126-129`. It is `do_scheduling`
  (`src/ooo_cpu.cc:461-475`).
- Do **not** add a stopgap that rejects sizes above 256. After the fix, 257–32767
  are well defined, and the stopgap would rule out the 4000-register sweeps the SMT
  critique relies on.

Renaming stays in program order only because an unscheduled entry's `ready_time`
never decreases along the ROB: it is written only at dispatch (`:431`) and, for
scheduled entries, at `:497`. Both today's code and upstream's skip a not-ready entry
and would rename a younger ready one if that ever broke. SMT interleaving is exactly
the kind of change that could break it. Either pin the invariant with a test or
`CHAMPSIM_ASSERT`, or make the stop explicit (`if (!scheduled) { if (ready_time >
now) break; ... }`), which schedules the same instructions today but diverges from
upstream. SMT Stage 1 must keep it per context.

Alternatives considered:

- **Reject sizes above 256 and bound `isAllocated`, leaving the check where it is.**
  Inert, but it leaves the spurious stops at every shipped configuration, forbids
  large-register experiments, and diverges from upstream. Only worth it if the real
  fix cannot land before someone needs a safe binary.
- **Keep architectural and physical operands in separate fields** instead of renaming
  in place. It gives the same results as the fix and removes the whole class of bug,
  including the deadlock printer's version ([B9](#b9-small-defects)) and the
  `pending_reg_` workaround in `cbp6_host.h`. It is a much larger change, better done
  afterwards as an inert refactor checked against the fixed baseline, and a natural
  base for SMT's per-context RATs.

**Tests to add.** New `test/cpp/src/202-scheduler-rename-gate.cc`: prefix 202 is
unused, and the name matches upstream's to ease a merge (note in the commit that an
upstream merge will then need an add/add conflict resolved). Build cores with
`champsim::core_builder{}.register_file_size(N)` as `200-rob-scheduling.cc` does, and
call `schedule_instruction()` directly.

1. **A full register file does not block scheduling behind in-flight writers.** 32
   registers and 32 ready writers of one register: all are scheduled and none is
   free. A following instruction with no registers must still be scheduled. Fails
   today, because the first scheduled writer is charged a destination.
2. **Renamed sources are not looked up in the architectural RAT.** 4 registers. A
   (sources 10, 11; destination 13) takes physical 0, 1 and 2, leaving 1 free. B
   (destination 12) must be scheduled. Fails today: `isAllocated(0)` and `(1)` read
   unmapped architectural entries, so A counts 3.
3. **Physical IDs above 256.** 260 registers and a 1024-wide schedule. 256 writers,
   then A as above, whose sources land at 256 or more. B must be scheduled, and the
   call must not throw. Fails today: the out-of-bounds read hits `backend_RAT[0..1]`,
   which are −1. With only the `.at()` bound applied, it fails with
   `std::out_of_range`. This observes the undefined behaviour deterministically
   without a sanitizer build.
4. **Renaming stays in order.** An unscheduled entry needing more registers than are
   free stops the walk, so a younger ready entry needing none stays unscheduled. This
   pins `break` over `continue`.
5. Optionally, an unscheduled entry whose unmapped sources plus destinations exceed
   the free count is not scheduled, and the free count is unchanged.

Existing tests should pass unchanged: 200, 201, 250 and 252 never make the check
bind. `250-load-scheduling.cc` relies on `core_builder`'s default of one register;
give it an explicit size. `test_operational_config_errors.py` pins 32767 as
accepted, but only through `--knobs`, which never runs the scheduler.

**Acceptance.**

- From a clean checkout of the parent (this working tree carries other edits), a
  fresh release build reproduces the "before" numbers above: mcf defaults 3,224,159,
  gcc defaults 1,507,986, sqlite LNC 1,641,083. That proves the measured binary
  matched the source. If another fix lands first, show it inert on these
  configurations or re-derive the numbers.
- The fixed build reproduces every "after" number above exactly. A mismatch means
  the ready-time argument or the emulation was wrong, and must be investigated.
- Cases 1–4 pass; 1–3 fail against the parent. `make test TEST_NUM=202` selects one
  file; full `make test` passes, and `make pytest` passes with `CHAMPSIM_BINARY` set
  to the canonical release binary.
- 4000 registers leaves `[phase]` byte-identical on mcf (defaults and LNC) and gcc
  (LNC).
- In a `-D_GLIBCXX_ASSERTIONS` build, LNC runs at 512 and 4000 registers finish
  cleanly on the fix and abort on the parent. Check that the parent's backtrace runs
  through `isAllocated` and `schedule_instruction`, since
  [B3](#b3-empty-sub-record-or-unreadable-traces-segfault)'s `front()` could abort
  first.
- `compare_optimization.py` reports NON-INERT on the protected traces, as expected
  (it needs `--before`/`--after` binaries, `--traces` and `--output`). It stops at the
  first difference, so characterize with paired `--toml` runs instead: the four
  protected traces at 1M/3M, and the long gate (5M/50M on the 14 SPEC26 workloads plus
  the mcf control, with `--timeout`), since this changes ROB traversal. `[config]` must
  be identical in every pair. Record the deltas in the commit message and the
  performance log.
- The documents listed under "Recorded results this affects" get a pre-fix note in
  the same series. The fixed binary becomes the accepted predecessor for later
  optimizations, and the SMT Stage 1 parity baseline is recorded only after it.

Related, but not part of this fix: a CI job built with `-D_GLIBCXX_ASSERTIONS`, since
nothing in CI can see an out-of-bounds `std::array` read today. Two further defects in
this code are listed under [B9](#b9-small-defects).

---

## B3. Empty, sub-record or unreadable traces segfault

**Status: open.** Severity: medium. Inherited from upstream ChampSim: the unchecked
`front()` dates from `1d587e9f` (2021), and the eof and stop rules from `82f0ac64`
and `73ee81d1`. The fix should be **inert**.

**Symptom.** Exit 139 (SIGSEGV) instead of `ERROR: trace '...'`, with or without
`-i`, for any of these:

- an empty file;
- a file shorter than one record (64 B v1, 512 B v2);
- a *valid* empty compressed container (`.xz`, `.gz`, `.bz2`, `.zst`);
- an unreadable uncompressed file;
- a v2 pipe emptied by the branch-type probe ([B7](#b7-the-v2-branch-type-probe-consumes-pipe-input));
- a process-substitution trace whose run wraps, because the reopened pipe is empty.

A zero-byte `.xz`/`.gz`/`.zst`/`.bz2` *file* already fails cleanly with
`truncated <codec> stream`, and has since `b80b2a7f`.

**Root cause.** `bulk_tracereader::operator()` calls `refill()` and then
`instr_buffer.front()` without checking that a record was read
(`inc/tracereader.h:146-151`).

- `refill()` converts only whole records (`:130-137`).
- `eof()` is false before the first read, and stays false forever on a stream that
  has only failbit set (`:105`). So `do_cycle` calls the reader on cycle 1
  (`src/champsim.cc:56-57`).
- `repeatable` reopens the source and calls the new reader unconditionally
  (`inc/repeatable.h:39-44`).

gdb puts the fault at `inc/tracereader.h:150`, called from `do_cycle`.

**Reproduction.** Run the existing binary with `-- empty.champsimtrace`, and again
with `-w 1000 -i 10000 --`; both exit 139. Other triggers:

- `lzma.compress(b'')` saved as `.xz` exits 139;
- a 10-byte or 63-byte file exits 139;
- a `chmod 000` copy of a valid trace exits 139;
- `-w 1000 -i 10000 -- <(cat r200.champsimtrace)` exits 139 at the first wrap.

The dossier's scratch generators are in the session scratchpad and can be recreated
from these descriptions.

**Fix (recommended).**

- In `refill()`, after converting records, throw a `std::runtime_error` naming the
  trace when `instr_buffer` is still empty. That is the only state in which `front()`
  meets an empty buffer. Keep the message helper out of line.
- Do not reopen a non-regular source (pipe, process substitution, named FIFO) under
  repeat. Check it with `std::filesystem::status` in `get_tracereader`; when `eof()`
  first becomes true, throw a named error instead ("cannot repeat non-regular trace
  X; use a regular file or reduce -w/-i"). This also removes the named-FIFO hang
  ([B8](#b8-trace-end-of-file-handling)), and it does not affect the CI
  `<(curl ...)` jobs, which never wrap.
- Optionally (the user's call), reject a trailing partial record. It is dropped
  silently today and pinned by `test/cpp/src/087-tracereader-v2.cc:48-69`. It fires
  only for runs that read to the end, and discards the last batch's complete
  records.

**Tests to add.**

- `test/cpp/src/093-tracereader-short-input.cc`: sizes {0, 1, sizeof(T)−1} for v1,
  v2 and CloudSuite; valid empty compressed containers; an unopenable path; 1- and
  2-record traces pinning today's behaviour.
- `test/cpp/src/094-repeatable-reopen.cc`: a regular-file reopen that yields records
  keeps working; a truncated reopen throws.
- `test/python/test_trace_short_inputs.py`: every crash case above exits 1 with
  `ERROR:` and the path, never 139.

The prefixes 093 and 094 are unused.

**Acceptance.**

- Every exit-139 case exits 1 with a named error, in both release and fast builds.
- Short traces that do repeat (1, 5 and 200 records at `-w 1000 -i 10000`) keep
  byte-identical `[phase]` tables.
- `compare_optimization.py` and `--regression` report inert.

---

## B4. A TLB prefetcher deadlocks the walk path

**Status: open.** Severity: medium. Found while reviewing the perfect-TLB fix; the binary
from before that fix deadlocks identically.

**Symptom.** Giving a DTLB a prefetcher deadlocks when the STLB is not perfect,
for example:

```
--set cache.cpu0_dtlb.prefetcher=next_line --set cache.cpu0_dtlb.pq_size=8 --set cache.cpu0_stlb.pq_size=8
```

The deadlock came at cycles 3580 and 6631, depending on configuration. The same
setup completes with a perfect STLB.

**Likely cause** (inferred): TLB prefetch misses are forwarded to the lower level's
prefetch queue, and the page table walker drains only its RQ (`src/ptw.cc:170-180`).
The TLB `pq_size` defaults to 0, so default runs never reach this.

**Fix.** Either have the walker serve its upper levels' PQ, or refuse a prefetcher on
a TLB whose lower level cannot serve prefetches, naming the key. Add a regression
test with a DTLB `next_line` prefetcher.

---

## B5. An MSHR merge of two virtual addresses strands a waiter

**Status: open.** Severity: medium, but latent: it needs two virtual blocks on one
physical block. That is reachable today through [B6](#b6-exhausting-physical-memory-silently-aliases-pages),
and was reachable through the perfect-TLB bug, now fixed. The code is upstream
ChampSim.

**Root cause.**

- `fill_type::merge` keeps a single `v_address`, the successor's (`src/cache.cc:118`).
- The core then completes returns by virtual block. The L1I path erases a waiter
  whose block check fails (`src/ooo_cpu.cc:725, 735`); the L1D path matches loads by
  virtual block (`:748`).
- A load or fetch is issued only once (`:240`, `:592`).

So the non-retained waiter never completes, and the run deadlocks. An L1D load can
be rescued by a later access to the same virtual block, so there the outcome is a
wrong latency or a deadlock.

**Fix.** Complete waiters by `instr_id` together with the block, as the L1I path
already does. Or keep a per-waiter virtual address through the merge. Either fix
changes one-context timing, so land it with a re-baseline. It is also part of the
SMT roadmap's load-completion decision (critique §8.2, on `feat/smt`).

---

## B6. Exhausting physical memory silently aliases pages

**Status: open.** Severity: low.

**Root cause.** When the free list runs out, `ppage_pop` refills it with every frame
without unmapping anything (`src/vmem.cc:122-130`). It prints only
`[VMEM] WARNING: Out of physical memory, freeing ppages`, and later faults get frames
that are still mapped. Distinct pages then share cache lines and legacy-DRAM
write-forwarding, and can trigger [B5](#b5-an-mshr-merge-of-two-virtual-addresses-strands-a-waiter).

The default legacy capacity is 16 GiB, so SPEC runs are unlikely to hit it. Test
fixtures use 16 MiB (`pmem.bank_rows=64`), and multi-core and SMT runs put several
footprints in one pool.

**Fix.** Make exhaustion fatal (throw, naming the capacity keys), or at least record
it in the statistics document.

---

## B7. The v2 branch-type probe consumes pipe input

**Status: open.** Severity: medium for anyone streaming v2 traces; silent.

**Root cause.** `v2_trace_declares_branch_type` opens a *second* stream on the same
path to peek at the first record (`src/tracereader.cc:71-95`, called at `:137`). On
a pipe or process substitution that read consumes data. strace shows
`read(3, ..., 8191) = 8191` on `/dev/fd/63`, and every later record is misaligned by
511 bytes.

The same bytes read two ways:

| Source | Instructions | Cycles |
|---|---|---|
| Pipe | 19,467 | 583,982 |
| Regular file | 19,507 | 114,971 |

A 5-record v2 pipe segfaults ([B3](#b3-empty-sub-record-or-unreadable-traces-segfault)).

**Fix.** Skip the probe for non-regular files (`std::filesystem::is_regular_file`),
or do the probe during the first real read with a flag held outside `repeatable`.
`docs/handoff-v2-explicit-branch-type-traces.md:87-92` explains why not inside the
reader.

---

## B8. Trace end-of-file handling

**Status: open.** Severity: low to medium. These are behaviours rather than crashes.
Fixing any of them changes the results of runs that wrap or omit `-i`, so each is
non-inert and needs its own decision.

- **Named FIFO hang.** `CLI::ExistingFile` accepts a FIFO, and a run that wraps
  blocks forever reopening it (`inc/repeatable.h:41`; wchan `wait_for_partner`). The
  B3 non-regular-source fix removes it.
- **No drain, and an empty ROI with exit 0.** Without `-i`, any reader's EOF ends
  every phase at once (`src/champsim.cc:144-147`) and drops in-flight work: a
  200-record trace retires 30. `-w 30000` on a 20,000-record trace reports
  `Simulation complete ... instructions: 0 cycles: 1` and exits 0. This is pinned by
  `test/cpp/src/502-phase-order.cc:81-104`. Emit a named error or warning when a
  measured phase retires nothing or stops short because of EOF. SMT Stage 4 needs
  defined semantics here.
- **N−1 records per lap.** `eof()` goes true while one record is still buffered
  (`inc/tracereader.h:105`), and `repeatable` discards it, so a lap is N−1 records
  (N when N is a multiple of 127). Pinned by `087-tracereader-v2.cc:256-286`.
- **Lap-message spam, and no lap count.** `*** Reached end of trace` prints once per
  lap: 250,123 lines for a 5-record trace at 1M instructions. Laps are not recorded
  in the statistics.
- **`-` is rejected.** CLAUDE.md says "a `-`/process substitution stream also
  works". `-` exits 105 (`File does not exist: -`, `src/main.cc:155`). Process
  substitution works only until the run wraps. `/dev/stdin` redirected from a regular
  file repeats correctly. Correct the document, or accept `-`.

---

## B9. Small defects

**Status: open.** Severity: low.

- `--hide-heartbeat` sets `O3_CPU::show_heartbeat`, which nothing reads
  (`src/main.cc:336-339`), so the flag does nothing.
- `champsim::program_ordered::matches_id(const T&)` returns `precedes(...)` instead
  of an ID match (`inc/instruction.h:73`). It is unused today, and wrong if anyone
  calls it.
- `O3_CPU::do_sq_forward_to_lq` is declared and never defined (`inc/ooo_cpu.h:182`).
- A one-character trace path such as `a` fails with
  `ERROR: basic_string::substr: __pos ... > this->size()`, because
  `get_tracereader_for_type` takes `fname.substr(size - 2)` with no length guard
  (`src/tracereader.cc:39`).
- `CACHE::end_phase` self-assigns its unused argument (`src/cache.cc:915`,
  `finished_cpu = finished_cpu;`).
- The deadlock printer calls `count_reg_dependencies` for entries not yet renamed,
  whose operands are still architectural IDs (`src/ooo_cpu.cc:843`). The range guard
  from `63889670` (`src/register_allocator.cc:85-91`) only stops the throw, so the
  count is meaningless for an architectural ID below the register-file size. Report 0
  for `!scheduled` entries. Diagnostics only, so the fix is inert. It is the mirror
  image of [B2](#b2-the-schedulers-register-check-runs-on-already-renamed-entries).
- The scheduler's register check counts a repeated unmapped source twice
  (`X0 = X1 op X1` needs 1 register and is charged 2), so it can stop the walk one
  register early. Deduplicating it changes timing, so it is non-inert; land it after
  B2, not with it.
