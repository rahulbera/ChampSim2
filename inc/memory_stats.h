#ifndef MEMORY_STATS_H
#define MEMORY_STATS_H

#include <cstdint>
#include <optional>
#include <vector>

#include "dram_stats.h"
#include "ramulator2_driver.h"

namespace champsim
{
struct ramulator2_statistics {
  // Admission and latency start at the first successful native fragment.
  // A partially submitted parent is admitted even while its feeder head remains.
  uint64_t accepted_reads = 0, accepted_writes = 0;
  uint64_t completed_reads = 0, completed_writes = 0;
  uint64_t accepted_fragments = 0, completed_fragments = 0, rejected_submissions = 0;
  // Per-phase event count of PREFETCH packets popped, in warmup or measured
  // phases, because their cache block is not wholly inside native capacity.
  // Stock physical-address prefetchers (next_line, va_ampm_lite) can cross the
  // top physical frame; legacy aliases such addresses by ignoring high bits.
  // The adapter never submits them: it answers a response-requested read at
  // once and drops the rest, and they count toward no other adapter counter
  // or latency sample. Out-of-range load/RFO/write/translation requests throw.
  uint64_t out_of_range_prefetches = 0;
  // Gauges include live work from earlier phases; event counters reset each phase.
  // Carry-over completions can therefore exceed this phase's acceptances.
  uint64_t outstanding_parents = 0, outstanding_fragments = 0;
  // Complete read lifetimes in ps, including response-suppressed/carry-over reads.
  // Write callbacks indicate native command issue/coalescing, not a drained bus.
  uint64_t total_read_latency_ps = 0, read_latency_samples = 0;
  native_memory_statistics native;
};

struct memory_statistics {
  std::vector<dram_stats> sim_dram, roi_dram;
  std::optional<ramulator2_statistics> sim_ramulator2{}, roi_ramulator2{};
};
} // namespace champsim

#endif
