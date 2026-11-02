#include <catch.hpp>
#include <cstdint>
#include <memory>
#include <vector>

#include "champsim.h"
#include "defs.h" // champsim::defs::num_cpus
#include "instruction.h"
#include "operable.h"
#include "static_environment.h"
#include "tracereader.h"
#include "vmem.h"

namespace champsim
{
long do_cycle(std::vector<std::reference_wrapper<champsim::operable>>&, const std::vector<std::reference_wrapper<O3_CPU>>&, std::vector<tracereader>&,
              const std::vector<std::vector<std::size_t>>&, chrono::clock&);
}

namespace
{
// The one load the shared synthetic trace issues: virtual page 0x12345.
constexpr champsim::address load_vaddr{0x12345000};
}

// Stage 6.5b end-to-end multicore case. Two cores run the SAME trace -- the
// identical instruction stream (same ip, same load virtual address) -- and the
// machine must place them at non-overlapping physical addresses, because each
// trace slot is stamped with a distinct asid {slot, slot} and VirtualMemory keys
// va_to_pa by that asid.
//
// RED LINE: 负载必须「多程序、物理地址不重叠」；跨核共享物理地址的负载结果无效、不得引用。
// This test pins the SAFE half of that rule: the multi-program, non-overlapping
// physical-address mechanism (per-asid va_to_pa) holds under the real do_cycle
// feed. It does NOT -- and cannot, in a unit test -- demonstrate the DANGEROUS
// half, that a shared-address workload produces invalid results; that failure
// mode is a missing coherence protocol, not a checkable invariant here.
TEST_CASE("Two cores running the same trace land in non-overlapping physical pages", "[multicore]")
{
  if constexpr (champsim::defs::num_cpus < 2) {
    SKIP("requires num_cpus >= 2");
  }
  champsim::runtime_config cfg;
  cfg.set("dram-model=legacy");
  cfg.set("pmem.bank_rows=64");
  cfg.set("vmem.randomization=false");
  auto env = std::make_unique<champsim::static_environment>(cfg);

  // The same instruction stream for both cores: one load from virtual page
  // 0x12345. ooo_model_instr{slot, input_instr{}} stamps asid {slot, slot},
  // matching main.cc's get_tracereader positional stamping.
  auto make_trace = [](uint8_t slot) {
    return champsim::tracereader{[slot] {
      ooo_model_instr instr{slot, input_instr{}};
      instr.ip = champsim::address{0x110000};
      instr.source_memory = {load_vaddr};
      return instr;
    }};
  };

  std::vector<champsim::tracereader> traces;
  traces.push_back(make_trace(0)); // asid {0,0}
  traces.push_back(make_trace(1)); // asid {1,1}

  std::vector<std::reference_wrapper<champsim::operable>> operables; // feed-only, mirroring 505
  std::vector<std::vector<std::size_t>> trace_index{{0}, {1}};       // core0 -> trace0, core1 -> trace1
  champsim::chrono::clock clock;
  auto cpus = env->cpu_view();
  champsim::do_cycle(operables, cpus, traces, trace_index, clock);

  // Both cores received the same instruction stream, each under its own asid.
  REQUIRE(!cpus.at(0).get().contexts.at(0).input_queue.empty());
  REQUIRE(!cpus.at(1).get().contexts.at(0).input_queue.empty());
  CHECK(cpus.at(0).get().contexts.at(0).input_queue.front().asid == champsim::address_space_id{0, 0});
  CHECK(cpus.at(1).get().contexts.at(0).input_queue.front().asid == champsim::address_space_id{1, 1});

  // The red line: the same virtual page resolves to disjoint physical pages.
  auto* vmem = env->ptw_view().at(0).get().vmem; // one shared VirtualMemory for both PTWs
  const auto ppage0 = vmem->va_to_pa({0, 0}, champsim::page_number{0x12345}).first;
  const auto ppage1 = vmem->va_to_pa({1, 1}, champsim::page_number{0x12345}).first;
  CHECK(ppage0 != ppage1);
}
