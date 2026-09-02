#include <array>
#include <catch.hpp>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "../../../prefetcher/generic_markov/generic_markov.h"
#include "address.h"
#include "cache.h"
#include "defaults.hpp"
#include "mocks.hpp"
#include "runtime_config.h"

// generic_markov is a measurement instrument, so these tests pin the MEASURED
// NUMBERS, not just that it runs. Every expectation below was computed by hand
// from the stream in the test, not read back from the implementation.

namespace
{
// The module needs a CACHE to bind to -- prefetcher_end_phase publishes into
// its roi_stats -- but the tests drive the module directly rather than through
// the cache's request path, so the exact address sequence is under control.
struct markov_harness {
  do_nothing_MRC mock_ll{};
  to_rq_MRP mock_ul{};
  CACHE cache;
  generic_markov pref;

  explicit markov_harness(const std::string& name, const std::vector<std::string>& knobs = {})
      : cache{champsim::cache_builder{champsim::defaults::default_llc}.name(name).upper_levels({&mock_ul.queues}).lower_level(&mock_ll.queues)}, pref{&cache}
  {
    champsim::runtime_config cfg{};
    for (const auto& knob : knobs) {
      cfg.set(knob);
    }
    pref.configure(cfg, "cache.llc.generic_markov");
    pref.prefetcher_begin_phase();
  }

  void access(uint64_t block, bool hit = false)
  {
    pref.prefetcher_cache_operate(champsim::address{champsim::block_number{block}}, champsim::address{}, static_cast<uint8_t>(hit ? 1 : 0), false,
                                  access_type::LOAD, 0);
  }

  void walk(const std::vector<uint64_t>& blocks)
  {
    for (auto block : blocks) {
      access(block);
    }
  }

  const cache_stats::module_stat_block& publish()
  {
    pref.prefetcher_end_phase();
    // Exactly one block, named for the module -- not a merged bag.
    REQUIRE(std::size(cache.roi_stats.module_stats) == 1);
    REQUIRE(cache.roi_stats.module_stats.front().module == "generic_markov");
    return cache.roi_stats.module_stats.front();
  }
};

int64_t count_of(const cache_stats::module_stat_block& stats, const std::string& name)
{
  const auto* found = stats.find(name);
  REQUIRE(found != nullptr);
  REQUIRE(std::holds_alternative<int64_t>(*found));
  return std::get<int64_t>(*found);
}

double rate_of(const cache_stats::module_stat_block& stats, const std::string& name)
{
  const auto* found = stats.find(name);
  REQUIRE(found != nullptr);
  REQUIRE(std::holds_alternative<double>(*found));
  return std::get<double>(*found);
}
} // namespace

TEST_CASE("The one-history model reproduces a hand-computed correlation table")
{
  // Stream A B A C A B A B, with A=0x10 B=0x20 C=0x30 as block numbers.
  //
  // Seven transitions produce exactly three keys:
  //   [A] -> {B:3, C:1}   cardinality 2, trained 4 times
  //   [B] -> {A:2}        cardinality 1, trained 2 times
  //   [C] -> {A:1}        cardinality 1, trained 1 time
  constexpr uint64_t A{0x10};
  constexpr uint64_t B{0x20};
  constexpr uint64_t C{0x30};

  markov_harness uut{"454-one-history"};
  uut.walk({A, B, A, C, A, B, A, B});

  SECTION("The table has the shape the stream implies")
  {
    REQUIRE(uut.pref.table_size() == 3);

    const auto* from_a = uut.pref.lookup({A});
    REQUIRE(from_a != nullptr);
    REQUIRE(std::size(from_a->candidates) == 2);
    REQUIRE(from_a->total_count == 4);

    const auto* from_b = uut.pref.lookup({B});
    REQUIRE(from_b != nullptr);
    REQUIRE(std::size(from_b->candidates) == 1);
    REQUIRE(from_b->total_count == 2);

    const auto* from_c = uut.pref.lookup({C});
    REQUIRE(from_c != nullptr);
    REQUIRE(from_c->total_count == 1);
  }

  SECTION("Every published statistic matches the hand computation")
  {
    const auto& stats = uut.publish();

    // Eight accesses give seven transitions; the first cannot train.
    REQUIRE(count_of(stats, "train_events") == 7);

    // Every access predicts once the window is full, which is immediately at
    // H=1. Five of the eight find a key that already exists.
    REQUIRE(count_of(stats, "predict_attempts") == 8);
    REQUIRE(count_of(stats, "predict_hits") == 5);
    REQUIRE(rate_of(stats, "lookup_hit_rate") == Catch::Approx(5.0 / 8.0));

    // Right sides met at those five hits: 1, 2, 1, 2, 1.
    REQUIRE(count_of(stats, "sum_cardinality") == 7);
    REQUIRE(rate_of(stats, "mean_cardinality_per_lookup") == Catch::Approx(7.0 / 5.0));

    // Training counts of the matched keys at the moment of the hit: 1,2,1,3,2.
    REQUIRE(count_of(stats, "sum_key_occurrences") == 9);
    REQUIRE(rate_of(stats, "mean_key_occurrences") == Catch::Approx(9.0 / 5.0));

    // Four predictions got a following access to be graded against.
    //
    // Two named it first, not three: the prediction from [A] -> {B:1, C:1} is a
    // count tie, and recency ranks C first because it was seen more recently.
    // The address tie-break this replaced would have ranked B first and scored
    // that one correct. The number moving IS the change under test.
    REQUIRE(count_of(stats, "scored_predictions") == 4);
    REQUIRE(count_of(stats, "top1_correct") == 2);
    REQUIRE(count_of(stats, "topk_correct") == 3);
    REQUIRE(rate_of(stats, "top1_rate") == Catch::Approx(0.5));
    REQUIRE(count_of(stats, "top1_ties") == 1);

    // The successor was in the candidate list at some rank on three of the four
    // -- the ordering- and degree-independent ceiling. The nesting must hold.
    REQUIRE(count_of(stats, "topall_correct") == 3);
    REQUIRE(count_of(stats, "top1_correct") <= count_of(stats, "topk_correct"));
    REQUIRE(count_of(stats, "topk_correct") <= count_of(stats, "topall_correct"));
    REQUIRE(count_of(stats, "topall_correct") <= count_of(stats, "scored_predictions"));

    // Five predictions emitted 1, 2, 1, 2, 1 addresses. Equal to sum_cardinality
    // here only because no key is wider than predict_degree; the k=1 test below
    // separates them.
    REQUIRE(count_of(stats, "predicted_addresses") == 7);

    // The map walk: cardinalities are 2, 1, 1 over three keys.
    REQUIRE(count_of(stats, "distinct_keys") == 3);
    REQUIRE(count_of(stats, "sum_cardinality_per_key") == 4);
    REQUIRE(rate_of(stats, "mean_cardinality_per_key") == Catch::Approx(4.0 / 3.0));
    REQUIRE(count_of(stats, "max_cardinality") == 2);

    // Only [C] was trained exactly once.
    REQUIRE(count_of(stats, "keys_seen_once") == 1);
  }
}

