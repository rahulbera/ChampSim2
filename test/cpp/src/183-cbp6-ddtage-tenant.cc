#include <catch.hpp>
#include <cstdint>
#include <vector>

#include "cbp6/cbp6_ddtage_tenant.h"

// Alberto Ros's CBP2025 submission (DD-TAGE) names its methods differently from
// the tenant interface champsim::cbp6::host calls, and -- the part that matters
// -- orders two of its arguments the other way round:
//
//   host  -> history_update(seq_no, piece, pc, brtype, PRED_DIR, RESOLVE_DIR, next_pc)
//   Ros   -> historyUpdate(pc, brtype, TAKEN, PRED, next_pc)
//
// so the adapter must pass resolve_dir where the submission expects `taken` and
// pred_dir where it expects `pred`. Swapping them trains the predictor on its
// own prediction rather than the true outcome: accuracy degrades and nothing
// asserts, which is exactly the class of bug that has to be pinned by a test
// rather than by reading the code.
//
// The adapter is a template over its base so the real predictor can be replaced
// here by a base that just records what it was handed.

namespace
{
struct recording_base {
  struct history_call {
    uint64_t pc;
    int brtype;
    bool taken;
    bool pred;
    uint64_t next_pc;
  };
  std::vector<history_call> history_calls;

  void historyUpdate(uint64_t pc, int brtype, bool taken, bool pred, uint64_t next_pc) { history_calls.push_back({pc, brtype, taken, pred, next_pc}); }
};

using tenant = champsim::cbp6::ddtage_tenant<recording_base>;
} // namespace

TEST_CASE("history_update hands the submission the resolved direction as 'taken'")
{
  tenant uut{};

  // Distinguishable: the branch was predicted not-taken and resolved taken.
  uut.history_update(/*seq_no*/ 7, /*piece*/ 0, /*pc*/ 0x400, /*brtype*/ 1, /*pred_dir*/ false, /*resolve_dir*/ true, /*next_pc*/ 0x500);

  REQUIRE(std::size(uut.history_calls) == 1);
  REQUIRE(uut.history_calls.front().taken == true); // the outcome, not the guess
  REQUIRE(uut.history_calls.front().pred == false);
}

TEST_CASE("history_update forwards pc, branch type and next pc unchanged")
{
  tenant uut{};

  uut.history_update(7, 0, 0x400, 2, false, true, 0x500);

  REQUIRE(std::size(uut.history_calls) == 1);
  REQUIRE(uut.history_calls.front().pc == 0x400);
  REQUIRE(uut.history_calls.front().brtype == 2);
  REQUIRE(uut.history_calls.front().next_pc == 0x500);
}

TEST_CASE("TrackOtherInst folds into the same history update")
{
  // The submission has no separate non-conditional path: spec_update() calls
  // historyUpdate() for every branch and selects the conditional case inside,
  // on `brtype & 1`.
  tenant uut{};

  uut.TrackOtherInst(/*pc*/ 0x600, /*brtype*/ 2, /*pred_dir*/ true, /*resolve_dir*/ false, /*next_pc*/ 0x700);

  REQUIRE(std::size(uut.history_calls) == 1);
  REQUIRE(uut.history_calls.front().pc == 0x600);
  REQUIRE(uut.history_calls.front().brtype == 2);
  REQUIRE(uut.history_calls.front().taken == false); // resolve_dir
  REQUIRE(uut.history_calls.front().pred == true);   // pred_dir
  REQUIRE(uut.history_calls.front().next_pc == 0x700);
}

TEST_CASE("terminate is a no-op because the submission has no end-of-run hook")
{
  // endCondDirPredictor() is empty in the submission; the host calls terminate()
  // unconditionally, so the adapter has to supply one.
  tenant uut{};

  uut.terminate();

  REQUIRE(std::empty(uut.history_calls));
}
