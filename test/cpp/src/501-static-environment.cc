#include <algorithm>
#include <catch.hpp>
#include <set>
#include <string>
#include <vector>

#include "runtime_config.h"
#include "static_environment.h"

// The hand-written machine. These tests pin the structure a generated
// environment used to guarantee: the component set, the wiring, and the
// per-cycle operate() order. A silently wrong channel parameter changes timing
// without failing anything else, so the shape is asserted here rather than
// left to an end-to-end diff.

TEST_CASE("The environment builds the standard hierarchy")
{
  champsim::runtime_config cfg{};
  champsim::static_environment env{cfg};

  REQUIRE(std::size(env.cpu_view()) == champsim::defs::num_cpus);
  REQUIRE(std::size(env.ptw_view()) == champsim::defs::num_cpus);
  // LLC plus six per core: L1I, L1D, L2C, ITLB, DTLB, STLB.
  REQUIRE(std::size(env.cache_view()) == 1 + (champsim::defs::num_cpus * 6));
}

TEST_CASE("Cache order is the per-cycle operate order, LLC first then per-core alphabetical")
{
  champsim::runtime_config cfg{};
  champsim::static_environment env{cfg};

  std::vector<std::string> names{};
  for (const CACHE& cache : env.cache_view()) {
    names.push_back(cache.NAME);
  }

  // Parameterised over the core count like the rest of this file: the order is
  // the LLC, then each core's six caches alphabetically. Spelling the
  // single-core list out here made this the one assertion that a two-core
  // inc/defs.h turned red for a reason unrelated to what it checks.
  std::vector<std::string> expected{"LLC"};
  for (std::size_t cpu = 0; cpu < champsim::defs::num_cpus; ++cpu) {
    for (const auto* level : {"DTLB", "ITLB", "L1D", "L1I", "L2C", "STLB"}) {
      expected.push_back(champsim::static_environment::cache_name(cpu, level));
    }
  }
  REQUIRE(names == expected);
}

TEST_CASE("Every operable appears exactly once, cores then caches then PTWs then DRAM")
{
  champsim::runtime_config cfg{};
  champsim::static_environment env{cfg};

  const auto operables = env.operable_view();
  REQUIRE(std::size(operables) == std::size(env.cpu_view()) + std::size(env.cache_view()) + std::size(env.ptw_view()) + 1);

  std::set<const champsim::operable*> unique{};
  for (const champsim::operable& op : operables) {
    unique.insert(&op);
  }
  REQUIRE(std::size(unique) == std::size(operables));
}

TEST_CASE("The LLC-to-DRAM feeder is one shared channel, not one per core")
{
  // Twelve edges per core plus ONE shared feeder. A per-core feeder is
  // invisible at a single core -- every statistic matches -- but leaves the
  // memory controller polling a channel that nothing ever writes. The
  // generated topology has exactly one DRAM edge at any core count (25
  // channels at two cores, not 26).
  champsim::runtime_config cfg{};
  champsim::static_environment env{cfg};

  REQUIRE(env.channels_built() == champsim::static_environment::channel_count(champsim::defs::num_cpus));
  STATIC_REQUIRE(champsim::static_environment::channel_count(1) == 13);
  STATIC_REQUIRE(champsim::static_environment::channel_count(2) == 25);
  STATIC_REQUIRE(champsim::static_environment::channel_count(4) == 49);
}

TEST_CASE("The runtime store configures the hand-written machine")
{
  champsim::runtime_config cfg{};
  cfg.set("cache.cpu0_l1d.sets=128");
  cfg.set("cache.llc.ways=4");
  cfg.set("ooo_cpu.cpu0.rob_size=512");
  champsim::static_environment env{cfg};

  const auto caches = env.cache_view();
  const auto find = [&caches](std::string_view name) -> const CACHE& {
    return *std::find_if(std::begin(caches), std::end(caches), [name](const CACHE& c) { return c.NAME == name; });
  };
  REQUIRE(find("cpu0_L1D").NUM_SET == 128);
  REQUIRE(find("LLC").NUM_WAY == 4);
  REQUIRE(env.cpu_view().front().get().ROB_SIZE == 512);
}

TEST_CASE("Defaults are the stock machine when nothing is configured")
{
  champsim::runtime_config cfg{};
  champsim::static_environment env{cfg};

  const auto caches = env.cache_view();
  const auto find = [&caches](std::string_view name) -> const CACHE& {
    return *std::find_if(std::begin(caches), std::end(caches), [name](const CACHE& c) { return c.NAME == name; });
  };
  REQUIRE(find("cpu0_L1D").NUM_SET == 64);
  REQUIRE(find("cpu0_L1D").NUM_WAY == 12);
  REQUIRE(find("cpu0_L1I").NUM_SET == 64);
  REQUIRE(find("cpu0_L1I").NUM_WAY == 8);
  REQUIRE(find("cpu0_L2C").NUM_SET == 1024);
  REQUIRE(find("LLC").NUM_SET == 2048);
  REQUIRE(find("LLC").NUM_WAY == 16);
  REQUIRE(env.cpu_view().front().get().ROB_SIZE == 352);
}

