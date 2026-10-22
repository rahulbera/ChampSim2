#include <algorithm>
#include <array>
#include <catch.hpp>
#include <cstdint>

#include "ooo_cpu.h"

// BMT B2.2: swap-out archives a virtual thread whose long-latency load was
// detected and frees its physical slot. The five segments are cancel load +
// scoped flush (flush_pipeline), rsave (rsave_registers), archive the unfetched
// trace residual, free the slot, and the livelock + zero-residue assertions.
// These tests pin the gate (off by default, and a refinement of bmt_enabled),
// the trigger, and the post-conditions of a swap-out (slot freed, thread
// suspended, committed RAT snapshotted, every physical register released).

namespace
{
const std::array<uint8_t, 2> asid{0, 0};

void emplace_detected_load(O3_CPU& cpu)
{
  auto& entry = cpu.ctx().LQ.at(0).emplace(champsim::address{0x1000}, 1, champsim::address{0x404}, asid);
  entry.fetch_issued = true;
  entry.bmt_detected = true;
  entry.load_waiter_id = 1;
}
} // namespace

TEST_CASE("B2.2 swap-out is off by default")
{
  O3_CPU cpu{champsim::core_builder{}.index(0).num_contexts(1).lq_size(4).bmt_enabled(true)};
  cpu.warmup = false;

  CHECK_FALSE(cpu.bmt_swap_enabled);

  emplace_detected_load(cpu);
  cpu.swap_out_detected_loads();

  // Still identity-bound and running: the swap gate is off.
  CHECK(cpu.slot_map[0] == 0);
  CHECK(cpu.virtual_threads[0].st == champsim::virtual_thread::state::running);
  CHECK(cpu.ctx().sim_stats.bmt_swap_outs == 0);
}

TEST_CASE("B2.2 swap-out refines detection and needs bmt_enabled too")
{
  O3_CPU cpu{champsim::core_builder{}.index(0).num_contexts(1).lq_size(4).bmt_swap_enabled(true)};
  cpu.warmup = false;

  // swap_enabled alone must not swap: detection (bmt_enabled) is what marks the
  // load, and without it the swap gate refuses to act.
  CHECK_FALSE(cpu.bmt_enabled);
  emplace_detected_load(cpu);
  cpu.swap_out_detected_loads();

  CHECK(cpu.slot_map[0] == 0);
  CHECK(cpu.virtual_threads[0].st == champsim::virtual_thread::state::running);
  CHECK(cpu.ctx().sim_stats.bmt_swap_outs == 0);
}

TEST_CASE("B2.2 swap_out_detected_loads archives a slot whose load was detected")
{
  O3_CPU cpu{champsim::core_builder{}.index(0).num_contexts(1).lq_size(4).bmt_enabled(true).bmt_swap_enabled(true)};
  cpu.warmup = false;
  cpu.virtual_threads[0].retired_since_swapin = 1; // satisfy the livelock guard

  emplace_detected_load(cpu);
  cpu.swap_out_detected_loads();

  CHECK(cpu.slot_map[0] == -1);
  CHECK(cpu.virtual_threads[0].st == champsim::virtual_thread::state::suspended);
  CHECK(cpu.ctx().sim_stats.bmt_swap_outs == 1);
}

TEST_CASE("B2.2 swap_out_context snapshots the committed RAT and releases its registers")
{
  O3_CPU cpu{champsim::core_builder{}.index(0).num_contexts(1).lq_size(4).register_file_size(128).bmt_enabled(true).bmt_swap_enabled(true)};
  cpu.warmup = false;

  auto& c = cpu.ctx();
  // One committed architectural register: rename_src_register maps an unmapped
  // register into both RATs, so backend_RAT[3] is the committed mapping.
  const PHYSICAL_REGISTER_ID phys = c.rename_src_register(3);
  const auto free_before = cpu.shared_prf->count_free_registers();

  emplace_detected_load(cpu);
  cpu.virtual_threads[0].retired_since_swapin = 1;

  cpu.swap_out_context(0);

  // Segment (c): the committed mapping was snapshotted into the IRB entry before
  // the registers were released.
  CHECK(cpu.virtual_threads[0].saved_rat[3] == phys);

  // Segment (e): the slot leaves no trace -- empty buffers/queues and both RATs
  // cleared. (The zero-residue assertions in swap_out_context re-check these.)
  CHECK(c.ROB.empty());
  CHECK(c.IFETCH_BUFFER.empty());
  CHECK(c.SQ.empty());
  CHECK(std::all_of(std::begin(c.LQ), std::end(c.LQ), [](const auto& e) { return !e.has_value(); }));
  CHECK(std::all_of(std::begin(c.frontend_RAT), std::end(c.frontend_RAT), [](PHYSICAL_REGISTER_ID p) { return p == -1; }));
  CHECK(std::all_of(std::begin(c.backend_RAT), std::end(c.backend_RAT), [](PHYSICAL_REGISTER_ID p) { return p == -1; }));

  // The committed register went back to the shared free pool exactly once.
  CHECK(cpu.shared_prf->count_free_registers() == free_before + 1);
}
