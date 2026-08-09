#include <catch.hpp>
#include <cstdint>

#include "instruction.h"
#include "ittage/ittage_btb.h"

// The ITTAGE BTB adapter. These tests cover the wiring and the guards, NOT the
// predictor's numerical behaviour -- that is Seznec's code, and the property
// that our parameterisation did not change it is established separately and far
// more strongly by tools/ittage_equiv, which requires bit-identical predictions
// against the pristine reference over 1.2M indirect branches.
//
// Stated explicitly because it is easy to over-read a green test file here: a
// wrong-but-plausible predictor would pass everything below. The two things
// that would catch that are the equivalence harness and the perfect_indirect
// oracle bound, not these tests.

namespace
{
struct test_cfg {
  static constexpr int NHIST = 7, LOGG = 9, TBITS = 11;
  static constexpr int MINHIST = 2, MAXHIST = 300;
  static constexpr int CWIDTH = 3, UWIDTH = 2;
};
using impl = champsim::ittage::btb_impl<test_cfg>;

constexpr champsim::address BR{0x4000};
constexpr champsim::address TARGET{0x8000};
constexpr champsim::address FALLTHROUGH{0x4004};
} // namespace

TEST_CASE("The storage budget is what the configuration claims")
{
  // An entry holds a truncated target plus tag, confidence and useful bits. The
  // uint64_t the simulator stores is a simulation artifact and is excluded --
  // if that convention ever changes, the reported budgets change with it, so it
  // is pinned here as well as in the module.
  STATIC_REQUIRE(impl::target_bits == 47);
  STATIC_REQUIRE(impl::entry_bits == 47 + 3 + 11 + 2);
  STATIC_REQUIRE(impl::entry_bits == 63);

  // 8 tables x 512 entries x 63 bits = 31.5 KB
  STATIC_REQUIRE(impl::storage_bits == 8L * 512L * 63L);
  REQUIRE(impl::storage_kb == Catch::Approx(31.5));
}

TEST_CASE("An unknown IP yields no prediction")
{
  impl uut{};
  uut.initialize();

  const auto [target, always_taken] = uut.prediction(BR);
  REQUIRE(target == champsim::address{});
  REQUIRE_FALSE(always_taken);
}

TEST_CASE("An indirect branch is answered by ITTAGE, never by the BTB's stale target")
{
  // The design decision under test: on an indirect branch the adapter reports
  // whatever ITTAGE says, including "nothing", rather than falling back to the
  // target the direct predictor happens to hold. Falling back would let
  // ITTAGE's measured accuracy borrow from the BTB and overstate it.
  impl uut{};
  uut.initialize();

  // Install the branch so the direct predictor knows its IP and type, and so it
  // holds TARGET as its own stored target.
  uut.update(BR, TARGET, BRANCH_INDIRECT, TARGET);

  const auto [target, always_taken] = uut.prediction(BR);

  // Whatever comes back must not be the direct predictor's copy of TARGET
  // delivered through the non-indirect path. Either ITTAGE has learned the
  // target (and reports always_taken), or it has not and reports nothing.
  if (target == champsim::address{}) {
    REQUIRE_FALSE(always_taken); // "no prediction"
  } else {
    REQUIRE(always_taken); // an indirect branch is predicted taken
  }
}

TEST_CASE("The unseeded-entry sentinel never escapes as a target")
{
  // ientry() initialises target = 0xdeadbeef. An entry that tag-matches before
  // it has ever been written would otherwise hand ChampSim that value, which
  // does not announce itself as wrong -- it merely mispredicts.
  impl uut{};
  uut.initialize();

  for (uint64_t ip = 0x4000; ip < 0x4000 + 4096; ip += 16) {
    const champsim::address addr{ip};
    uut.update(addr, TARGET, BRANCH_INDIRECT, TARGET);
    const auto [target, always_taken] = uut.prediction(addr);
    REQUIRE(target != champsim::address{champsim::ittage::unseeded_target_sentinel});
  }
}

TEST_CASE("Returns go to the return stack, not to ITTAGE")
{
  impl uut{};
  uut.initialize();

  constexpr champsim::address CALL{0x5000};
  constexpr champsim::address RET{0x9100};

  // Teach the direct predictor that RET is a return. This also consumes the RAS
  // entry, because calibrate_call_size() pops -- in a real run the prediction
  // for a given return happens BEFORE its update, so the pop is correct
  // behaviour and the test has to respect that ordering.
  uut.update(CALL, champsim::address{0x9000}, BRANCH_DIRECT_CALL, champsim::address{0x9000});
  uut.update(RET, champsim::address{0x5004}, BRANCH_RETURN, champsim::address{0x5004});

  // Now the realistic sequence: a call is seen, and the next return is predicted.
  uut.update(CALL, champsim::address{0x9000}, BRANCH_DIRECT_CALL, champsim::address{0x9000});
  const auto [target, always_taken] = uut.prediction(RET);
  REQUIRE(always_taken); // the RAS always reports taken
  // The RAS predicts call_ip + call size, i.e. near CALL -- definitively not
  // anywhere ITTAGE would have pointed.
  REQUIRE(target.to<uint64_t>() >= CALL.to<uint64_t>());
  REQUIRE(target.to<uint64_t>() <= CALL.to<uint64_t>() + 16);
}

TEST_CASE("A conditional branch keeps the direct predictor's target and direction")
{
  impl uut{};
  uut.initialize();

  uut.update(BR, TARGET, BRANCH_CONDITIONAL, TARGET);

  const auto [target, always_taken] = uut.prediction(BR);
  REQUIRE(target == TARGET);
  REQUIRE_FALSE(always_taken); // conditionals are not always-taken
}

TEST_CASE("A not-taken branch feeds the fall-through, not a zero, into the history")
{
  // ChampSim's branch_target is zero for a not-taken branch, and the vendored
  // HistoryUpdate derives its path history solely from that argument
  // (PATH = (target >> 2) ^ (target >> 6)). Passing zero would inject a
  // constant into phist and corrupt every subsequent index.
  //
  // Observed in predictor state rather than through prediction(): a predictor
  // that has learned nothing answers "no prediction" whatever history it holds,
  // so comparing predictions here would pass even if next_pc were ignored --
  // which is exactly how the first version of this test fooled itself.
  impl with_fallthrough{};
  impl with_zero{};
  with_fallthrough.initialize();
  with_zero.initialize();

  for (uint64_t i = 0; i < 64; ++i) {
    const champsim::address ip{0x4000 + 16 * i};
    with_fallthrough.update(ip, champsim::address{}, BRANCH_CONDITIONAL, FALLTHROUGH);
    with_zero.update(ip, champsim::address{}, BRANCH_CONDITIONAL, champsim::address{});
  }

  INFO("if these match, next_pc is being ignored and branch_target's zero reached the history");
  REQUIRE(with_fallthrough.predictor_state().phist != with_zero.predictor_state().phist);
}

TEST_CASE("An indirect branch feeds its real target into the history")
{
  // The complement: for a taken indirect branch the target IS the architectural
  // next PC, and it must reach the history.
  impl a{};
  impl b{};
  a.initialize();
  b.initialize();

  a.update(BR, TARGET, BRANCH_INDIRECT, TARGET);
  b.update(BR, champsim::address{0xC000}, BRANCH_INDIRECT, champsim::address{0xC000});

  REQUIRE(a.predictor_state().phist != b.predictor_state().phist);
}
