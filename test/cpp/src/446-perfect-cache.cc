#include <catch.hpp>

#include "cache.h"
#include "champsim.h"
#include "defaults.hpp"
#include "mocks.hpp"

// A perfect cache hits on EVERY lookup, including the first access to a block
// that was never filled, so that a run can measure headroom: "what is the
// performance if every request hits here?"
//
// A perfect cache reporting a 100% hit rate proves nothing on its own -- it
// follows structurally, exactly as a "perfect predictor" reporting 0 MPKI does.
// The load-bearing checks are therefore that the level BELOW goes silent and
// that the same access on a NON-perfect cache misses; the control scenario
// below is what makes the rest of this file decisive.

SCENARIO("A perfect cache hits on a block it has never filled")
{
  using namespace std::literals;
  auto [type, str] = GENERATE(table<access_type, std::string_view>({std::pair{access_type::LOAD, "load"sv}, std::pair{access_type::RFO, "RFO"sv},
                                                                    std::pair{access_type::PREFETCH, "prefetch"sv}, std::pair{access_type::WRITE, "write"sv},
                                                                    std::pair{access_type::TRANSLATION, "translation"sv}}));

  GIVEN("An empty perfect cache")
  {
    constexpr auto hit_latency = 7;
    do_nothing_MRC mock_ll;
    to_rq_MRP mock_ul;
    CACHE uut{champsim::cache_builder{champsim::defaults::default_l1d}
                  .name("446-perfect-" + std::string(str))
                  .upper_levels({&mock_ul.queues})
                  .lower_level(&mock_ll.queues)
                  .hit_latency(hit_latency)
                  .set_perfect()
                  .prefetch_activate(access_type::LOAD, access_type::RFO, access_type::PREFETCH, access_type::WRITE, access_type::TRANSLATION)};

    std::array<champsim::operable*, 3> elements{{&uut, &mock_ll, &mock_ul}};

    for (auto elem : elements) {
      elem->initialize();
      elem->warmup = false;
      elem->begin_phase();
    }

    WHEN("A " + std::string{str} + " packet is issued to an address that was never filled")
    {
      static uint64_t id = 1;
      decltype(mock_ul)::request_type test;
      test.address = champsim::address{0xfeedface};
      test.is_translated = true;
      test.instr_id = id++;
      test.cpu = 0;
      test.type = type;

      const auto initial_hits = uut.sim_stats.hits.value_or(std::pair{test.type, test.cpu}, 0);
      const auto initial_misses = uut.sim_stats.misses.value_or(std::pair{test.type, test.cpu}, 0);

      auto test_result = mock_ul.issue(test);
      THEN("This issue is received") { REQUIRE(test_result); }

      for (uint64_t i = 0; i < 2 * hit_latency; ++i)
        for (auto elem : elements)
          elem->_operate();

      THEN("It returns after exactly the hit latency")
      {
        REQUIRE_THAT(mock_ul.packets, Catch::Matchers::SizeIs(1));
        REQUIRE_THAT(mock_ul.packets.back(), champsim::test::ReturnedMatcher(hit_latency, 1));
      }

      THEN("It counts as a hit, not a miss")
      {
        CHECK(uut.sim_stats.hits.value_or(std::pair{test.type, test.cpu}, 0) == initial_hits + 1);
        CHECK(uut.sim_stats.misses.value_or(std::pair{test.type, test.cpu}, 0) == initial_misses);
      }

      THEN("Nothing is sent to the lower level") { CHECK(mock_ll.packet_count() == 0); }

      THEN("No MSHR is occupied") { CHECK(uut.get_mshr_occupancy() == 0); }
    }
  }
}

SCENARIO("A cache that is not perfect misses on a block it has never filled")
{
  // The control for the scenario above: without perfect, the identical access
  // misses and reaches the lower level. Without this, the test would pass even
  // if the flag did nothing.
  GIVEN("An empty cache that is not perfect")
  {
    constexpr auto hit_latency = 7;
    do_nothing_MRC mock_ll;
    to_rq_MRP mock_ul;
    CACHE uut{champsim::cache_builder{champsim::defaults::default_l1d}
                  .name("446-not-perfect")
                  .upper_levels({&mock_ul.queues})
                  .lower_level(&mock_ll.queues)
                  .hit_latency(hit_latency)
                  .reset_perfect()};

    std::array<champsim::operable*, 3> elements{{&uut, &mock_ll, &mock_ul}};

    for (auto elem : elements) {
      elem->initialize();
      elem->warmup = false;
      elem->begin_phase();
    }

    WHEN("A packet is issued to an address that was never filled")
    {
      decltype(mock_ul)::request_type test;
      test.address = champsim::address{0xfeedface};
      test.is_translated = true;
      test.instr_id = 1;
      test.cpu = 0;
      test.type = access_type::LOAD;

      const auto initial_misses = uut.sim_stats.misses.value_or(std::pair{test.type, test.cpu}, 0);

      REQUIRE(mock_ul.issue(test));

      for (uint64_t i = 0; i < 2 * hit_latency; ++i)
        for (auto elem : elements)
          elem->_operate();

      THEN("It counts as a miss") { CHECK(uut.sim_stats.misses.value_or(std::pair{test.type, test.cpu}, 0) == initial_misses + 1); }

      THEN("The request reaches the lower level") { CHECK(mock_ll.packet_count() == 1); }
    }
  }
}

