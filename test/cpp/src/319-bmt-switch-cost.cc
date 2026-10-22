#include <array>
#include <catch.hpp>

#include "instr.h"
#include "mocks.hpp"
#include "ooo_cpu.h"

// BMT B2.4: swap-cost parameterization (bmt_switch: instant vs firmware) and the
// switch-when-ready fetch gate deferred from B2.3 (bmt-design.md section 3 step 5
// and section 6). instant models the swap atomically (the default, so a BMT-off
// run stays byte-identical to B2.3); firmware charges the paper's ~60-cycle
// switch latency, split 30 cycles of rsave+overhead at swap-out and 30 cycles of
// rrestore+overhead at swap-in, held on the slot as a fetch resume deadline. The
// switch-when-ready count lives on the virtual thread -- the number of its loads
// still in flight since swap-out, drained by orphaned returns -- and the bound
// slot defers fetch until it reaches zero.

namespace
{
const std::array<uint8_t, 2> asid{0, 0};

void emplace_inflight_load(O3_CPU& cpu)
{
  auto& entry = cpu.ctx().LQ.at(0).emplace(champsim::address{0x1000}, 1, champsim::address{0x404}, asid);
  entry.fetch_issued = true;
}
} // namespace

TEST_CASE("B2.4 bmt_switch defaults to instant (no swap cost)")
{
  O3_CPU cpu{champsim::core_builder{}.index(0).num_contexts(1).lq_size(4)};

  CHECK(cpu.bmt_switch_mode_ == champsim::bmt_switch_mode::instant);
  CHECK(cpu.bmt_swap_out_cost == champsim::chrono::clock::duration{});
  CHECK(cpu.bmt_swap_in_cost == champsim::chrono::clock::duration{});
}

TEST_CASE("B2.4 bmt_switch firmware charges the split ~60-cycle cost")
{
  O3_CPU cpu{champsim::core_builder{}.index(0).num_contexts(1).lq_size(4).bmt_switch(champsim::bmt_switch_mode::firmware)};

  CHECK(cpu.bmt_switch_mode_ == champsim::bmt_switch_mode::firmware);
  // 30 cycles each (rsave+overhead / rrestore+overhead), scaled by the clock period.
  CHECK(cpu.bmt_swap_out_cost == 30 * cpu.clock_period);
  CHECK(cpu.bmt_swap_in_cost == 30 * cpu.clock_period);
}

TEST_CASE("B2.4 firmware swap-out charges the slot a resume deadline")
{
  O3_CPU cpu{
      champsim::core_builder{}.index(0).num_contexts(1).lq_size(4).bmt_enabled(true).bmt_swap_enabled(true).bmt_switch(champsim::bmt_switch_mode::firmware)};
  cpu.warmup = false;
  cpu.current_time = champsim::chrono::clock::time_point{champsim::chrono::clock::duration{1000}};
  cpu.virtual_threads[0].retired_since_swapin = 1; // satisfy the livelock guard

  cpu.swap_out_context(0);

  CHECK(cpu.swap_resume_time[0] == cpu.current_time + cpu.bmt_swap_out_cost);
}

TEST_CASE("B2.4 firmware swap-in accumulates the rrestore share onto the deadline")
{
  O3_CPU cpu{
      champsim::core_builder{}.index(0).num_contexts(1).lq_size(4).bmt_enabled(true).bmt_swap_enabled(true).bmt_switch(champsim::bmt_switch_mode::firmware)};
  cpu.warmup = false;
  cpu.current_time = champsim::chrono::clock::time_point{champsim::chrono::clock::duration{1000}};
  cpu.virtual_threads[0].retired_since_swapin = 1;

  cpu.swap_out_context(0);
  cpu.swap_in_context(0, 0);

  // A same-cycle out-then-in folds the two shares into one full switch latency.
  CHECK(cpu.swap_resume_time[0] == cpu.current_time + cpu.bmt_swap_out_cost + cpu.bmt_swap_in_cost);
}

