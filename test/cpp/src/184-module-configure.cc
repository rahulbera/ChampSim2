#include <catch.hpp>
#include <string>

#include "mocks.hpp"
#include "ooo_cpu.h"
#include "runtime_config.h"

// The configure() hook: a module may accept runtime-configuration values for
// its internal knobs, delivered after construction and before initialize().
// The hook is optional (SFINAE-detected, like every other hook); the prefix
// names this instance's knob table (e.g. "ooo_cpu.cpu0.basic_btb").

namespace
{
struct configurable_bp : champsim::modules::branch_predictor {
  using branch_predictor::branch_predictor;

  // Static because the module is constructed inside the core and the test has
  // no handle to the instance.
  static inline long received_theta = -1;      // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
  static inline std::string received_prefix{}; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
  static void reset()
  {
    received_theta = -1;
    received_prefix.clear();
  }

  void configure(const champsim::runtime_config& cfg, std::string_view prefix)
  {
    received_prefix = std::string{prefix};
    received_theta = cfg.value<long>(std::string{prefix} + ".theta", 42);
  }

  bool predict_branch(champsim::address /*ip*/) { return false; }
};

struct plain_bp : champsim::modules::branch_predictor {
  using branch_predictor::branch_predictor;
  bool predict_branch(champsim::address /*ip*/) { return false; }
  // deliberately no configure()
};

struct counting_final_bp : champsim::modules::branch_predictor {
  using branch_predictor::branch_predictor;
  static inline int final_calls = 0; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
  bool predict_branch(champsim::address /*ip*/) { return false; }
  void branch_predictor_final_stats() { ++final_calls; }
};
} // namespace

TEST_CASE("A module's configure hook receives the store and its prefix")
{
  configurable_bp::reset();
  do_nothing_MRC mock_L1I;
  do_nothing_MRC mock_L1D;
  O3_CPU uut{champsim::core_builder{}.branch_predictor<configurable_bp>().fetch_queues(&mock_L1I.queues).data_queues(&mock_L1D.queues)};

  champsim::runtime_config cfg{};
  cfg.set("ooo_cpu.cpu0.configurable.theta=99");
  uut.branch_module_pimpl->impl_configure(cfg, "ooo_cpu.cpu0.configurable");

  REQUIRE(configurable_bp::received_prefix == "ooo_cpu.cpu0.configurable");
  REQUIRE(configurable_bp::received_theta == 99);
}

TEST_CASE("A module's configure hook sees its baked default when the store is empty")
{
  configurable_bp::reset();
  do_nothing_MRC mock_L1I;
  do_nothing_MRC mock_L1D;
  O3_CPU uut{champsim::core_builder{}.branch_predictor<configurable_bp>().fetch_queues(&mock_L1I.queues).data_queues(&mock_L1D.queues)};

  champsim::runtime_config cfg{};
  uut.branch_module_pimpl->impl_configure(cfg, "ooo_cpu.cpu0.configurable");

  REQUIRE(configurable_bp::received_theta == 42);
}

TEST_CASE("A module without configure is silently skipped")
{
  do_nothing_MRC mock_L1I;
  do_nothing_MRC mock_L1D;
  O3_CPU uut{champsim::core_builder{}.branch_predictor<plain_bp>().fetch_queues(&mock_L1I.queues).data_queues(&mock_L1D.queues)};

  champsim::runtime_config cfg{};
  REQUIRE_NOTHROW(uut.branch_module_pimpl->impl_configure(cfg, "ooo_cpu.cpu0.plain"));
}

TEST_CASE("An installed replacement pimpl takes over from the constructed one")
{
  // Runtime module selection swaps the type-erased pimpl after construction
  // and before any hook has fired. The install setter is the seam the
  // generated environment constructor uses.
  counting_final_bp::final_calls = 0;
  do_nothing_MRC mock_L1I;
  do_nothing_MRC mock_L1D;
  O3_CPU uut{champsim::core_builder{}.branch_predictor<plain_bp>().fetch_queues(&mock_L1I.queues).data_queues(&mock_L1D.queues)};

  uut.install_branch_module(std::make_unique<O3_CPU::branch_module_model<counting_final_bp>>(&uut));
  uut.branch_module_pimpl->impl_branch_predictor_final_stats();

  REQUIRE(counting_final_bp::final_calls == 1);
}
