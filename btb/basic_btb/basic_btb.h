#ifndef BTB_BASIC_BTB_H
#define BTB_BASIC_BTB_H

#include <array>
#include <cstdint>

#include "address.h"
#include "direct_predictor.h"
#include "indirect_predictor.h"
#include "modules.h"
#include "return_stack.h"
#include "runtime_config.h"

class basic_btb : champsim::modules::btb
{
  // B3.4 per-virtual small state: the RAS (return_stack, i.e. its address stack
  // plus the call-size calibration table) is keyed by asid[0] (the virtual
  // thread's trace slot) when the core's bmt.per_virtual_small_state is on, so
  // each virtual context keeps its own return history across a swap-out/swap-in.
  // Default (off) index is 0 == a single shared RAS, byte-identical to the prior
  // layout. The BTB body (direct/indirect predictors) stays shared. Sized to the
  // full uint8_t asid[0] range.
  static constexpr std::size_t NUM_ASID_KEYS = 1u << 8;
  std::array<return_stack, NUM_ASID_KEYS> ras{};

  indirect_predictor indirect{};
  direct_predictor direct{};

  // B3.4 per-virtual small state switch, read from the core's [ooo_cpu.cpu0.bmt]
  // per_virtual_small_state knob in configure(). Off keeps the single shared RAS;
  // on keys it by asid[0]. The module reads the knob itself (rather than the core)
  // because a BTB module cannot see O3_CPU's members.
  bool per_virtual_ = false;

  // B3.4 key: asid[0] when per_virtual_ is on, else 0.
  std::size_t ras_key(champsim::address_space_id asid) const;

public:
  using btb::btb;
  basic_btb() : btb(nullptr) {}

  // SMT: single per-core instance, keyed by ASID (direct tag + indirect hash fold
  // asid; Stage 2.5), so two contexts on one core share it safely.
  static constexpr champsim::modules::smt_capability capability = champsim::modules::smt_capability::shared;

  // void initialize_btb();
  void configure(const champsim::runtime_config& cfg, std::string_view prefix);
  std::pair<champsim::address, bool> btb_prediction(champsim::address ip, champsim::address_space_id asid);
  void update_btb(champsim::address ip, champsim::address branch_target, bool taken, uint8_t branch_type, champsim::address_space_id asid);

  // B3.4 fingerprint of the per-asid RAS, for the swap-out/swap-in assertion.
  uint64_t small_state_fingerprint(champsim::address_space_id asid) const;
};

#endif
