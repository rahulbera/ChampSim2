/*
 * Hosts a CBP2025 (CBP6) conditional branch predictor inside ChampSim.
 *
 * The tenant predictor is used unmodified; this class supplies the environment
 * it expects. A tenant must provide the method set the championship's
 * submissions implement against (see cbp2025/cond_branch_predictor_interface.cc):
 *
 *     void setup()
 *     bool predict(uint64_t seq_no, uint8_t piece, uint64_t pc)
 *     void history_update(uint64_t seq_no, uint8_t piece, uint64_t pc, int brtype,
 *                         bool pred_dir, bool resolve_dir, uint64_t next_pc)
 *     void TrackOtherInst(uint64_t pc, int brtype, bool pred_dir,
 *                         bool resolve_dir, uint64_t next_pc)
 *     void update(uint64_t seq_no, uint8_t piece, uint64_t pc, bool resolve_dir,
 *                 bool pred_dir, uint64_t next_pc)
 *     void terminate()
 *
 * Timing note: CBP6 splits the update in two -- speculative history right after
 * the prediction (spec_update, cbp2025/lib/bp.cc:112) and the non-speculative
 * table update at the branch's execute cycle, out of program order
 * (cbp2025/lib/uarchsim.cc:352). ChampSim has a single resolution hook, fired at
 * fetch in program order (src/ooo_cpu.cc:166), so the two collapse into one
 * call here. Every branch therefore predicts against tables its immediate
 * predecessor has already updated, which is more favourable than the CBP6
 * environment the tenant was tuned in. Restoring the split needs an
 * execute-time hook in O3_CPU.
 */

#ifndef CBP6_HOST_H
#define CBP6_HOST_H

#include <cstdint>
#include <stdexcept>
#include <string>

#include "address.h"
#include "cbp6/cbp6_types.h"

namespace champsim::cbp6
{
// CBP6 submissions keep their tables in namespace-scope globals rather than in
// the predictor object, so every core's module instance would share one set of
// tables. ChampSim builds one module per core and would report silently wrong
// numbers; refuse instead. Call with the owning core's index.
inline void require_single_core(uint32_t cpu_index, const char* predictor_name)
{
  if (cpu_index != 0) {
    throw std::runtime_error{std::string{predictor_name} + " keeps its state in namespace-scope globals and cannot be used on more than one core"};
  }
}

template <typename Tenant>
class host
{
  Tenant tenant_{};

  // ChampSim's instr_id is not passed to branch hooks, but predict and resolve
  // fire back to back for the same instruction inside do_predict_branch, so a
  // local counter identifies a dynamic branch just as well -- and avoids
  // reaching into the core's instruction queue to read one.
  uint64_t seq_no_{0};
  bool last_prediction_{false};

public:
  [[nodiscard]] Tenant& tenant() { return tenant_; }
  [[nodiscard]] const Tenant& tenant() const { return tenant_; }

  void initialize() { tenant_.setup(); }
  void finish() { tenant_.terminate(); }

  // Returns the direction to report to ChampSim.
  bool predict(champsim::address ip, uint8_t branch_type)
  {
    if (!is_direction_predicted(branch_type)) {
      // Matches CBP6, which asks the tenant only about conditional branches and
      // forces pred_taken = true for every other class (lib/bp.cc:128,164).
      // Skipping the call also matters for correctness: tenants checkpoint
      // history on every predict() and only release it in update(), which runs
      // for conditional branches alone.
      return true;
    }

    ++seq_no_;
    last_prediction_ = tenant_.predict(seq_no_, 0, ip.to<uint64_t>());
    return last_prediction_;
  }

  // `next_pc` must be the architectural next PC: the branch target when taken,
  // the fall-through when not. Passing ChampSim's branch_target directly is
  // wrong for a not-taken branch, where it is zero -- tenants test `nextPC < PC`
  // to detect backward (loop) branches, so zero makes every not-taken
  // conditional look like a loop.
  void resolve(champsim::address ip, bool taken, uint8_t branch_type, champsim::address next_pc)
  {
    if (!is_branch(branch_type)) {
      return;
    }

    const auto pc = ip.to<uint64_t>();
    const auto npc = next_pc.to<uint64_t>();
    const int brtype = brtype_of(branch_type);

    if (is_direction_predicted(branch_type)) {
      tenant_.history_update(seq_no_, 0, pc, brtype, last_prediction_, taken, npc);
      tenant_.update(seq_no_, 0, pc, taken, last_prediction_, npc);
    } else {
      tenant_.TrackOtherInst(pc, brtype, true, taken, npc);
    }
  }
};
} // namespace champsim::cbp6

#endif
