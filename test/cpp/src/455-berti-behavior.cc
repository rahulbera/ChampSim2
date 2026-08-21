#include <algorithm>
#include <catch.hpp>

#include "../../../prefetcher/berti/berti.h"
#include "cache.h"
#include "defaults.hpp"
#include "mocks.hpp"

namespace
{
constexpr uint64_t PAGE_BYTES = 4096; // this build's compile-time page size

champsim::address line_address(uint64_t page, uint64_t offset) { return champsim::address{(page * PAGE_BYTES) + (offset * BLOCK_SIZE)}; }
} // namespace

SCENARIO("The berti prefetcher learns a stride from demand fills and replays it")
{
  GIVEN("A cache with a berti prefetcher")
  {
    do_nothing_MRC mock_ll;
    do_nothing_MRC mock_lt;
    to_rq_MRP mock_ul{[](auto x, auto y) { return x.v_address == y.v_address; }};
    CACHE uut{champsim::cache_builder{champsim::defaults::default_l1d}
                  .name("455-uut")
                  .upper_levels({&mock_ul.queues})
                  .lower_level(&mock_ll.queues)
                  .lower_translate(&mock_lt.queues)
                  .prefetcher<berti>()};

    std::array<champsim::operable*, 4> elements{{&mock_ll, &mock_lt, &mock_ul, &uut}};

    for (auto elem : elements) {
      elem->initialize();
      elem->warmup = false;
      elem->begin_phase();
    }

    static uint64_t id = 1;
    const auto test_ip = champsim::address{0xcafecafe};

    // Issues one demand and lets it drain through a fill, so the gap between
    // successive demands exceeds the fill latency -- the condition under
    // which berti correlates two accesses into a stride.
    auto issue_demand = [&](champsim::address addr) {
      decltype(mock_ul)::request_type pkt;
      pkt.address = addr;
      pkt.v_address = addr;
      pkt.ip = test_ip;
      pkt.instr_id = id++;
      pkt.cpu = 0;
      pkt.is_translated = true;
      auto result = mock_ul.issue(pkt);
      REQUIRE(result);
      for (auto i = 0; i < 500; ++i)
        for (auto elem : elements)
          elem->_operate();
    };

    WHEN("Two accesses of the same stride train the current page, then a third arrives")
    {
      const uint64_t page = 3;
      issue_demand(line_address(page, 10));
      issue_demand(line_address(page, 14));
      issue_demand(line_address(page, 11));
      issue_demand(line_address(page, 15));

      THEN("No prefetches were issued during training") { REQUIRE(mock_ll.packet_count() == 4); }

      issue_demand(line_address(page, 19));

      THEN("Exactly one prefetch of the trained successor line is issued")
      {
        const auto expected_pf = line_address(page, 19 + 4);
        REQUIRE(std::count(mock_ll.addresses.begin(), mock_ll.addresses.end(), expected_pf) == 1);
        REQUIRE(mock_ll.packet_count() == 6);
      }
    }

    WHEN("The learned stride would carry the prefetch past the page boundary")
    {
      const uint64_t page = 5;
      issue_demand(line_address(page, 10));
      issue_demand(line_address(page, 50));
      issue_demand(line_address(page, 11));
      issue_demand(line_address(page, 51));
      issue_demand(line_address(page, 52));

      THEN("The cross-page prefetch is suppressed")
      {
        const auto crossed_pf = line_address(page + 1, 28);
        CHECK(std::count(mock_ll.addresses.begin(), mock_ll.addresses.end(), crossed_pf) == 0);
        REQUIRE(mock_ll.packet_count() == 5);
      }
    }
  }
}
