#ifndef BTB_BASIC_BTB_INDIRECT_PREDICTOR_H
#define BTB_BASIC_BTB_INDIRECT_PREDICTOR_H

#include <array>
#include <bitset>
#include <cstdint>
#include <utility>

#include "address.h"
#include "champsim.h"
#include "defs.h"
#include "msl/bits.h"

struct indirect_predictor {
  static constexpr std::size_t size = 4096;
  // The target table is shared by the core's contexts; each context keeps its
  // own conditional history, so a sibling's branches never move this context's
  // index.
  std::array<champsim::address, size> predictor = {};
  std::array<std::bitset<champsim::msl::lg2(size)>, champsim::defs::max_contexts> conditional_history = {};

  std::pair<champsim::address, bool> prediction(champsim::address ip, champsim::context_index context, champsim::address_space_id asid);
  void update_target(champsim::address ip, champsim::address branch_target, champsim::context_index context, champsim::address_space_id asid);
  void update_direction(champsim::context_index context, bool taken);
};

#endif