TEST_CASE("The key is the whole sequence, not its last address")
{
  // The failure this catches is a two-history model that silently keys on one
  // address: it would report the same numbers as H=1 and nothing would say so.
  constexpr uint64_t A{0x10};
  constexpr uint64_t B{0x20};
  constexpr uint64_t C{0x30};
  constexpr uint64_t D{0x40};

  markov_harness uut{"454-two-history", {"cache.llc.generic_markov.history_length=2"}};
  uut.walk({A, B, C, A, B, D});

  // Trained: [A,B]->C, [B,C]->A, [C,A]->B, [A,B]->D.
  REQUIRE(uut.pref.table_size() == 3);

  const auto* pair = uut.pref.lookup({A, B});
  REQUIRE(pair != nullptr);
  REQUIRE(std::size(pair->candidates) == 2);
  REQUIRE(pair->total_count == 2);

  // The single address is not a key at H=2.
  REQUIRE(uut.pref.lookup({A}) == nullptr);
  REQUIRE(uut.pref.lookup({B}) == nullptr);

  const auto& stats = uut.publish();
  REQUIRE(count_of(stats, "train_events") == 4);
  REQUIRE(count_of(stats, "distinct_keys") == 3);

  // The assertions above all read the TABLE, which step 1 builds. They say
  // nothing about the key step 3 looks up -- a predictor that trains at H=2 but
  // predicts on the last address alone would satisfy every one of them, while
  // driving lookup_hit_rate to zero for every H > 1 in the sweep. That is the
  // number the design leans on as its load-bearing sanity check, so pin the
  // predict path directly.
  //
  // Four accesses have a full window; only the second [A,B] finds a key. Its
  // prediction names C, and the access that follows is D.
  REQUIRE(count_of(stats, "predict_attempts") == 5);
  REQUIRE(count_of(stats, "predict_hits") == 1);
  REQUIRE(count_of(stats, "scored_predictions") == 1);
  REQUIRE(count_of(stats, "top1_correct") == 0);
}

TEST_CASE("predict_degree governs the top-k, and top-k is not an alias for top-1")
{
  // In every other stream in this file the graded successor is either at rank 1
  // or absent from the candidate list entirely, so topk_correct == top1_correct
  // throughout and three separate defects would go unnoticed: topk aliased to
  // top1, `keep` ignoring predict_degree, and `keep` pinned to 1.
  //
  // Here [A] accumulates {B,C,D,E}, all with count 1, so the count signal is
  // silent and recency decides: most recent first, i.e. E, D, C, B -- the
  // REVERSE of insertion. The final access is D, which sits at rank 2. It
  // scores top-k at k=2 and not at k=1, and never scores top-1.
  constexpr uint64_t A{0x10};
  constexpr uint64_t B{0x20};
  constexpr uint64_t C{0x30};
  constexpr uint64_t D{0x40};
  constexpr uint64_t E{0x50};
  const std::vector<uint64_t> stream{A, B, A, C, A, D, A, E, A, D};

  SECTION("At k=2 the rank-2 successor scores")
  {
    markov_harness uut{"454-degree-2", {"cache.llc.generic_markov.predict_degree=2"}};
    uut.walk(stream);

    const auto& stats = uut.publish();
    REQUIRE(count_of(stats, "scored_predictions") == 4);
    REQUIRE(count_of(stats, "top1_correct") == 0);
    REQUIRE(count_of(stats, "topk_correct") == 1);
  }

  SECTION("At k=1 it does not")
  {
    markov_harness uut{"454-degree-1", {"cache.llc.generic_markov.predict_degree=1"}};
    uut.walk(stream);

    const auto& stats = uut.publish();
    REQUIRE(count_of(stats, "scored_predictions") == 4);
    REQUIRE(count_of(stats, "top1_correct") == 0);
    REQUIRE(count_of(stats, "topk_correct") == 0);
  }
}

TEST_CASE("Predictions decided by recency rather than by count are counted")
{
  // top1_rate is only interpretable next to this. At large H nearly every
  // multi-successor key is an all-way tie -- every successor seen once -- so
  // the count signal is silent and recency alone picks rank 1. The counter
  // separates "frequency knew" from "recency knew".
  //
  // Same stream as above: three of the four graded predictions come from a key
  // whose candidates all have count 1.
  constexpr uint64_t A{0x10};
  constexpr uint64_t B{0x20};
  constexpr uint64_t C{0x30};
  constexpr uint64_t D{0x40};
  constexpr uint64_t E{0x50};

  markov_harness uut{"454-ties"};
  uut.walk({A, B, A, C, A, D, A, E, A, D});

  const auto& stats = uut.publish();
  REQUIRE(count_of(stats, "scored_predictions") == 4);
  REQUIRE(count_of(stats, "top1_ties") == 3);
  REQUIRE(rate_of(stats, "top1_tie_rate") == Catch::Approx(0.75));
}

TEST_CASE("A prediction is graded against the next access, not the one that made it")
{
  // This is the off-by-one that would make the instrument useless while
  // looking excellent: grading a prediction against the address that produced
  // it reports a near-perfect predictor on any stream.
  constexpr uint64_t A{0x10};
  constexpr uint64_t B{0x20};

  SECTION("A successor that does not repeat scores zero")
  {
    // A A B: the second A trains [A]->A and predicts A. The next access is B,
    // so the prediction was wrong. Grading against the CURRENT access would
    // have compared the prediction A with the access A and scored it correct.
    markov_harness uut{"454-grading-wrong"};
    uut.walk({A, A, B});

    const auto& stats = uut.publish();
    REQUIRE(count_of(stats, "scored_predictions") == 1);
    REQUIRE(count_of(stats, "top1_correct") == 0);
  }

  SECTION("A successor that does repeat scores one")
  {
    markov_harness uut{"454-grading-right"};
    uut.walk({A, A, A});

    const auto& stats = uut.publish();
    REQUIRE(count_of(stats, "scored_predictions") == 1);
    REQUIRE(count_of(stats, "top1_correct") == 1);
  }
}

