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
