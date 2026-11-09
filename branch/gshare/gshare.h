#ifndef BRANCH_GSHARE_H
#define BRANCH_GSHARE_H

#include <array>
#include <bitset>

#include "defs.h"
#include "modules.h"
#include "msl/fwcounter.h"

struct gshare : champsim::modules::branch_predictor {
  static constexpr std::size_t GLOBAL_HISTORY_LENGTH = 14;
  static constexpr std::size_t COUNTER_BITS = 2;
  static constexpr std::size_t GS_HISTORY_TABLE_SIZE = 16384;

  std::array<std::bitset<GLOBAL_HISTORY_LENGTH>, champsim::defs::max_contexts> branch_history_vector{};
  std::array<champsim::msl::fwcounter<COUNTER_BITS>, GS_HISTORY_TABLE_SIZE> gs_history_table;

  using branch_predictor::branch_predictor;

  // SMT: the counter table is one shared per-core instance whose hash folds the
  // asid (the fold is a hash that can alias), while the branch history register
  // is kept per context so two threads do not mix their outcomes in one GHR.
  static constexpr champsim::modules::smt_capability capability = champsim::modules::smt_capability::shared;

  static std::size_t gs_table_hash(champsim::address ip, std::bitset<GLOBAL_HISTORY_LENGTH> bh_vector, champsim::address_space_id asid);
  bool predict_branch(champsim::address ip, champsim::address predicted_target, bool always_taken, uint8_t branch_type, champsim::context_index context,
                      champsim::address_space_id asid);
  void last_branch_result(champsim::address ip, champsim::address branch_target, bool taken, uint8_t branch_type, champsim::context_index context,
                          champsim::address_space_id asid);
};

#endif
