#include <catch.hpp>
#include <memory>

#include "defs.h" // champsim::defs::num_cpus
#include "static_environment.h"
#include "vmem.h"

TEST_CASE("Multicore detailed PTW separates per-ASID pages and shares only the CR3 root", "[ptw-multicore]")
{
  if constexpr (champsim::defs::num_cpus < 2) {
    SKIP("requires num_cpus >= 2");
  }
  // 6.5a verdict on the third 6.4 debt: under two-core DETAILED mode the two
  // PTWs do NOT displace each other's PTE/data pages -- those are keyed by the
  // REQUEST ASID ({0,0} vs {1,1}) -- but they DO share one CR3 root, because
  // static_environment.cc:291 builds every PTW with .asid({0,0}).
  champsim::runtime_config cfg;
  cfg.set("dram-model=legacy");
  cfg.set("pmem.bank_rows=64");
  cfg.set("vmem.randomization=false");
  auto env = std::make_unique<champsim::static_environment>(cfg);

  auto& ptw0 = env->ptw_view().at(0).get();
  auto& ptw1 = env->ptw_view().at(1).get();
  auto* vmem = ptw0.vmem; // one shared VirtualMemory for both PTWs

  // (1) The CR3 root is SHARED -- the .asid({0,0}) construction smell, pinned
  // as a sentinel: a future per-ASID root (.asid({cpu,cpu})) will fail this.
  CHECK(ptw0.CR3_addr == ptw1.CR3_addr);

  // (2) Data pages do NOT displace: same vaddr, different ASID -> different ppage.
  const auto p0 = vmem->va_to_pa({0, 0}, champsim::page_number{0x12345}).first;
  const auto p1 = vmem->va_to_pa({1, 1}, champsim::page_number{0x12345}).first;
  CHECK(p0 != p1);

  // (3) Walk PTE pages do NOT displace: same vaddr/level, different ASID -> different PTE entry.
  const auto t0 = vmem->get_pte_pa({0, 0}, champsim::page_number{0x12345}, 3).first;
  const auto t1 = vmem->get_pte_pa({1, 1}, champsim::page_number{0x12345}, 3).first;
  CHECK(t0 != t1);
}