TEST_CASE("Equal-count candidates are ranked by recency, most recent first")
{
  // The tie-break under test. Recency replaced an ascending-address rule, and
  // the stream is chosen so the two DISAGREE -- the more recent candidate is
  // also the higher-addressed one. The predecessor of this test used a stream
  // where address and recency happened to agree, so it passed under both and
  // proved nothing.
  //
  // A B A C A C at H=1: [A] collects B then C, both count 1, and C is both
  // later and higher-addressed. The last prediction must name C.
  //
  //   address order (old)  -> B first -> top1_correct 0
  //   insertion order      -> B first -> top1_correct 0
  //   recency (new)        -> C first -> top1_correct 1
  constexpr uint64_t A{0x10};
  constexpr uint64_t B{0x20};
  constexpr uint64_t C{0x30};

  markov_harness uut{"454-recency"};
  uut.walk({A, B, A, C, A, C});

  const auto* from_a = uut.pref.lookup({A});
  REQUIRE(from_a != nullptr);
  REQUIRE(std::size(from_a->candidates) == 2);

  const auto& stats = uut.publish();
  REQUIRE(count_of(stats, "scored_predictions") == 2);
  REQUIRE(count_of(stats, "top1_correct") == 1);
}

TEST_CASE("Recency is refreshed when a candidate's count is incremented")
{
  // The easy half of the tie-break to get wrong: stamping only on insert. A
  // candidate seen a thousand times would then keep the recency of its first
  // sighting, and the rule would silently mean "least recently INTRODUCED".
  //
  // A C A B A B A C A C at H=1, degree 1. [A] ends with B and C both at count
  // 2. C was INSERTED first but RE-OBSERVED last, so the two rules disagree:
  //
  //   stamp on insert only -> B ranks first -> top1_correct 3
  //   refresh on increment -> C ranks first -> top1_correct 4
  constexpr uint64_t A{0x10};
  constexpr uint64_t B{0x20};
  constexpr uint64_t C{0x30};

  markov_harness uut{"454-recency-refresh", {"cache.llc.generic_markov.predict_degree=1"}};
  uut.walk({A, C, A, B, A, B, A, C, A, C});

  const auto& stats = uut.publish();
  REQUIRE(count_of(stats, "scored_predictions") == 6);
  REQUIRE(count_of(stats, "top1_correct") == 4);
}

TEST_CASE("predicted_addresses counts addresses emitted, not predictions made")
{
  // The denominator for accuracy at degree k. One prediction emits up to k
  // addresses and at most one can be right, so scored_predictions overstates
  // the accuracy and sum_cardinality understates it -- neither is the answer.
  //
  // Same stream as the worked example. At k=1 exactly one address goes out per
  // hit, so predicted_addresses equals predict_hits (5) while sum_cardinality
  // is 7: the two are only equal when no key is wider than k.
  constexpr uint64_t A{0x10};
  constexpr uint64_t B{0x20};
  constexpr uint64_t C{0x30};

  markov_harness uut{"454-emitted", {"cache.llc.generic_markov.predict_degree=1"}};
  uut.walk({A, B, A, C, A, B, A, B});

  const auto& stats = uut.publish();
  REQUIRE(count_of(stats, "predict_hits") == 5);
  REQUIRE(count_of(stats, "predicted_addresses") == 5);
  REQUIRE(count_of(stats, "sum_cardinality") == 7);
}

TEST_CASE("topall_correct is independent of ordering policy and of degree")
{
  // The ceiling: was the successor in the candidate list AT ALL. It is read off
  // the training find_if, so it must not move when predict_degree changes --
  // that independence is the whole claim, and is what makes it a bound on any
  // ranking policy rather than a property of this one.
  constexpr uint64_t A{0x10};
  constexpr uint64_t B{0x20};
  constexpr uint64_t C{0x30};
  const std::vector<uint64_t> stream{A, B, A, C, A, B, A, B};

  int64_t first = 0;
  for (const auto* degree : {"cache.llc.generic_markov.predict_degree=1", "cache.llc.generic_markov.predict_degree=4"}) {
    markov_harness uut{std::string{"454-topall-"} + degree, {degree}};
    uut.walk(stream);
    const auto& stats = uut.publish();

    REQUIRE(count_of(stats, "topall_correct") == 3);
    REQUIRE(count_of(stats, "topk_correct") <= count_of(stats, "topall_correct"));
    if (first == 0) {
      first = count_of(stats, "topall_correct");
    }
    REQUIRE(count_of(stats, "topall_correct") == first);
  }
}

TEST_CASE("Cardinality percentiles are exact nearest-rank over the keys")
{
  // The walk below builds five keys with cardinalities 1, 1, 1, 2, 3:
  //   [5] -> {1,2,3}   [4] -> {1,2}   [1] -> {4}   [2] -> {5}   [3] -> {9}
  // Nearest rank over three-of-cardinality-1, one-of-2, one-of-3:
  //   p50 target ceil(2.5)=3 -> reached at cardinality 1
  //   p75 target ceil(3.75)=4 -> reached at cardinality 2
  //   p90 target ceil(4.5)=5 -> reached at cardinality 3
  markov_harness uut{"454-percentiles"};
  uut.walk({5, 1, 4, 1, 4, 2, 5, 2, 5, 3, 9});

  const auto& stats = uut.publish();

  REQUIRE(count_of(stats, "train_events") == 10);
  REQUIRE(count_of(stats, "distinct_keys") == 5);
  REQUIRE(count_of(stats, "sum_cardinality_per_key") == 8);
  REQUIRE(rate_of(stats, "mean_cardinality_per_key") == Catch::Approx(8.0 / 5.0));
  REQUIRE(count_of(stats, "max_cardinality") == 3);
  REQUIRE(count_of(stats, "p50_cardinality") == 1);
  REQUIRE(count_of(stats, "p75_cardinality") == 2);
  REQUIRE(count_of(stats, "p90_cardinality") == 3);

  // Only [3] was trained exactly once.
  REQUIRE(count_of(stats, "keys_seen_once") == 1);
}

