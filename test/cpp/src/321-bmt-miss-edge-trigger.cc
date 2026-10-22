#include <array>
#include <catch.hpp>
#include <cstdint>

#include "cache.h"
#include "defaults.hpp"
#include "ooo_cpu.h"

// BMT B2.5: the miss-edge detection source. Where B2.1 flags a load after a
// fixed number of in-flight cycles, the `miss` arm flags it the cycle the LLC
// confirms the load's miss -- read as an increment of the LLC's per-context
// load-miss census over this core's last-seen snapshot. The optional dwell
// waits N cycles after the edge before swap-out acts. These tests pin the
// builder defaults (latency / dwell 0 / no LLC, i.e. byte-identical to B2.4),
// the knob overrides, the null-LLC guard, the edge-then-detect ordering, the
// no-double-count latch, and the dwell gate on swap-out.

namespace
{
const std::array<uint8_t, 2> asid{0, 0};

void emplace_inflight_load(O3_CPU& cpu, std::size_t slot, uint64_t waiter_id)
{
  auto& entry = cpu.ctx().LQ.at(slot).emplace(champsim::address{0x1000}, waiter_id, champsim::address{0x404}, asid);
  entry.fetch_issued = true;
  entry.load_waiter_id = waiter_id;
}
} // namespace

TEST_CASE("B2.5 bmt_trigger defaults to latency and the dwell to zero")
{
  O3_CPU cpu{champsim::core_builder{}.index(0).num_contexts(1).lq_size(4)};

  CHECK(cpu.bmt_trigger_mode_ == champsim::bmt_trigger_mode::latency);
  CHECK(cpu.bmt_miss_dwell == 0);
  CHECK(cpu.llc == nullptr);
}

TEST_CASE("B2.5 bmt_trigger/bmt_miss_dwell builder overrides set the members")
{
  O3_CPU cpu{champsim::core_builder{}.index(0).num_contexts(1).lq_size(4)
                 .bmt_trigger(champsim::bmt_trigger_mode::miss)
                 .bmt_miss_dwell(25)};

  CHECK(cpu.bmt_trigger_mode_ == champsim::bmt_trigger_mode::miss);
  CHECK(cpu.bmt_miss_dwell == 25);
}

TEST_CASE("B2.5 miss arm with no wired LLC is inert")
{
  O3_CPU cpu{champsim::core_builder{}.index(0).num_contexts(1).lq_size(4).bmt_enabled(true).bmt_trigger(champsim::bmt_trigger_mode::miss)};
  cpu.warmup = false;

  emplace_inflight_load(cpu, 0, 1);

  cpu.detect_long_latency_loads();

  CHECK(cpu.ctx().sim_stats.bmt_long_latency_detections == 0);
}

TEST_CASE("B2.5 miss arm detects only after the LLC confirms the miss")
{
  CACHE llc{champsim::cache_builder{champsim::defaults::default_l1d}.per_context_counters(true)};
  O3_CPU cpu{champsim::core_builder{}.index(0).num_contexts(1).lq_size(4).bmt_enabled(true).bmt_trigger(champsim::bmt_trigger_mode::miss).llc(&llc)};
  cpu.warmup = false;

  emplace_inflight_load(cpu, 0, 1);

  // The load is in flight, but the LLC has confirmed no miss for asid 0 yet: the
  // edge has not arrived, so nothing is detected regardless of issue age.
  cpu.detect_long_latency_loads();
  CHECK(cpu.ctx().sim_stats.bmt_long_latency_detections == 0);

  // The LLC confirms a load miss for asid 0. The next scan consumes that edge.
  llc.sim_stats.per_context_load_misses[0] = 1;
  cpu.detect_long_latency_loads();
  CHECK(cpu.ctx().sim_stats.bmt_long_latency_detections == 1);
  CHECK(cpu.ctx().LQ.at(0)->bmt_detected);
  CHECK(cpu.ctx().LQ.at(0)->bmt_detected_at == cpu.current_time);

  // The bmt_detected latch holds: re-scanning the same edge does not double-count.
  cpu.detect_long_latency_loads();
  CHECK(cpu.ctx().sim_stats.bmt_long_latency_detections == 1);
}

TEST_CASE("B2.5 miss dwell defers swap-out until the dwell elapses")
{
  O3_CPU cpu{champsim::core_builder{}.index(0).num_contexts(1).lq_size(4).bmt_enabled(true).bmt_swap_enabled(true)
                 .bmt_trigger(champsim::bmt_trigger_mode::miss)
                 .bmt_miss_dwell(10)};
  cpu.warmup = false;
  cpu.virtual_threads[0].retired_since_swapin = 1; // satisfy the livelock guard

  auto& entry = cpu.ctx().LQ.at(0).emplace(champsim::address{0x1000}, 1, champsim::address{0x404}, asid);
  entry.fetch_issued = true;
  entry.bmt_detected = true;
  entry.bmt_detected_at = cpu.current_time; // the edge arrived "now"
  entry.load_waiter_id = 1;

  // Same cycle: elapsed 0 < dwell 10, so the load is detected but the swap-out
  // holds off.
  cpu.swap_out_detected_loads();
  CHECK(cpu.slot_map[0] == 0);

  // Past the dwell: the same detected load now ages enough to swap out.
  cpu.current_time += 10 * cpu.clock_period;
  cpu.swap_out_detected_loads();
  CHECK(cpu.slot_map[0] == -1);
}
