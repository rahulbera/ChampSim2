#include <array>
#include <catch.hpp>
#include <cstdint>

#include "../../../branch/gshare/gshare.h"
#include "../../../branch/hashed_perceptron/hashed_perceptron.h"
#include "../../../branch/perceptron/perceptron.h"
#include "../../../btb/basic_btb/basic_btb.h"
#include "instruction.h"

// The default modules are `shared`: one per-core instance whose tables the
// core's hardware contexts share, while what a real SMT core replicates per
// thread is kept per context: basic_btb's return stack and its indirect
// predictor's conditional history, and the direction predictors' global
// history. Context 0 runs in asid {0,0}. Each isolation case runs the sibling's
// traffic in context 1, where it must leave context 0's answer unchanged, and
// then the same traffic in context 0, where it must change it: that control
// shows the case would catch a copy shared by both contexts. The sibling runs
// in asid {0,1} and in {0,0}, so the asid cannot be what keeps the contexts
// apart. The last case pins the other half: the indirect target table is
// shared.

namespace
{
constexpr champsim::context_index ctx0{0};
constexpr champsim::context_index ctx1{1};
constexpr champsim::address_space_id asid0{0, 0};
constexpr std::array<champsim::address_space_id, 2> sibling_asids{{{0, 1}, {0, 0}}};

// Context 0 calls from 0x1000 into 0x5000 and returns from 0x5010 to 0x1004,
// which teaches the direct BTB that 0x5010 is a return and the return stack the
// 4-byte call size, then calls again. The sibling makes one call from 0x2000
// before context 0's return is predicted. On a stack of its own, context 0's
// return predicts 0x1004; with the sibling's call on top it predicts 0x2004.
std::pair<champsim::address, bool> return_after_sibling_call(champsim::context_index sibling, champsim::address_space_id sibling_asid)
{
  basic_btb uut;
  uut.update_btb(champsim::address{0x1000}, champsim::address{0x5000}, true, BRANCH_DIRECT_CALL, ctx0, asid0);
  uut.update_btb(champsim::address{0x5010}, champsim::address{0x1004}, true, BRANCH_RETURN, ctx0, asid0);
  uut.update_btb(champsim::address{0x1000}, champsim::address{0x5000}, true, BRANCH_DIRECT_CALL, ctx0, asid0);

  uut.update_btb(champsim::address{0x2000}, champsim::address{0x6000}, true, BRANCH_DIRECT_CALL, sibling, sibling_asid);
  return uut.btb_prediction(champsim::address{0x5010}, ctx0, asid0);
}

// Context 0 trains an indirect branch at 0x7000 with an empty conditional
// history; the sibling then resolves one taken conditional branch (a not-taken
// one would leave an empty history at zero, shared or not). With a history of
// its own, context 0 still reads the slot it trained; once its own history
// shifts, its index moves off that slot.
std::pair<champsim::address, bool> indirect_after_sibling_conditional(champsim::context_index sibling, champsim::address_space_id sibling_asid)
{
  basic_btb uut;
  uut.update_btb(champsim::address{0x7000}, champsim::address{0x9abc0}, true, BRANCH_INDIRECT, ctx0, asid0);
  uut.update_btb(champsim::address{0x3000}, champsim::address{0x3100}, true, BRANCH_CONDITIONAL, sibling, sibling_asid);
  return uut.btb_prediction(champsim::address{0x7000}, ctx0, asid0);
}

// Context 0 runs 400 branches at 0x401000 whose outcome alternates, so only its
// own global history predicts them. With `interleave`, each is followed by one
// of the sibling's branches (an LCG over four sites and outcomes), predicted and
// resolved back to back as O3_CPU::do_predict_branch does. Returns context 0's
// predictions.
template <typename Predictor>
std::array<bool, 400> predictions(bool interleave, champsim::context_index sibling, champsim::address_space_id sibling_asid)
{
  constexpr champsim::address ip{0x401000};

  Predictor uut{nullptr};
  std::array<bool, 400> result{};
  uint32_t lcg = 12345;

  for (std::size_t i = 0; i < result.size(); ++i) {
    const bool taken = (i % 2) == 0; // alternating
    result[i] = uut.predict_branch(ip, champsim::address{}, true, BRANCH_CONDITIONAL, ctx0, asid0);
    uut.last_branch_result(ip, champsim::address{}, taken, BRANCH_CONDITIONAL, ctx0, asid0);

    if (interleave) {
      lcg = lcg * 1103515245u + 12345u;
      const champsim::address cip{0x7b0000 + 0x40 * ((lcg >> 8) % 4)};
      const bool ctaken = ((lcg >> 16) & 1) != 0;
      (void)uut.predict_branch(cip, champsim::address{}, true, BRANCH_CONDITIONAL, sibling, sibling_asid);
      uut.last_branch_result(cip, champsim::address{}, ctaken, BRANCH_CONDITIONAL, sibling, sibling_asid);
    }
  }

  return result;
}
} // namespace

TEST_CASE("basic_btb keeps the return stack per context")
{
  for (const auto& sibling_asid : sibling_asids) {
    CAPTURE(static_cast<int>(sibling_asid[1]));
    CHECK(return_after_sibling_call(ctx1, sibling_asid).first == champsim::address{0x1004});

    // Control: the same call made in context 0 is on context 0's stack.
    CHECK(return_after_sibling_call(ctx0, sibling_asid).first == champsim::address{0x2004});
  }
}

TEST_CASE("basic_btb keeps the indirect conditional history per context")
{
  for (const auto& sibling_asid : sibling_asids) {
    CAPTURE(static_cast<int>(sibling_asid[1]));
    CHECK(indirect_after_sibling_conditional(ctx1, sibling_asid).first == champsim::address{0x9abc0});

    // Control: the same conditional resolved in context 0 moves context 0's index.
    CHECK(indirect_after_sibling_conditional(ctx0, sibling_asid).first != champsim::address{0x9abc0});
  }
}

TEMPLATE_TEST_CASE("A branch predictor keeps its global history per context", "", gshare, perceptron, hashed_perceptron)
{
  const auto alone = predictions<TestType>(false, ctx1, asid0);

  for (const auto& sibling_asid : sibling_asids) {
    CAPTURE(static_cast<int>(sibling_asid[1]));
    CHECK(predictions<TestType>(true, ctx1, sibling_asid) == alone);

    // Control: the same branches resolved in context 0 change its predictions.
    CHECK(predictions<TestType>(true, ctx0, sibling_asid) != alone);
  }
}

TEST_CASE("basic_btb's contexts share one indirect target table")
{
  // Context 1 trains an indirect branch; context 0, in the same address space and
  // with an empty conditional history like context 1's, predicts the target
  // context 1 trained. A table replicated per context would leave context 0's
  // slot empty.
  basic_btb uut;
  constexpr champsim::address_space_id asid{0, 0};
  uut.update_btb(champsim::address{0x7000}, champsim::address{0x9abc0}, true, BRANCH_INDIRECT, champsim::context_index{1}, asid);

  const auto [target, taken] = uut.btb_prediction(champsim::address{0x7000}, champsim::context_index{0}, asid);
  CHECK(target == champsim::address{0x9abc0});
  CHECK(taken);
}
