/*
 * DD-TAGE -- the predictor from "A Deep Dive Into TAGE-SC-L", Alberto Ros
 * (Universidad de Murcia), CBP2025 (CBP6) submission -- hosted in ChampSim.
 *
 * Placed 5th of 9 in the championship, at roughly -0.2% BrMisPKI against the
 * CBP6 TAGE-SC-L baseline, in a self-reported storage budget of 191.90 KB
 * (verified: the vendored copy's own predictorsize() reports 191.900269 KB).
 *
 * Unlike RUNLTS, this predictor uses only PC and branch history -- it never
 * touches the CBP2025 register-value channel. It therefore transfers into
 * ChampSim at FULL STRENGTH: there is no analogue of cbp6_runlts_norv's missing
 * component, and a result from this module is the predictor as submitted.
 *
 * The submission's method names differ from the ones champsim::cbp6::host calls,
 * so the tenant is a thin adapter (see the .cc). The differences are naming and
 * argument order only; no predictor logic is changed.
 *
 * SINGLE CORE ONLY, as with the other CBP6 tenants: predictor state lives in
 * namespace-scope globals shared by every instance.
 */

#ifndef BRANCH_CBP6_DDTAGE_H
#define BRANCH_CBP6_DDTAGE_H

#include <cstdint>

#include "address.h"
#include "modules.h"

class cbp6_ddtage : champsim::modules::branch_predictor
{
public:
  using branch_predictor::branch_predictor;

  void initialize_branch_predictor();
  bool predict_branch(champsim::address ip, champsim::address predicted_target, bool always_taken, uint8_t branch_type);
  void last_branch_result(champsim::address ip, champsim::address branch_target, bool taken, uint8_t branch_type);
  void branch_predictor_final_stats();
  void branch_execute_resolve(uint64_t instr_id, champsim::address ip, champsim::address branch_target, bool taken, uint8_t branch_type);
};

#endif
