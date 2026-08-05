#include <catch.hpp>

#include <cstdint>

#include "instr.h"
#include "mocks.hpp"
#include "ooo_cpu.h"

// ChampSim gives branch predictors three hooks: initialize, predict, and
// last_branch_result. Prefetchers and replacement policies additionally get a
// final-stats hook, and CBP6 predictors need somewhere to run terminate() and
// report their own counters. These tests cover the added dispatch.

namespace
{
struct counting_branch_predictor : champsim::modules::branch_predictor {
  using branch_predictor::branch_predictor;

  // Static because the module is constructed inside the core and there is no
  // handle to the instance from the test.
  static inline int final_stats_calls = 0; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

  static void reset() { final_stats_calls = 0; }

  bool predict_branch(champsim::address /*ip*/) { return false; }
  void last_branch_result(champsim::address /*ip*/, champsim::address /*target*/, bool /*taken*/, uint8_t /*type*/) {}

  void branch_predictor_final_stats() { ++final_stats_calls; }
};

struct silent_branch_predictor : champsim::modules::branch_predictor {
  using branch_predictor::branch_predictor;
  bool predict_branch(champsim::address /*ip*/) { return false; }
  // deliberately no branch_predictor_final_stats
};
} // namespace

TEST_CASE("A branch predictor's final-stats hook is dispatched")
{
  counting_branch_predictor::reset();

  do_nothing_MRC mock_L1I, mock_L1D;
  O3_CPU uut{champsim::core_builder{}.fetch_queues(&mock_L1I.queues).data_queues(&mock_L1D.queues).branch_predictor<counting_branch_predictor>()};

  REQUIRE(counting_branch_predictor::final_stats_calls == 0);

  uut.impl_branch_predictor_final_stats();

  REQUIRE(counting_branch_predictor::final_stats_calls == 1);
}

TEST_CASE("A branch predictor without a final-stats hook is not required to have one")
{
  // The hook is optional, exactly like the prefetcher's. A predictor that omits
  // it must still compile and run.
  do_nothing_MRC mock_L1I, mock_L1D;
  O3_CPU uut{champsim::core_builder{}.fetch_queues(&mock_L1I.queues).data_queues(&mock_L1D.queues).branch_predictor<silent_branch_predictor>()};

  REQUIRE_NOTHROW(uut.impl_branch_predictor_final_stats());
}

TEST_CASE("The final-stats trait detects the hook")
{
  STATIC_REQUIRE(champsim::modules::branch_predictor::has_final_stats<counting_branch_predictor>);
  STATIC_REQUIRE_FALSE(champsim::modules::branch_predictor::has_final_stats<silent_branch_predictor>);
}
