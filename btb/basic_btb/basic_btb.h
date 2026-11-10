#ifndef BTB_BASIC_BTB_H
#define BTB_BASIC_BTB_H

#include <array>

#include "address.h"
#include "defs.h"
#include "direct_predictor.h"
#include "indirect_predictor.h"
#include "modules.h"
#include "return_stack.h"
#include "runtime_config.h"

class basic_btb : champsim::modules::btb
{
  std::array<return_stack, champsim::defs::max_contexts> ras{};
  std::array<indirect_predictor, champsim::defs::max_contexts> indirect{};
  direct_predictor direct{};

public:
  using btb::btb;
  basic_btb() : btb(nullptr) {}

  // SMT: the direct predictor is one shared per-core table whose tag folds the
  // asid (the fold is a hash that can alias), while the return stack and the
  // indirect conditional history are kept per context.
  static constexpr champsim::modules::smt_capability capability = champsim::modules::smt_capability::shared;

  // void initialize_btb();
  void configure(const champsim::runtime_config& cfg, std::string_view prefix);
  std::pair<champsim::address, bool> btb_prediction(champsim::address ip, champsim::context_index context, champsim::address_space_id asid);
  void update_btb(champsim::address ip, champsim::address branch_target, bool taken, uint8_t branch_type, champsim::context_index context,
                  champsim::address_space_id asid);
};

#endif
