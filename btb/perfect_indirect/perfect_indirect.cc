// ChampSim's SET_ASIDE_CHAMPSIM_MODULE mechanism is not re-entrant; performing
// the set-aside here, at the outermost level, avoids the nesting. See
// branch/cbp6_tagescl64/cbp6_tagescl64.cc for the full explanation.
#undef CHAMPSIM_MODULE
#include "ooo_cpu.h"
#define CHAMPSIM_MODULE

#include "instruction.h"
#include "perfect_indirect.h"

std::pair<champsim::address, bool> perfect_indirect::btb_prediction(champsim::address ip)
{
  auto btb_entry = direct.check_hit(ip);

  // No prediction for this IP. Deliberately NOT overridden: the oracle replaces
  // the indirect *target* predictor, not the BTB's knowledge that a given PC is
  // a branch at all. Overriding this would fold direct-BTB capacity into the
  // measurement and overstate what an indirect predictor can deliver.
  if (!btb_entry.has_value()) {
    return {champsim::address{}, false};
  }

  if (btb_entry->type == perfect_indirect_impl::direct_predictor::branch_info::RETURN) {
    return ras.prediction();
  }

  if (btb_entry->type == perfect_indirect_impl::direct_predictor::branch_info::INDIRECT) {
    // The instruction under prediction is input_queue.front() -- see
    // branch/perfect_branch/perfect_branch.h for why that holds. always_taken is
    // reported true exactly as basic_btb's indirect predictor does, so this
    // module is safe underneath any direction predictor.
    if (intern_ != nullptr && !std::empty(intern_->input_queue)) {
      return {intern_->input_queue.front().branch_target, true};
    }
    return {champsim::address{}, true};
  }

  return {btb_entry->target, btb_entry->type != perfect_indirect_impl::direct_predictor::branch_info::CONDITIONAL};
}

void perfect_indirect::update_btb(champsim::address ip, champsim::address branch_target, bool taken, uint8_t branch_type)
{
  if (branch_type == BRANCH_DIRECT_CALL || branch_type == BRANCH_INDIRECT_CALL) {
    ras.push(ip);
  }

  // No indirect-target update: the oracle needs no training.

  if (branch_type == BRANCH_RETURN) {
    ras.calibrate_call_size(branch_target);
  }

  direct.update(ip, branch_target, branch_type);
}
