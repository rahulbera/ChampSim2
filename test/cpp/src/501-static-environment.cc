#include <algorithm>
#include <catch.hpp>
#include <set>
#include <string>
#include <vector>
#include <fmt/format.h>
#include <fmt/ranges.h>

#include "dram_controller.h"
#include "ramulator2_driver.h"
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

TEST_CASE("Omitted and explicit legacy selectors describe the same effective machine")
{
  champsim::runtime_config omitted, explicit_legacy;
  explicit_legacy.set("dram-model=legacy");
  champsim::static_environment omitted_env{omitted}, explicit_env{explicit_legacy};
  REQUIRE(omitted.consulted() == explicit_legacy.consulted());
  REQUIRE(omitted.unconsulted_keys().empty());
  REQUIRE(explicit_legacy.unconsulted_keys().empty());
  REQUIRE(omitted_env.memory_view().size() == champsim::data::bytes{17179869184});
  REQUIRE(explicit_env.memory_view().size() == omitted_env.memory_view().size());
  REQUIRE(omitted_env.memory_view().name() == "legacy");
}

TEST_CASE("The legacy controller itself remains the environment's single memory operable")
{
  champsim::runtime_config cfg;
  champsim::static_environment env{cfg};
  const auto operables = env.operable_view();
  auto& memory = env.memory_view().clocked_component();
  REQUIRE(&operables.back().get() == &memory);
  REQUIRE(dynamic_cast<MEMORY_CONTROLLER*>(&memory) != nullptr);
  REQUIRE(memory.clock_period == champsim::chrono::picoseconds{625});
}

TEST_CASE("The time quantum uses the selected memory clock without consulting legacy frequency")
{
  champsim::runtime_config cfg;
  cfg.set("pmem.frequency=100000");
  REQUIRE(champsim::static_environment::time_quantum(cfg, champsim::chrono::picoseconds{173}) == champsim::chrono::picoseconds{173});
  REQUIRE(champsim::static_environment::time_quantum(cfg, champsim::chrono::picoseconds{1000}) == champsim::chrono::picoseconds{250});
  REQUIRE(cfg.unconsulted_keys().size() == 1);
}

