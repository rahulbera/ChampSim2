#include <catch.hpp>

#include "ooo_cpu.h"

// BMT B3.0: the per-event trace log (bmt.trace_events). Default off, so a
// default run stays byte-identical to B2.4 -- the switch gates every stderr
// line and every aggregate accumulator, and none of it is observable when off.
// These tests pin the default, the builder override, and that a swap-out with
// the switch on stamps the accumulator and the swap-out timestamp the swap-in
// interval and in-flight wait both measure from. The event lines themselves go
// to stderr and are exercised only through their side effects (counts and
// timestamps), not their formatting.

TEST_CASE("B3.0 bmt_trace_events defaults to off")
{
  O3_CPU cpu{champsim::core_builder{}.index(0).num_contexts(1).lq_size(4)};

  CHECK_FALSE(cpu.bmt_trace_events);
  CHECK(cpu.bmt_trace_swap_outs == 0);
  CHECK(cpu.bmt_trace_defers == 0);
  CHECK(cpu.bmt_swap_in_intervals.empty());
  CHECK(cpu.bmt_inflight_waits.empty());
}

TEST_CASE("B3.0 bmt_trace_events builder override sets the member")
{
  O3_CPU cpu{champsim::core_builder{}.index(0).num_contexts(1).lq_size(4).bmt_trace_events(true)};

  CHECK(cpu.bmt_trace_events);
}

TEST_CASE("B3.0 a traced swap-out stamps the swap-out count and timestamp")
{
  O3_CPU cpu{champsim::core_builder{}.index(0).num_contexts(1).lq_size(4).bmt_enabled(true).bmt_swap_enabled(true).bmt_trace_events(true)};
  cpu.warmup = false;
  cpu.current_time = champsim::chrono::clock::time_point{champsim::chrono::clock::duration{1000}};
  cpu.virtual_threads[0].retired_since_swapin = 1; // satisfy the livelock guard

  cpu.swap_out_context(0);

  CHECK(cpu.bmt_trace_swap_outs == 1);
  CHECK(cpu.virtual_threads[0].swap_out_time.has_value());
  CHECK(*cpu.virtual_threads[0].swap_out_time == cpu.current_time);
  // The slot's LQ was empty at swap-out, so there is no in-flight wait pending.
  CHECK_FALSE(cpu.virtual_threads[0].inflight_wait_pending);
}
