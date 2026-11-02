// ChampSim's SET_ASIDE_CHAMPSIM_MODULE mechanism is not re-entrant; the
// set-aside is performed here, at the outermost level. See
// branch/cbp6_tagescl64/cbp6_tagescl64.cc for the full explanation.
#undef CHAMPSIM_MODULE
#include "ooo_cpu.h"
#define CHAMPSIM_MODULE

#include <array>
#include <iterator>

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

// Static storage duration, as with the CBP6 tenants: the vendored predictor's
// initialisation is not guaranteed to write every member, and static storage is
// zero-initialised before construction. One independent predictor per
// (core, context): the outer dimension is defs::num_cpus so core N's slot N is
// never shared with core M's slot N (Stage 6 hazard B -- the old context-only
// keying aliased ctx0 across cores). The array stays function-local (rather
// than a member) so every element still gets zero-init-before-construct.
std::array<std::array<impl_type, champsim::defs::max_contexts>, champsim::defs::num_cpus>& shared()
{
  static std::array<std::array<impl_type, champsim::defs::max_contexts>, champsim::defs::num_cpus> instances;
  return instances;
}

// Null-safe core index: tests drive this module without a bound core
// (intern_ == nullptr; see test/cpp/src/186-module-per-context-isolation.cc).
// Those always target core 0. In production every module instance is bound to
// exactly one O3_CPU by the environment.
std::size_t core_of(const O3_CPU* cpu)
{
  return cpu == nullptr ? 0 : cpu->cpu;
}
} // namespace

void ittage_64kb::configure(const champsim::runtime_config& cfg, std::string_view prefix)
{
  for (auto& per_core : shared())
    for (auto& inst : per_core)
      inst.configure_direct(cfg, prefix);
}

void ittage_64kb::initialize_btb()
{
  // initialize() is a no-op today, but run it per context so any future
  // per-instance initialisation stays correct.
  for (auto& per_core : shared())
    for (auto& inst : per_core)
      inst.initialize();
}

std::pair<champsim::address, bool> ittage_64kb::btb_prediction(champsim::address ip, champsim::context_index context, champsim::address_space_id asid)
{
  return shared()[core_of(intern_)][context.value].prediction(ip, asid);
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

  shared()[core_of(intern_)][context.value].update(ip, branch_target, branch_type, next_pc, asid);
}
