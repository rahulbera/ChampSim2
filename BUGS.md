# Known bugs

A running task list of the open defects in this fork, most of them found during the
SMT roadmap review (`docs/smt-integration/2026-09-21-roadmap-critique.md` §10, on the
`feat/smt` branch). Each entry states the symptom, the root cause with `file:line`, how
to reproduce it, and the recommended fix.

Only open bugs are listed. When a bug is fixed, delete its entry in the same commit
as the fix, and put the evidence in the commit message. IDs are never reused, so a gap
in the numbering is a fixed bug. `git log -S '## B<n>.' -- BUGS.md` lists the commit
that added an entry and, newest first, the one that fixed it. Fixed so far:

- B1, perfect TLBs mapping every page to physical page 0 (`fcd13089`, before this file
  existed);
- B2, the scheduler's register check running on already-renamed instructions.

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
| [B3](#b3-empty-sub-record-or-unreadable-traces-segfault) | Empty, sub-record or unreadable traces segfault | Medium | Open |
| [B4](#b4-a-tlb-prefetcher-deadlocks-the-walk-path) | A TLB prefetcher deadlocks the walk path | Medium | Open |
| [B5](#b5-an-mshr-merge-of-two-virtual-addresses-strands-a-waiter) | An MSHR merge of two virtual addresses strands a waiter | Medium (latent) | Open |
| [B6](#b6-exhausting-physical-memory-silently-aliases-pages) | Exhausting physical memory silently aliases pages | Low | Open |
| [B7](#b7-the-v2-branch-type-probe-consumes-pipe-input) | The v2 branch-type probe consumes pipe input | Medium | Open |
| [B8](#b8-trace-end-of-file-handling) | Trace end-of-file handling: FIFO hang, no drain, N−1 laps, `-` | Low–Medium | Open |
| [B9](#b9-small-defects) | Small defects (no-op flag, wrong overload, dead declaration, path parsing, register counts, no CI bounds checks) | Low | Open |
| [B10](#b10-ci-on-master-fails-in-four-independent-ways) | CI on `master` fails in four independent ways | Medium | Open |

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
- No CI build checks `std::array` or `std::vector` bounds, which is how B2's
  out-of-bounds RAT read went unseen. The `native_sanitize` job's
  `-fsanitize=address,undefined` misses such a read when it stays inside the
  enclosing object, and so does `-fsanitize=bounds` (GCC 13.3). Add a non-native
  job built with `-D_GLIBCXX_ASSERTIONS`, or `-fsanitize=bounds-strict`.
- The deadlock printer calls `count_reg_dependencies` for entries not yet renamed,
  whose operands are still architectural IDs (`src/ooo_cpu.cc:843`). The range guard
  from `63889670` (`src/register_allocator.cc:85-91`) only stops the throw, so the
  count is meaningless for an architectural ID below the register-file size. Report 0
  for `!scheduled` entries. Diagnostics only, so the fix is inert. It is the mirror
  image of B2, the scheduler's register check, which read renamed entries' physical
  IDs as architectural ones.
- The scheduler's register check counts a repeated unmapped source twice
  (`X0 = X1 op X1` needs 1 register and is charged 2), so it can stop the walk one
  register early. Deduplicating it changes timing, so it is non-inert and needs its
  own re-baseline.

---

## B10. CI on `master` fails in four independent ways

**Status: open.** Severity: medium. Every "Run Tests" run on `master` has failed
since at least 2026-09-21, the first one recorded, and every "Update Documentation"
run since 2026-08-05. A red build cannot show a new regression, and a job that fails
to build never runs its tests. Each failure stops its own job, so fixing one can
reveal another behind it.

Three causes are identified, one is not:

1. **Clang 12–15 cannot compile `test/cpp/src/706-ramulator2-differential.cc`**
   (from `9047baec`): `reference to local binding 'pid' declared in enclosing
   function` at `:495` and `:497`, and the same for `fragment`. The lambdas passed to
   `require` refer to the structured bindings of
   `const auto [pid, fragment] = owner[attempt];` (`:493`). That is C++20; Clang
   accepts it from 16, and GCC accepts it already.
   - **Fix:** plain variables, `const auto pid = owner[attempt].first;` and
     `const auto fragment = owner[attempt].second;`. Clang 12.0.1 and 15.0.7
     (conda-forge) reproduce these errors and compile the fix cleanly.
   - Clang 12–15 compile 601 without error.
2. **The `python` job fails
   `test_ramulator2_build.RamulatorBuildTests.test_make_dry_run_does_not_prepare_build_artifacts`**
   (from `73f317fc`). It runs `make -n ramulator2` at the repository root. That goal
   hard-includes `_configuration.mk` (`config/build_rules.mk:88-92`), which only
   `./config.sh` writes, and the `python` job (`.github/workflows/test.yml:199-218`)
   never runs `config.sh`. The result is `No rule to make target
   '_configuration.mk'`. The `overriding recipe` warning beside it in the log is a
   symptom of the same missing file. Locally it passes because the tree is configured.
   In a clean `git archive` export with the dependencies linked in it fails with CI's
   message, and passes after `./config.sh`.
   - **Fix:** run `./config.sh` in the `python` job before the tests, or rewrite the
     test to dry-run beside a stub fragment, as its neighbour
     `test_fresh_enabled_dry_run_prints_missing_dependencies_without_building`
     already does for the same reason.
3. **`publish-wiki` ("Update Documentation") fails** because
   `.github/workflows/docs.yml` checks out `ref: gh-pages`, a branch this fork does not
   have (`git ls-remote origin 'refs/heads/gh-pages*'` is empty). The fetch fails
   three times and the job stops, on every push to `master`.
   - **Fix:** create an orphan `gh-pages` branch (and enable Pages if the site is
     wanted), or skip the job outside upstream with
     `if: github.repository == 'ChampSim/ChampSim'`, or drop `master` from its
     triggers.
4. **macOS Clang: cause not identified.** `make test/bin/000-test-main` exits 2
   after about 225 compiles, with no compiler error and no inner-make message,
   identically before and after `fcd13089`–`ed01cca7`. The log names no failing
   command. Reproduce on a Mac with `make --debug=j`, or with `-j1` and
   `SHELL='sh -x'`. The runner's make version is not in the log; Apple's
   `/usr/bin/make` is GNU Make 3.81, which may not support everything the build
   rules use (inferred, not verified).

**Next in line: `upload_coveralls`** (`.github/workflows/test.yml:694`). It `needs`
every cpp job, so it has been skipped in every run, and it runs for the first time
once the matrix passes. It is expected to fail then (inferred; it has never run):
`coverallsapp/github-action@master` uploads to a Coveralls project that exists only
for upstream (`coveralls.io/github/rahulbera/ChampSim2` is 404). Guard it with
`if: github.repository == 'ChampSim/ChampSim'`, register the repository on
Coveralls, or remove the job.

**Acceptance.** All "Run Tests" matrix jobs, `python`, `stats_output` and "Update
Documentation" (or its deliberate removal) are green on a push to `master`, and the
old-compiler jobs actually run the Catch2 suite rather than stopping at the build.
