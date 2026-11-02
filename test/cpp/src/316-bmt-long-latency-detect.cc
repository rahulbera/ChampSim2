#include <array>
#include <catch.hpp>
#include <cstdint>

#include "ooo_cpu.h"

// BMT B2.1: long-latency-load detection is pure observation. A load that stays
// in flight (fetch_issued) past bmt.miss_threshold core cycles is marked
// "detected main-memory access" and charged to a per-context census; nothing in
// the pipeline changes. These tests pin the gate (off by default, warmup
// excluded), the threshold boundary, the no-double-count latch, and the
// parallel-second-miss census.

namespace
{
const std::array<uint8_t, 2> asid{0, 0};

// Emplaces one in-flight load in context 0's LQ slot `slot`, issued at `issue`
// with a unique load_waiter_id. The pipeline normally stamps issue_time when the
// load is issued to memory; a unit test sets it directly.
void emplace_inflight_load(O3_CPU& cpu, std::size_t slot, champsim::chrono::clock::time_point issue, uint64_t waiter_id)
{
  auto& entry = cpu.ctx().LQ.at(slot).emplace(champsim::address{0x1000}, waiter_id, champsim::address{0x404}, asid);
  entry.fetch_issued = true;
  entry.issue_time = issue;
  entry.load_waiter_id = waiter_id;
}
} // namespace

TEST_CASE("B2.1 detection is off by default and the threshold is 56 cycles")
{
  O3_CPU cpu{champsim::core_builder{}.index(0).num_contexts(1).lq_size(4)};

  CHECK_FALSE(cpu.bmt_enabled);
  CHECK(cpu.bmt_miss_threshold == 56 * cpu.clock_period);

  // An in-flight load long past the threshold is not detected while the master
  // switch is off: the gate is on bmt_enabled, independent of warmup.
  cpu.warmup = false;
  emplace_inflight_load(cpu, 0, champsim::chrono::clock::time_point{}, 1);
  cpu.current_time = champsim::chrono::clock::time_point{champsim::chrono::picoseconds{1000000}};

  cpu.detect_long_latency_loads();

  CHECK(cpu.ctx().sim_stats.bmt_long_latency_detections == 0);
  CHECK(cpu.ctx().sim_stats.bmt_detections_with_parallel_miss == 0);
}

TEST_CASE("B2.1 detection is suppressed during warmup")
{
  O3_CPU cpu{champsim::core_builder{}.index(0).num_contexts(1).lq_size(4).bmt_enabled(true).bmt_miss_threshold(10)};
  // warmup is true by default; leave it set.

  emplace_inflight_load(cpu, 0, champsim::chrono::clock::time_point{}, 1);
  cpu.current_time = champsim::chrono::clock::time_point{champsim::chrono::picoseconds{1000000}};

  cpu.detect_long_latency_loads();

  CHECK(cpu.ctx().sim_stats.bmt_long_latency_detections == 0);
}

TEST_CASE("B2.1 detection counts a load reaching the threshold, once")
{
  O3_CPU cpu{champsim::core_builder{}.index(0).num_contexts(1).lq_size(4).bmt_enabled(true).bmt_miss_threshold(10)};
  cpu.warmup = false;

  emplace_inflight_load(cpu, 0, champsim::chrono::clock::time_point{champsim::chrono::picoseconds{0}}, 1);

  // Exactly at threshold: current_time - issue_time == 10 cycles == threshold,
  // so the strict-less-than gate falls through and the load is detected.
  cpu.current_time = champsim::chrono::clock::time_point{champsim::chrono::picoseconds{10 * 250}};
  cpu.detect_long_latency_loads();

  CHECK(cpu.ctx().sim_stats.bmt_long_latency_detections == 1);
  CHECK(cpu.ctx().sim_stats.bmt_detection_rob_occupancy == 0); // empty ROB
  CHECK(cpu.ctx().sim_stats.bmt_detection_lq_occupancy == 1);  // one allocated LQ entry
  CHECK(cpu.ctx().sim_stats.bmt_detections_with_parallel_miss == 0); // alone in flight

  // Re-scanning the same cycle must not double-count: the bmt_detected latch
  // keeps the census at one.
  cpu.detect_long_latency_loads();
  CHECK(cpu.ctx().sim_stats.bmt_long_latency_detections == 1);
}

TEST_CASE("B2.1 detection stays silent below the threshold")
{
  O3_CPU cpu{champsim::core_builder{}.index(0).num_contexts(1).lq_size(4).bmt_enabled(true).bmt_miss_threshold(10)};
  cpu.warmup = false;

  emplace_inflight_load(cpu, 0, champsim::chrono::clock::time_point{champsim::chrono::picoseconds{0}}, 1);

  // One cycle short: 9 cycles elapsed < 10, so no detection.
  cpu.current_time = champsim::chrono::clock::time_point{champsim::chrono::picoseconds{9 * 250}};
  cpu.detect_long_latency_loads();

  CHECK(cpu.ctx().sim_stats.bmt_long_latency_detections == 0);
}

TEST_CASE("B2.1 detection records a parallel second miss")
{
  O3_CPU cpu{champsim::core_builder{}.index(0).num_contexts(1).lq_size(4).bmt_enabled(true).bmt_miss_threshold(10)};
  cpu.warmup = false;

  emplace_inflight_load(cpu, 0, champsim::chrono::clock::time_point{champsim::chrono::picoseconds{0}}, 1);
  emplace_inflight_load(cpu, 1, champsim::chrono::clock::time_point{champsim::chrono::picoseconds{0}}, 2);

  cpu.current_time = champsim::chrono::clock::time_point{champsim::chrono::picoseconds{10 * 250}};
  cpu.detect_long_latency_loads();

  CHECK(cpu.ctx().sim_stats.bmt_long_latency_detections == 2);
  // Each of the two loads sees the other in flight at its own detection, so
  // both detections carry the parallel-second-miss flag.
  CHECK(cpu.ctx().sim_stats.bmt_detections_with_parallel_miss == 2);
  // Each detection sampled an LQ with both entries allocated: 2 * 2.
  CHECK(cpu.ctx().sim_stats.bmt_detection_lq_occupancy == 4);
}
