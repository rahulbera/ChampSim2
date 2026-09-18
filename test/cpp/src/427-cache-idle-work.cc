#include <catch.hpp>

#include "cache.h"
#include "defaults.hpp"
#include "mocks.hpp"

namespace
{
std::map<CACHE*, long> cycle_calls;

struct cycle_counter : champsim::modules::prefetcher {
  using prefetcher::prefetcher;

  void prefetcher_cycle_operate() { ++cycle_calls.at(intern_); }
};

template <typename MRP>
typename MRP::request_type request(champsim::address address)
{
  typename MRP::request_type result;
  result.address = address;
  result.cpu = 0;
  result.response_requested = true;
  return result;
}
} // namespace

TEST_CASE("Idle cache ticks retain upstream rotation and prefetcher cycle hooks")
{
  do_nothing_MRC mock_ll;
  to_rq_MRP first_ul;
  to_rq_MRP second_ul;
  CACHE uut{champsim::cache_builder{champsim::defaults::default_l1d}
                .name("427a-uut")
                .upper_levels({&first_ul.queues, &second_ul.queues})
                .lower_level(&mock_ll.queues)
                .tag_bandwidth(champsim::bandwidth::maximum_type{1})
                .prefetcher<cycle_counter>()};

  std::array<champsim::operable*, 4> elements{{&uut, &mock_ll, &first_ul, &second_ul}};
  for (auto* element : elements) {
    element->initialize();
    element->warmup = false;
    element->begin_phase();
  }
  cycle_calls.insert_or_assign(&uut, 0);

  uut._operate();
  REQUIRE(cycle_calls.at(&uut) == 1);

  REQUIRE(first_ul.issue(request<to_rq_MRP>(champsim::address{0x1000})));
  REQUIRE(second_ul.issue(request<to_rq_MRP>(champsim::address{0x2000})));
  uut._operate();

  REQUIRE(cycle_calls.at(&uut) == 2);
  REQUIRE(first_ul.queues.RQ.empty());
  REQUIRE(second_ul.queues.RQ.size() == 1);
}

TEST_CASE("Zero tag bandwidth holds admitted and queued work while cache cycle hooks continue")
{
  do_nothing_MRC mock_ll;
  to_rq_MRP mock_ul;
  CACHE uut{champsim::cache_builder{champsim::defaults::default_l1d}
                .name("427b-uut")
                .upper_levels({&mock_ul.queues})
                .lower_level(&mock_ll.queues)
                .hit_latency(2)
                .tag_bandwidth(champsim::bandwidth::maximum_type{1})
                .prefetcher<cycle_counter>()};

  std::array<champsim::operable*, 3> elements{{&uut, &mock_ll, &mock_ul}};
  for (auto* element : elements) {
    element->initialize();
    element->warmup = false;
    element->begin_phase();
  }
  cycle_calls.insert_or_assign(&uut, 0);

  uut.MAX_TAG = champsim::bandwidth::maximum_type{0};
  REQUIRE(mock_ul.issue(request<to_rq_MRP>(champsim::address{0x3000})));
  uut._operate();
  uut._operate();
  REQUIRE(mock_ul.queues.RQ.size() == 1);
  REQUIRE(cycle_calls.at(&uut) == 2);

  uut.MAX_TAG = champsim::bandwidth::maximum_type{1};
  uut._operate();
  REQUIRE(mock_ul.queues.RQ.empty());

  uut.MAX_TAG = champsim::bandwidth::maximum_type{0};
  for (int i = 0; i < 4; ++i) {
    uut._operate();
    mock_ll._operate();
  }
  REQUIRE(mock_ll.packet_count() == 0);
  REQUIRE(cycle_calls.at(&uut) == 7);

  uut.MAX_TAG = champsim::bandwidth::maximum_type{1};
  uut._operate();
  mock_ll._operate();
  REQUIRE(mock_ll.packet_count() == 1);
  REQUIRE(cycle_calls.at(&uut) == 8);
}

TEST_CASE("Zero fill bandwidth holds ready fills until bandwidth is restored")
{
  release_MRC mock_ll;
  to_rq_MRP mock_ul;
  CACHE uut{champsim::cache_builder{champsim::defaults::default_l1d}
                .name("427c-uut")
                .upper_levels({&mock_ul.queues})
                .lower_level(&mock_ll.queues)
                .hit_latency(1)
                .fill_latency(1)
                .tag_bandwidth(champsim::bandwidth::maximum_type{1})
                .fill_bandwidth(champsim::bandwidth::maximum_type{0})};

  std::array<champsim::operable*, 3> elements{{&uut, &mock_ll, &mock_ul}};
  for (auto* element : elements) {
    element->initialize();
    element->warmup = false;
    element->begin_phase();
  }

  REQUIRE(mock_ul.issue(request<to_rq_MRP>(champsim::address{0x4000})));
  for (int i = 0; i < 8; ++i)
    for (auto* element : elements)
      element->_operate();
  REQUIRE(mock_ll.packet_count() == 1);

  mock_ll.release_all();
  for (int i = 0; i < 8; ++i)
    for (auto* element : elements)
      element->_operate();
  REQUIRE(mock_ul.packets.back().return_time == 0);

  uut.MAX_FILL = champsim::bandwidth::maximum_type{1};
  for (int i = 0; i < 8; ++i)
    for (auto* element : elements)
      element->_operate();
  REQUIRE(mock_ul.packets.back().return_time > 0);
}
