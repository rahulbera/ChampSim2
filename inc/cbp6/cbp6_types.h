/*
 * Shared vocabulary for hosting CBP2025 (CBP6) branch predictors in ChampSim.
 *
 * CBP6 submissions are written against the interface in cbp2025/cbp.h, which
 * differs from ChampSim's branch-predictor module API in both shape and
 * timing. This header holds the parts of the translation that carry semantic
 * content -- the branch-type encoding and the gating rules -- so they can be
 * tested independently of any particular predictor.
 */

#ifndef CBP6_TYPES_H
#define CBP6_TYPES_H

#include <cstdint>

#include "instruction.h"

namespace champsim::cbp6
{
// CBP6 predictors take a `brtype` bitfield rather than an enum. The encoding is
// set by the championship submissions; see
// submission_44-Koizumi/cond_branch_predictor_interface.cc:67-89.
inline constexpr int BRTYPE_COND = 1;
inline constexpr int BRTYPE_INDIRECT = 2;
inline constexpr int BRTYPE_CALL = 4;
inline constexpr int BRTYPE_RETURN = 8;

// Whether ChampSim considers this instruction a branch at all. ChampSim calls
// predict_branch for every instruction, branch or not (src/ooo_cpu.cc:135-138),
// so the adapter must filter before touching the tenant predictor.
[[nodiscard]] inline constexpr bool is_branch(uint8_t branch_type) { return branch_type != NOT_BRANCH; }

// Whether the tenant predictor should be asked for a direction. CBP6 calls
// get_cond_dir_prediction only for conditional branches (cbp2025/lib/bp.cc:80-89)
// and forces pred_taken = true for every other branch class (bp.cc:128,164).
//
// BRANCH_OTHER is included because ChampSim's own misprediction check compares
// direction for it exactly as for BRANCH_CONDITIONAL (src/ooo_cpu.cc:151-153);
// forcing it taken would diverge from how ChampSim scores it.
[[nodiscard]] inline constexpr bool is_direction_predicted(uint8_t branch_type)
{
  return branch_type == BRANCH_CONDITIONAL || branch_type == BRANCH_OTHER;
}

// Project ChampSim's branch_type enum onto the CBP6 brtype bitfield.
[[nodiscard]] inline constexpr int brtype_of(uint8_t branch_type)
{
  switch (branch_type) {
  case BRANCH_CONDITIONAL:
  case BRANCH_OTHER: // no CBP6 counterpart; scored as a conditional by ChampSim
    return BRTYPE_COND;
  case BRANCH_INDIRECT:
    return BRTYPE_INDIRECT;
  case BRANCH_DIRECT_CALL:
    return BRTYPE_CALL;
  case BRANCH_INDIRECT_CALL:
    return BRTYPE_CALL | BRTYPE_INDIRECT;
  case BRANCH_RETURN:
    return BRTYPE_RETURN | BRTYPE_INDIRECT;
  case BRANCH_DIRECT_JUMP:
  default:
    return 0;
  }
}
} // namespace champsim::cbp6

#endif