TEST_CASE("The native environment serves its shared LLC feeder with no legacy configuration", "[native-required]")
{
  if (!champsim::ramulator2_available()) {
    SKIP("native build disabled");
  }
  struct native_configuration {
    const char* name;
    int64_t period_ps, capacity_bytes;
  };
  for (const auto fixture : {native_configuration{"ddr4", 833, 8589934592LL}, native_configuration{"lpddr5", 1453, 1073741824}}) {
    CAPTURE(fixture.name);
    champsim::runtime_config cfg;
    cfg.set("dram-model=ramulator2");
    cfg.set(std::string{"ramulator2.config=configs/ramulator2/"} + fixture.name + ".yaml");
    champsim::static_environment env{cfg};
    auto& backend = env.memory_view();
    auto& memory = backend.clocked_component();
    const auto operables = env.operable_view();
    REQUIRE(backend.name() == "ramulator2");
    REQUIRE(&operables.back().get() == &memory);
    REQUIRE(operables.size() == env.cpu_view().size() + env.cache_view().size() + env.ptw_view().size() + 1);
    REQUIRE(env.channels_built() == champsim::static_environment::channel_count(champsim::defs::num_cpus));
    REQUIRE(backend.config_record().has_value());
    REQUIRE(backend.size().count() == fixture.capacity_bytes);
    REQUIRE(memory.clock_period.count() == fixture.period_ps);
    REQUIRE(cfg.unconsulted_keys().empty());
    for (const auto& [key, value] : cfg.consulted()) {
      REQUIRE(key.compare(0, 5, "pmem.") != 0);
    }
    const auto fastest = std::min_element(operables.begin(), operables.end(), [](const champsim::operable& left, const champsim::operable& right) {
      return left.clock_period < right.clock_period;
    });
    REQUIRE(champsim::static_environment::time_quantum(cfg, memory.clock_period) == fastest->get().clock_period);
    auto caches = env.cache_view();
    auto llc = std::find_if(caches.begin(), caches.end(), [](const CACHE& cache) { return cache.NAME == "LLC"; });
    REQUIRE(llc != caches.end());
    auto* feeder = llc->get().lower_level;
    memory.warmup = false;
    memory.begin_phase();
    for (std::size_t cpu = 0; cpu < champsim::defs::num_cpus; ++cpu) {
      champsim::channel::request_type request;
      request.address = champsim::address{0x100000 + cpu * 4096};
      request.v_address = champsim::address{0x200000 + cpu * 4096};
      request.pf_metadata = static_cast<uint32_t>(cpu + 1);
      request.cpu = static_cast<uint32_t>(cpu);
      REQUIRE(feeder->add_rq(request));
    }
    for (int cycle = 0; cycle < 10000 && feeder->returned.size() < champsim::defs::num_cpus; ++cycle) {
      memory._operate();
    }
    REQUIRE(feeder->RQ.empty());
    REQUIRE(feeder->returned.size() == champsim::defs::num_cpus);
    for (const auto& response : feeder->returned) {
      const auto cpu = response.pf_metadata - 1;
      REQUIRE(response.address == champsim::address{0x100000 + cpu * 4096});
      REQUIRE(response.v_address == champsim::address{0x200000 + cpu * 4096});
    }
    memory.end_phase(0);
    const auto stats = backend.statistics();
    REQUIRE(stats.sim_dram.empty());
    REQUIRE(stats.roi_dram.empty());
    REQUIRE(stats.sim_ramulator2.has_value());
    REQUIRE(stats.roi_ramulator2.has_value());
    REQUIRE(stats.sim_ramulator2->completed_reads == champsim::defs::num_cpus);
    REQUIRE(stats.roi_ramulator2->completed_reads == champsim::defs::num_cpus);
    backend.finalize();
  }
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
  const auto keys = {"pmem.refresh_period",
                     "pmem.channels",
                     "pmem.ranks",
                     "pmem.bankgroups",
                     "pmem.banks",
                     "pmem.bank_rows",
                     "pmem.bank_columns",
                     "pmem.channel_width",
                     "pmem.refreshes_per_period",
                     "vmem.pte_page_size",
                     "cache.llc.sets",
                     "cache.llc.ways",
                     "cache.cpu0_l1d.sets",
                     "cache.cpu0_l1d.ways",
                     "cache.cpu0_stlb.ways",
                     "ooo_cpu.cpu0.dib.sets",
                     "ooo_cpu.cpu0.dib.ways",
                     "ooo_cpu.cpu0.dib.inorder_width",
                     "ooo_cpu.cpu0.dib.hit_buffer_size"};

  for (const auto* key : keys) {
    DYNAMIC_SECTION("zero " << key)
    {
      champsim::runtime_config cfg{};
      cfg.set(std::string{key} + "=0");
      REQUIRE_THROWS_WITH(champsim::static_environment{cfg}, Catch::Matchers::ContainsSubstring(key));
    }
  }
}

TEST_CASE("Invalid nonzero operational geometry is refused with the responsible key")
{
  const auto invalid_values = {
      "pmem.channels=3",
      "pmem.ranks=3",
      "pmem.bankgroups=3",
      "pmem.banks=3",
      "pmem.bank_rows=3",
      "pmem.bank_columns=96",
      "pmem.channel_width=3",
      "pmem.channel_width=128",
      "vmem.pte_page_size=512",
      "vmem.pte_page_size=3072",
      "vmem.pte_page_size=8192",
      "cache.llc.max_fill=-1",
      "ooo_cpu.cpu0.fetch_width=-1",
      "ptw.cpu0_ptw.max_read=-1",
      "ooo_cpu.cpu0.register_file_size=0",
      "ooo_cpu.cpu0.register_file_size=32768",
  };

  for (const auto* assignment : invalid_values) {
    DYNAMIC_SECTION(assignment)
    {
      champsim::runtime_config cfg{};
      cfg.set(assignment);
      const std::string key{assignment,
                            static_cast<std::size_t>(std::find(assignment, assignment + std::char_traits<char>::length(assignment), '=') - assignment)};
      REQUIRE_THROWS_WITH(champsim::static_environment{cfg}, Catch::Matchers::ContainsSubstring(key));
    }
  }
}