TEST_CASE("Occupancy thresholds measure how few keys carry the reoccurrence")
{
  // Walk 1,1,1,1,1,2,2,2,3,4,9 gives ten adjacent pairs and four keys:
  //   [1] trained 5x   [2] trained 3x   [3] trained 1x   [4] trained 1x
  // Repeat mass drops each key's first sighting: 4 + 2 + 0 + 0 = 6.
  // Taking the most frequent first, and allowing part of a bucket:
  //   O50 target ceil(3.0)=3 -> [1] alone contributes 4          -> 1 key
  //   O80 target ceil(4.8)=5 -> [1] gives 4, [2] closes the gap  -> 2 keys
  //   O90 target ceil(5.4)=6 -> same two keys reach exactly 6    -> 2 keys
  //   O95 target ceil(5.7)=6 -> unchanged                        -> 2 keys
  // Against ALL ten occurrences instead, target ceil(8.0)=8: [1] gives 5 and
  // [2] gives 3, so 2 keys -- the singletons still never enter.
  markov_harness uut{"454-occupancy"};
  uut.walk({1, 1, 1, 1, 1, 2, 2, 2, 3, 4, 9});

  const auto& stats = uut.publish();

  REQUIRE(count_of(stats, "train_events") == 10);
  REQUIRE(count_of(stats, "distinct_keys") == 4);
  REQUIRE(count_of(stats, "total_key_occurrences") == 10);
  REQUIRE(count_of(stats, "keys_seen_once") == 2);
  REQUIRE(count_of(stats, "repeat_occurrences") == 6);

  REQUIRE(count_of(stats, "o50_keys") == 1);
  REQUIRE(count_of(stats, "o80_keys") == 2);
  REQUIRE(count_of(stats, "o90_keys") == 2);
  REQUIRE(count_of(stats, "o95_keys") == 2);
  REQUIRE(count_of(stats, "o80_keys_by_total_occurrence") == 2);

  REQUIRE(rate_of(stats, "o50_key_frac") == Catch::Approx(0.25));
  REQUIRE(rate_of(stats, "o80_key_frac") == Catch::Approx(0.50));
  REQUIRE(rate_of(stats, "o90_key_frac") == Catch::Approx(0.50));
  REQUIRE(rate_of(stats, "o95_key_frac") == Catch::Approx(0.50));
  REQUIRE(rate_of(stats, "o80_key_frac_by_total_occurrence") == Catch::Approx(0.50));
}

TEST_CASE("Occupancy takes only its share of a bucket, not the whole bucket")
{
  // Walk 1,1,1,1,2,2,2,3,3,3,4,9 -- eleven pairs, four keys:
  //   [1] 4x   [2] 3x   [3] 3x   [4] 1x
  // Repeat mass 3 + 2 + 2 + 0 = 7, and [2] and [3] share one histogram bucket.
  //   O50 target ceil(3.5)=4 -> [1] gives 3, then ONE of the pair closes it -> 2
  // Taking the whole bucket would answer 3, which is not the minimum. That is
  // the only case in this file where the two differ.
  markov_harness uut{"454-occupancy-partial"};
  uut.walk({1, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 9});

  const auto& stats = uut.publish();

  REQUIRE(count_of(stats, "distinct_keys") == 4);
  REQUIRE(count_of(stats, "total_key_occurrences") == 11);
  REQUIRE(count_of(stats, "repeat_occurrences") == 7);

  REQUIRE(count_of(stats, "o50_keys") == 2);
  REQUIRE(count_of(stats, "o80_keys") == 3);
  REQUIRE(count_of(stats, "o90_keys") == 3);
  REQUIRE(count_of(stats, "o95_keys") == 3);
  REQUIRE(count_of(stats, "o80_keys_by_total_occurrence") == 3);

  REQUIRE(rate_of(stats, "o50_key_frac") == Catch::Approx(0.50));
  REQUIRE(rate_of(stats, "o80_key_frac") == Catch::Approx(0.75));
}

TEST_CASE("With no key seen twice there is no repeat mass to occupy")
{
  // 1,2,3,4 trains [1],[2],[3] exactly once each. Repeat mass is zero, so no
  // number of keys reaches any fraction of it and the answer is zero keys --
  // not "all of them", which is what a mass-less loop falling through would
  // give. The by-total-occ variant still has ten-elevenths of a denominator
  // and needs ceil(0.8*3)=3 of the three occurrences, so it takes all three.
  markov_harness uut{"454-occupancy-empty"};
  uut.walk({1, 2, 3, 4});

  const auto& stats = uut.publish();

  REQUIRE(count_of(stats, "distinct_keys") == 3);
  REQUIRE(count_of(stats, "keys_seen_once") == 3);
  REQUIRE(count_of(stats, "repeat_occurrences") == 0);

  REQUIRE(count_of(stats, "o50_keys") == 0);
  REQUIRE(count_of(stats, "o80_keys") == 0);
  REQUIRE(count_of(stats, "o95_keys") == 0);
  REQUIRE(rate_of(stats, "o80_key_frac") == Catch::Approx(0.0));

  REQUIRE(count_of(stats, "o80_keys_by_total_occurrence") == 3);
}

TEST_CASE("Occupancy coverage credits the keys that actually earned it")
{
  // walk 1,2,1,2,1,2,1,2,3,4,3,4,5,6,7,8,9,10 -- seventeen pairs, nine keys:
  //   [1] 4x, top1 3   [2] 4x, top1 2   [3] 2x, top1 1   [4] 2x, top1 0
  //   [5]..[9] 1x each, top1 0
  // Repeat mass 3+3+1+1 = 8.
  //   O50 target ceil(4)=4   -> the two count-4 keys      -> 2 keys, top1 3+2=5
  //   O90 target ceil(7.2)=8 -> plus both count-2 keys    -> 4 keys, top1 5+1=6
  // O80 (target 7) needs ONE of the two count-2 keys, and they are tied: [3]
  // carries a win and [4] does not, so its credit is 5 or 6 depending which the
  // map yields. Deterministic per run, arbitrary between the two -- so only the
  // unambiguous thresholds are pinned exactly.
  markov_harness uut{"454-occ-coverage"};
  uut.walk({1, 2, 1, 2, 1, 2, 1, 2, 3, 4, 3, 4, 5, 6, 7, 8, 9, 10});

  const auto& stats = uut.publish();

  REQUIRE(count_of(stats, "distinct_keys") == 9);
  REQUIRE(count_of(stats, "repeat_occurrences") == 8);
  REQUIRE(count_of(stats, "top1_correct") == 6);
  REQUIRE(count_of(stats, "topall_correct") == 6);

  REQUIRE(count_of(stats, "o50_keys") == 2);
  REQUIRE(count_of(stats, "o80_keys") == 3);
  REQUIRE(count_of(stats, "o90_keys") == 4);

  // The point of the metric: 2 of 9 keys carry 5 of the 6 wins.
  REQUIRE(count_of(stats, "o50_top1_correct") == 5);
  REQUIRE(count_of(stats, "o50_topall_correct") == 5);
  REQUIRE(count_of(stats, "o90_top1_correct") == 6);
  REQUIRE(count_of(stats, "o90_topall_correct") == 6);

  const auto o80_top1 = count_of(stats, "o80_top1_correct");
  REQUIRE(o80_top1 >= 5);
  REQUIRE(o80_top1 <= 6);

  REQUIRE(rate_of(stats, "o50_top1_coverage") == Catch::Approx(5.0 / static_cast<double>(count_of(stats, "predict_attempts"))).margin(0.005));
}

