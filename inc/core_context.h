#ifndef CORE_CONTEXT_H
#define CORE_CONTEXT_H

#include <array>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <queue>
#include <vector>

#include "core_stats.h"
#include "instruction.h"
#include "register_allocator.h"
#include "util/lru_table.h"

// Per-hardware-context (per-thread) pipeline state.
//
// A core_context holds everything a single hardware thread owns privately: its
// own frontend buffers, its own reorder buffer, its own load/store queues, its
// own register alias tables, and its own branch predictor state. A physical
// core with N hardware contexts will hold N core_context objects. Everything
// the threads share (L1 caches, the physical register file and its free list,
// bandwidth and capacity constants) stays on the core.
//
// Stage 1 has exactly one context per core and does not change the order of
// operations, so single-context results remain bit-identical.

struct LSQ_ENTRY : champsim::program_ordered<LSQ_ENTRY> {
  champsim::address virtual_address{};
  champsim::address ip{};
  champsim::chrono::clock::time_point ready_time{champsim::chrono::clock::time_point::max()};

  std::array<uint8_t, 2> asid = {std::numeric_limits<uint8_t>::max(), std::numeric_limits<uint8_t>::max()};
  bool fetch_issued = false;

  uint64_t producer_id = std::numeric_limits<uint64_t>::max();
  std::vector<std::reference_wrapper<std::optional<LSQ_ENTRY>>> lq_depend_on_me{};

  // Unique token naming this LQ entry as a load waiter. instr_id is one-to-many
  // with LQ entries (an instruction with several source-memory operands allocates
  // one entry each), so it cannot name a single waiter. The core assigns this
  // monotonically at allocation and echoes it through the memory system in
  // channel::request/response.load_waiters.
  uint64_t load_waiter_id = std::numeric_limits<uint64_t>::max();

  // B2.1: the cycle the load was issued to the memory system (fetch_issued
  // becomes true), and whether the long-latency detection has already fired for
  // it. Both are inert unless BMT is enabled -- they feed no pipeline decision
  // and are written regardless, so a default-off run is byte-identical.
  champsim::chrono::clock::time_point issue_time{};
  bool bmt_detected = false;

  // B2.5: the moment detection fired (bmt_detected became true). Only the
  // miss-edge trigger reads it, to model the optional dwell -- the N cycles the
  // switch waits after the LLC miss is confirmed before swap-out acts. The
  // latency arm swaps out in the same cycle it detects, so it leaves this at
  // the epoch. Inert (epoch) while BMT is off.
  champsim::chrono::clock::time_point bmt_detected_at{};

  LSQ_ENTRY(champsim::address addr, champsim::program_ordered<LSQ_ENTRY>::id_type id, champsim::address ip, std::array<uint8_t, 2> asid);
  void finish(ooo_model_instr& rob_entry) const;
  void finish(std::deque<ooo_model_instr>::iterator begin, std::deque<ooo_model_instr>::iterator end) const;
};

namespace champsim
{

struct dib_shift {
  champsim::data::bits shamt;
  auto operator()(champsim::address val) const { return val.slice_upper(shamt); }
};

// The DIB key is (asid, ip): two address spaces never share a decoded window.
// The set index stays ip>>shamt; the tag is that window XORed with the ASID.
// In N=1 asid is {0,0}, so the folded tag is numerically the original tag.
struct dib_entry {
  champsim::address ip{};
  champsim::address_space_id asid{};
};

struct dib_index {
  champsim::data::bits shamt;
  auto operator()(const dib_entry& entry) const { return entry.ip.slice_upper(shamt); }
};

struct dib_tag {
  champsim::data::bits shamt;
  auto operator()(const dib_entry& entry) const { return entry.ip.slice_upper(shamt).to<uint64_t>() ^ champsim::asid_word(entry.asid); }
};

using dib_type = champsim::lru_table<dib_entry, dib_index, dib_tag>;

struct core_context {
  using stats_type = cpu_stats;

  // Frontend and backend instruction buffers.
  std::deque<ooo_model_instr> IFETCH_BUFFER;
  std::deque<ooo_model_instr> DISPATCH_BUFFER;
  std::deque<ooo_model_instr> DECODE_BUFFER;
  std::deque<ooo_model_instr> ROB;
  std::deque<ooo_model_instr> DIB_HIT_BUFFER;
  std::deque<ooo_model_instr> input_queue;

  std::vector<std::optional<LSQ_ENTRY>> LQ;
  std::deque<LSQ_ENTRY> SQ;

  // Monotonic source of the per-LQ-entry load_waiter_id token.
  uint64_t next_load_waiter_id = 0;

  dib_type DIB;

  // The hardware-context index of this thread. It is what keys the producer
  // identity assertions in rename/retire: registers are private to a thread, so
  // the assertion is on thread identity, not asid. The core assigns this after
  // filling the context vector (the minimal test constructor leaves it 0).
  champsim::context_index ctx_index{};

