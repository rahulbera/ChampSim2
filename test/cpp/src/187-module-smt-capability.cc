#include <catch.hpp>

#include "../../../branch/bimodal/bimodal.h"
#include "../../../branch/cbp6_ddtage/cbp6_ddtage.h"
#include "../../../branch/cbp6_runlts_norv/cbp6_runlts_norv.h"
#include "../../../branch/cbp6_tagescl192/cbp6_tagescl192.h"
#include "../../../branch/cbp6_tagescl64/cbp6_tagescl64.h"
#include "../../../branch/gshare/gshare.h"
#include "../../../branch/hashed_perceptron/hashed_perceptron.h"
#include "../../../branch/perceptron/perceptron.h"
#include "../../../branch/perfect_branch/perfect_branch.h"
#include "../../../btb/basic_btb/basic_btb.h"
#include "../../../btb/blbp_64kb/blbp_64kb.h"
#include "../../../btb/blbp_64kb_tuned/blbp_64kb_tuned.h"
#include "../../../btb/ittage_32kb/ittage_32kb.h"
#include "../../../btb/ittage_64kb/ittage_64kb.h"
#include "../../../btb/perfect_btb/perfect_btb.h"
#include "instruction.h"
#include "mocks.hpp"
#include "ooo_cpu.h"

// Stage 3.3, capability matrix. How a module stores its state with respect to a
// core's hardware contexts, advertised either explicitly (a `static constexpr
// smt_capability capability` member) or by the conservative default
// (`single_context`, so an SMT-unsafe module is refused rather than silently
// sharing its single copy past one context). The environment folds this over a
// module set with min_smt_capability and refuses at configuration time via
// check_smt_capability.
//
// The trait is a compile-time property; the guard and the fold are the runtime
// plumbing the environment's select_modules uses.

namespace
{
using champsim::modules::smt_capability;
using champsim::modules::smt_capability_of;
} // namespace

TEST_CASE("The CBP6 tenants declare global_state")
{
  // Their tables are namespace-scope globals shared by every instance, so they
  // opt into the only capability that admits process-wide static state.
  REQUIRE(smt_capability_of<cbp6_tagescl64>::value == smt_capability::global_state);
  REQUIRE(smt_capability_of<cbp6_tagescl192>::value == smt_capability::global_state);
  REQUIRE(smt_capability_of<cbp6_runlts_norv>::value == smt_capability::global_state);
  REQUIRE(smt_capability_of<cbp6_ddtage>::value == smt_capability::global_state);
}

TEST_CASE("A module with no declared capability defaults to single_context")
{
  // The oracles declare no capability, so they fall to the default: one
  // instance per core, safe on any core but not past one hardware context.
  REQUIRE(smt_capability_of<perfect_branch>::value == smt_capability::single_context);
  REQUIRE(smt_capability_of<perfect_btb>::value == smt_capability::single_context);
}

TEST_CASE("The per-context BTBs advertise per_context capability")
{
  REQUIRE(smt_capability_of<blbp_64kb>::value == smt_capability::per_context);
  REQUIRE(smt_capability_of<blbp_64kb_tuned>::value == smt_capability::per_context);
  REQUIRE(smt_capability_of<ittage_32kb>::value == smt_capability::per_context);
  REQUIRE(smt_capability_of<ittage_64kb>::value == smt_capability::per_context);
}

TEST_CASE("The ASID-keyed per-core modules advertise shared capability")
{
  // Stage 3.4 classification: these hold one instance per core whose tables fold
  // ASID into the index/hash (Stage 2.5), so two hardware contexts of different
  // address spaces land in disjoint entries. They are `shared`, not per_context:
  // replicating them would waste state the ASID keying already isolates.
  REQUIRE(smt_capability_of<basic_btb>::value == smt_capability::shared);
  REQUIRE(smt_capability_of<hashed_perceptron>::value == smt_capability::shared);
  REQUIRE(smt_capability_of<bimodal>::value == smt_capability::shared);
  REQUIRE(smt_capability_of<gshare>::value == smt_capability::shared);
  REQUIRE(smt_capability_of<perceptron>::value == smt_capability::shared);
}

TEST_CASE("min_smt_capability keeps the most restrictive capability")
{
  using champsim::modules::min_smt_capability;

  // The enum is ordered most-restrictive first: global_state < single_context
  // < per_context < shared.
  REQUIRE(min_smt_capability(smt_capability::global_state, smt_capability::single_context) == smt_capability::global_state);
  REQUIRE(min_smt_capability(smt_capability::global_state, smt_capability::per_context) == smt_capability::global_state);
  REQUIRE(min_smt_capability(smt_capability::global_state, smt_capability::shared) == smt_capability::global_state);
  REQUIRE(min_smt_capability(smt_capability::single_context, smt_capability::per_context) == smt_capability::single_context);
  REQUIRE(min_smt_capability(smt_capability::single_context, smt_capability::shared) == smt_capability::single_context);
  REQUIRE(min_smt_capability(smt_capability::per_context, smt_capability::shared) == smt_capability::per_context);
  REQUIRE(min_smt_capability(smt_capability::shared, smt_capability::shared) == smt_capability::shared);
}

