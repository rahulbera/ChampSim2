#include <catch.hpp>
#include <cstdint>
#include <string>
#include <string_view>

#include "../../../btb/blbp_64kb/blbp_64kb.h"
#include "../../../btb/blbp_64kb_tuned/blbp_64kb_tuned.h"
#include "../../../btb/ittage_32kb/ittage_32kb.h"
#include "../../../btb/ittage_64kb/ittage_64kb.h"
#include "instruction.h"
#include "mocks.hpp"
#include "ooo_cpu.h"
#include "runtime_config.h"

// Module-level per-context and per-core isolation. BLBP and ITTAGE
// replicate their state in a std::array indexed by champsim::context_index, so
// training one hardware context's direct BTB must not be visible to another.
// These tests drive the module hooks directly with two distinct context indices
// and check that each context's answer comes from its own slice; the core
// variant drives two separately-constructed cores and checks that each core's
// instance keeps its own storage (the old ITTAGE shells held one function-local
// static array shared by every core).
//
// The direct-BTB path is used because it is deterministic and independent of the
// indirect predictor: a BRANCH_DIRECT_JUMP with taken=true stores the target in
// the direct predictor and (for ITTAGE) only touches the path history, never the
// core's input queue -- so a module constructed without a core (intern_ ==
// nullptr) is safe to drive here.

namespace
{
constexpr champsim::address ip{0x110000};
constexpr champsim::address target_a{0xaaa000};
constexpr champsim::address target_b{0xbbb000};
constexpr champsim::address_space_id asid{0, 0};

// Train context {0} toward target_a and context {1} toward target_b, then check
// each reads its own back. The cross-checks are the isolation assertion: if the
// two contexts shared one direct BTB, the later write would win on both reads.
template <typename Btb>
void check_isolation()
{
  Btb uut; // default-constructed without a core: intern_ == nullptr

  uut.update_btb(ip, target_a, true, BRANCH_DIRECT_JUMP, champsim::context_index{0}, asid);
  uut.update_btb(ip, target_b, true, BRANCH_DIRECT_JUMP, champsim::context_index{1}, asid);

  const auto [pred_a, taken_a] = uut.btb_prediction(ip, champsim::context_index{0}, asid);
  const auto [pred_b, taken_b] = uut.btb_prediction(ip, champsim::context_index{1}, asid);

  REQUIRE(pred_a == target_a);
  REQUIRE(taken_a);
  REQUIRE(pred_b == target_b);
  REQUIRE(taken_b);
}

// Train core 0's direct BTB and confirm core 1 -- a separate instance of the
// same module, built with its own core -- does not see it. The old ITTAGE shells
// held one function-local static array shared by every core, so core 0's
// training would leak into core 1; each instance now owns its own per-context
// storage. Core 1's direct BTB is shrunk to 4 sets x 1 way so that, were the two
// cores to share one table, the resize and the two training IPs (which collide in
// one set) would both betray the sharing.
template <typename Btb>
void check_core_isolation(std::string_view name)
{
  do_nothing_MRC mock_L1I_0, mock_L1D_0, mock_L1I_1, mock_L1D_1;

  O3_CPU core0{champsim::core_builder{}.btb<Btb>().fetch_queues(&mock_L1I_0.queues).data_queues(&mock_L1D_0.queues)};
  O3_CPU core1{champsim::core_builder{}.btb<Btb>().fetch_queues(&mock_L1I_1.queues).data_queues(&mock_L1D_1.queues)};

  champsim::runtime_config cfg{};
  const auto prefix = std::string{"ooo_cpu.cpu1."} + std::string{name};
  cfg.set(prefix + ".direct.sets=4");
  cfg.set(prefix + ".direct.ways=1");
  core1.btb_module_pimpl->impl_configure(cfg, prefix);

  // Train core 0's direct BTB on two taken direct jumps (context 0, asid {0,0}).
  core0.btb_module_pimpl->impl_update_btb(champsim::address{0x330000}, champsim::address{0xccc000}, true, BRANCH_DIRECT_JUMP, champsim::context_index{},
                                          champsim::address_space_id{});
  core0.btb_module_pimpl->impl_update_btb(champsim::address{0x331000}, champsim::address{0xddd000}, true, BRANCH_DIRECT_JUMP, champsim::context_index{},
                                          champsim::address_space_id{});

  // Core 1 has its own storage and predicts neither address.
  const auto core1_pred_a =
      core1.btb_module_pimpl->impl_btb_prediction(champsim::address{0x330000}, BRANCH_DIRECT_JUMP, champsim::context_index{}, champsim::address_space_id{});
  REQUIRE(core1_pred_a.first == champsim::address{});
  REQUIRE(core1_pred_a.second == false);
  const auto core1_pred_b =
      core1.btb_module_pimpl->impl_btb_prediction(champsim::address{0x331000}, BRANCH_DIRECT_JUMP, champsim::context_index{}, champsim::address_space_id{});
  REQUIRE(core1_pred_b.first == champsim::address{});
  REQUIRE(core1_pred_b.second == false);

  // Core 0 reads both targets back from its own storage.
  const auto core0_pred_a =
      core0.btb_module_pimpl->impl_btb_prediction(champsim::address{0x330000}, BRANCH_DIRECT_JUMP, champsim::context_index{}, champsim::address_space_id{});
  REQUIRE(core0_pred_a == std::pair{champsim::address{0xccc000}, true});
  const auto core0_pred_b =
      core0.btb_module_pimpl->impl_btb_prediction(champsim::address{0x331000}, BRANCH_DIRECT_JUMP, champsim::context_index{}, champsim::address_space_id{});
  REQUIRE(core0_pred_b == std::pair{champsim::address{0xddd000}, true});
}
} // namespace

TEST_CASE("blbp_64kb keeps each context's direct BTB separate") { check_isolation<blbp_64kb>(); }

TEST_CASE("blbp_64kb_tuned keeps each context's direct BTB separate") { check_isolation<blbp_64kb_tuned>(); }

TEST_CASE("ittage_32kb keeps each context's direct BTB separate") { check_isolation<ittage_32kb>(); }

TEST_CASE("ittage_64kb keeps each context's direct BTB separate") { check_isolation<ittage_64kb>(); }

TEST_CASE("blbp_64kb keeps each core's state separate") { check_core_isolation<blbp_64kb>("blbp_64kb"); }

TEST_CASE("blbp_64kb_tuned keeps each core's state separate") { check_core_isolation<blbp_64kb_tuned>("blbp_64kb_tuned"); }

TEST_CASE("ittage_32kb keeps each core's state separate") { check_core_isolation<ittage_32kb>("ittage_32kb"); }

TEST_CASE("ittage_64kb keeps each core's state separate") { check_core_isolation<ittage_64kb>("ittage_64kb"); }
