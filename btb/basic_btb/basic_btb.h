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
  // A return stack per hardware context: a sibling's calls and returns must not
  // move this context's stack. The indirect target table and the direct BTB are
  // shared; the indirect predictor keeps its conditional history per context.
  std::array<return_stack, champsim::defs::max_contexts> ras{};
  indirect_predictor indirect{};
  direct_predictor direct{};

public:
  using btb::btb;
  basic_btb() : btb(nullptr) {}

  // SMT: one per-core instance whose tables are shared by the core's contexts.
  // The direct BTB compares the asid in its tag; the indirect table folds the
  // asid into its index, a hash, so two address spaces can alias there. The
  // return stack and the indirect predictor's conditional history are per
  // context, selected by the context index the core passes to both hooks.
  static constexpr champsim::modules::smt_capability capability = champsim::modules::smt_capability::shared;

  // void initialize_btb();
  void configure(const champsim::runtime_config& cfg, std::string_view prefix);
  std::pair<champsim::address, bool> btb_prediction(champsim::address ip, champsim::context_index context, champsim::address_space_id asid);
  void update_btb(champsim::address ip, champsim::address branch_target, bool taken, uint8_t branch_type, champsim::context_index context,
                  champsim::address_space_id asid);
};

#endif
