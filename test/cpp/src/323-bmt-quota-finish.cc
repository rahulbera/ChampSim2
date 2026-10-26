#include <catch.hpp>

#include "instr.h"
#include "mocks.hpp"
#include "ooo_cpu.h"

// BMT B3.1.5: the quota-finish seal (ooo_cpu.cpu0.quota_finish). When off (the
// default) the whole mechanism is inert: mark_quota_finished() never marks a
// thread `finished` and reclaim_finished_threads() never frees a slot, so a
// default run is byte-identical to the pre-seal simulator. When on, a virtual
// thread that has retired its phase budget (quota_length) is sealed: marked
// `finished`, its trace read stops, swap-out skips it, and its slot is reclaimed
// once its pipeline drains -- the paper's "fixed total instruction budget" window.

TEST_CASE("B3.1.5 quota_finish defaults off (inert)")
{
  O3_CPU cpu{champsim::core_builder{}.index(0).num_contexts(1).lq_size(4)};

  CHECK(cpu.quota_finish == false);
  CHECK(cpu.quota_length == 0);
}

TEST_CASE("B3.1.5 mark_quota_finished is inert while quota_finish is off")
{
  O3_CPU cpu{champsim::core_builder{}.index(0).num_contexts(1).lq_size(4)};
  cpu.warmup = false;
  cpu.quota_length = 100;                     // pretend a budget is stamped
  cpu.virtual_threads[0].num_retired = 1000;  // already far past it

  cpu.mark_quota_finished();

  // No thread may be marked finished while the switch is off.
  CHECK(cpu.virtual_threads[0].st == champsim::virtual_thread::state::running);
}

TEST_CASE("B3.1.5 reclaim_finished_threads is inert while quota_finish is off")
{
  O3_CPU cpu{champsim::core_builder{}.index(0).num_contexts(1).lq_size(4)};
  cpu.warmup = false;
  // Force a "finished" state to prove the gate (not the state) blocks the reclaim.
  cpu.virtual_threads[0].st = champsim::virtual_thread::state::finished;

  cpu.reclaim_finished_threads();

  // The identity binding must remain intact: the slot stays bound.
  CHECK(cpu.slot_map[0] == 0);
}

TEST_CASE("B3.1.5 mark_quota_finished seals a thread at its budget when on")
{
  O3_CPU cpu{champsim::core_builder{}.index(0).num_contexts(1).lq_size(4).quota_finish(true)};
  cpu.warmup = false;
  cpu.quota_length = 100;
  cpu.virtual_threads[0].num_retired = 100;

  cpu.mark_quota_finished();

  CHECK(cpu.virtual_threads[0].st == champsim::virtual_thread::state::finished);
}

TEST_CASE("B3.1.5 reclaim_finished_threads does not free a finished thread's slot in a slot arm")
{
  O3_CPU cpu{champsim::core_builder{}.index(0).num_contexts(2).num_virtual_threads(2).lq_size(4).quota_finish(true)};
  cpu.warmup = false;
  cpu.virtual_threads[0].st = champsim::virtual_thread::state::finished;

  // C_virt == C_phys: there is no cold (suspended) thread to inherit the slot, so
  // reclaim must NOT free it -- doing so would leave a spare empty slot that a
  // sibling could migrate into and scramble the per-context accounting.
  cpu.reclaim_finished_threads();

  CHECK(cpu.slot_map[0] == 0);
  CHECK(cpu.slot_map[1] == 1);
}

TEST_CASE("B3.1.5 reclaim_finished_threads frees a drained finished thread's slot in a BMT arm")
{
  O3_CPU cpu{champsim::core_builder{}.index(0).num_contexts(2).num_virtual_threads(4).lq_size(4).quota_finish(true)};
  cpu.warmup = false;
  cpu.virtual_threads[0].st = champsim::virtual_thread::state::finished;

  // C_virt > C_phys: a freshly-constructed context is fully drained (empty
  // buffers, empty LSQ), so the drained predicate fires and the slot is released
  // for a suspended thread to swap into.
  cpu.reclaim_finished_threads();

  CHECK(cpu.slot_map[0] == -1);
  CHECK(cpu.slot_map[1] == 1);
}
