#ifndef MEMORY_STATS_H
#define MEMORY_STATS_H

#include <vector>

#include "dram_stats.h"

namespace champsim
{
struct memory_statistics {
  std::vector<dram_stats> sim_dram, roi_dram;
};
} // namespace champsim

#endif
