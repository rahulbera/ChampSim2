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

// B3.1.6 no-beneficiary clamp: a swap-out whose vacated slot no OTHER virtual
// thread could occupy is pure self-thrash. The clamp skips it at the top of
// swap_out_context() -- before the livelock guard -- when every other vthread is
// `finished`. Inert while quota_finish is off (no thread is ever finished).
TEST_CASE("B3.1.6 no-beneficiary clamp skips swap-out when every other vthread is finished")
{
  O3_CPU cpu{champsim::core_builder{}.index(0).num_contexts(2).num_virtual_threads(2).lq_size(4).quota_finish(true)};
  cpu.warmup = false;
  // ③ tail: vthread 0 is the last survivor; every other vthread has finished.
  cpu.virtual_threads[1].st = champsim::virtual_thread::state::finished;
  cpu.virtual_threads[0].retired_since_swapin = 1;  // clear the livelock guard, so
                                                    // only the clamp can block.

  cpu.swap_out_context(0);

  // No swap-out: the thread stays seated and waits out its miss.
  CHECK(cpu.virtual_threads[0].st == champsim::virtual_thread::state::running);
  CHECK(cpu.slot_map[0] == 0);
}

TEST_CASE("B3.1.6 no-beneficiary clamp lets swap-out proceed when another vthread is unfinished")
{
  O3_CPU cpu{champsim::core_builder{}.index(0).num_contexts(2).num_virtual_threads(2).lq_size(4).quota_finish(true)};
  cpu.warmup = false;
  // ① partner-in-seat: vthread 1 is still running (not finished), so a swap-out
  // still has a beneficiary. The pair behavior is unchanged.
  cpu.virtual_threads[0].retired_since_swapin = 1;

  cpu.swap_out_context(0);

  CHECK(cpu.virtual_threads[0].st == champsim::virtual_thread::state::suspended);
  CHECK(cpu.slot_map[0] == -1);
}

TEST_CASE("B3.1.6 no-beneficiary clamp is default-inert (no finished thread while quota_finish is off)")
{
  O3_CPU cpu{champsim::core_builder{}.index(0).num_contexts(2).num_virtual_threads(4).lq_size(4)};
  cpu.warmup = false;
  // Normal BMT arm with quota_finish OFF: no thread is ever marked `finished`, so
  // the predicate never fires and the swap-out proceeds exactly as before.
  cpu.virtual_threads[0].retired_since_swapin = 1;

  cpu.swap_out_context(0);

  CHECK(cpu.virtual_threads[0].st == champsim::virtual_thread::state::suspended);
  CHECK(cpu.slot_map[0] == -1);
}
