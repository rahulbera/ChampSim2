#ifndef STATIC_ENVIRONMENT_H
#define STATIC_ENVIRONMENT_H

#include <string>
#include <vector>

#include "cache.h"
#include "defs.h"
#include "dram_controller.h"
#include "environment.h"
#include "ooo_cpu.h"
#include "ptw.h"
#include "runtime_config.h"
#include "vmem.h"

namespace champsim
{
/*
 * The simulated machine, written by hand rather than generated.
 *
 * The hierarchy is the standard one -- per core: L1I, L1D, L2C, ITLB, DTLB,
 * STLB, PTW; shared: LLC, DRAM -- and changing its SHAPE is a code edit here.
 * Every scalar is a runtime lookup with a champsim::defaults fallback, so a
 * run with no configuration file behaves exactly as the stock machine.
 *
 * Member declaration order is the construction order and is load-bearing:
 * channels first (everything points into them), then DRAM, vmem (holds a
 * DRAM reference), PTWs, caches, cores. Each vector is built completely
 * before any pointer into it is handed out -- reallocation would dangle them.
 */
class static_environment final : public environment
{
public:
  explicit static_environment(const runtime_config& cfg);

  std::vector<std::reference_wrapper<O3_CPU>> cpu_view() final;
  std::vector<std::reference_wrapper<CACHE>> cache_view() final;
  std::vector<std::reference_wrapper<PageTableWalker>> ptw_view() final;
  MEMORY_CONTROLLER& dram_view() final;
  std::vector<std::reference_wrapper<operable>> operable_view() final;

  // The per-core component names, which are also the runtime-configuration
  // key prefixes: cache.cpu0_l1d.sets, ooo_cpu.cpu0.rob_size, ...
  static std::string core_name(std::size_t cpu);
  static std::string cache_name(std::size_t cpu, std::string_view level);
  static std::string ptw_name(std::size_t cpu);

  // The channel count for a given core count: twelve edges per core plus the
  // ONE shared LLC -> DRAM feeder. Exposed so a test can pin the formula --
  // a per-core feeder is indistinguishable from a shared one at a single core,
  // and would leave DRAM polling a channel nothing ever writes.
  static constexpr std::size_t channel_count(std::size_t cpus) { return (cpus * 12) + 1; }

  [[nodiscard]] std::size_t channels_built() const { return std::size(channels); }

private:
  std::vector<channel> channels;
  MEMORY_CONTROLLER DRAM;
  VirtualMemory vmem;
  std::vector<PageTableWalker> ptws;
  std::vector<CACHE> caches;
  std::vector<O3_CPU> cores;
};
} // namespace champsim

#endif
