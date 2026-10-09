# Staged optimizations

Performance work that has been measured, scoped and deliberately **not done yet**, with
enough detail to pick up later. Each entry gives the key idea and how to proceed, as
numbered steps. Completed work is in `docs/research-log/Performance/`, and the LTO/PGO log
there explains where these came from.

## Common rules for every entry

- **Behaviour-preserving only.** `tools/perf/compare_optimization.py` must find complete
  `[phase]` and `[config]` parity. Run it on the four protected traces with
  `configs/lnc.toml` and on module overlays that exercise the non-default modules, and
  never adjust expected statistics to make a change pass.
- **One change at a time**, each gated against its immediate predecessor, with its own
  commit.
- **Measure allocations directly.** ChampSim made about 12-15 heap allocations per
  simulated instruction before the `decode_instruction` move fix and the per-packet move
  fixes, and about 8.5-10 after them (sqlite 11.65 → 8.55, mcf 14.56 → 10.04). The
  shares quoted below are of the original total. `tools/perf/malloc_counts.c`
  counts them. A backtrace-sampling variant, `malloc_sites.c` in the LTO/PGO evidence
  directory (`…/2026-09-27-lto-pgo/14-alloc-sources/`), attributes them to call sites
  through `addr2line -i`. Count against a `TCMALLOC=0` build, because tcmalloc's own
  `malloc` bypasses an interposer.
- **Expect a modest gain.** Fast builds link tcmalloc, which leaves the allocator only
  1.3-2.0% of samples. Removing allocations now buys that share plus whatever locality it
  brings, so a few percent at most. These entries are staged because their reach through
  the code is wide, not because they are unimportant.

## 1. Inline storage for each instruction's operand lists

**Key idea.** `ooo_model_instr` holds `source_registers` and `destination_registers`
(`std::vector<PHYSICAL_REGISTER_ID>`) and `source_memory` and `destination_memory`
(`std::vector<champsim::address>`). Every instruction built from the trace allocates them
afresh (`inc/instruction.h`, the constructor from a trace record), which is about 19-22% of
all heap allocations. Their lengths are bounded by the trace format: v1 and v2 records
carry at most `NUM_INSTR_SOURCES` = 4 sources and `NUM_INSTR_DESTINATIONS` = 2
destinations, and the cloudsuite format up to `NUM_INSTR_DESTINATIONS_SPARC` = 4.
Fixed-capacity inline storage removes those allocations and keeps an instruction's
operands next to the rest of it. `registers_instrs_depend_on_me` has no such bound and
stays a `std::vector`.

**How to proceed.**

1. Find every writer of the four lists, not only the trace constructor. Grep
   `source_registers`, `destination_registers`, `source_memory` and `destination_memory`
   across `src`, `inc`, `branch`, `btb`, `prefetcher`, `replacement` and `test`. Anything
   that appends beyond the trace operands, or a test that builds long lists, sets the real
   capacity. The capacity must also cover all three trace formats.
2. Write `champsim::inline_vector<T, N>` in `inc/util/`. It needs exactly the part of the
   `std::vector` interface those uses need: `begin`/`end`, `size`, `empty`, `operator[]`,
   `data`, `push_back`/`emplace_back`, `clear`, and `erase`, including the
   remove-erase idiom. Overflow must `throw` rather than assert, because a corrupt trace can
   trigger it and a fast build would compile an assertion out. Keep it header-only and free
   of vcpkg dependencies: the `tools/` harnesses build with nothing but `g++ -I../../inc`.
3. Give it unit tests: capacity, overflow, erase, iteration, copy and move.
4. Switch the four member types. Rebuild every module and test, and syntax-check with the
   floor compilers (conda GCC 9 and Clang 12, per `CLAUDE.md`). The modules that read the
   lists are the ones to watch: `perfect_branch`, the `cbp6_*` host, and anything that takes
   the lists by `std::vector&`.
5. Measure `sizeof(ooo_model_instr)` before and after. Inline storage makes each
   instruction larger, so a move copies more bytes instead of swapping pointers. That
   changes how many instructions fit in one `std::deque` block (entry 2).