  // Per-context register-rename state: the architectural alias tables. The
  // physical register file and its free list are shared and stay on the core
  // (RegisterAllocator); this context holds only a pointer to them. A rename
  // therefore reads and writes this pool through reg_allocator, which the core
  // guarantees outlives every context.
  std::array<PHYSICAL_REGISTER_ID, std::numeric_limits<uint8_t>::max() + 1> frontend_RAT, backend_RAT;
  RegisterAllocator* reg_allocator = nullptr;

  // Rename methods that read or write the per-context RATs. They are moved here
  // line-for-line from RegisterAllocator; the physical-register pool they touch
  // is the shared reg_allocator above.
  PHYSICAL_REGISTER_ID rename_dest_register(int16_t reg, champsim::program_ordered<ooo_model_instr>::id_type producer_id);
  PHYSICAL_REGISTER_ID rename_src_register(int16_t reg);
  void retire_dest_register(PHYSICAL_REGISTER_ID physreg);
  void reset_frontend_RAT();
  bool isAllocated(PHYSICAL_REGISTER_ID archreg) const;

  // V2.5: release every physical register this context's drained architectural
  // state still holds back to the shared free pool, and clear both RATs. Called
  // only after the trace has EOF'd and the pipeline (front-end through ROB and
  // LSQ) is empty, so frontend_RAT == backend_RAT and each mapped register is
  // exactly one backend_RAT entry. Idempotent in effect: an empty RAT frees
  // nothing.
  void reclaim_registers();

  // B2.2 scoped flush: discard every in-flight (uncommitted) instruction of this
  // context and release the physical registers they hold, leaving only the
  // committed architectural state in backend_RAT. Clears the front-end buffers,
  // the ROB and both queues; reset_frontend_RAT() rolls the rename alias table
  // back to the committed mapping; then every busy register this context owns
  // that is not a backend_RAT value is freed. Memory requests already issued
  // keep flying in the channel (value-less model: they only warm the cache). The
  // caller must rsave the committed RAT (rsave_registers) before the slot is
  // reused, and free the slot (slot_map = -1).
  void flush_pipeline();

  // B2.2 rsave: snapshot this context's committed architectural register mapping
  // (backend_RAT) into `saved_rat` -- the value-less IRB entry of bmt-design.md
  // section 4.3 (which physical register each architectural register maps to,
  // not register data) -- then release those registers to the shared free pool
  // and clear both RATs. Pairs with flush_pipeline(): in-flight registers are
  // already freed, so backend_RAT is exactly the committed state.
  void rsave_registers(std::array<PHYSICAL_REGISTER_ID, std::numeric_limits<uint8_t>::max() + 1>& saved_rat);

  // B2.3 rrestore: rebuild this context's committed architectural register
  // mapping from the value-less IRB entry `saved_rat`. For each architectural
  // register the snapshot says was mapped, allocate a FRESH physical register
  // from the shared free pool (the snapshot's stored IDs are stale -- they were
  // released at rsave and may already name a sibling's register), stamp its
  // metadata, and write both RATs so frontend == backend (the committed state).
  // Reads `saved_rat` only; the caller owns clearing it afterward.
  void rrestore_registers(const std::array<PHYSICAL_REGISTER_ID, std::numeric_limits<uint8_t>::max() + 1>& saved_rat);

  // The RAT half of the deadlock printer; RegisterAllocator::print_deadlock()
  // prints the physical-file half. Called back-to-back in that order by
  // O3_CPU::print_deadlock() so the combined output matches the original.
  void print_deadlock();

  // Fetch freeze bookkeeping for this context.
  champsim::chrono::clock::time_point fetch_resume_time{};
  champsim::chrono::clock::time_point fetch_stall_begin{};
  bool fetch_stalled_on_mispredict = false;

  // Per-context statistics and phase bookkeeping.
  stats_type roi_stats{}, sim_stats{};

  long long num_retired = 0;

  champsim::chrono::clock::time_point begin_phase_time{};
  long long begin_phase_instr = 0;
  champsim::chrono::clock::time_point finish_phase_time{};
  long long finish_phase_instr = 0;

  // Stage 5.3: whether this context has already latched its own ROI completion
  // (the cycle it first retired `length` phase instructions). Reset in
  // begin_phase; set once by O3_CPU::record_roi_completion so the first crossing
  // wins and later cycles cannot overwrite sim_stats.roi_cycles.
  bool roi_complete_recorded = false;

  // V2.5: thread completion, armed by the phase loop when this context's trace
  // reaches EOF (only reachable with sim.trace_repeat=false; under the default
  // repeat=true the tracereader never reports EOF, so both flags stay false and
  // this whole path is dead code). `trace_eof` means no more input will ever
  // arrive; `finished` is latched once the pipeline has fully drained and its
  // physical registers have been reclaimed, after which it never allocates again.
  bool trace_eof = false;
  bool finished = false;

  champsim::chrono::clock::time_point last_heartbeat_time{};
  long long last_heartbeat_instr = 0;

