// ChampSim's SET_ASIDE_CHAMPSIM_MODULE mechanism is not re-entrant; the
// set-aside is performed here, at the outermost level. See
// branch/cbp6_tagescl64/cbp6_tagescl64.cc for the full explanation.
#undef CHAMPSIM_MODULE
#include "ooo_cpu.h"
#define CHAMPSIM_MODULE

#include <array>
#include <iterator>
#include <memory>

#include "defs.h"
#include "ittage/ittage_btb.h"
#include "ittage_64kb.h"

namespace
{
// The storage parameters, and the only thing that distinguishes this module
// from the other ITTAGE configurations.
struct config {
  static constexpr int NHIST = 7;   // tagged tables (plus the base table)
  static constexpr int LOGG = 10;   // log2 entries per table
  static constexpr int TBITS = 11;  // tag width
  static constexpr int MINHIST = 2; // geometric history series bounds
  static constexpr int MAXHIST = 300;
  static constexpr int CWIDTH = 3; // confidence counter width
  static constexpr int UWIDTH = 2; // useful counter width
};

using impl_type = champsim::ittage::btb_impl<config>;

// Compile-time budget check, so a configuration edit cannot silently move the
// storage this module is supposed to represent.
static_assert(impl_type::entry_bits == 64, "entry layout changed; revisit the storage accounting");
static_assert(impl_type::storage_bits == static_cast<long>(7 + 1) * (1L << 10) * 64, "storage budget drifted");
} // namespace

// One independent predictor per hardware context. Held behind a pimpl so the
// vendored impl_type -- and its size -- stays out of the header; the header
// forward-declares `storage` and frees it through storage_deleter.
struct ittage_64kb::storage {
  std::array<impl_type, champsim::defs::max_contexts> contexts;
};

void ittage_64kb::storage_deleter::operator()(storage* s) const { delete s; }

std::unique_ptr<ittage_64kb::storage, ittage_64kb::storage_deleter> ittage_64kb::make_storage()
{
  // new storage() -- parentheses included -- value-initialises the aggregate,
  // which zero-initialises the contexts array before the vendored predictors'
  // constructors run. The vendored constructor does not write every member, so
  // reproducing that zero-init-before-construct guarantee is the reason the
  // storage is heap-allocated here rather than a by-value member.
  return std::unique_ptr<storage, storage_deleter>(new storage());
}

void ittage_64kb::configure(const champsim::runtime_config& cfg, std::string_view prefix)
{
  for (auto& inst : state->contexts)
    inst.configure_direct(cfg, prefix);
}

void ittage_64kb::initialize_btb()
{
  // initialize() is a no-op today, but run it per context so any future
  // per-instance initialisation stays correct.
  for (auto& inst : state->contexts)
    inst.initialize();
}

std::pair<champsim::address, bool> ittage_64kb::btb_prediction(champsim::address ip, champsim::context_index context, champsim::address_space_id asid)
{
  return state->contexts[context.value].prediction(ip, asid);
}

void ittage_64kb::update_btb(champsim::address ip, champsim::address branch_target, bool taken, uint8_t branch_type, champsim::context_index context,
                             champsim::address_space_id asid)
{
  // The architectural next PC: the target when taken, the fall-through when not.
  // ChampSim's branch_target is zero for a not-taken branch, and feeding zero
  // into ITTAGE's path history injects a constant into every subsequent index.
  // During this hook the branch is still input_queue.front() -- it is popped
  // only after do_init_instruction returns (src/ooo_cpu.cc) -- so the successor
  // is the following entry. The successor is read from the *context's own*
  // input queue.
  champsim::address next_pc = branch_target;
  if (!taken && intern_ != nullptr && context.value < intern_->contexts.size() && std::size(intern_->contexts[context.value].input_queue) > 1) {
    next_pc = std::next(std::begin(intern_->contexts[context.value].input_queue))->ip;
  }

  state->contexts[context.value].update(ip, branch_target, branch_type, next_pc, asid);
}
