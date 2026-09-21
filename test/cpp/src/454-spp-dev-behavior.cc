#include <algorithm>
#include <array>
#include <catch.hpp>
#include <cstdint>
#include <iterator>
#include <memory>
#include <vector>

#include "../../../prefetcher/spp_dev/spp_dev.h"
#include "address.h"
#include "cache.h"
#include "defaults.hpp"
#include "mocks.hpp"

namespace
{
using delta_type = typename spp_dev::offset_type::difference_type;

// The signature tables are reached directly here. Neither read_pattern nor
// update_entry touches the bound cache, so the module needs no owner.
auto make_detached_spp_dev()
{
  auto uut = std::make_unique<spp_dev>(nullptr);
  uut->ST._parent = uut.get();
  uut->PT._parent = uut.get();
  uut->FILTER._parent = uut.get();
  uut->GHR._parent = uut.get();
  return uut;
}
} // namespace

SCENARIO("The spp_dev pattern table stops appending when the candidate queue is full")
{
  GIVEN("A signature whose every pattern table way is maximally confident")
  {
    auto uut = make_detached_spp_dev();

    constexpr uint32_t signature = 0x123;
    const auto set = spp_dev::get_hash(signature) % spp_dev::PT_SET;
    uut->PT.c_sig[set] = 1;
    for (std::size_t way = 0; way < spp_dev::PT_WAY; way++) {
      uut->PT.c_delta[set][way] = 1; // local_conf == 100 * c_delta / c_sig == 100
      uut->PT.delta[set][way] = static_cast<delta_type>(way + 1);
    }

    // A perfectly accurate history keeps the lookahead confident past depth 0, which
    // is what lets it run deeper than the queue is long.
    uut->GHR.global_accuracy = 100;

    WHEN("The lookahead reads the pattern far more times than the queue can hold")
    {
      // The production hook sizes both queues at the cache's MSHR_SIZE.
      constexpr std::size_t queue_size = 8;
      std::vector<uint32_t> confidence_q(queue_size, 0);
      std::vector<delta_type> delta_q(queue_size, 0);
      confidence_q[0] = 100;

      uint32_t lookahead_conf = 100, pf_q_tail = 0, depth = 0;
      bool within_bounds = true;
      for (int iteration = 0; iteration < 20; iteration++) {
        auto lookahead_way = static_cast<uint32_t>(spp_dev::PT_WAY);
        uut->PT.read_pattern(signature, delta_q, confidence_q, lookahead_way, lookahead_conf, pf_q_tail, depth);
        within_bounds = within_bounds && (pf_q_tail <= confidence_q.size());
      }

      THEN("The queue tail never passes the end of the queue") { REQUIRE(within_bounds); }
    }

    WHEN("The pattern table has no counts for the signature")
    {
      constexpr std::size_t queue_size = 4;
      std::vector<uint32_t> confidence_q(queue_size, 0);
      std::vector<delta_type> delta_q(queue_size, 0);
      uut->PT.c_sig[set] = 0;

      uint32_t lookahead_conf = 100, depth = 0;
      auto lookahead_way = static_cast<uint32_t>(spp_dev::PT_WAY);
      auto pf_q_tail = static_cast<uint32_t>(queue_size); // a full queue

      uut->PT.read_pattern(signature, delta_q, confidence_q, lookahead_way, lookahead_conf, pf_q_tail, depth);

      THEN("The miss is not recorded past the end of the queue") { REQUIRE(pf_q_tail == queue_size); }
    }
  }
}

SCENARIO("The spp_dev global register always finds a replacement victim")
{
  GIVEN("A global register whose entries are all maximally confident")
  {
    auto uut = make_detached_spp_dev();

    for (std::size_t i = 0; i < spp_dev::MAX_GHR_ENTRY; i++) {
      uut->GHR.valid[i] = 1;
      uut->GHR.sig[i] = static_cast<uint32_t>(i + 1);
      uut->GHR.confidence[i] = 100; // attainable whenever c_delta == c_sig
      uut->GHR.offset[i] = spp_dev::offset_type{i + 1};
      uut->GHR.delta[i] = 1;
    }

    WHEN("A prefetch with an unseen page offset crosses the page boundary")
    {
      constexpr uint32_t new_sig = 0xbeef;
      uut->GHR.update_entry(new_sig, 100, spp_dev::offset_type{0}, 2);

      THEN("It replaces one of the maximally confident entries")
      {
        REQUIRE(std::find(std::begin(uut->GHR.sig), std::end(uut->GHR.sig), new_sig) != std::end(uut->GHR.sig));
      }
    }

    WHEN("One entry is less confident than the rest")
    {
      constexpr uint32_t new_sig = 0xbeef;
      constexpr std::size_t least_confident = 5;
      uut->GHR.confidence[least_confident] = 50;

      uut->GHR.update_entry(new_sig, 100, spp_dev::offset_type{0}, 2);

      THEN("That entry is the victim, as it was before the maximally confident case was handled") { REQUIRE(uut->GHR.sig[least_confident] == new_sig); }
    }

    WHEN("A prefetch repeats a page offset the register already holds")
    {
      constexpr uint32_t new_sig = 0xbeef;
      uut->GHR.update_entry(new_sig, 100, spp_dev::offset_type{3}, 2);

      THEN("The matching entry is updated in place rather than replaced")
      {
        REQUIRE(uut->GHR.sig[2] == new_sig); // offset[2] was set to 3 above
        REQUIRE(std::count(std::begin(uut->GHR.sig), std::end(uut->GHR.sig), new_sig) == 1);
      }
    }
  }
}

SCENARIO("The spp_dev prefetcher issues prefetches for a strided stream")
{
  GIVEN("A cache with a small MSHR and the spp_dev prefetcher")
  {
    do_nothing_MRC mock_ll;
    to_rq_MRP mock_ul;
    // A small MSHR makes the candidate queues short, so the lookahead reaches the end
    // of them within a handful of accesses rather than only under a deep lookahead.
    CACHE uut{champsim::cache_builder{champsim::defaults::default_l2c}
                  .name("454-uut")
                  .mshr_size(4)
                  .upper_levels({&mock_ul.queues})
                  .lower_level(&mock_ll.queues)
                  .prefetcher<spp_dev>()};

    std::array<champsim::operable*, 3> elements{{&mock_ll, &mock_ul, &uut}};

    for (auto elem : elements) {
      elem->initialize();
      elem->warmup = false;
      elem->begin_phase();
    }

    WHEN("A long strided stream crosses several pages")
    {
      uint64_t id = 1;
      for (uint64_t block = 0; block < 512; block++) {
        decltype(mock_ul)::request_type test;
        test.address = champsim::address{champsim::block_number{champsim::address{0x1'0000}} + static_cast<int64_t>(block)};
        test.ip = champsim::address{0xcafecafe};
        test.instr_id = id++;
        test.cpu = 0;

        mock_ul.issue(test);

        for (int i = 0; i < 4; i++)
          for (auto elem : elements)
            elem->_operate();
      }

      for (int i = 0; i < 500; i++)
        for (auto elem : elements)
          elem->_operate();

      THEN("Prefetches reach the lower level") { REQUIRE(uut.sim_stats.pf_issued > 0); }
    }
  }
}