TEST_CASE("Coverage separates top-1 from top-all on the same key")
{
  // walk 0,1,0,1,0,1,0,2,0,2,0 -- [0] ends with candidates {1:3, 2:2}. On the
  // second 0->2 the successor IS among the candidates but is ranked second
  // behind 1, so that event credits top-all and not top-1. [0] therefore
  // carries top1 2, topall 3 -- the only shape in this file where a key's two
  // credits differ, and the only thing that catches top-all being charged as
  // top-1.
  //   [0] 5x  top1 2  topall 3
  //   [1] 3x  top1 2  topall 2
  //   [2] 2x  top1 1  topall 1
  // Repeat mass 4+2+1 = 7, and no two keys tie, so every threshold is exact:
  //   O50 ceil(3.5)=4 -> [0]            -> top1 2, topall 3
  //   O80 ceil(5.6)=6 -> [0],[1]        -> top1 4, topall 5
  //   O90 ceil(6.3)=7 -> [0],[1],[2]    -> top1 5, topall 6
  markov_harness uut{"454-occ-coverage-split"};
  uut.walk({0, 1, 0, 1, 0, 1, 0, 2, 0, 2, 0});

  const auto& stats = uut.publish();

  REQUIRE(count_of(stats, "distinct_keys") == 3);
  REQUIRE(count_of(stats, "repeat_occurrences") == 7);
  REQUIRE(count_of(stats, "top1_correct") == 5);
  REQUIRE(count_of(stats, "topall_correct") == 6);

  REQUIRE(count_of(stats, "o50_keys") == 1);
  REQUIRE(count_of(stats, "o50_top1_correct") == 2);
  REQUIRE(count_of(stats, "o50_topall_correct") == 3);

  REQUIRE(count_of(stats, "o80_keys") == 2);
  REQUIRE(count_of(stats, "o80_top1_correct") == 4);
  REQUIRE(count_of(stats, "o80_topall_correct") == 5);

  REQUIRE(count_of(stats, "o90_keys") == 3);
  REQUIRE(count_of(stats, "o90_top1_correct") == 5);
  REQUIRE(count_of(stats, "o90_topall_correct") == 6);

  // Fan-out of those same sets: [0] has two candidates, [1] and [2] one each.
  REQUIRE(count_of(stats, "o50_keys_w_cardinality_1_1") == 0);
  REQUIRE(count_of(stats, "o50_keys_w_cardinality_2_2") == 1);
  REQUIRE(rate_of(stats, "o50_mean_cardinality") == Catch::Approx(2.0));

  REQUIRE(count_of(stats, "o80_keys_w_cardinality_1_1") == 1);
  REQUIRE(count_of(stats, "o80_keys_w_cardinality_2_2") == 1);
  REQUIRE(rate_of(stats, "o80_mean_cardinality") == Catch::Approx(1.5));

  REQUIRE(count_of(stats, "o90_keys_w_cardinality_1_1") == 2);
  REQUIRE(count_of(stats, "o90_keys_w_cardinality_2_2") == 1);
  REQUIRE(rate_of(stats, "o90_mean_cardinality") == Catch::Approx(4.0 / 3.0).margin(0.005));
}

TEST_CASE("Occupancy fan-out lands in the same bands as the whole table")
{
  // A hub with n successors is the ONLY key with any repeat mass (each
  // successor is met once), so every occupancy set is exactly {hub} and its
  // fan-out band is the band for n. Sweeping n across every edge is what
  // catches an off-by-one in the band index -- the 64|65 edge especially,
  // since that is where the doubling loop terminates. n=1 is excluded: a hub
  // with one successor is met once, so it is a singleton with no repeat mass
  // and every occupancy set is empty. The 1_1 band is covered below instead.
  const auto [width, band] = GENERATE(table<uint64_t, std::string>({
      {2, "2_2"},
      {3, "3_4"},
      {4, "3_4"},
      {5, "5_8"},
      {8, "5_8"},
      {9, "9_16"},
      {16, "9_16"},
      {17, "17_32"},
      {32, "17_32"},
      {33, "33_64"},
      {64, "33_64"},
      {65, "65_plus"},
      {130, "65_plus"},
  }));

  markov_harness uut{"454-occ-fanout-" + std::to_string(width)};
  std::vector<uint64_t> walk{};
  for (uint64_t i = 1; i <= width; ++i) {
    walk.push_back(0);
    walk.push_back(i);
  }
  walk.push_back(0);
  uut.walk(walk);

  const auto& stats = uut.publish();
  REQUIRE(count_of(stats, "max_cardinality") == static_cast<int64_t>(width));

  for (const auto* prefix : {"o50", "o80", "o90"}) {
    const std::string p{prefix};
    REQUIRE(count_of(stats, p + "_keys") == 1);
    int64_t total{0};
    for (const auto* b : {"1_1", "2_2", "3_4", "5_8", "9_16", "17_32", "33_64", "65_plus"}) {
      const auto n = count_of(stats, p + "_keys_w_cardinality_" + b);
      REQUIRE(n == (b == band ? 1 : 0));
      total += n;
    }
    // The bands partition the set, exactly as they do the whole table.
    REQUIRE(total == count_of(stats, p + "_keys"));
    REQUIRE(rate_of(stats, p + "_mean_cardinality") == Catch::Approx(static_cast<double>(width)).margin(0.005));
  }
}

TEST_CASE("A fixed key budget takes the most-recurred keys, ties included")
{
  // Same walk as the coverage case: [1] 4x(top1 3), [2] 4x(top1 2),
  // [3] 2x(top1 1), [4] 2x(top1 0), [5]..[9] 1x. Nine keys, six wins.
  // A budget of 50000 exceeds the table, so it must take all nine and match the
  // whole-table figures exactly -- a budget cannot invent or drop credit.
  markov_harness uut{"454-budget"};
  uut.walk({1, 2, 1, 2, 1, 2, 1, 2, 3, 4, 3, 4, 5, 6, 7, 8, 9, 10});

  const auto& stats = uut.publish();

  REQUIRE(count_of(stats, "distinct_keys") == 9);
  REQUIRE(count_of(stats, "top1_correct") == 6);

  // Budget larger than the table: everything, frac exactly 1.
  REQUIRE(rate_of(stats, "top_50000_key_frac") == Catch::Approx(1.0));
  REQUIRE(count_of(stats, "top_50000_top1_correct") == 6);
  REQUIRE(count_of(stats, "top_50000_topall_correct") == 6);
  REQUIRE(rate_of(stats, "top_50000_mean_cardinality") == Catch::Approx(rate_of(stats, "mean_cardinality_per_key")).margin(0.005));

  int64_t total{0};
  for (const auto* b : {"1_1", "2_2", "3_4", "5_8", "9_16", "17_32", "33_64", "65_plus"}) {
    total += count_of(stats, std::string{"top_50000_keys_w_cardinality_"} + b);
  }
  REQUIRE(total == 9);

  // A budget is monotone in size and never exceeds the whole table.
  REQUIRE(count_of(stats, "top_1000_top1_correct") <= count_of(stats, "top_10000_top1_correct"));
  REQUIRE(count_of(stats, "top_10000_top1_correct") <= count_of(stats, "top_50000_top1_correct"));
  REQUIRE(rate_of(stats, "top_1000_key_frac") == Catch::Approx(1.0));
}

