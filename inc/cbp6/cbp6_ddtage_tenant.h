/*
 * Presents Alberto Ros's CBP2025 submission (DD-TAGE, "A Deep Dive Into
 * TAGE-SC-L") through the tenant interface champsim::cbp6::host calls.
 *
 * The submission's predictor class supplies setup(), predict() and update()
 * with signatures the host already matches, but names its history hook
 * historyUpdate(), has no TrackOtherInst() and no terminate(). This fills those
 * three gaps. Naming and argument order only -- no predictor logic is added,
 * removed or reordered, and every call made here appears in the submission's own
 * cond_branch_predictor_interface.cc in the same place.
 *
 * The argument order is the reason this is a separate, tested header rather than
 * three lines in the module:
 *
 *   host  -> history_update(seq_no, piece, pc, brtype, PRED_DIR, RESOLVE_DIR, next_pc)
 *   Ros   -> historyUpdate(pc, brtype, TAKEN, PRED, next_pc)
 *
 * The two middle arguments are swapped. Passing them straight through would
 * train the predictor on its own prediction instead of the resolved direction --
 * accuracy degrades, nothing asserts. test/cpp/src/183-cbp6-ddtage-tenant.cc
 * pins the mapping.
 *
 * Templated on the base so that test can substitute a base that records what it
 * was handed; the module instantiates it over the vendored predictor.
 */

#ifndef CBP6_DDTAGE_TENANT_H
#define CBP6_DDTAGE_TENANT_H

#include <cstdint>

namespace champsim::cbp6
{
template <typename Base>
struct ddtage_tenant : Base {
  // setup(), predict(seq_no, piece, pc) and
  // update(seq_no, piece, pc, resolve_dir, pred_dir, next_pc) are inherited
  // unchanged -- the submission already spells those the way the host calls
  // them, including update()'s (resolveDir, pred) order.

  void history_update(uint64_t seq_no, uint8_t piece, uint64_t pc, int brtype, bool pred_dir, bool resolve_dir, uint64_t next_pc)
  {
    (void)seq_no;
    (void)piece; // the submission's historyUpdate is not sequence-numbered
    Base::historyUpdate(pc, brtype, resolve_dir, pred_dir, next_pc);
  }

  // The submission has no separate non-conditional path: its spec_update() calls
  // historyUpdate() for every branch, conditional or not, and selects the
  // conditional case inside historyUpdate on `brtype & 1`. Folding that in here
  // rather than in the host keeps the host's contract uniform across tenants.
  void TrackOtherInst(uint64_t pc, int brtype, bool pred_dir, bool resolve_dir, uint64_t next_pc)
  {
    Base::historyUpdate(pc, brtype, resolve_dir, pred_dir, next_pc);
  }

  // The submission's endCondDirPredictor() is empty -- nothing is flushed or
  // freed at end of run -- but the host calls terminate() unconditionally.
  void terminate() {}
};
} // namespace champsim::cbp6

#endif