TEST_CASE("check_smt_capability refuses global_state off cpu0 and single-copy state past one context")
{
  using Catch::Matchers::ContainsSubstring;
  using champsim::modules::check_smt_capability;

  // global_state: admitted on cpu0 with one context, refused everywhere else.
  REQUIRE_NOTHROW(check_smt_capability(smt_capability::global_state, 0, 1, "ooo_cpu.cpu0.branch_predictor", "cbp6_tagescl64"));
  REQUIRE_THROWS_WITH(check_smt_capability(smt_capability::global_state, 1, 1, "ooo_cpu.cpu1.branch_predictor", "cbp6_tagescl64"),
                      ContainsSubstring("this module keeps its state in process-wide static storage"));
  REQUIRE_THROWS_WITH(check_smt_capability(smt_capability::global_state, 1, 1, "ooo_cpu.cpu1.branch_predictor", "cbp6_tagescl64"),
                      ContainsSubstring("which every core would share, so it can run only on cpu0"));
  REQUIRE_THROWS_WITH(check_smt_capability(smt_capability::global_state, 0, 2, "ooo_cpu.cpu0.branch_predictor", "cbp6_tagescl64"),
                      ContainsSubstring("cannot serve the 2 hardware contexts of this core"));
  REQUIRE_THROWS_WITH(check_smt_capability(smt_capability::global_state, 1, 1, "ooo_cpu.cpu1.branch_predictor", "cbp6_tagescl64"),
                      ContainsSubstring("runtime config: ooo_cpu.cpu1.branch_predictor = cbp6_tagescl64"));

  // single_context: admitted on any core with one context, refused past one.
  REQUIRE_NOTHROW(check_smt_capability(smt_capability::single_context, 0, 1, "ooo_cpu.cpu0.btb", "perfect_btb"));
  REQUIRE_NOTHROW(check_smt_capability(smt_capability::single_context, 1, 1, "ooo_cpu.cpu1.btb", "perfect_btb"));
  REQUIRE_THROWS_WITH(check_smt_capability(smt_capability::single_context, 0, 2, "ooo_cpu.cpu0.btb", "perfect_btb"),
                      ContainsSubstring("declares neither smt_capability::per_context nor smt_capability::shared"));
  REQUIRE_THROWS_WITH(check_smt_capability(smt_capability::single_context, 1, 2, "ooo_cpu.cpu1.btb", "perfect_btb"),
                      ContainsSubstring("cannot serve the 2 hardware contexts of this core"));

  // per_context and shared are admitted anywhere, at any context count.
  REQUIRE_NOTHROW(check_smt_capability(smt_capability::per_context, 0, 2, "ooo_cpu.cpu0.btb", "blbp_64kb"));
  REQUIRE_NOTHROW(check_smt_capability(smt_capability::per_context, 1, 2, "ooo_cpu.cpu1.btb", "blbp_64kb"));
  REQUIRE_NOTHROW(check_smt_capability(smt_capability::shared, 0, 2, "ooo_cpu.cpu0.btb", "basic_btb"));
  REQUIRE_NOTHROW(check_smt_capability(smt_capability::shared, 1, 2, "ooo_cpu.cpu1.btb", "basic_btb"));
  REQUIRE_NOTHROW(check_smt_capability(smt_capability::single_context, 1, 1, "ooo_cpu.cpu1.branch_predictor", "perfect_branch"));
}

TEST_CASE("The oracle modules pass select_modules' check on a second core with one context")
{
  do_nothing_MRC mock_L1I, mock_L1D;
  O3_CPU core{champsim::core_builder{}.branch_predictor<perfect_branch>().btb<perfect_btb>().fetch_queues(&mock_L1I.queues).data_queues(&mock_L1D.queues)};

  // The oracles declare no capability, so they default to single_context, which
  // select_modules admits on a second core as long as that core has one context.
  REQUIRE(core.branch_module_pimpl->capability() == smt_capability::single_context);
  REQUIRE(core.btb_module_pimpl->capability() == smt_capability::single_context);
  REQUIRE_NOTHROW(champsim::modules::check_smt_capability(core.btb_module_pimpl->capability(), 1, 1, "ooo_cpu.cpu1.btb", "perfect_btb"));
}

TEST_CASE("The type-erased pimpl reports its module's capability")
{
  do_nothing_MRC mock_L1I, mock_L1D;

  O3_CPU global_state_core{champsim::core_builder{}.branch_predictor<cbp6_tagescl64>().fetch_queues(&mock_L1I.queues).data_queues(&mock_L1D.queues)};
  REQUIRE(global_state_core.branch_module_pimpl->capability() == smt_capability::global_state);

  O3_CPU single_context_core{champsim::core_builder{}.btb<perfect_btb>().fetch_queues(&mock_L1I.queues).data_queues(&mock_L1D.queues)};
  REQUIRE(single_context_core.btb_module_pimpl->capability() == smt_capability::single_context);

  O3_CPU per_context_core{champsim::core_builder{}.btb<blbp_64kb>().fetch_queues(&mock_L1I.queues).data_queues(&mock_L1D.queues)};
  REQUIRE(per_context_core.btb_module_pimpl->capability() == smt_capability::per_context);

  O3_CPU shared_core{champsim::core_builder{}.btb<basic_btb>().fetch_queues(&mock_L1I.queues).data_queues(&mock_L1D.queues)};
  REQUIRE(shared_core.btb_module_pimpl->capability() == smt_capability::shared);

  O3_CPU shared_branch_core{champsim::core_builder{}.branch_predictor<bimodal>().fetch_queues(&mock_L1I.queues).data_queues(&mock_L1D.queues)};
  REQUIRE(shared_branch_core.branch_module_pimpl->capability() == smt_capability::shared);

  O3_CPU single_branch_core{champsim::core_builder{}.branch_predictor<perfect_branch>().fetch_queues(&mock_L1I.queues).data_queues(&mock_L1D.queues)};
  REQUIRE(single_branch_core.branch_module_pimpl->capability() == smt_capability::single_context);
}