6. Gate it: parity on v1 and v2 traces, plus a build with `CHAMPSIM_TRACE_MEMORY_VALUES=1`
   (which changes the instruction layout). The allocation count should fall by about a
   fifth. Then take KIPS.

## 2. Ring buffers instead of `std::deque` for the pipeline queues

**Key idea.** `IFETCH_BUFFER`, `DECODE_BUFFER`, `DIB_HIT_BUFFER`, `DISPATCH_BUFFER`, `ROB`,
`input_queue` and the trace reader's `instr_buffer` are all `std::deque<ooo_model_instr>`.
libstdc++ allocates deque storage in 512-byte blocks. `ooo_model_instr` is 200 bytes (688
with `CHAMPSIM_TRACE_MEMORY_VALUES=1`), so a block holds two instructions, or one. Every
second push allocates a block and every second pop frees one: about 18-21% of all heap
allocations, around 4% per queue. Each queue except the two
trace-side ones has a configured maximum (`ifetch_buffer_size`, `decode_buffer_size`,
`dib.hit_buffer_size`, `dispatch_buffer_size`, `rob_size`), so a ring buffer allocated once
at construction removes the churn. Slots are reused, which also keeps the queues compact.

**How to proceed.**

1. Confirm the per-queue allocation shares with the site sampler, and measure
   `sizeof(ooo_model_instr)` (entry 1 changes it).
2. List how each queue is used: `push_back`/`emplace_back`, front removal, which ranges
   are erased, `std::find_if`, `std::merge`, `std::back_inserter`, iterator arithmetic,
   and above all **reference stability**. `registers_instrs_depend_on_me` holds references
   into the ROB. `std::deque` keeps element addresses stable across `push_back` and front
   removal, and so does a ring buffer that never relocates. Middle erases would break them.
   Today the ROB is only erased at its front, at retirement (`ROB.erase(retire_begin,
   retire_end)` in `src/ooo_cpu.cc`), since a trace-driven core never flushes wrong-path
   instructions. Recheck this before relying on it.
3. Write `champsim::circular_buffer<T>`: capacity fixed at construction, random-access
   iterators that satisfy the standard algorithms used above, `push_back`/`emplace_back`,
   `pop_front`, and `erase` restricted to a prefix or suffix, which should `throw` on any
   other range. Include `value_type`, so `std::back_inserter` works. Unit-test it with
   wraparound in mind.
4. Convert one queue per commit, simplest first: `DIB_HIT_BUFFER` and `DECODE_BUFFER`, then
   `DISPATCH_BUFFER` and `IFETCH_BUFFER`, then the `ROB`. Handle `input_queue` and
   `instr_buffer` separately: they have no configured bound and are refilled in bulk.
5. Gate each conversion. `CLAUDE.md` asks for the long gate (5M/50M over the 14 SPEC26
   workloads) for any change to ROB traversal, so the ROB conversion needs it.
6. Watch the deadlock diagnostics and `print_deadlock` code paths, which iterate these
   queues, and the tests that build `O3_CPU` directly.

## 3. Inline storage for `to_return`

**Key idea.** `CACHE::tag_lookup_type::to_return` and `CACHE::fill_type::to_return` are
`std::vector<std::deque<response_type>*>`. `initiate_tag_check` allocates a new
one-element vector for every request that wants a response, which is about 7% of all heap
allocations. Nearly every list has one entry, and none can have more than the cache's number
of upper-level channels (three for the STLB), because merges only union lists within one
cache. A small inline container removes almost all of those allocations.

**How to proceed.**

1. Establish the real maximum: the largest `upper_levels` in `src/static_environment.cc`,
   plus the mock caches and channels in the tests.
2. Reuse `inline_vector` from entry 1 with that capacity, and keep the overflow `throw`.
3. Change the member type in `inc/cache.h`, and adapt `fill_type::merge`, whose
   `std::set_union` only needs iterators, and `return_to_all` in `src/cache.cc`.
4. Look at `instr_depend_on_me` the same way: measure its length distribution before
   choosing a capacity, because unlike `to_return` it has no structural bound (merged misses
   append to it).
5. Gate: parity, allocation count, KIPS.
