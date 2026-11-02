#include <algorithm>
#include <catch.hpp>

#include "ooo_cpu.h"

// BMT B2.3: swap-in re-binds a suspended virtual thread to an empty physical
// slot -- LRR selection over the C_virt pool, rrestore of the committed register
// mapping from the value-less IRB snapshot, restoration of the unfetched trace
// residual, and the IRB conservation invariant (rrestore allocates exactly the
// registers rsave released). These tests pin the gate (off by default, and a
// refinement of bmt_enabled), the round-robin selection, the per-slot restore,
// the conservation + snapshot-consumption post-conditions, and the register-file
// backpressure that defers a restore the shared pool cannot yet back.

namespace
{
void suspend_slot(O3_CPU& cpu, std::size_t slot)
{
  cpu.virtual_threads[slot].retired_since_swapin = 1; // satisfy the livelock guard
  cpu.swap_out_context(slot);
}
} // namespace

TEST_CASE("B2.3 swap-in is off by default")
{
  O3_CPU cpu{champsim::core_builder{}.index(0).num_contexts(1).lq_size(4).bmt_enabled(true)};
  cpu.warmup = false;

  CHECK_FALSE(cpu.bmt_swap_enabled);

  cpu.slot_map[0] = -1;
  cpu.virtual_threads[0].st = champsim::virtual_thread::state::suspended;
  cpu.swap_in_suspended_threads();

  // Still empty and suspended: the swap gate is off.
  CHECK(cpu.slot_map[0] == -1);
  CHECK(cpu.virtual_threads[0].st == champsim::virtual_thread::state::suspended);
}

TEST_CASE("B2.3 swap-in refines swap-out and needs bmt_enabled too")
{
  O3_CPU cpu{champsim::core_builder{}.index(0).num_contexts(1).lq_size(4).bmt_swap_enabled(true)};
  cpu.warmup = false;

  // swap_enabled alone must not swap in: bmt_enabled is the master gate.
  CHECK_FALSE(cpu.bmt_enabled);
  cpu.slot_map[0] = -1;
  cpu.virtual_threads[0].st = champsim::virtual_thread::state::suspended;
  cpu.swap_in_suspended_threads();

  CHECK(cpu.slot_map[0] == -1);
  CHECK(cpu.virtual_threads[0].st == champsim::virtual_thread::state::suspended);
}

TEST_CASE("B2.3 swap_in_context restores the committed RAT, frees the slot's IRB entry, and conserves registers")
{
  O3_CPU cpu{champsim::core_builder{}.index(0).num_contexts(1).lq_size(4).register_file_size(128).bmt_enabled(true).bmt_swap_enabled(true)};
  cpu.warmup = false;
  auto& c = cpu.ctx();

  // Commit one architectural register, then swap out so rsave snapshots it.
  const PHYSICAL_REGISTER_ID old_phys = c.rename_src_register(3);
  suspend_slot(cpu, 0);

  REQUIRE(cpu.slot_map[0] == -1);
  REQUIRE(cpu.virtual_threads[0].saved_rat[3] == old_phys);

  const auto free_before = cpu.shared_prf->count_free_registers();
  cpu.swap_in_context(0, 0);

  CHECK(cpu.slot_map[0] == 0);
  CHECK(cpu.virtual_threads[0].st == champsim::virtual_thread::state::running);
  CHECK(cpu.virtual_threads[0].retired_since_swapin == 0);

  // rrestore rebuilt both RATs with a FRESH physical register -- not the stale
  // ID the snapshot stored (that was released at rsave and may be re-issued).
  const PHYSICAL_REGISTER_ID new_phys = c.backend_RAT[3];
  CHECK(new_phys != -1);
  CHECK(c.frontend_RAT[3] == new_phys);
  CHECK(new_phys != old_phys);

  // IRB conservation: one register left the free pool, and free+busy is whole.
  CHECK(cpu.shared_prf->count_free_registers() == free_before - 1);

  // The IRB entry was consumed: the snapshot is cleared.
  CHECK(std::all_of(std::begin(cpu.virtual_threads[0].saved_rat), std::end(cpu.virtual_threads[0].saved_rat),
                    [](PHYSICAL_REGISTER_ID p) { return p == -1; }));
}

TEST_CASE("B2.3 swap_in_suspended_threads LRR-selects the next suspended thread")
{
  O3_CPU cpu{champsim::core_builder{}.index(0).num_contexts(2).num_virtual_threads(4).lq_size(4).bmt_enabled(true).bmt_swap_enabled(true)};
  cpu.warmup = false;

  // Constructor: slots 0,1 bound (running); tail threads 2,3 suspended.
  CHECK(cpu.virtual_threads[2].st == champsim::virtual_thread::state::suspended);
  CHECK(cpu.virtual_threads[3].st == champsim::virtual_thread::state::suspended);

  // Swap out slot 0's thread 0 (empty pipeline: rsave snapshots nothing).
  suspend_slot(cpu, 0);
  REQUIRE(cpu.slot_map[0] == -1);

  // Start the rotation past thread 0, so LRR prefers the tail threads and skips
  // the still-bound thread 1. The first suspended candidate from the pointer is
  // thread 2.
  cpu.virtual_round_robin = 1;
  cpu.swap_in_suspended_threads();

  CHECK(cpu.slot_map[0] == 2);
  CHECK(cpu.virtual_threads[2].st == champsim::virtual_thread::state::running);
  CHECK(cpu.virtual_threads[0].st == champsim::virtual_thread::state::suspended);
  CHECK(cpu.slot_map[1] == 1); // sibling slot untouched
}

TEST_CASE("B2.3 swap_in_suspended_threads defers a restore the register file cannot back")
{
  O3_CPU cpu{champsim::core_builder{}.index(0).num_contexts(2).num_virtual_threads(4).lq_size(4).register_file_size(4).bmt_enabled(true).bmt_swap_enabled(true)};
  cpu.warmup = false;

  // Swap thread 0 out of slot 0 (empty RAT, so the IRB snapshot maps nothing),
  // then overwrite the snapshot to name more registers than the whole file holds.
  cpu.virtual_threads[0].retired_since_swapin = 1;
  cpu.swap_out_context(0);
  REQUIRE(cpu.slot_map[0] == -1);
  REQUIRE(cpu.virtual_threads[0].st == champsim::virtual_thread::state::suspended);

  // Point the rotation back at thread 0 (swap-out advanced it past 0) and give it
  // a 5-entry snapshot against a 4-register file: rrestore cannot allocate it.
  cpu.virtual_round_robin = 0;
  for (std::size_t i = 0; i < 5; ++i)
    cpu.virtual_threads[0].saved_rat[i] = static_cast<PHYSICAL_REGISTER_ID>(i);

  cpu.swap_in_suspended_threads();

  // Backpressure: the slot stays empty and the thread stays suspended (the
  // snapshot is not consumed) until the shared pool has room.
  CHECK(cpu.slot_map[0] == -1);
  CHECK(cpu.virtual_threads[0].st == champsim::virtual_thread::state::suspended);
  CHECK(cpu.virtual_threads[0].saved_rat[0] != -1);
}