  // Minimal context bound to a shared physical register file. Tests use this to
  // exercise the RAT methods directly; the DIB geometry is irrelevant there.
  explicit core_context(RegisterAllocator* shared_prf) : DIB(1, 1), reg_allocator(shared_prf)
  {
    frontend_RAT.fill(-1); // default value for no mapping
    backend_RAT.fill(-1);
  }

  core_context(RegisterAllocator* shared_prf, std::size_t lq_size, std::size_t dib_set, std::size_t dib_way, unsigned long dib_window)
      : LQ(lq_size), DIB(dib_set, dib_way, {champsim::data::bits{champsim::lg2(dib_window)}}, {champsim::data::bits{champsim::lg2(dib_window)}}),
        reg_allocator(shared_prf)
  {
    frontend_RAT.fill(-1); // default value for no mapping
    backend_RAT.fill(-1);
  }
};

// BMT (Balanced Multithreading) virtual thread: the *persistent* state of one
// virtual context. A physical slot (core_context) is the pipeline lens that is
// reused by whichever virtual thread is bound to it; this record survives a
// swap-out and is rebound on swap-in. B1 builds only the identity fields and the
// state machine -- the RAT snapshot / PC / predictor state / input_queue archive
// of bmt-design.md §1 are B2 fields and are deliberately not declared yet, so a
// B1 virtual thread holds no resource and no swap behavior exists.
struct virtual_thread {
  // Identity. virtual_id indexes the virtual_thread pool (0..C_virt-1);
  // trace_slot is the trace_index key naming which positional trace feeds it.
  std::size_t virtual_id{};
  std::size_t trace_slot{};

  // B2.2 swap-out archive: the unfetched trace residual this virtual thread had
  // not yet fetched when it was swapped out. The tracereader keeps its own
  // position (it pauses naturally when the slot stops reading it), so archiving
  // the input_queue is the whole of "save PC" in a trace-driven core; swap-in
  // moves it back into the physical slot's input_queue.
  std::deque<ooo_model_instr> input_queue{};

  // B2.2 rsave: the committed architectural register mapping snapshot (the
  // value-less IRB entry, bmt-design.md section 4.3). -1 means "not mapped"; the
  // O3_CPU constructor fills it so a never-swapped thread reads a clean RAT.
  std::array<PHYSICAL_REGISTER_ID, std::numeric_limits<uint8_t>::max() + 1> saved_rat{};

  // B2.2 livelock guard: instructions retired since this virtual thread was last
  // swapped in. Swap-out defers (skips) while this is < 1, so a swap-in ->
  // immediate-swap-out cycle that never retires cannot livelock the core.
  long long retired_since_swapin = 0;

  // B3.1 per-thread cumulative retirement (the weighted-speedup per-thread
  // decomposition denominator). Accumulates every retire while bound and, unlike
  // retired_since_swapin, is NOT cleared on swap-in -- it is this thread's
  // lifetime instruction count for the phase. begin_phase resets it so the
  // end-of-ROI SUMMARY reports exactly the ROI retirement per trace. Inert
  // (0) when BMT is off.
  long long num_retired = 0;

  // B2.4 switch-when-ready gate (bmt-design.md section 3 step 5): the number of
  // this virtual thread's loads that were still in flight when it was last
  // swapped out, and have not yet returned. Swap-out counts them before the
  // scoped flush clears the LQ; every orphaned return (a load the cleared LQ no
  // longer absorbs) decrements it. It travels with the thread across re-binds --
  // the bound slot defers fetch (initialize_instruction_impl) until it reaches
  // zero, so the re-executed load hits the cache the original request warmed.
  // Inert when BMT is off.
  long outstanding_loads = 0;

  // B3.0 per-event trace timestamps (bmt.trace_events). swap_out_time is the
  // moment this thread last swapped out -- both the swap-in interval and the
  // switch-when-ready in-flight wait measure from it. inflight_wait_pending
  // marks a swap-out that still had loads in flight whose drain-to-zero wait
  // has not yet been recorded. Inert (empty / false) while the trace switch is
  // off.
  std::optional<champsim::chrono::clock::time_point> swap_out_time{};
  bool inflight_wait_pending = false;

  // B3.1 per-thread swap census riders on the B3.0 aggregate summary.
  // swap_out_count is the number of times this thread was swapped out; cold_cycles
  // is the summed swap-out -> swap-in interval (the thread's total IRB
  // "cold-storage" residency). A first-bind has no prior swap-out and contributes
  // nothing. Both only accumulate while a swap actually happens (never during
  // warmup, where swap-out/in are gated off), so begin_phase need not reset them.
  // Emitted by the SUMMARY only when bmt.trace_events is on.
  uint64_t swap_out_count = 0;
  long long cold_cycles = 0;

  // State machine. `running` = bound to a physical slot; `suspended` = swapped
  // out (recoverable, rebindable); `finished` = trace EOF, never re-bound. B1
  // never leaves `running`; B2.2 moves it to `suspended` on swap-out.
  enum class state : uint8_t { running, suspended, finished };
  state st{state::running};
};

} // namespace champsim

#endif