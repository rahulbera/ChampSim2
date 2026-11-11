#include <array>
#include <catch.hpp>
#include <vector>

#include "defaults.hpp"
#include "dram_controller.h"
#include "instruction.h"
#include "mocks.hpp"
#include "ptw.h"
#include "runtime_config.h"
#include "static_environment.h"
#include "vmem.h"

// A page table walker's CR3 is the page-table root of the address space it is
// built for, read from the virtual memory keyed by asid. Two walkers in two
// address spaces must not share a root, and the environment must build each
// core's walker in the space that core's trace stamps on its requests.

TEST_CASE("Page table walkers built for different address spaces start at different roots")
{
  MEMORY_CONTROLLER dram{champsim::chrono::picoseconds{3200},
                         champsim::chrono::picoseconds{6400},
                         std::size_t{18},
                         std::size_t{18},
                         std::size_t{18},
                         std::size_t{38},
                         champsim::chrono::microseconds{64000},
                         {},
                         64,
                         64,
                         1,
                         champsim::data::bytes{8},
                         1024,
                         1024,
                         4,
                         4,
                         4,
                         8192};
  VirtualMemory vmem{champsim::data::bytes{1 << 12}, 5, champsim::chrono::nanoseconds{640}, dram};

  PageTableWalker ptw0{champsim::ptw_builder{champsim::defaults::default_ptw}.name("606-a0").virtual_memory(&vmem).asid({0, 0})};
  PageTableWalker ptw1{champsim::ptw_builder{champsim::defaults::default_ptw}.name("606-a1").virtual_memory(&vmem).asid({1, 1})};

  REQUIRE(ptw0.CR3_addr != ptw1.CR3_addr);
  REQUIRE(ptw0.CR3_addr == vmem.get_pte_pa({0, 0}, champsim::page_number{}, vmem.pt_levels).first);
  REQUIRE(ptw1.CR3_addr == vmem.get_pte_pa({1, 1}, champsim::page_number{}, vmem.pt_levels).first);
}

TEST_CASE("A walker shared by two address spaces walks each from its own root")
{
  MEMORY_CONTROLLER dram{champsim::chrono::picoseconds{3200},
                         champsim::chrono::picoseconds{6400},
                         std::size_t{18},
                         std::size_t{18},
                         std::size_t{18},
                         std::size_t{38},
                         champsim::chrono::microseconds{64000},
                         {},
                         64,
                         64,
                         1,
                         champsim::data::bytes{8},
                         1024,
                         1024,
                         4,
                         4,
                         4,
                         8192};
  VirtualMemory vmem{champsim::data::bytes{1 << 12}, 5, champsim::chrono::nanoseconds{640}, dram};
  do_nothing_MRC mock_ll;
  to_rq_MRP mock_ul;
  PageTableWalker uut{champsim::ptw_builder{champsim::defaults::default_ptw}
                          .name("606-b-uut")
                          .clock_period(champsim::chrono::picoseconds{3200})
                          .upper_levels({&mock_ul.queues})
                          .lower_level(&mock_ll.queues)
                          .virtual_memory(&vmem)
                          .asid({0, 0})};

  std::array<champsim::operable*, 3> elements{{&mock_ul, &uut, &mock_ll}};
  uut.warmup = false;
  uut.begin_phase();

  auto walk = [&](uint8_t a) {
    decltype(mock_ul)::request_type req;
    req.address = champsim::address{0xdeadbeef};
    req.v_address = req.address;
    req.cpu = 0;
    req.asid[0] = a;
    req.asid[1] = a;
    REQUIRE(mock_ul.issue(req));
    for (auto i = 0; i < 10000; ++i)
      for (auto elem : elements)
        elem->_operate();
  };

  walk(0);
  REQUIRE(mock_ll.packet_count() == 5);
  REQUIRE(champsim::page_number{mock_ll.addresses.at(0)} == champsim::page_number{uut.CR3_addr});

  walk(1);
  REQUIRE(mock_ll.packet_count() == 10);
  REQUIRE(champsim::page_number{mock_ll.addresses.at(5)} == champsim::page_number{vmem.get_pte_pa({1, 1}, champsim::page_number{}, vmem.pt_levels).first});
  REQUIRE(champsim::page_number{mock_ll.addresses.at(5)} != champsim::page_number{uut.CR3_addr});

  walk(0);
  REQUIRE(mock_ll.packet_count() == 12);
}

TEST_CASE("Each core's page table walker starts at the root of the address space its core's requests carry")
{
  champsim::runtime_config cfg{};
  champsim::static_environment env{cfg};
  auto ptws = env.ptw_view();

  REQUIRE(std::size(ptws) == champsim::defs::num_cpus);

  std::vector<champsim::address> roots{};
  roots.reserve(std::size(ptws));

  for (std::size_t cpu = 0; cpu < std::size(ptws); ++cpu) {
    INFO("cpu " << cpu);
    const auto& ptw = ptws.at(cpu).get();
    auto expected = ptw.vmem->get_pte_pa(ooo_model_instr{static_cast<uint8_t>(cpu), input_instr{}}.asid, champsim::page_number{}, ptw.vmem->pt_levels).first;
    REQUIRE(ptw.CR3_addr == expected);
    roots.push_back(ptw.CR3_addr);
  }

  // No two cores may share a page-table root. At N=1 this is vacuous, but the
  // per-cpu stamping that makes it true at every core count is what the fix
  // pins.
  for (std::size_t i = 0; i < std::size(roots); ++i) {
    for (std::size_t j = i + 1; j < std::size(roots); ++j) {
      REQUIRE(roots[i] != roots[j]);
    }
  }
}

TEST_CASE("A core's walker is built in the address space of that core's first context")
{
  champsim::runtime_config cfg{};
  cfg.set("ooo_cpu.cpu0.num_contexts=2");
  champsim::static_environment env{cfg};
  auto ptws = env.ptw_view();

  const auto& ptw = ptws.at(0).get();
  std::size_t first_slot = 0;
  const auto slot = static_cast<uint8_t>(first_slot);
  first_slot += 2; // cpu0's two contexts advance the slot past its first context

  REQUIRE(ptw.CR3_asid == ooo_model_instr{slot, input_instr{}}.asid);
  REQUIRE(ptw.CR3_addr == ptw.vmem->get_pte_pa(ptw.CR3_asid, champsim::page_number{}, ptw.vmem->pt_levels).first);
}