TEST_CASE("B2.4 instant swap charges no resume deadline")
{
  O3_CPU cpu{champsim::core_builder{}.index(0).num_contexts(1).lq_size(4).bmt_enabled(true).bmt_swap_enabled(true)};
  cpu.warmup = false;
  cpu.current_time = champsim::chrono::clock::time_point{champsim::chrono::clock::duration{1000}};
  cpu.virtual_threads[0].retired_since_swapin = 1;

  cpu.swap_out_context(0);
  cpu.swap_in_context(0, 0);

  CHECK(cpu.swap_resume_time[0] == champsim::chrono::clock::time_point{});
}

TEST_CASE("B2.4 swap-out counts in-flight loads and the count survives the re-bind")
{
  O3_CPU cpu{champsim::core_builder{}.index(0).num_contexts(1).lq_size(4).bmt_enabled(true).bmt_swap_enabled(true)};
  cpu.warmup = false;
  cpu.virtual_threads[0].retired_since_swapin = 1;
  emplace_inflight_load(cpu);

  cpu.swap_out_context(0);
  CHECK(cpu.virtual_threads[0].outstanding_loads == 1);

  cpu.swap_in_context(0, 0);
  // The count travels with the thread: it is not reset at re-bind, so a thread
  // re-bound before its orphans return is still gated on their return.
  CHECK(cpu.virtual_threads[0].outstanding_loads == 1);
}

TEST_CASE("B2.4 an orphaned return decrements the outstanding-load count")
{
  do_nothing_MRC l1i, l1d;
  O3_CPU cpu{champsim::core_builder{}.fetch_queues(&l1i.queues).data_queues(&l1d.queues).num_contexts(1).lq_size(4).bmt_enabled(true).bmt_swap_enabled(true)};
  cpu.virtual_threads[0].outstanding_loads = 1;

  // A returned load no live LQ entry absorbs (the LQ is empty): an orphan from a
  // swap-out that cleared it. The response's load_waiters names the orphaned load.
  l1d.queues.returned.push_back(
      champsim::channel::response_type{champsim::address{0x1000}, champsim::address{0x1000}, champsim::address{}, 0, 0, 0, 0, {}, {1}});

  cpu.handle_memory_return();

  CHECK(cpu.virtual_threads[0].outstanding_loads == 0);
}

TEST_CASE("B2.4 fetch is gated by the firmware resume deadline")
{
  O3_CPU cpu{champsim::core_builder{}.index(0).num_contexts(1).lq_size(4)};
  auto& c = cpu.ctx();
  c.input_queue.push_back(champsim::test::instruction_with_ip(uint64_t{0x1000}));
  cpu.swap_resume_time[0] = champsim::chrono::clock::time_point{champsim::chrono::clock::duration{1000}};
  cpu.current_time = champsim::chrono::clock::time_point{champsim::chrono::clock::duration{500}};
  champsim::bandwidth bw{champsim::bandwidth::maximum_type{4}};

  cpu.initialize_instruction_impl(c, champsim::context_index{0}, bw);

  CHECK(c.IFETCH_BUFFER.empty()); // gated: nothing fetched
}

TEST_CASE("B2.4 fetch is gated while the bound thread's loads are in flight")
{
  O3_CPU cpu{champsim::core_builder{}.index(0).num_contexts(1).lq_size(4)};
  auto& c = cpu.ctx();
  c.input_queue.push_back(champsim::test::instruction_with_ip(uint64_t{0x1000}));
  cpu.virtual_threads[0].outstanding_loads = 1; // the bound thread's orphan still flying
  champsim::bandwidth bw{champsim::bandwidth::maximum_type{4}};

  cpu.initialize_instruction_impl(c, champsim::context_index{0}, bw);

  CHECK(c.IFETCH_BUFFER.empty()); // switch-when-ready: fetch waits for the return
}

TEST_CASE("B2.4 fetch proceeds once both gates are open")
{
  O3_CPU cpu{champsim::core_builder{}.index(0).num_contexts(1).lq_size(4)};
  auto& c = cpu.ctx();
  c.input_queue.push_back(champsim::test::instruction_with_ip(uint64_t{0x1000}));
  champsim::bandwidth bw{champsim::bandwidth::maximum_type{4}};

  cpu.initialize_instruction_impl(c, champsim::context_index{0}, bw);

  CHECK_FALSE(c.IFETCH_BUFFER.empty()); // no gate: the residual was fetched
  CHECK(c.input_queue.empty());
}
