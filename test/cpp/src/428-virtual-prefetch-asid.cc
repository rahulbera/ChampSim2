#include <algorithm>
#include <catch.hpp>
#include <deque>

#include "channel.h"
#include "defaults.hpp"
#include "mocks.hpp"

// A virtual prefetch is translated in the address space of the demand that
// triggered it: prefetch_line stamps the packet with the issuing core's asid,
// {cpu, cpu}, so the walker resolves it in the same page table as the demand.
// Left at the {255,255} default, the prefetch would be walked in a phantom
// address space of its own.

namespace
{
// A translator that records every translation request and answers it with a
// fixed physical page, so the test can observe the cpu/asid a translation was
// issued under.
struct recording_translator : champsim::operable {
  champsim::channel queues{};
  std::deque<champsim::channel::request_type> recorded{};

  long operate() override
  {
    for (const auto& pkt : queues.RQ) {
      recorded.push_back(pkt);
      auto response = champsim::channel::response_type{pkt};
      response.data = champsim::address{0xcafe000};
      queues.returned.push_back(response);
    }
    queues.RQ.clear();
    return 1; // never deadlock
  }
};
} // namespace

TEST_CASE("A virtual prefetch is translated in the address space of the demand that triggered it")
{
  auto core = GENERATE(as<uint8_t>{}, 0, 1);

  recording_translator mock_translator;
  do_nothing_MRC mock_ll;
  to_rq_MRP mock_ul{[](auto x, auto y) {
    return x.v_address == y.v_address;
  }};

  CACHE uut{champsim::cache_builder{champsim::defaults::default_l1d}
                .name("428-uut")
                .virtual_prefetch(true)
                .upper_levels({&mock_ul.queues})
                .lower_level(&mock_ll.queues)
                .lower_translate(&mock_translator.queues)
                .hit_latency(2)
                .fill_latency(2)};

  std::array<champsim::operable*, 4> elements{{&uut, &mock_ll, &mock_ul, &mock_translator}};
  for (auto elem : elements) {
    elem->initialize();
    elem->warmup = false;
    elem->begin_phase();
  }

  // A demand in this core's address space teaches the cache which core it is
  // prefetching for.
  champsim::channel::request_type req;
  req.address = champsim::address{0xdead000};
  req.v_address = champsim::address{0xdead000};
  req.is_translated = false;
  req.cpu = core;
  req.asid[0] = core;
  req.asid[1] = core;

  REQUIRE(mock_ul.issue(req));

  for (int i = 0; i < 20; ++i) {
    for (auto elem : elements)
      elem->_operate();
  }

  REQUIRE(uut.prefetch_line(champsim::address{0xbeef000}, true, 0));

  for (int i = 0; i < 20; ++i) {
    for (auto elem : elements)
      elem->_operate();
  }

  auto beef = std::find_if(std::begin(mock_translator.recorded), std::end(mock_translator.recorded),
                           [](const auto& pkt) { return pkt.address == champsim::address{0xbeef000}; });
  REQUIRE(beef != std::end(mock_translator.recorded));
  CHECK(static_cast<int>(beef->cpu) == static_cast<int>(core));
  CHECK(static_cast<int>(beef->asid[0]) == static_cast<int>(core));
  CHECK(static_cast<int>(beef->asid[1]) == static_cast<int>(core));
}
