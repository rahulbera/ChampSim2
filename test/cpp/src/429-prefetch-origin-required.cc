#include <algorithm>
#include <catch.hpp>
#include <cstddef>
#include <string>
#include <string_view>

#include "../../../prefetcher/ip_stride/ip_stride.h"
#include "../../../prefetcher/next_line/next_line.h"
#include "../../../prefetcher/no/no.h"
#include "../../../prefetcher/spp_dev/spp_dev.h"
#include "../../../prefetcher/va_ampm_lite/va_ampm_lite.h"

#include "cache.h"
#include "defaults.hpp"
#include "runtime_config.h"
#include "static_environment.h"

// A cache that translates its prefetches reads the address space a prefetch is
// issued in: one configured with virtual_prefetch (L1I's default), or an
// asid-keyed TLB (any cache given virtual_memory). At more than one hardware
// context the origin-less entry points -- the 3-argument prefetch_line and the
// 6-argument impl_prefetcher_cache_operate -- cannot know which context's space
// to translate in, so the cache refuses them rather than walk another context's
// page table. A prefetcher whose prefetcher_cache_operate takes no asid and cpu
// is refused the same way at configuration time.
namespace
{
// A prefetcher that implements only the 6-argument prefetcher_cache_operate: no
// asid, no cpu, so on a translating multi-context cache it has no address space
// to issue in.
struct origin_less_prefetcher : champsim::modules::prefetcher {
  using prefetcher::prefetcher;
  uint32_t prefetcher_cache_operate(champsim::address, champsim::address, bool, bool, access_type, uint32_t metadata_in) { return metadata_in; }
};

template <typename Prefetcher>
bool operates_without_origin()
{
  CACHE cache{champsim::cache_builder{champsim::defaults::default_l1i}.name("cpu0_L1I").prefetcher<Prefetcher>()};
  return cache.pref_module_pimpl->impl_operates_without_origin();
}
} // namespace

TEST_CASE("Only an origin-less prefetcher reports operating without an origin")
{
  CHECK(::operates_without_origin<::origin_less_prefetcher>());
  CHECK_FALSE(::operates_without_origin<no>());
  CHECK_FALSE(::operates_without_origin<next_line>());
  CHECK_FALSE(::operates_without_origin<ip_stride>());
  CHECK_FALSE(::operates_without_origin<spp_dev>());
  CHECK_FALSE(::operates_without_origin<va_ampm_lite>());
}

TEST_CASE("check_prefetch_origin refuses an origin-less prefetcher on a translating cache with two contexts")
{
  // Two contexts, virtual prefetch: the refusal fires.
  CACHE virtual_two{champsim::cache_builder{champsim::defaults::default_l1i}.name("cpu0_L1I").virtual_prefetch(true).prefetcher<origin_less_prefetcher>()};
  virtual_two.contexts_per_core = 2;
  REQUIRE_THROWS_WITH(virtual_two.check_prefetch_origin("cache.cpu0_l1i.prefetcher", "origin_less_prefetcher"),
                      Catch::Matchers::StartsWith("runtime config: cache.cpu0_l1i.prefetcher = origin_less_prefetcher: ") &&
                          Catch::Matchers::ContainsSubstring("2 hardware contexts"));

  // One context: the origin-less prefetcher is unambiguous, so it passes.
  CACHE virtual_one{champsim::cache_builder{champsim::defaults::default_l1i}.name("cpu0_L1I").virtual_prefetch(true).prefetcher<origin_less_prefetcher>()};
  virtual_one.contexts_per_core = 1;
  CHECK_NOTHROW(virtual_one.check_prefetch_origin("cache.cpu0_l1i.prefetcher", "origin_less_prefetcher"));

  // Physical (non-translating): no refusal even at two contexts.
  CACHE physical_two{champsim::cache_builder{champsim::defaults::default_l1i}.name("cpu0_L1I").virtual_prefetch(false).prefetcher<origin_less_prefetcher>()};
  physical_two.contexts_per_core = 2;
  CHECK_NOTHROW(physical_two.check_prefetch_origin("cache.cpu0_l1i.prefetcher", "origin_less_prefetcher"));

  // A prefetcher that carries its origin is fine at two contexts.
  CACHE with_origin{champsim::cache_builder{champsim::defaults::default_l1i}.name("cpu0_L1I").virtual_prefetch(true).prefetcher<next_line>()};
  with_origin.contexts_per_core = 2;
  CHECK_NOTHROW(with_origin.check_prefetch_origin("cache.cpu0_l1i.prefetcher", "next_line"));
}