TEST_CASE("Every parameter the deleted JSON configuration carried is still a knob")
{
  // The migration's contract: nothing the old champsim_config.json could set
  // may have become unreachable. These are its 147 leaf keys translated into
  // runtime-key spelling, minus the four that are compile-time by design
  // (block_size, page_size, num_cores -> inc/defs.h; executable_name -> the
  // makefile) and sim.heartbeat_frequency, which main() reads rather than the
  // environment.
  //
  // Two of these were genuinely dropped for a while and are the reason this
  // test exists: every cache's prefetch_activate (a SET of access types, which
  // the scalar store holds as the JSON's own comma-separated string) and
  // vmem.randomization (whose meaning depends on its type -- false disables,
  // an integer is the seed).
  const std::vector<std::string> from_json{
      "cache.cpu0_dtlb.latency",
      "cache.cpu0_dtlb.max_fill",
      "cache.cpu0_dtlb.max_tag_check",
      "cache.cpu0_dtlb.mshr_size",
      "cache.cpu0_dtlb.pq_size",
      "cache.cpu0_dtlb.prefetch_as_load",
      "cache.cpu0_dtlb.rq_size",
      "cache.cpu0_dtlb.sets",
      "cache.cpu0_dtlb.ways",
      "cache.cpu0_dtlb.wq_size",
      "cache.cpu0_itlb.latency",
      "cache.cpu0_itlb.max_fill",
      "cache.cpu0_itlb.max_tag_check",
      "cache.cpu0_itlb.mshr_size",
      "cache.cpu0_itlb.pq_size",
      "cache.cpu0_itlb.prefetch_as_load",
      "cache.cpu0_itlb.rq_size",
      "cache.cpu0_itlb.sets",
      "cache.cpu0_itlb.ways",
      "cache.cpu0_itlb.wq_size",
      "cache.cpu0_l1d.latency",
      "cache.cpu0_l1d.max_fill",
      "cache.cpu0_l1d.max_tag_check",
      "cache.cpu0_l1d.mshr_size",
      "cache.cpu0_l1d.pq_size",
      "cache.cpu0_l1d.prefetch_activate",
      "cache.cpu0_l1d.prefetch_as_load",
      "cache.cpu0_l1d.prefetcher",
      "cache.cpu0_l1d.rq_size",
      "cache.cpu0_l1d.sets",
      "cache.cpu0_l1d.virtual_prefetch",
      "cache.cpu0_l1d.ways",
      "cache.cpu0_l1d.wq_size",
      "cache.cpu0_l1i.latency",
      "cache.cpu0_l1i.max_fill",
      "cache.cpu0_l1i.max_tag_check",
      "cache.cpu0_l1i.mshr_size",
      "cache.cpu0_l1i.pq_size",
      "cache.cpu0_l1i.prefetch_activate",
      "cache.cpu0_l1i.prefetch_as_load",
      "cache.cpu0_l1i.prefetcher",
      "cache.cpu0_l1i.rq_size",
      "cache.cpu0_l1i.sets",
      "cache.cpu0_l1i.virtual_prefetch",
      "cache.cpu0_l1i.ways",
      "cache.cpu0_l1i.wq_size",
      "cache.cpu0_l2c.latency",
      "cache.cpu0_l2c.max_fill",
      "cache.cpu0_l2c.max_tag_check",
      "cache.cpu0_l2c.mshr_size",
      "cache.cpu0_l2c.pq_size",
      "cache.cpu0_l2c.prefetch_activate",
      "cache.cpu0_l2c.prefetch_as_load",
      "cache.cpu0_l2c.prefetcher",
      "cache.cpu0_l2c.rq_size",
      "cache.cpu0_l2c.sets",
      "cache.cpu0_l2c.virtual_prefetch",
      "cache.cpu0_l2c.ways",
      "cache.cpu0_l2c.wq_size",
      "cache.cpu0_stlb.latency",
      "cache.cpu0_stlb.max_fill",
      "cache.cpu0_stlb.max_tag_check",
      "cache.cpu0_stlb.mshr_size",
      "cache.cpu0_stlb.pq_size",
      "cache.cpu0_stlb.prefetch_as_load",
      "cache.cpu0_stlb.rq_size",
      "cache.cpu0_stlb.sets",
      "cache.cpu0_stlb.ways",
      "cache.cpu0_stlb.wq_size",
      "cache.llc.frequency",
      "cache.llc.latency",
      "cache.llc.max_fill",
      "cache.llc.max_tag_check",
      "cache.llc.mshr_size",
      "cache.llc.pq_size",
      "cache.llc.prefetch_activate",
      "cache.llc.prefetch_as_load",
      "cache.llc.prefetcher",
      "cache.llc.replacement",
      "cache.llc.rq_size",
      "cache.llc.sets",
      "cache.llc.virtual_prefetch",
      "cache.llc.ways",
      "cache.llc.wq_size",
      "ooo_cpu.cpu0.branch_predictor",
      "ooo_cpu.cpu0.btb",
      "ooo_cpu.cpu0.decode_buffer_size",
      "ooo_cpu.cpu0.decode_latency",
      "ooo_cpu.cpu0.decode_width",
      "ooo_cpu.cpu0.dib.sets",
      "ooo_cpu.cpu0.dib.ways",
      "ooo_cpu.cpu0.dib.window_size",
      "ooo_cpu.cpu0.dispatch_buffer_size",
      "ooo_cpu.cpu0.dispatch_latency",
      "ooo_cpu.cpu0.dispatch_width",
      "ooo_cpu.cpu0.execute_latency",
      "ooo_cpu.cpu0.execute_width",
      "ooo_cpu.cpu0.fetch_width",
      "ooo_cpu.cpu0.frequency",
      "ooo_cpu.cpu0.ifetch_buffer_size",
      "ooo_cpu.cpu0.lq_size",
      "ooo_cpu.cpu0.lq_width",
      "ooo_cpu.cpu0.mispredict_penalty",
      "ooo_cpu.cpu0.register_file_size",
      "ooo_cpu.cpu0.retire_width",
      "ooo_cpu.cpu0.rob_size",
      "ooo_cpu.cpu0.schedule_latency",
      "ooo_cpu.cpu0.scheduler_size",
      "ooo_cpu.cpu0.sq_size",
      "ooo_cpu.cpu0.sq_width",
      "pmem.bank_columns",
      "pmem.bank_rows",
      "pmem.bankgroups",
      "pmem.banks",
      "pmem.channel_width",
      "pmem.channels",
      "pmem.data_rate",
      "pmem.ranks",
      "pmem.refresh_period",
      "pmem.refreshes_per_period",
      "pmem.rq_size",
      "pmem.tcas",
      "pmem.tras",
      "pmem.trcd",
      "pmem.trp",
      "pmem.wq_size",
      "ptw.cpu0_ptw.max_read",
      "ptw.cpu0_ptw.max_write",
      "ptw.cpu0_ptw.mshr_size",
      "ptw.cpu0_ptw.pscl2_set",
      "ptw.cpu0_ptw.pscl2_way",
      "ptw.cpu0_ptw.pscl3_set",
      "ptw.cpu0_ptw.pscl3_way",
      "ptw.cpu0_ptw.pscl4_set",
      "ptw.cpu0_ptw.pscl4_way",
      "ptw.cpu0_ptw.pscl5_set",
      "ptw.cpu0_ptw.pscl5_way",
      "ptw.cpu0_ptw.rq_size",
      "vmem.minor_fault_penalty",
      "vmem.num_levels",
      "vmem.pte_page_size",
      "vmem.randomization",
  };

  champsim::runtime_config cfg{};
  champsim::static_environment env{cfg};

  std::set<std::string> consulted{};
  for (const auto& [key, value] : cfg.consulted()) {
    consulted.insert(key);
  }

  std::vector<std::string> unreachable{};
  for (const auto& key : from_json) {
    if (consulted.find(key) == std::end(consulted)) {
      unreachable.push_back(key);
    }
  }
  INFO("no longer configurable: " << fmt::format("{}", fmt::join(unreachable, ", ")));
  REQUIRE(std::empty(unreachable));
}
