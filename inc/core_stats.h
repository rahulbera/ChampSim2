#ifndef CORE_STATS_H
#define CORE_STATS_H

#include <cstddef>
#include <cstdint>
#include <string>

#include "event_counter.h"
#include "instruction.h"

// Counters are charged to the hardware context that caused them and sum to the
// core totals, except cycles_on_wrong_path (context-cycles); begin_cycles and
// end_cycles are the core's phase window and are never summed.
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

  // Decoded-instruction-buffer accesses. One lookup is charged per instruction
  // that reaches the fetch stage, so the two counters sum to one lookup per
  // fetched instruction; the total is derived rather than counted separately
  // and so cannot drift from its parts.
  //
  // A hit means this instruction's window was DECODED earlier and is still
  // resident -- NOT merely that a neighbour was fetched alongside it. The DIB
  // is filled at decode (`do_dib_update`), long after `check_dib` has already
  // classified the whole fetch group, so a cold group misses in its entirety:
  // four instructions sharing a 16-byte window are four misses on their first
  // pass, and hit only when that code runs again. `dib_misses` therefore counts
  // instructions, not distinct windows, and is not a code-footprint proxy.
  uint64_t dib_hits = 0;
  uint64_t dib_misses = 0;

  [[nodiscard]] auto instrs() const { return end_instrs - begin_instrs; }
  [[nodiscard]] auto cycles() const { return end_cycles - begin_cycles; }
  [[nodiscard]] auto dib_lookups() const { return dib_hits + dib_misses; }
};

cpu_stats operator-(cpu_stats lhs, cpu_stats rhs);

namespace champsim
{
// Names one (core, context) slot in the statistics document: its table keys and
// the human label. One hardware context keeps the old flat name (`cpu<k>`,
// `CPU <k>`); several nest every context, context 0 included (`cpu<k>.ctx<c>`,
// `CPU <k> ctx <c>`), so `cpu<k>` is always a core.
struct context_slot {
  uint32_t cpu = 0;
  std::size_t context = 0;
  std::size_t contexts = 1; // hardware contexts on this core
  [[nodiscard]] std::string core_key() const { return "cpu" + std::to_string(cpu); }
  [[nodiscard]] std::string context_key() const { return "ctx" + std::to_string(context); }
  [[nodiscard]] std::string path() const { return contexts > 1 ? core_key() + "." + context_key() : core_key(); }
  [[nodiscard]] std::string label() const { return "CPU " + std::to_string(cpu) + (contexts > 1 ? " ctx " + std::to_string(context) : std::string{}); }
};
} // namespace champsim

#endif