TEST_CASE("The origin-less prefetch_line refuses on a translating cache with two contexts")
{
  const auto pf_addr = champsim::address{0xbeef000};

  // Two contexts: the origin-less form has no address space to name.
  CACHE virtual_two{champsim::cache_builder{champsim::defaults::default_l1i}.name("cpu0_L1I").virtual_prefetch(true).prefetcher<next_line>()};
  virtual_two.contexts_per_core = 2;
  REQUIRE_THROWS_WITH(virtual_two.prefetch_line(pf_addr, true, 0),
                      Catch::Matchers::StartsWith("cpu0_L1I: prefetch_line(pf_addr, fill_this_level, prefetch_metadata) names no address space"));

  // One context: the origin-less form stamps {cpu,cpu} and succeeds.
  CACHE virtual_one{champsim::cache_builder{champsim::defaults::default_l1i}.name("cpu0_L1I").virtual_prefetch(true).prefetcher<next_line>()};
  virtual_one.contexts_per_core = 1;
  CHECK(virtual_one.prefetch_line(pf_addr, true, 0));

  // The explicit 5-argument form always succeeds, even at two contexts.
  CHECK(virtual_two.prefetch_line(pf_addr, true, 0, champsim::address_space_id{0, 0}, 0));

  // A physical cache does not translate, so the origin-less form succeeds at two contexts.
  CACHE physical_two{champsim::cache_builder{champsim::defaults::default_l1i}.name("cpu0_L1I").virtual_prefetch(false).prefetcher<next_line>()};
  physical_two.contexts_per_core = 2;
  CHECK(physical_two.prefetch_line(pf_addr, true, 0));
}

TEST_CASE("The environment tells each cache its core's context count, and the LLC the maximum")
{
  std::size_t num_contexts = GENERATE(1, 2);
  CAPTURE(num_contexts);

  champsim::runtime_config cfg{};
  cfg.set("cache.cpu0_l1i.prefetcher=next_line");
  cfg.set("ooo_cpu.cpu0.num_contexts=" + std::to_string(num_contexts));
  champsim::static_environment env{cfg};

  const auto caches = env.cache_view();
  const auto find = [&caches](std::string_view name) -> const CACHE& {
    return *std::find_if(std::begin(caches), std::end(caches), [name](const CACHE& c) { return c.NAME == name; });
  };

  CHECK(find("LLC").contexts_per_core == num_contexts);
  for (const auto* level : {"DTLB", "ITLB", "L1D", "L1I", "L2C", "STLB"}) {
    CAPTURE(level);
    CHECK(find(champsim::static_environment::cache_name(0, level)).contexts_per_core == num_contexts);
  }
  for (std::size_t cpu = 1; cpu < champsim::defs::num_cpus; ++cpu) {
    for (const auto* level : {"DTLB", "ITLB", "L1D", "L1I", "L2C", "STLB"}) {
      CAPTURE(cpu, level);
      CHECK(find(champsim::static_environment::cache_name(cpu, level)).contexts_per_core == 1);
    }
  }

  // Only a translating cache refuses an origin-less prefetch: virtual prefetch
  // (L1I) or an asid-keyed TLB (ITLB, DTLB, STLB). Physical caches and the LLC
  // stay exempt.
  for (const auto* level : {"L1I", "ITLB", "DTLB", "STLB"}) {
    CAPTURE(level);
    CHECK(find(champsim::static_environment::cache_name(0, level)).prefetch_origin_required() == (num_contexts > 1));
  }
  for (const auto* level : {"L1D", "L2C"}) {
    CAPTURE(level);
    CHECK_FALSE(find(champsim::static_environment::cache_name(0, level)).prefetch_origin_required());
  }
  CHECK_FALSE(find("LLC").prefetch_origin_required());
}
