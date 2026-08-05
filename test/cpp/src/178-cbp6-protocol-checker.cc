#include <catch.hpp>

#include <cstdint>

#include "cbp6/cbp6_protocol.h"
#include "instruction.h"

using champsim::cbp6::call_kind;
using champsim::cbp6::call_record;
using champsim::cbp6::protocol_checker;

namespace
{
call_record predict_of(uint64_t seq, uint64_t pc, bool pred)
{
  return {call_kind::predict, champsim::cbp6::BRTYPE_COND, pred, false, seq, pc, 0};
}
call_record hist_of(uint64_t seq, uint64_t pc, bool pred, bool taken, uint64_t next_pc)
{
  return {call_kind::history_update, champsim::cbp6::BRTYPE_COND, pred, taken, seq, pc, next_pc};
}
call_record update_of(uint64_t seq, uint64_t pc, bool pred, bool taken, uint64_t next_pc)
{
  return {call_kind::update, champsim::cbp6::BRTYPE_COND, pred, taken, seq, pc, next_pc};
}
call_record track_of(uint64_t pc, int brtype, uint64_t next_pc)
{
  return {call_kind::track_other, brtype, true, true, 0, pc, next_pc};
}
} // namespace

TEST_CASE("A well-formed conditional branch passes the protocol checker")
{
  protocol_checker uut;
  uut.observe(predict_of(1, 0x400, true));
  uut.observe(hist_of(1, 0x400, true, true, 0x500));
  uut.observe(update_of(1, 0x400, true, true, 0x500));

  REQUIRE(uut.finish());
  REQUIRE(std::empty(uut.violations()));
  REQUIRE(uut.conditional_branches() == 1);
}

TEST_CASE("A well-formed unconditional branch passes the protocol checker")
{
  protocol_checker uut;
  uut.observe(track_of(0x400, champsim::cbp6::BRTYPE_RETURN | champsim::cbp6::BRTYPE_INDIRECT, 0x900));

  REQUIRE(uut.finish());
  REQUIRE(uut.other_branches() == 1);
}

TEST_CASE("A predict with no matching update is a checkpoint leak")
{
  // Tenants checkpoint history on every predict() and release it only in
  // update(). A predict that is never updated leaks that checkpoint, and over a
  // long run leaks unbounded memory.
  protocol_checker uut;
  uut.observe(predict_of(1, 0x400, true));
  uut.observe(predict_of(2, 0x408, true));
  uut.observe(hist_of(2, 0x408, true, true, 0x500));
  uut.observe(update_of(2, 0x408, true, true, 0x500));

  REQUIRE_FALSE(uut.finish());
  REQUIRE_FALSE(std::empty(uut.violations()));
}

TEST_CASE("An update whose sequence number does not match its predict is caught")
{
  protocol_checker uut;
  uut.observe(predict_of(1, 0x400, true));
  uut.observe(hist_of(1, 0x400, true, true, 0x500));
  uut.observe(update_of(2, 0x400, true, true, 0x500)); // wrong seq_no

  REQUIRE_FALSE(uut.finish());
}

TEST_CASE("An update for a different PC than its predict is caught")
{
  protocol_checker uut;
  uut.observe(predict_of(1, 0x400, true));
  uut.observe(hist_of(1, 0x408, true, true, 0x500)); // wrong pc

  REQUIRE_FALSE(uut.finish());
}

TEST_CASE("A predicted direction that changes between predict and update is caught")
{
  // The direction reported to ChampSim must be the one the tenant is told it
  // predicted, or the tenant trains against a decision that was never made.
  protocol_checker uut;
  uut.observe(predict_of(1, 0x400, true));
  uut.observe(hist_of(1, 0x400, false, true, 0x500)); // pred_dir flipped

  REQUIRE_FALSE(uut.finish());
}

TEST_CASE("Sequence numbers must strictly increase")
{
  protocol_checker uut;
  uut.observe(predict_of(5, 0x400, true));
  uut.observe(hist_of(5, 0x400, true, true, 0x500));
  uut.observe(update_of(5, 0x400, true, true, 0x500));
  uut.observe(predict_of(5, 0x408, true)); // reused

  REQUIRE_FALSE(uut.finish());
}

TEST_CASE("A not-taken conditional branch must carry a real fall-through address")
{
  // ChampSim's branch_target is zero for a not-taken branch. Forwarding that
  // would make tenants that test `nextPC < PC` treat every not-taken
  // conditional as a backward branch.
  protocol_checker uut;
  uut.observe(predict_of(1, 0x400, false));
  uut.observe(hist_of(1, 0x400, false, false, 0)); // next_pc == 0

  REQUIRE_FALSE(uut.finish());
}

TEST_CASE("TrackOtherInst must not be used for conditional branches")
{
  protocol_checker uut;
  uut.observe(track_of(0x400, champsim::cbp6::BRTYPE_COND, 0x500));

  REQUIRE_FALSE(uut.finish());
}

TEST_CASE("The checker reports how much it saw")
{
  protocol_checker uut;
  for (uint64_t i = 1; i <= 4; ++i) {
    uut.observe(predict_of(i, 0x400, true));
    uut.observe(hist_of(i, 0x400, true, true, 0x500));
    uut.observe(update_of(i, 0x400, true, true, 0x500));
  }
  uut.observe(track_of(0x800, champsim::cbp6::BRTYPE_CALL, 0x900));

  REQUIRE(uut.finish());
  REQUIRE(uut.conditional_branches() == 4);
  REQUIRE(uut.other_branches() == 1);
  REQUIRE(uut.records() == 13);
}

TEST_CASE("TrackOtherInst must carry pred_dir and resolve_dir both true")
{
  // RUNLTS's wrapper forwards these two swapped. That is invisible only while
  // both are true, which holds because the path is unconditional branches.
  protocol_checker uut;
  uut.observe({call_kind::track_other, champsim::cbp6::BRTYPE_RETURN, true, false, 0, 0x400, 0x900});

  REQUIRE_FALSE(uut.finish());
}
