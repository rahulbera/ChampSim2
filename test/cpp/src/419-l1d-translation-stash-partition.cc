#include <array>
#include <catch.hpp>

#include "channel.h"
#include "defaults.hpp"
#include "mocks.hpp"

// The L1D translation stash is gated in equal shares across hardware
// contexts. An untranslated entry is admitted only while the stash as a whole is
// under MSHR_SIZE and, with more than one context, while the entry's own address
// space is under its equal share. The counts are taken before the drain and
// include the still-untranslated inflight_tag_check entries, any of which may
// return to the stash.

namespace
{
champsim::channel::request_type make_req(uint8_t asid, uint64_t addr)
{
  champsim::channel::request_type req;
  req.address = champsim::address{addr};
  req.v_address = champsim::address{addr};
  req.is_translated = false;
  req.type = access_type::LOAD;
  req.asid[0] = asid;
  req.asid[1] = asid;
  req.cpu = 0;
  return req;
}
} // namespace

TEST_CASE("A stash that begins the cycle full admits no untranslated entry in that cycle")
{
  release_MRC mock_translator; // holds walks until released
  do_nothing_MRC mock_ll;
  champsim::channel core_channel;

  CACHE uut{champsim::cache_builder{champsim::defaults::default_l1d}
                .name("419a-stash-full-cycle")
                .upper_levels({&core_channel})
                .lower_level(&mock_ll.queues)
                .lower_translate(&mock_translator.queues)
                .hit_latency(1)
                .tag_bandwidth(champsim::bandwidth::maximum_type{16})
                .mshr_size(4)};

  std::array<champsim::operable*, 3> elements{{&uut, &mock_ll, &mock_translator}};
  for (auto elem : elements) {
    elem->initialize();
    elem->warmup = false;
    elem->begin_phase();
  }

  // Fill the four-entry stash with unanswered walks.
  for (unsigned i = 0; i < 4; ++i)
    REQUIRE(core_channel.add_rq(make_req(1, 0x1000 * (i + 1))));
  uut._operate();             // admit four, issue four walks
  mock_translator._operate(); // translator consumes the four walks into held packets
  uut._operate();             // the four entries become ready and drop into the stash
  REQUIRE(core_channel.rq_occupancy() == 0);
  REQUIRE(mock_translator.packet_count() == 4);

  // A fifth load arrives in the same cycle one walk returns. The gate is taken at
  // cycle start, when the stash was full, so the fifth is not admitted this cycle.
  REQUIRE(core_channel.add_rq(make_req(1, 0x5000)));
  mock_translator.release(champsim::address{0x1000}); // one walk answers now
  uut._operate();
  CHECK(core_channel.rq_occupancy() == 1); // the fifth load still waits

  // The next cycle the returned walk has freed a slot, so the fifth enters.
  uut._operate();
  CHECK(core_channel.rq_occupancy() == 0);
}

TEST_CASE("Each context's untranslated entries are admitted up to its equal share of the stash")
{
  release_MRC mock_translator;
  do_nothing_MRC mock_ll;
  champsim::channel channel_a;
  champsim::channel channel_b;

  CACHE uut{champsim::cache_builder{champsim::defaults::default_l1d}
                .name("419b-equal-share")
                .upper_levels({&channel_a, &channel_b})
                .lower_level(&mock_ll.queues)
                .lower_translate(&mock_translator.queues)
                .hit_latency(1)
                .tag_bandwidth(champsim::bandwidth::maximum_type{16})
                .mshr_size(16)
                .num_contexts(2)};

  std::array<champsim::operable*, 3> elements{{&uut, &mock_ll, &mock_translator}};
  for (auto elem : elements) {
    elem->initialize();
    elem->warmup = false;
    elem->begin_phase();
  }

  // Context A fills its whole eight-entry share and its walks go unanswered, so
  // the stash holds A's eight untranslated entries.
  for (unsigned i = 0; i < 8; ++i)
    REQUIRE(channel_a.add_rq(make_req(1, 0x1000 * (i + 1))));
  uut._operate();             // admit A's eight, issue eight walks
  mock_translator._operate(); // hold the eight walks
  uut._operate();             // the eight entries drop into the stash
  REQUIRE(channel_a.rq_occupancy() == 0);
  REQUIRE(mock_translator.packet_count() == 8);

  // A's share is now full, so its ninth load waits; B's share is empty, so its
  // first load enters.
  REQUIRE(channel_a.add_rq(make_req(1, 0x9000)));
  REQUIRE(channel_b.add_rq(make_req(2, 0x2000)));
  uut._operate();
  CHECK(channel_a.rq_occupancy() == 1); // A's ninth waits
  CHECK(channel_b.rq_occupancy() == 0); // B's first enters
}