SCENARIO("A perfect cache never sends a request down, however many distinct blocks it sees")
{
  GIVEN("An empty perfect cache")
  {
    constexpr auto hit_latency = 2;
    constexpr long block_count = 64; // deliberately more blocks than the cache has ways
    do_nothing_MRC mock_ll;
    to_rq_MRP mock_ul;
    CACHE uut{champsim::cache_builder{champsim::defaults::default_l1d}
                  .name("446-perfect-many")
                  .upper_levels({&mock_ul.queues})
                  .lower_level(&mock_ll.queues)
                  .hit_latency(hit_latency)
                  .sets(8)
                  .ways(2)
                  .set_perfect()};

    std::array<champsim::operable*, 3> elements{{&uut, &mock_ll, &mock_ul}};

    for (auto elem : elements) {
      elem->initialize();
      elem->warmup = false;
      elem->begin_phase();
    }

    WHEN("Many distinct blocks are accessed")
    {
      for (long i = 0; i < block_count; ++i) {
        decltype(mock_ul)::request_type test;
        test.address = champsim::address{0x1000 + (static_cast<uint64_t>(i) * BLOCK_SIZE)};
        test.is_translated = true;
        test.instr_id = static_cast<uint64_t>(i) + 1;
        test.cpu = 0;
        test.type = access_type::LOAD;
        REQUIRE(mock_ul.issue(test));

        for (auto elem : elements)
          elem->_operate();
      }

      for (uint64_t i = 0; i < 100; ++i)
        for (auto elem : elements)
          elem->_operate();

      THEN("Every one of them hit") { CHECK(uut.sim_stats.hits.value_or(std::pair{access_type::LOAD, 0u}, 0) == block_count); }

      THEN("None of them missed") { CHECK(uut.sim_stats.misses.value_or(std::pair{access_type::LOAD, 0u}, 0) == 0); }

      THEN("The lower level stayed silent") { CHECK(mock_ll.packet_count() == 0); }
    }
  }
}

SCENARIO("A perfect cache returns the data the request carried")
{
  // A block that was never filled has no stored data, so the response echoes
  // the request's own value -- which for a v2 trace is what that operand held.
  GIVEN("An empty perfect cache")
  {
    constexpr auto hit_latency = 3;
    const champsim::address payload{0xcafef00d};

    champsim::address returned_data{};
    do_nothing_MRC mock_ll;
    to_rq_MRP mock_ul{[&](auto req, auto rsp) {
      if (req.address == rsp.address) {
        returned_data = rsp.data;
        return true;
      }
      return false;
    }};

    CACHE uut{champsim::cache_builder{champsim::defaults::default_l1d}
                  .name("446-perfect-data")
                  .upper_levels({&mock_ul.queues})
                  .lower_level(&mock_ll.queues)
                  .hit_latency(hit_latency)
                  .set_perfect()};

    std::array<champsim::operable*, 3> elements{{&uut, &mock_ll, &mock_ul}};

    for (auto elem : elements) {
      elem->initialize();
      elem->warmup = false;
      elem->begin_phase();
    }

    WHEN("A packet carrying a data value is issued")
    {
      decltype(mock_ul)::request_type test;
      test.address = champsim::address{0xdeadbeef};
      test.data = payload;
      test.is_translated = true;
      test.instr_id = 1;
      test.cpu = 0;
      test.type = access_type::LOAD;

      REQUIRE(mock_ul.issue(test));

      for (uint64_t i = 0; i < 4 * hit_latency; ++i)
        for (auto elem : elements)
          elem->_operate();

      THEN("The response carries that same value") { CHECK(returned_data == payload); }
    }
  }
}
