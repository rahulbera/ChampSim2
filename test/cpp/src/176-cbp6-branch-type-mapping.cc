#include <catch.hpp>

#include "cbp6/cbp6_types.h"
#include "instruction.h"

// CBP2025 predictors receive a `brtype` bitfield rather than an enum. The
// encoding is fixed by the championship's submissions -- see
// submission_44-Koizumi/cond_branch_predictor_interface.cc:67-89, which maps
// InstClass onto cond(1) / indirect(2) / call(4) / return(8). ChampSim's
// branch_type enum has to be projected onto the same encoding, so these values
// are asserted explicitly rather than left implicit in a switch.

TEST_CASE("ChampSim branch types map onto the CBP6 brtype bitfield")
{
  using champsim::cbp6::brtype_of;

  REQUIRE(brtype_of(BRANCH_DIRECT_JUMP) == 0);
  REQUIRE(brtype_of(BRANCH_CONDITIONAL) == 1);
  REQUIRE(brtype_of(BRANCH_INDIRECT) == 2);
  REQUIRE(brtype_of(BRANCH_DIRECT_CALL) == 4);
  REQUIRE(brtype_of(BRANCH_INDIRECT_CALL) == 6); // call | indirect
  REQUIRE(brtype_of(BRANCH_RETURN) == 10);       // return | indirect
}

TEST_CASE("The CBP6 brtype bits compose as documented")
{
  using namespace champsim::cbp6;

  REQUIRE((brtype_of(BRANCH_INDIRECT_CALL) & BRTYPE_CALL) != 0);
  REQUIRE((brtype_of(BRANCH_INDIRECT_CALL) & BRTYPE_INDIRECT) != 0);
  REQUIRE((brtype_of(BRANCH_RETURN) & BRTYPE_RETURN) != 0);
  REQUIRE((brtype_of(BRANCH_RETURN) & BRTYPE_INDIRECT) != 0);
  REQUIRE((brtype_of(BRANCH_CONDITIONAL) & BRTYPE_COND) != 0);
  REQUIRE((brtype_of(BRANCH_DIRECT_JUMP) & BRTYPE_COND) == 0);
}

TEST_CASE("Only conditional branches are direction-predicted")
{
  using champsim::cbp6::is_direction_predicted;

  // CBP6 calls get_cond_dir_prediction only for conditional branches
  // (cbp2025/lib/bp.cc:80-89) and forces pred_taken=true for every other
  // branch class (bp.cc:128,164).
  REQUIRE(is_direction_predicted(BRANCH_CONDITIONAL));

  REQUIRE_FALSE(is_direction_predicted(BRANCH_DIRECT_JUMP));
  REQUIRE_FALSE(is_direction_predicted(BRANCH_INDIRECT));
  REQUIRE_FALSE(is_direction_predicted(BRANCH_DIRECT_CALL));
  REQUIRE_FALSE(is_direction_predicted(BRANCH_INDIRECT_CALL));
  REQUIRE_FALSE(is_direction_predicted(BRANCH_RETURN));
  REQUIRE_FALSE(is_direction_predicted(NOT_BRANCH));
}

TEST_CASE("BRANCH_OTHER is treated as a conditional branch")
{
  // ChampSim's own misprediction check compares direction for BRANCH_OTHER
  // exactly as it does for BRANCH_CONDITIONAL (src/ooo_cpu.cc:151-153), so the
  // adapter predicts it rather than forcing it taken. CBP6 has no counterpart
  // class, so it is mapped onto the conditional encoding.
  REQUIRE(champsim::cbp6::is_direction_predicted(BRANCH_OTHER));
  REQUIRE(champsim::cbp6::brtype_of(BRANCH_OTHER) == champsim::cbp6::BRTYPE_COND);
}

TEST_CASE("Non-branch instructions are not passed to the predictor")
{
  // ChampSim calls predict_branch for EVERY instruction (src/ooo_cpu.cc:135-138),
  // so the adapter must reject non-branches before doing any work: RUNLTS
  // checkpoints history on every predict() and only erases it in update(),
  // which runs for conditional branches alone.
  REQUIRE_FALSE(champsim::cbp6::is_branch(NOT_BRANCH));
  REQUIRE(champsim::cbp6::is_branch(BRANCH_CONDITIONAL));
  REQUIRE(champsim::cbp6::is_branch(BRANCH_RETURN));
}
