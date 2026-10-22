#include <catch.hpp>

#include "instr.h"
#include "mocks.hpp"
#include "ooo_cpu.h"

// BMT B3.1: per-virtual-thread cumulative retirement (the weighted-speedup
// per-thread decomposition denominator) and its two swap census riders
// (per-thread swap-out count and cold-storage cycles). num_retired accumulates
// on the retire path, is NOT cleared on swap-in (unlike the livelock guard it
// sits beside), and is reset by begin_phase so the end-of-ROI SUMMARY reports
// the ROI retirement only. The riders only accumulate while a swap actually
// happens (never during warmup), so they need no phase reset. All three are
// pure observation: a default run is byte-identical.

TEST_CASE("B3.1 per-thread counters are zero/off by default")
{
  O3_CPU cpu{champsim::core_builder{}.index(0).num_contexts(1).lq_size(4)};

  CHECK(cpu.virtual_threads[0].num_retired == 0);
  CHECK(cpu.virtual_threads[0].swap_out_count == 0);
  CHECK(cpu.virtual_threads[0].cold_cycles == 0);
}

TEST_CASE("B3.1 the retire path accumulates the per-thread retirement (identity binding)")
{
  do_nothing_MRC mock_L1I, mock_L1D;
  O3_CPU uut{champsim::core_builder{}
                 .retire_width(champsim::bandwidth::maximum_type{1})
                 .fetch_queues(&mock_L1I.queues)
                 .data_queues(&mock_L1D.queues)};

  uut.ctx().ROB.push_back(champsim::test::instruction_with_ip(1));
  uut.ctx().ROB.front().completed = true;

  for (auto op : std::array<champsim::operable*, 3>{{&uut, &mock_L1I, &mock_L1D}})
    op->_operate();

  // The slot retires one instruction, and the identity-bound virtual thread
  // tracks it exactly -- the new counter is a pure observation, not a behavior.
  REQUIRE(uut.ctx().num_retired == 1);
  REQUIRE(uut.virtual_threads[0].num_retired == 1);
}

TEST_CASE("B3.1 swap-in preserves the per-thread retirement while clearing the livelock guard")
{
  O3_CPU cpu{champsim::core_builder{}.index(0).num_contexts(1).lq_size(4).bmt_enabled(true).bmt_swap_enabled(true)};
  cpu.warmup = false;

  cpu.virtual_threads[0].num_retired = 42;
  cpu.virtual_threads[0].retired_since_swapin = 1; // satisfy the livelock guard
  cpu.swap_out_context(0);
  cpu.swap_in_context(0, 0);

  // The livelock guard resets so the next swap-out must retire >= 1 first; the
  // cumulative count does not -- it is the thread's lifetime phase total.
  CHECK(cpu.virtual_threads[0].retired_since_swapin == 0);
  CHECK(cpu.virtual_threads[0].num_retired == 42);
}

TEST_CASE("B3.1 begin_phase resets the per-thread retirement")
{
  O3_CPU cpu{champsim::core_builder{}.index(0).num_contexts(2).num_virtual_threads(4).lq_size(4)};

  cpu.virtual_threads[0].num_retired = 42;
  cpu.virtual_threads[3].num_retired = 7;

  cpu.begin_phase();

  CHECK(cpu.virtual_threads[0].num_retired == 0);
  CHECK(cpu.virtual_threads[3].num_retired == 0);
}

TEST_CASE("B3.1 the swap census riders accumulate on swap-out/in, and a first-bind contributes nothing")
{
  O3_CPU cpu{champsim::core_builder{}.index(0).num_contexts(1).lq_size(4).bmt_enabled(true).bmt_swap_enabled(true).bmt_trace_events(true)};
  cpu.warmup = false;
  cpu.current_time = champsim::chrono::clock::time_point{champsim::chrono::clock::duration{1000}};
  cpu.virtual_threads[0].retired_since_swapin = 1;

  cpu.swap_out_context(0);
  CHECK(cpu.virtual_threads[0].swap_out_count == 1);

  // Advance the clock so the swap-in sees a nonzero out-to-in interval.
  cpu.current_time += 100 * cpu.clock_period;
  cpu.swap_in_context(0, 0);

  CHECK(cpu.virtual_threads[0].cold_cycles == 100);
  CHECK(cpu.virtual_threads[0].swap_out_count == 1); // swap-in does not touch it
}