TEST_CASE("The stash as a whole admits nothing untranslated once it holds MSHR_SIZE entries")
{
  release_MRC mock_translator; // holds every walk
  do_nothing_MRC mock_ll;
  champsim::channel core_channel;

  CACHE uut{champsim::cache_builder{champsim::defaults::default_l1d}
                .name("419c-total-bound")
                .upper_levels({&core_channel})
                .lower_level(&mock_ll.queues)
                .lower_translate(&mock_translator.queues)
                .hit_latency(1)
                .tag_bandwidth(champsim::bandwidth::maximum_type{16})
                .mshr_size(4)};

  std::array<champsim::operable*, 3> elements{{&uut, &mock_ll, &mock_translator}};
  for (auto elem : elements) {
    elem->initialize();
    elem->warmup = false;
    elem->begin_phase();
  }

  // Two address spaces put two walks each on the wire (2 + 2) and the translator
  // holds them unanswered, so the stash holds MSHR_SIZE untranslated entries.
  for (unsigned i = 0; i < 2; ++i) {
    REQUIRE(core_channel.add_rq(make_req(1, 0x1000 * (i + 1))));
    REQUIRE(core_channel.add_rq(make_req(2, 0x3000 * (i + 1))));
  }
  uut._operate();             // admit four, issue four walks
  mock_translator._operate(); // hold the four walks
  uut._operate();             // the four entries drop into the stash
  REQUIRE(core_channel.rq_occupancy() == 0);
  REQUIRE(mock_translator.packet_count() == 4);

  // A third address space has nothing stashed, but the stash as a whole is full,
  // so its load waits even though its own count is zero.
  REQUIRE(core_channel.add_rq(make_req(3, 0x5000)));
  for (int i = 0; i < 4; ++i) {
    uut._operate();
    CHECK(core_channel.rq_occupancy() == 1); // the third asid waits each cycle
  }
}

TEST_CASE("A cache refuses a context count that does not divide its MSHRs")
{
  // (16,1), (5,1) and (16,2) divide evenly and build.
  REQUIRE_NOTHROW(CACHE{champsim::cache_builder{champsim::defaults::default_l1d}.mshr_size(16).num_contexts(1)});
  REQUIRE_NOTHROW(CACHE{champsim::cache_builder{champsim::defaults::default_l1d}.mshr_size(5).num_contexts(1)});
  REQUIRE_NOTHROW(CACHE{champsim::cache_builder{champsim::defaults::default_l1d}.mshr_size(16).num_contexts(2)});

  // (5,2), (1,2) and (16,0) do not divide evenly (or have no context) and throw.
  REQUIRE_THROWS(CACHE{champsim::cache_builder{champsim::defaults::default_l1d}.mshr_size(5).num_contexts(2)});
  REQUIRE_THROWS(CACHE{champsim::cache_builder{champsim::defaults::default_l1d}.mshr_size(1).num_contexts(2)});
  REQUIRE_THROWS(CACHE{champsim::cache_builder{champsim::defaults::default_l1d}.mshr_size(16).num_contexts(0)});
}

TEST_CASE("With two contexts the stash never exceeds MSHR_SIZE or a context's share, even within one cycle")
{
  release_MRC mock_translator; // holds every walk
  do_nothing_MRC mock_ll;
  champsim::channel channel_a;
  champsim::channel channel_b;

  CACHE uut{champsim::cache_builder{champsim::defaults::default_l1d}
                .name("419e-inflight-count")
                .upper_levels({&channel_a, &channel_b})
                .lower_level(&mock_ll.queues)
                .lower_translate(&mock_translator.queues)
                .hit_latency(1)
                .tag_bandwidth(champsim::bandwidth::maximum_type{16})
                .mshr_size(4)
                .num_contexts(2)};

  std::array<champsim::operable*, 3> elements{{&uut, &mock_ll, &mock_translator}};
  for (auto elem : elements) {
    elem->initialize();
    elem->warmup = false;
    elem->begin_phase();
  }

  // Six loads per context, walks never answered. Each context may hold two
  // untranslated entries, and the stash as a whole four, so in each of eight
  // cycles both channels keep at least four loads and the translator holds at
  // most four walks.
  for (unsigned i = 0; i < 6; ++i) {
    REQUIRE(channel_a.add_rq(make_req(1, 0x1000 * (i + 1))));
    REQUIRE(channel_b.add_rq(make_req(2, 0x2000 * (i + 1))));
  }

  for (int i = 0; i < 8; ++i) {
    uut._operate();
    mock_translator._operate();
  }
  CHECK(channel_a.rq_occupancy() >= 4);
  CHECK(channel_b.rq_occupancy() >= 4);
  CHECK(mock_translator.packet_count() <= 4);

  // Answer one of A's walks: A's share frees one slot, so A admits one more load
  // while B's share is untouched.
  mock_translator.release(champsim::address{0x1000});
  uut._operate();
  mock_translator._operate();
  uut._operate();
  CHECK(channel_a.rq_occupancy() == 3);
  CHECK(channel_b.rq_occupancy() == 4);
}