TEST_CASE("A budget truncates a table larger than itself")
{
  // 1500 keys, every one met exactly once, so the budget is the ONLY thing that
  // can bound the answer -- nothing else in the module distinguishes them. A
  // 1000-key budget must keep 1000 of 1500; a 10000-key budget must keep all
  // 1500. Without truncation both report 1500 and this is the only case in the
  // file that can tell the difference.
  markov_harness uut{"454-budget-truncate-real"};
  std::vector<uint64_t> walk{};
  for (uint64_t i = 1; i <= 1501; ++i) {
    walk.push_back(i);
  }
  uut.walk(walk);

  const auto& stats = uut.publish();

  REQUIRE(count_of(stats, "distinct_keys") == 1500);
  REQUIRE(count_of(stats, "keys_seen_once") == 1500);

  REQUIRE(count_of(stats, "top_1000_keys_w_cardinality_1_1") == 1000);
  REQUIRE(rate_of(stats, "top_1000_key_frac") == Catch::Approx(1000.0 / 1500.0).margin(0.005));

  REQUIRE(count_of(stats, "top_10000_keys_w_cardinality_1_1") == 1500);
  REQUIRE(rate_of(stats, "top_10000_key_frac") == Catch::Approx(1.0));
  REQUIRE(rate_of(stats, "top_50000_key_frac") == Catch::Approx(1.0));

  // Every key is a singleton, so no occupancy set can reach any of them.
  REQUIRE(count_of(stats, "repeat_occurrences") == 0);
  REQUIRE(count_of(stats, "o80_keys") == 0);
}

TEST_CASE("A budget smaller than the table keeps only the hottest keys")
{
  // [0] is met 5 times, [1] 3, [2] 2, and five more keys once each -- eight
  // keys total. A budget of 1000 takes them all, so to exercise truncation the
  // check is against the O-sets, which cut at the same histogram: O50 keeps one
  // key and must therefore be a strict subset of any budget that keeps more.
  markov_harness uut{"454-budget-truncate"};
  uut.walk({0, 1, 0, 1, 0, 1, 0, 2, 0, 2, 0});

  const auto& stats = uut.publish();

  REQUIRE(count_of(stats, "distinct_keys") == 3);
  REQUIRE(count_of(stats, "o50_keys") == 1);

  // Budget covers the table, so it matches the whole-table totals.
  REQUIRE(count_of(stats, "top_1000_top1_correct") == count_of(stats, "top1_correct"));
  REQUIRE(count_of(stats, "top_1000_topall_correct") == count_of(stats, "topall_correct"));

  // And it is a superset of O50, which holds only [0].
  REQUIRE(count_of(stats, "top_1000_top1_correct") >= count_of(stats, "o50_top1_correct"));
  REQUIRE(count_of(stats, "top_1000_keys_w_cardinality_2_2") == count_of(stats, "o50_keys_w_cardinality_2_2"));
  REQUIRE(count_of(stats, "top_1000_keys_w_cardinality_1_1") == 2);
}

TEST_CASE("Successor deltas land in the narrowest signed width that holds them")
{
  // [100] is given four successors at deltas +1, -8, +7 and +8 from its own
  // address. Signed 4-bit two's complement covers exactly [-8, +7], so the
  // first three fit in 4 bits and +8 is the first that does not -- it needs 8.
  // Getting the sign convention wrong (magnitude, or unsigned) moves -8.
  markov_harness uut{"454-delta-narrow"};
  uut.walk({100, 101, 100, 92, 100, 107, 100, 108, 100});

  const auto& stats = uut.publish();

  REQUIRE(count_of(stats, "distinct_keys") == 5);
  // [100] holds all four; the four successors each hold [100] at deltas
  // -1, +8, -7, -8 respectively -- of which +8 needs 8 bits.
  REQUIRE(count_of(stats, "all_delta_4b_candidates") == 6);
  REQUIRE(count_of(stats, "all_delta_8b_candidates") == 2);
  REQUIRE(count_of(stats, "all_delta_16b_candidates") == 0);
  REQUIRE(count_of(stats, "all_delta_wider_candidates") == 0);

  // The buckets partition every stored candidate exactly once.
  int64_t total{0};
  for (const auto* b : {"all_delta_4b_candidates", "all_delta_8b_candidates", "all_delta_16b_candidates", "all_delta_24b_candidates",
                        "all_delta_32b_candidates", "all_delta_wider_candidates"}) {
    total += count_of(stats, b);
  }
  REQUIRE(total == count_of(stats, "sum_cardinality_per_key"));
}

TEST_CASE("The delta keeps its sign, and two's complement is asymmetric")
{
  // Chain, not a round trip, so each edge exists in ONE direction only:
  //   [5000] -> {4872}   delta -128
  //   [4872] -> {4873}   delta +1
  // Signed 8-bit two's complement is [-128, +127]: -128 FITS, +128 does not.
  // So the sign is the whole difference between an 8-bit and a 16-bit slot,
  // and measuring the delta backwards moves that candidate one bucket out.
  markov_harness uut{"454-delta-sign"};
  uut.walk({5000, 4872, 4873});

  const auto& stats = uut.publish();

  REQUIRE(count_of(stats, "distinct_keys") == 2);
  REQUIRE(count_of(stats, "all_delta_4b_candidates") == 1); // +1
  REQUIRE(count_of(stats, "all_delta_8b_candidates") == 1); // -128
  REQUIRE(count_of(stats, "all_delta_16b_candidates") == 0);
}

TEST_CASE("At H=2 the delta is measured from the key's LAST address")
{
  // Key [1000, 100000] -> {100001}. Anchored on the last element the delta is
  // +1; anchored on the first it would be 99001, which needs 17 bits. Only a
  // history longer than 1 can tell those apart -- at H=1 front() and back()
  // are the same element.
  markov_harness uut{"454-delta-anchor", {"cache.llc.generic_markov.history_length=2"}};
  uut.walk({1000, 100000, 100001, 100002});

  const auto& stats = uut.publish();

  REQUIRE(count_of(stats, "distinct_keys") == 2);
  REQUIRE(count_of(stats, "all_delta_4b_candidates") == 2);
  REQUIRE(count_of(stats, "all_delta_16b_candidates") == 0);
  REQUIRE(count_of(stats, "all_delta_24b_candidates") == 0);
}

