#include <algorithm>
#include <catch.hpp>

#include "../../../prefetcher/sms/sms.h"
#include "cache.h"
#include "defaults.hpp"
#include "mocks.hpp"

namespace
{
constexpr std::size_t REGION_BYTES = 2048;
constexpr uint64_t TRIGGER_OFFSET = 5;
constexpr uint64_t PATTERN_OFFSET = 9;

champsim::address line_address(uint64_t region, uint64_t offset) { return champsim::address{(region * REGION_BYTES) + (offset * BLOCK_SIZE)}; }

champsim::address pc_for_region(uint64_t region) { return champsim::address{0x100000 + region}; }
} // namespace

SCENARIO("The sms prefetcher replays a trained generation into a new region")
{
  GIVEN("A cache with an sms prefetcher")
  {
    do_nothing_MRC mock_ll;
    do_nothing_MRC mock_lt;
    to_rq_MRP mock_ul{[](auto x, auto y) { return x.v_address == y.v_address; }};
    CACHE uut{champsim::cache_builder{champsim::defaults::default_l1d}
                  .name("454-uut")
                  .upper_levels({&mock_ul.queues})
                  .lower_level(&mock_ll.queues)
                  .lower_translate(&mock_lt.queues)
                  .prefetcher<sms>()};

    std::array<champsim::operable*, 4> elements{{&mock_ll, &mock_lt, &mock_ul, &uut}};

    for (auto elem : elements) {
      elem->initialize();
      elem->warmup = false;
      elem->begin_phase();
    }

    static uint64_t id = 1;

    auto issue_access = [&](champsim::address addr, champsim::address ip) {
      decltype(mock_ul)::request_type pkt;
      pkt.address = addr;
      pkt.v_address = addr;
      pkt.ip = ip;
      pkt.instr_id = id++;
      pkt.cpu = 0;
      pkt.is_translated = true;
      auto result = mock_ul.issue(pkt);
      REQUIRE(result);
      for (auto i = 0; i < 100; ++i)
        for (auto elem : elements)
          elem->_operate();
    };

    WHEN("Thirty-three regions are trained with a two-line generation")
    {
      // Each region contributes one accumulation-table entry; the insertion
      // that overflows the table evicts this region's generation into the PHT.
      for (uint64_t region = 0; region < 33; ++region) {
        issue_access(line_address(region, TRIGGER_OFFSET), pc_for_region(region));
        issue_access(line_address(region, PATTERN_OFFSET), pc_for_region(region));
      }

      THEN("No prefetches were issued during training") { REQUIRE(mock_ll.packet_count() == 66); }
    }

    WHEN("A new region triggers with a trained signature")
    {
      for (uint64_t region = 0; region < 33; ++region) {
        issue_access(line_address(region, TRIGGER_OFFSET), pc_for_region(region));
        issue_access(line_address(region, PATTERN_OFFSET), pc_for_region(region));
      }

      const auto probe_pc = pc_for_region(0);
      const auto probe_region = 40;
      issue_access(line_address(probe_region, TRIGGER_OFFSET), probe_pc);

      THEN("The rest of the trained pattern is prefetched")
      {
        const auto expected_pf = line_address(probe_region, PATTERN_OFFSET);
        REQUIRE(std::count(mock_ll.addresses.begin(), mock_ll.addresses.end(), expected_pf) == 1);
        REQUIRE(mock_ll.packet_count() == 67 + 1);
      }
    }
  }
}
