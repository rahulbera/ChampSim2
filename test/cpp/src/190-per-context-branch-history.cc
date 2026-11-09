#include <array>
#include <catch.hpp>
#include <cstdint>

#include "../../../branch/gshare/gshare.h"
#include "../../../branch/hashed_perceptron/hashed_perceptron.h"
#include "../../../branch/perceptron/perceptron.h"
#include "../../../btb/basic_btb/basic_btb.h"
#include "instruction.h"

// The branch direction predictors keep their global history (and
// basic_btb its return stack and indirect conditional history) per hardware
// context, while the predictor/table storage stays one shared per-core
// instance whose index folds the asid. Interleaving a second context's activity
// between a context's own training and its prediction must therefore leave that
// prediction unchanged; were the history shared, the second context's branches
// would mix their outcomes into the first context's history and change its
// answer. The scenarios below hold whether the two siblings run in the same
// address space {0,0} or different ones {0,1}, which is why the asid is an
// argument rather than a fixed {0,0}.

namespace
{
// Train ctx0's call/return pair (the call pushes 0x1000, the return learns the
// 4-byte call size and re-primes the stack), then place a ctx1 call at 0x2000
// between them. ctx0's return must still read its own stack and predict 0x1004;
// a shared return stack would have 0x2000 on top and predict 0x2004.
void check_return_stack_isolation(champsim::address_space_id asid)
{
  basic_btb uut;

  uut.update_btb(champsim::address{0x1000}, champsim::address{0x5000}, true, BRANCH_DIRECT_CALL, champsim::context_index{0}, asid);
  uut.update_btb(champsim::address{0x5010}, champsim::address{0x1004}, true, BRANCH_RETURN, champsim::context_index{0}, asid);
  uut.update_btb(champsim::address{0x1000}, champsim::address{0x5000}, true, BRANCH_DIRECT_CALL, champsim::context_index{0}, asid);

  uut.update_btb(champsim::address{0x2000}, champsim::address{0x6000}, true, BRANCH_DIRECT_CALL, champsim::context_index{1}, asid);

  const auto [target, taken] = uut.btb_prediction(champsim::address{0x5010}, champsim::context_index{0}, asid);
  CHECK(target == champsim::address{0x1004});
  CHECK(taken);
}

// Train ctx0's indirect target, then let a ctx1 conditional shift only its own
// conditional history. ctx0's indirect prediction must be unaffected; a shared
// conditional history would change the hash ctx0 reads.
void check_indirect_isolation(champsim::address_space_id asid)
{
  basic_btb uut;

  uut.update_btb(champsim::address{0x7000}, champsim::address{0x9abc0}, true, BRANCH_INDIRECT, champsim::context_index{0}, asid);
  uut.update_btb(champsim::address{0x8000}, champsim::address{}, false, BRANCH_CONDITIONAL, champsim::context_index{1}, asid);

  const auto [target, taken] = uut.btb_prediction(champsim::address{0x7000}, champsim::context_index{0}, asid);
  CHECK(target == champsim::address{0x9abc0});
  CHECK(taken);
}

// Run 400 alternating branches at 0x401000 in ctx0, optionally interleaving an
// LCG stream (four branch sites) in ctx1. The ctx0 predictions are returned for
// comparison; with per-context history they are identical whether or not ctx1
// runs beside them.
template <typename Predictor>
std::array<bool, 400> run_predictions(bool interleave)
{
  constexpr champsim::address ip{0x401000};
  constexpr champsim::address_space_id asid{0, 0};

  Predictor uut{nullptr};
  std::array<bool, 400> predictions{};
  uint32_t lcg = 12345;

  for (std::size_t i = 0; i < predictions.size(); ++i) {
    const bool taken = (i % 2) == 0; // alternating
    predictions[i] = uut.predict_branch(ip, champsim::address{}, true, BRANCH_CONDITIONAL, champsim::context_index{0}, asid);
    uut.last_branch_result(ip, champsim::address{}, taken, BRANCH_CONDITIONAL, champsim::context_index{0}, asid);

    if (interleave) {
      lcg = lcg * 1103515245u + 12345u;
      const champsim::address cip{0x7b0000 + 0x40 * ((lcg >> 8) % 4)};
      const bool ctaken = (lcg >> 16) & 1;
      (void)uut.predict_branch(cip, champsim::address{}, true, BRANCH_CONDITIONAL, champsim::context_index{1}, asid);
      uut.last_branch_result(cip, champsim::address{}, ctaken, BRANCH_CONDITIONAL, champsim::context_index{1}, asid);
    }
  }

  return predictions;
}

template <typename Predictor>
void check_history_isolation()
{
  const auto isolated = run_predictions<Predictor>(false);
  const auto polluted = run_predictions<Predictor>(true);

  // The 400 predictions are compared in four 100-branch chunks.
  for (std::size_t chunk = 0; chunk < 4; ++chunk) {
    bool identical = true;
    for (std::size_t i = chunk * 100; i < (chunk + 1) * 100; ++i)
      identical = identical && (isolated[i] == polluted[i]);
    CHECK(identical);
  }
}
} // namespace

TEST_CASE("basic_btb keeps the return stack per context")
{
  check_return_stack_isolation(champsim::address_space_id{0, 0});
  check_return_stack_isolation(champsim::address_space_id{0, 1});
}

TEST_CASE("basic_btb keeps the indirect conditional history per context")
{
  check_indirect_isolation(champsim::address_space_id{0, 0});
  check_indirect_isolation(champsim::address_space_id{0, 1});
}

TEMPLATE_TEST_CASE("A branch predictor keeps its global history per context", "", gshare, perceptron, hashed_perceptron)
{
  check_history_isolation<TestType>();
}