TEST_CASE("A far successor needs the wide bucket")
{
  // Deltas at each boundary: 2^7 = 128 is the first needing 16 bits, 2^15 the
  // first needing 24, 2^23 the first needing 32, and 2^31 exceeds 32 bits.
  const auto [delta, bucket] = GENERATE(table<int64_t, std::string>({
      {127, "all_delta_8b_candidates"},
      {128, "all_delta_16b_candidates"},
      {32767, "all_delta_16b_candidates"},
      {32768, "all_delta_24b_candidates"},
      {8388607, "all_delta_24b_candidates"},
      {8388608, "all_delta_32b_candidates"},
      {2147483647, "all_delta_32b_candidates"},
      {2147483648, "all_delta_wider_candidates"},
  }));

  // Base is large enough that base-delta stays positive for the return edge.
  const uint64_t base{4000000000ULL};
  markov_harness uut{"454-delta-far-" + std::to_string(delta)};
  uut.walk({base, base + static_cast<uint64_t>(delta), base});

  const auto& stats = uut.publish();

  // Two keys: [base] -> {base+delta} and [base+delta] -> {base}. Both edges
  // have the same magnitude, opposite signs, so both land in `bucket` --
  // except at a boundary that is asymmetric in two's complement, which -2^31
  // is: it FITS in 32 bits while +2^31 does not.
  // The walk makes BOTH edges, +delta and -delta. Two's complement is
  // asymmetric at these boundaries -- -2^n fits in n+1 bits while +2^n does not
  // -- so the negative edge may land one bucket below the positive one.
  REQUIRE(count_of(stats, bucket) >= 1);
  int64_t total{0};
  for (const auto* b : {"all_delta_4b_candidates", "all_delta_8b_candidates", "all_delta_16b_candidates", "all_delta_24b_candidates",
                        "all_delta_32b_candidates", "all_delta_wider_candidates"}) {
    total += count_of(stats, b);
  }
  REQUIRE(total == 2);
}

TEST_CASE("A key that never predicted correctly contributes no coverage")
{
  // [0] is met 5 times and its successor is different every time, so it never
  // predicts correctly -- but it is by far the most frequent key and is the
  // whole of the O50 set. Coverage must be zero, not "the set is big so the
  // number is big".
  markov_harness uut{"454-occ-coverage-barren"};
  uut.walk({0, 1, 0, 2, 0, 3, 0, 4, 0, 5, 0});

  const auto& stats = uut.publish();

  REQUIRE(count_of(stats, "o50_keys") >= 1);
  REQUIRE(count_of(stats, "top1_correct") == 0);
  REQUIRE(count_of(stats, "o50_top1_correct") == 0);
  REQUIRE(count_of(stats, "o80_top1_correct") == 0);
  REQUIRE(rate_of(stats, "o80_top1_coverage") == Catch::Approx(0.0));
}

TEST_CASE("The successor-cardinality bands partition the table")
{
  // Five keys with cardinalities 1, 1, 1, 2, 3 -- the same walk the percentile
  // case uses, so the two pin the same map from different angles:
  //   [5] -> {1,2,3}   [4] -> {1,2}   [1] -> {4}   [2] -> {5}   [3] -> {9}
  // This is also what pins the 1|2 edge, which the width sweep below cannot
  // reach: a one-successor hub is indistinguishable from its own successor.
  markov_harness uut{"454-card-bands"};
  uut.walk({5, 1, 4, 1, 4, 2, 5, 2, 5, 3, 9});

  const auto& stats = uut.publish();

  REQUIRE(count_of(stats, "distinct_keys") == 5);
  REQUIRE(count_of(stats, "keys_w_cardinality_1_1") == 3);
  REQUIRE(count_of(stats, "keys_w_cardinality_2_2") == 1);
  REQUIRE(count_of(stats, "keys_w_cardinality_3_4") == 1);
  for (const auto* empty : {"keys_w_cardinality_5_8", "keys_w_cardinality_9_16", "keys_w_cardinality_17_32", "keys_w_cardinality_33_64"}) {
    REQUIRE(count_of(stats, empty) == 0);
  }

  int64_t total{0};
  for (const auto* band : {"keys_w_cardinality_1_1", "keys_w_cardinality_2_2", "keys_w_cardinality_3_4", "keys_w_cardinality_5_8", "keys_w_cardinality_9_16",
                           "keys_w_cardinality_17_32", "keys_w_cardinality_33_64", "keys_w_cardinality_65_plus"}) {
    total += count_of(stats, band);
  }
  REQUIRE(total == count_of(stats, "distinct_keys"));
}

TEST_CASE("Successor-cardinality bands are closed on both ends")
{
  // A hub with n distinct successors has cardinality n; each successor is
  // followed only by the hub, so all n of them have cardinality 1. Sweeping n
  // across every band edge is what pins the boundaries -- a band that is
  // half-open, or that starts a doubling one early, moves a key at 2, 3, 4, 5,
  // 8, 9, 16, 17, 32 or 33 into the wrong bucket and nowhere else.
  const auto [width, band] = GENERATE(table<uint64_t, std::string>({
      {2, "keys_w_cardinality_2_2"},
      {3, "keys_w_cardinality_3_4"},
      {4, "keys_w_cardinality_3_4"},
      {5, "keys_w_cardinality_5_8"},
      {8, "keys_w_cardinality_5_8"},
      {9, "keys_w_cardinality_9_16"},
      {16, "keys_w_cardinality_9_16"},
      {17, "keys_w_cardinality_17_32"},
      {32, "keys_w_cardinality_17_32"},
      {33, "keys_w_cardinality_33_64"},
      {64, "keys_w_cardinality_33_64"},
      {65, "keys_w_cardinality_65_plus"},
  }));

  markov_harness uut{"454-card-band-edge-" + std::to_string(width)};
  std::vector<uint64_t> walk{};
  for (uint64_t i = 1; i <= width; ++i) {
    walk.push_back(0);
    walk.push_back(i);
  }
  walk.push_back(0);
  uut.walk(walk);

  const auto& stats = uut.publish();

  REQUIRE(count_of(stats, "max_cardinality") == static_cast<int64_t>(width));
  REQUIRE(count_of(stats, "distinct_keys") == static_cast<int64_t>(width) + 1);
  REQUIRE(count_of(stats, "keys_w_cardinality_1_1") == static_cast<int64_t>(width));

  // Exactly one key outside the cardinality-1 band, and it is in `band`.
  for (const auto* candidate : {"keys_w_cardinality_2_2", "keys_w_cardinality_3_4", "keys_w_cardinality_5_8", "keys_w_cardinality_9_16",
                                "keys_w_cardinality_17_32", "keys_w_cardinality_33_64", "keys_w_cardinality_65_plus"}) {
    REQUIRE(count_of(stats, candidate) == (candidate == band ? 1 : 0));
  }
}