TEST_CASE("Every component is wired: no cache has a null lower level except the LLC's DRAM edge")
{
  champsim::runtime_config cfg{};
  champsim::static_environment env{cfg};

  for (const CACHE& cache : env.cache_view()) {
    INFO("cache " << cache.NAME);
    REQUIRE(cache.lower_level != nullptr);
    REQUIRE_FALSE(std::empty(cache.upper_levels));
  }
  // The PTW's lower_level is private; its presence is proven by the run-level
  // equivalence check rather than reached into here.
  REQUIRE(std::size(env.ptw_view()) == champsim::defs::num_cpus);
}

TEST_CASE("The computed time quantum is the machine's actual smallest clock period")
{
  // do_phase() ticks the shared clock by min(clock_period) over the operables,
  // so a duration given in CYCLES -- vmem's minor fault penalty -- has to be
  // scaled by that. It is computed from the configuration before any component
  // exists, so nothing structural forces the two to agree: if the sweep misses
  // a component, or a frequency key is renamed, they diverge and only
  // cycle-denominated durations come out wrong, silently.
  //
  // Each case raises one component past the 4000 MHz the rest share, so the
  // quantum is decided by a different component every time.
  const auto quantum_matches = [](const std::vector<std::string>& assignments) {
    champsim::runtime_config cfg{};
    for (const auto& assignment : assignments) {
      cfg.set(assignment);
    }
    champsim::static_environment env{cfg};

    auto smallest = champsim::chrono::clock::duration::max();
    for (const champsim::operable& op : env.operable_view()) {
      smallest = std::min(smallest, op.clock_period);
    }
    return champsim::static_environment::time_quantum(cfg) == smallest;
  };

  REQUIRE(quantum_matches({}));
  REQUIRE(quantum_matches({"cache.cpu0_l1d.frequency=5000"}));
  REQUIRE(quantum_matches({"cache.llc.frequency=8000"}));
  REQUIRE(quantum_matches({"ooo_cpu.cpu0.frequency=6000"}));
  REQUIRE(quantum_matches({"ptw.cpu0_ptw.frequency=7000"}));
  REQUIRE(quantum_matches({"cache.cpu0_stlb.frequency=9000"}));
  REQUIRE(quantum_matches({"pmem.frequency=12000"}));
  // Every component slower than DRAM, so the memory controller decides it.
  REQUIRE(quantum_matches({"cache.cpu0_dtlb.frequency=100", "cache.cpu0_itlb.frequency=100", "cache.cpu0_l1d.frequency=100", "cache.cpu0_l1i.frequency=100",
                           "cache.cpu0_l2c.frequency=100", "cache.cpu0_stlb.frequency=100", "cache.llc.frequency=100", "ooo_cpu.cpu0.frequency=100",
                           "ptw.cpu0_ptw.frequency=100"}));
}

TEST_CASE("A geometry knob of zero is refused, not crashed on")
{
  // Every key below reached a division or an assertion inside a component:
  // pmem.refreshes_per_period and pmem.channel_width divided by zero (SIGFPE),
  // the rest tripped an assert (SIGABRT). A research simulator taking a
  // configuration from the command line has to reject the value, not die on
  // it -- and the two DIB knobs did neither, silently building a structure
  // that can never hit.
  const auto keys = {"pmem.refresh_period",   "pmem.channels",        "pmem.ranks",          "pmem.bankgroups",           "pmem.banks",
                     "pmem.bank_rows",        "pmem.bank_columns",    "pmem.channel_width",  "pmem.refreshes_per_period", "vmem.pte_page_size",
                     "cache.llc.sets",        "cache.llc.ways",       "cache.cpu0_l1d.sets", "cache.cpu0_l1d.ways",       "cache.cpu0_stlb.ways",
                     "ooo_cpu.cpu0.dib.sets", "ooo_cpu.cpu0.dib.ways"};

  for (const auto* key : keys) {
    DYNAMIC_SECTION("zero " << key)
    {
      champsim::runtime_config cfg{};
      cfg.set(std::string{key} + "=0");
      REQUIRE_THROWS_WITH(champsim::static_environment{cfg}, Catch::Matchers::ContainsSubstring(key));
    }
  }
}
