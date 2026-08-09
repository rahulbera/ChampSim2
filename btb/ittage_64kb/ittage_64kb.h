/*
 * ITTAGE indirect target predictor, paper-class budget.
 *
 * A configuration shell: it names the storage parameters and forwards to the
 * shared adapter in inc/ittage/ittage_btb.h, which is where all the logic and
 * all the failure-mode guards live. The predictor itself is Seznec's, vendored
 * once at inc/ittage/ittage.hpp.
 *
 * Storage: 63.0 KB by the accounting in inc/ittage/ittage_btb.h (47-bit target +
 * tag + confidence + useful bit per entry; the simulator's uint64_t is an
 * artifact and excluded). Asserted at compile time in the .cc.
 */

#ifndef BTB_ITTAGE_64KB_H
#define BTB_ITTAGE_64KB_H

#include <cstdint>
#include <utility>

#include "address.h"
#include "modules.h"

class ittage_64kb : champsim::modules::btb
{
public:
  using btb::btb;
  ittage_64kb() : btb(nullptr) {}

  void initialize_btb();
  std::pair<champsim::address, bool> btb_prediction(champsim::address ip);
  void update_btb(champsim::address ip, champsim::address branch_target, bool taken, uint8_t branch_type);
};

#endif
