#ifndef CORE_STATS_H
#define CORE_STATS_H

#include <cstdint>
#include <string>

#include "event_counter.h"
#include "instruction.h"

struct cpu_stats {
  std::string name;
  long long begin_instrs = 0;
  long long begin_cycles = 0;
  long long end_instrs = 0;
  long long end_cycles = 0;
  uint64_t total_rob_occupancy_at_branch_mispredict = 0;

  // Cycles fetch spent frozen after a misprediction, i.e. cycles that produced
  // no useful work. ChampSim does not fetch a wrong path, so this is the
  // equivalent of CBP2025's CycWP: the interval between detecting the
  // misprediction and fetch restarting, penalty included.
  uint64_t cycles_on_wrong_path = 0;

  champsim::stats::event_counter<branch_type> total_branch_types = {};
  champsim::stats::event_counter<branch_type> branch_type_misses = {};

  [[nodiscard]] auto instrs() const { return end_instrs - begin_instrs; }
  [[nodiscard]] auto cycles() const { return end_cycles - begin_cycles; }
};

cpu_stats operator-(cpu_stats lhs, cpu_stats rhs);

#endif
