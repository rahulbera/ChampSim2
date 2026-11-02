
/*
 * This file implements a basic Branch Target Buffer (BTB) structure.
 * It uses a set-associative BTB to predict the targets of non-return branches,
 * and it uses a small Return Address Stack (RAS) to predict the target of
 * returns.
 */

#include "basic_btb.h"

#include "instruction.h"

std::pair<champsim::address, bool> basic_btb::btb_prediction(champsim::address ip, champsim::address_space_id asid)
{
  // use BTB for all other branches + direct calls
  auto btb_entry = direct.check_hit(ip, asid);

  // no prediction for this IP
  if (!btb_entry.has_value())
    return {champsim::address{}, false};

  if (btb_entry->type == direct_predictor::branch_info::RETURN)
    return ras[ras_key(asid)].prediction();

  if (btb_entry->type == direct_predictor::branch_info::INDIRECT)
    return indirect.prediction(ip, asid);

  return {btb_entry->target, btb_entry->type != direct_predictor::branch_info::CONDITIONAL};
}

void basic_btb::update_btb(champsim::address ip, champsim::address branch_target, bool taken, uint8_t branch_type, champsim::address_space_id asid)
{
  auto& ras_ = ras[ras_key(asid)];

  // add something to the RAS
  if (branch_type == BRANCH_DIRECT_CALL || branch_type == BRANCH_INDIRECT_CALL)
    ras_.push(ip);

  // updates for indirect branches
  if ((branch_type == BRANCH_INDIRECT) || (branch_type == BRANCH_INDIRECT_CALL))
    indirect.update_target(ip, branch_target, asid);

  if (branch_type == BRANCH_CONDITIONAL)
    indirect.update_direction(taken);

  if (branch_type == BRANCH_RETURN)
    ras_.calibrate_call_size(branch_target);

  direct.update(ip, branch_target, branch_type, asid);
}

void basic_btb::configure(const champsim::runtime_config& cfg, std::string_view prefix)
{
  // The direct predictor's table is runtime-sized; the constexpr defaults are
  // the shipped 64KB-class geometry. Runs before any lookup, so rebuilding the
  // empty table loses nothing.
  // positive_value: lru_table validates sets itself but never ways, and a
  // zero-way table silently never hits -- a sweep point that must fail, not
  // report a degenerate predictor's numbers.
  const auto sets = cfg.positive_value<std::size_t>(std::string{prefix} + ".sets", direct_predictor::sets);
  const auto ways = cfg.positive_value<std::size_t>(std::string{prefix} + ".ways", direct_predictor::ways);
  direct.resize(sets, ways);

  // B3.4: the knob lives under the core's [ooo_cpu.cpu0.bmt] table, which is the
  // module prefix minus its trailing ".basic_btb" component. Consulted here
  // (default off) so a default run keeps the shared RAS.
  const auto dot = prefix.rfind('.');
  const auto core_prefix = (dot == std::string_view::npos) ? std::string_view{} : prefix.substr(0, dot);
  per_virtual_ = cfg.value<bool>(std::string{core_prefix} + ".bmt.per_virtual_small_state", false);
}

std::size_t basic_btb::ras_key(champsim::address_space_id asid) const
{
  // B3.4: key the RAS by the virtual thread only when the per-virtual small state
  // switch is on. Off stays at index 0 == the single shared RAS, byte-identical to
  // the prior layout.
  return per_virtual_ ? asid[0] : 0;
}

uint64_t basic_btb::small_state_fingerprint(champsim::address_space_id asid) const
{
  // FNV-1a over the return-address stack contents. The absolute value is
  // irrelevant; the swap assertion only compares snapshot-then vs snapshot-now.
  const auto& stack = ras[ras_key(asid)].stack;
  uint64_t h = 1469598103934665603ull;
  for (const auto a : stack) {
    h ^= a.to<uint64_t>();
    h *= 1099511628211ull;
  }
  return h;
}
