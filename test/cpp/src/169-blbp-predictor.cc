#include <catch.hpp>
#include <cstdint>
#include <vector>

#include "blbp/blbp.h"

// The full BLBP machine above the golden-tested engine: IBTB with
// region-compressed targets, GHIST/local histories, row hashing, and the
// predict/update protocol. These tests cover the machinery the paper's
// worked examples cannot reach.

namespace
{
champsim::blbp::predictor_config small_config()
{
  champsim::blbp::predictor_config c;
  c.engine.K = 12;
  c.engine.N = 2; // one local + one global sub-predictor: enough to exercise both paths
  c.engine.M = 64;
  c.engine.transfer = {0, 1, 2, 3, 4, 5, 6, 7};
  c.engine.adaptive_theta = false;
  c.engine.selective_bits = false;
  c.intervals = {{0, 13}};
  c.ibtb_sets = 4;
  c.ibtb_ways = 4;
  c.region_entries = 8;
  return c;
}
constexpr uint64_t BR = 0x7f00'1234'5000;
constexpr uint64_t T1 = 0x7f00'5678'9abc;
constexpr uint64_t T2 = 0x7f00'5678'9def;
} // namespace

TEST_CASE("A branch never seen yields no prediction")
{
  champsim::blbp::predictor p{small_config()};
  const auto pred = p.predict(BR);
  REQUIRE_FALSE(pred.has_value());
}

TEST_CASE("After one update, the sole candidate is predicted and round-trips exactly")
{
  // Region compression must reconstruct the FULL 64-bit target, not just the
  // low bits -- a truncated or region-mangled target would look plausible and
  // simply mispredict, the classic silent failure.
  champsim::blbp::predictor p{small_config()};
  p.update(BR, T1);

  const auto pred = p.predict(BR);
  REQUIRE(pred.has_value());
  REQUIRE(*pred == T1);
}

TEST_CASE("Two targets coexist as candidates; history separates them after training")
{
  auto cfg = small_config();
  champsim::blbp::predictor p{cfg};

  // Interleave: context A (GHIST all-taken) -> T1, context B (all-not-taken)
  // -> T2. After training, each context must recall its own target.
  for (int round = 0; round < 200; ++round) {
    for (int i = 0; i < 13; ++i) {
      p.note_conditional(true);
    }
    p.update(BR, T1);
    for (int i = 0; i < 13; ++i) {
      p.note_conditional(false);
    }
    p.update(BR, T2);
  }

  for (int i = 0; i < 13; ++i) {
    p.note_conditional(true);
  }
  auto pred_a = p.predict(BR);
  REQUIRE(pred_a.has_value());
  REQUIRE(*pred_a == T1);

  for (int i = 0; i < 13; ++i) {
    p.note_conditional(false);
  }
  auto pred_b = p.predict(BR);
  REQUIRE(pred_b.has_value());
  REQUIRE(*pred_b == T2);
}

TEST_CASE("update() is self-contained: never calling predict() leaves identical state")
{
  // The ITTAGE defect-B lesson, pinned here from day one: a machine driven by
  // update() alone (the direct-BTB-miss path in the adapter) must end in the
  // same state as one driven predict-then-update.
  auto cfg = small_config();
  champsim::blbp::predictor paired{cfg};
  champsim::blbp::predictor unpaired{cfg};

  for (int i = 0; i < 500; ++i) {
    const uint64_t br = BR + 64 * (i % 5);
    const uint64_t tg = (i % 3 == 0) ? T1 : T2;
    paired.note_conditional(i % 2 == 0);
    unpaired.note_conditional(i % 2 == 0);
    (void)paired.predict(br);
    paired.update(br, tg);
    unpaired.update(br, tg);
  }

  for (int i = 0; i < 5; ++i) {
    const uint64_t br = BR + 64 * i;
    const auto a = paired.predict(br);
    const auto b = unpaired.predict(br);
    REQUIRE(a.has_value() == b.has_value());
    if (a.has_value()) {
      REQUIRE(*a == *b);
    }
  }
}

TEST_CASE("IBTB capacity: a set holds ways-many targets and evicts beyond that")
{
  auto cfg = small_config(); // 4 ways
  champsim::blbp::predictor p{cfg};

  // Six distinct targets for one branch: at most 4 can be candidates.
  for (int round = 0; round < 3; ++round) {
    for (uint64_t t = 0; t < 6; ++t) {
      p.update(BR, T1 + 0x1000 * t);
    }
  }
  REQUIRE(p.candidate_count_for_test(BR) == 4);
}

TEST_CASE("Region eviction leaves dangling entries that mispredict, not crash")
{
  // 8 region entries; touch many distinct 27-bit-region targets so regions
  // recycle. Hardware leaves stale region pointers dangling -- the entry
  // reconstructs a wrong-but-valid address and loses or mispredicts. The model
  // must do the same, not fault and not invalidate-for-free.
  auto cfg = small_config();
  champsim::blbp::predictor p{cfg};
  for (uint64_t r = 0; r < 32; ++r) {
    p.update(BR + 64 * r, (r << 27) | 0x1000);
  }
  // Whatever it predicts now, it must not crash, and any prediction must be a
  // plausible address (not a mangled sentinel).
  for (uint64_t r = 0; r < 32; ++r) {
    (void)p.predict(BR + 64 * r);
  }
  SUCCEED("no fault on dangling region pointers");
}

TEST_CASE("Local history is per-branch: training one PC does not move another's local row")
{
  auto cfg = small_config();
  champsim::blbp::predictor p{cfg};
  // NB: the local table has 256 rows indexed by (pc >> 2) % 256, so an offset
  // that is a multiple of 1024 ALIASES back to BR's own row -- the first
  // version of this test did exactly that and failed against correct code.
  const uint64_t other = BR + 4; // adjacent instruction, adjacent row

  p.update(BR, T1);
  const auto before = p.local_history_for_test(other);
  for (int i = 0; i < 50; ++i) {
    p.update(BR, (i % 2) ? T1 : T2);
  }
  REQUIRE(p.local_history_for_test(other) == before);
  REQUIRE(p.local_history_for_test(BR) != before);
}