TEST_CASE("The miss stream filter keeps hits out of the sequence entirely")
{
  // A filtered access must not enter the window either. If it only skipped
  // training, the sequence would be over lookups-with-gaps rather than over
  // misses, and the correlation learned would be neither.
  constexpr uint64_t A{0x10};
  constexpr uint64_t X{0x20};
  constexpr uint64_t B{0x30};

  markov_harness uut{"454-miss-stream", {"cache.llc.generic_markov.train_on=miss"}};
  uut.access(A, false);
  uut.access(X, true); // a hit -- must be invisible to the model
  uut.access(B, false);

  REQUIRE(uut.pref.table_size() == 1);

  const auto* from_a = uut.pref.lookup({A});
  REQUIRE(from_a != nullptr);
  REQUIRE(std::size(from_a->candidates) == 1);
  REQUIRE(from_a->candidates.front().addr == B);

  REQUIRE(uut.pref.lookup({X}) == nullptr);
}

TEST_CASE("Nothing is prefetched unless the knob asks for it")
{
  constexpr uint64_t A{0x10};
  constexpr uint64_t B{0x20};

  SECTION("The instrument is inert by default")
  {
    markov_harness uut{"454-inert"};
    uut.walk({A, B, A, B, A, B});
    REQUIRE(uut.cache.sim_stats.pf_requested == 0);
  }

  SECTION("The reserved knob really does gate the prefetches")
  {
    markov_harness uut{"454-issuing", {"cache.llc.generic_markov.issue_prefetch=true"}};
    uut.walk({A, B, A, B, A, B});

    // The exact count, not just "more than zero". Four of the six accesses find
    // a key, and each has a single candidate.
    REQUIRE(uut.cache.sim_stats.pf_requested == 4);
  }

  SECTION("Prefetches go to the cacheline address, not the raw block number")
  {
    // generic_markov stores block NUMBERS and has to undo that when it calls
    // prefetch_line. Dropping the block_number wrapper would prefetch a byte
    // address LOG2_BLOCK_SIZE bits too low -- every request landing in block 0
    // -- and a count-only assertion would not notice.
    markov_harness uut{"454-issuing-address", {"cache.llc.generic_markov.issue_prefetch=true"}};

    std::array<champsim::operable*, 3> elements{{&uut.mock_ll, &uut.mock_ul, &uut.cache}};
    for (auto* elem : elements) {
      elem->initialize();
      elem->warmup = false;
      elem->begin_phase();
    }

    uut.walk({A, B, A, B, A, B});

    // Let the prefetches drain out of the internal PQ and miss down to the
    // mock below, which records what it was asked for.
    for (int i = 0; i < 100; ++i) {
      for (auto* elem : elements) {
        elem->_operate();
      }
    }

    REQUIRE_FALSE(std::empty(uut.mock_ll.addresses));
    for (auto seen : uut.mock_ll.addresses) {
      const auto block = champsim::block_number{seen}.to<uint64_t>();
      REQUIRE((block == A || block == B));
    }
  }
}

TEST_CASE("A phase boundary resets the counters but not the model")
{
  // The warmup/ROI split. Counters describe one phase, but the table AND the
  // sliding window carry across: the address stream does not restart at a
  // phase boundary, and clearing either would inject a cold start into the
  // first accesses of the region of interest that no real prefetcher suffers.
  //
  // In a full run this is what makes train_events equal the ROI access count
  // exactly, rather than falling short by the history length.
  constexpr uint64_t A{0x10};
  constexpr uint64_t B{0x20};

  markov_harness uut{"454-phase-boundary"};

  // Three accesses, not two: the third finds [A] in the table and leaves a
  // PENDING prediction across the boundary. With only two, both lookups miss,
  // has_pending is false at the boundary, and the pending lifetime this test
  // names is never exercised -- clearing pending in begin_phase would pass.
  uut.walk({A, B, A});

  uut.pref.prefetcher_begin_phase();
  uut.access(B);

  // The window survived: the pre-boundary [B] could only train against the
  // post-boundary A if it was still in the window.
  const auto* from_b = uut.pref.lookup({B});
  REQUIRE(from_b != nullptr);
  REQUIRE(from_b->candidates.front().addr == A);

  // The table survived: [A] was learned before the boundary and is found after.
  REQUIRE(uut.pref.table_size() == 2);

  const auto& stats = uut.publish();

  // The counters did not survive: one access since the boundary, and because
  // the window was already full it both trained and predicted.
  REQUIRE(count_of(stats, "train_events") == 1);
  REQUIRE(count_of(stats, "predict_attempts") == 1);
  REQUIRE(count_of(stats, "predict_hits") == 1);

  // The pending prediction survived too, and was graded against the first
  // access after the boundary. That is the right call -- the address stream
  // does not restart -- but it is a contract, so pin it: clearing pending in
  // begin_phase would give 0 and 0 here.
  REQUIRE(count_of(stats, "scored_predictions") == 1);
  REQUIRE(count_of(stats, "top1_correct") == 1);

  // distinct_keys is NOT a per-phase number -- it describes the whole-run map.
  // This asymmetry is deliberate and is why it must not be divided by a
  // per-phase counter.
  REQUIRE(count_of(stats, "distinct_keys") == 2);
}

TEST_CASE("A misspelled stream selection is rejected by name")
{
  // Silently falling back to the default stream would produce a plausible
  // result for the wrong experiment.
  do_nothing_MRC mock_ll{};
  to_rq_MRP mock_ul{};
  CACHE cache{champsim::cache_builder{champsim::defaults::default_llc}.name("454-bad-knob").upper_levels({&mock_ul.queues}).lower_level(&mock_ll.queues)};
  generic_markov pref{&cache};

  champsim::runtime_config cfg{};
  cfg.set("cache.llc.generic_markov.train_on=misses");

  // The TYPE matters, not just that it throws: main.cc wraps environment
  // construction in `catch (const std::runtime_error&)`, so a logic_error
  // would escape and abort the process instead of printing a diagnostic.
  REQUIRE_THROWS_AS(pref.configure(cfg, "cache.llc.generic_markov"), std::runtime_error);
}

TEST_CASE("A zero history length is refused rather than silently degenerate")
{
  // Zero would make every key the empty sequence: one entry, perfect
  // recurrence, meaningless numbers.
  do_nothing_MRC mock_ll{};
  to_rq_MRP mock_ul{};
  CACHE cache{champsim::cache_builder{champsim::defaults::default_llc}.name("454-zero-history").upper_levels({&mock_ul.queues}).lower_level(&mock_ll.queues)};
  generic_markov pref{&cache};

  champsim::runtime_config cfg{};
  cfg.set("cache.llc.generic_markov.history_length=0");
  REQUIRE_THROWS_AS(pref.configure(cfg, "cache.llc.generic_markov"), std::runtime_error);
}
