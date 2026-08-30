#include <array>
#include <catch.hpp>

#include "cache.h"
#include "defaults.hpp"
#include "mocks.hpp"

/*
 * A demand that merges into a prefetch still in flight is LATE, not useful: the
 * prefetch hid part of the miss latency, never all of it. release_MRC is what
 * makes the distinction testable -- it holds the fill until released, so the
 * merge happens with the prefetch genuinely outstanding.
 */

namespace
{
struct late_testbed {
  constexpr static uint64_t hit_latency = 4;
  champsim::address addr{0xdeadbeef};

  release_MRC mock_ll;
  to_rq_MRP mock_ul;
  CACHE uut{champsim::cache_builder{champsim::defaults::default_l1d}
                .name("433-uut")
                .upper_levels({&mock_ul.queues})
                .lower_level(&mock_ll.queues)
                .hit_latency(hit_latency)};

  std::array<champsim::operable*, 3> elements{{&mock_ll, &mock_ul, &uut}};

  late_testbed()
  {
    for (auto* elem : elements) {
      elem->initialize();
      elem->warmup = false;
      elem->begin_phase();
    }
  }

  void run(uint64_t cycles)
  {
    for (uint64_t i = 0; i < cycles; ++i) {
      for (auto* elem : elements) {
        elem->_operate();
      }
    }
  }

  void issue_demand(champsim::address a)
  {
    decltype(mock_ul)::request_type pkt;
    pkt.address = a;
    pkt.v_address = a;
    pkt.type = access_type::LOAD;
    pkt.cpu = 0;
    REQUIRE(mock_ul.issue(pkt));
  }
};
} // namespace

SCENARIO("A demand that merges into an in-flight prefetch is counted late")
{
  GIVEN("A prefetch outstanding in the MSHR")
  {
    late_testbed testbed{};

    REQUIRE(testbed.uut.prefetch_line(testbed.addr, true, 0));
    testbed.run(2 * late_testbed::hit_latency);

    REQUIRE_THAT(testbed.uut.MSHR, Catch::Matchers::SizeIs(1));
    REQUIRE(testbed.uut.sim_stats.pf_late == 0);

    WHEN("A demand for the same address arrives before the fill returns")
    {
      testbed.issue_demand(testbed.addr);
      testbed.run(2 * late_testbed::hit_latency);

      THEN("The prefetch is counted late and not useful")
      {
        REQUIRE(testbed.uut.sim_stats.pf_late == 1);
        REQUIRE(testbed.uut.sim_stats.pf_useful == 0);
      }

      AND_WHEN("The fill is released")
      {
        testbed.mock_ll.release_all();
        testbed.run(8 * late_testbed::hit_latency);

        THEN("The demand is served")
        {
          REQUIRE_THAT(testbed.uut.MSHR, Catch::Matchers::SizeIs(0));
          REQUIRE_THAT(testbed.mock_ul.packets, Catch::Matchers::SizeIs(1));
          REQUIRE(testbed.mock_ul.packets.front().return_time > 0);
        }

        // The merge promoted the fill to LOAD, so the block is not marked
        // prefetched and cannot be counted useful when a later demand hits it.
        THEN("It is not also counted useful") { REQUIRE(testbed.uut.sim_stats.pf_useful == 0); }

        AND_WHEN("The address is demanded again")
        {
          testbed.issue_demand(testbed.addr);
          testbed.run(4 * late_testbed::hit_latency);

          THEN("Neither counter moves")
          {
            REQUIRE(testbed.uut.sim_stats.pf_late == 1);
            REQUIRE(testbed.uut.sim_stats.pf_useful == 0);
          }
        }
      }
    }
  }
}

SCENARIO("A demand merging into another demand is not counted late")
{
  GIVEN("A demand outstanding in the MSHR")
  {
    late_testbed testbed{};

    testbed.issue_demand(testbed.addr);
    testbed.run(2 * late_testbed::hit_latency);

    REQUIRE_THAT(testbed.uut.MSHR, Catch::Matchers::SizeIs(1));

    WHEN("A second demand for the same address arrives")
    {
      testbed.issue_demand(testbed.addr);
      testbed.run(2 * late_testbed::hit_latency);

      THEN("Nothing is counted late") { REQUIRE(testbed.uut.sim_stats.pf_late == 0); }
    }
  }
}
