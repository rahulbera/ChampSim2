#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "channel.h"
#include "defs.h"
#include "operable.h"
#include "ramulator2_driver.h"
#include "ramulator2_memory_backend.h"
#include "runtime_config.h"
// Missing submission/ticking/callback forwarding must fail this real native request.
TEST_CASE("The native DDR4 driver completes an external read", "[native-required]")
{
  if (!champsim::ramulator2_available())
    SKIP("native build disabled");
  champsim::runtime_config cfg;
  cfg.set("ramulator2.config=configs/ramulator2/ddr4.yaml");
  auto driver = champsim::make_ramulator2_driver(cfg);
  REQUIRE(driver->transaction_bytes() == 64);
  REQUIRE(driver->clock_period().count() == 833);
  REQUIRE(driver->size().count() == 8589934592LL);
  bool completed = false;
  REQUIRE(driver->send(false, 0x100000, 0, 64, [&] { completed = true; }));
  for (int cycle = 0; cycle < 10000 && !completed; ++cycle)
    driver->tick();
  REQUIRE(completed);
}

namespace
{
std::string fixture(const std::string& name = "ddr4")
{
  std::ifstream file("configs/ramulator2/" + name + ".yaml");
  if (!file)
    throw std::runtime_error("missing native test fixture");
  return {std::istreambuf_iterator<char>{file}, {}};
}
std::string changed(std::string text, const std::string& from, const std::string& to)
{
  const auto pos = text.find(from);
  if (pos == std::string::npos)
    throw std::runtime_error("invalid test replacement");
  text.replace(pos, from.size(), to);
  return text;
}
struct temporary_yaml {
  std::filesystem::path path;
  explicit temporary_yaml(const std::string& text)
  {
    static unsigned sequence = 0;
    path = std::filesystem::temp_directory_path() / ("champsim-native-test-" + std::to_string(++sequence) + ".yaml");
    std::ofstream file(path);
    file << text;
  }
  ~temporary_yaml()
  {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
  champsim::runtime_config config() const
  {
    champsim::runtime_config cfg;
    cfg.set("ramulator2.config=" + path.string());
    return cfg;
  }
};
int64_t counter(const champsim::native_memory_statistics& stats, std::vector<std::string> path)
{
  const auto found = std::find_if(stats.values.begin(), stats.values.end(), [&](const auto& item) { return item.path == path; });
  if (found == stats.values.end())
    throw std::runtime_error("missing native counter");
  return std::get<int64_t>(found->value);
}
} // namespace

TEST_CASE("The optional native build reports and enforces availability")
{
  if (const auto* expected = std::getenv("CHAMPSIM_EXPECT_RAMULATOR2"))
    REQUIRE(champsim::ramulator2_available() == (std::string{expected} == "1"));
  if (!champsim::ramulator2_available()) {
    champsim::runtime_config cfg;
    REQUIRE_THROWS_WITH(champsim::make_ramulator2_driver(cfg), Catch::Matchers::ContainsSubstring("not available"));
  }
}

TEST_CASE("LPDDR5 serves its smaller native transactions")
{
  if (!champsim::ramulator2_available())
    SKIP("native build disabled");
  champsim::runtime_config cfg;
  cfg.set("ramulator2.config=configs/ramulator2/lpddr5.yaml");
  auto driver = champsim::make_ramulator2_driver(cfg);
  REQUIRE(driver->transaction_bytes() == 32);
  REQUIRE(driver->clock_period().count() == 1453);
  REQUIRE(driver->size().count() == 1073741824);
  unsigned completed = 0;
  REQUIRE(driver->send(false, 0x100000, 0, 32, [&] { ++completed; }));
  REQUIRE(driver->send(false, 0x100020, 0, 32, [&] { ++completed; }));
  for (int i = 0; i < 10000 && completed != 2; ++i)
    driver->tick();
  REQUIRE(completed == 2);
}

TEST_CASE("Native driver rejects malformed exported tables before native indexing")
{
  if (!champsim::ramulator2_available())
    SKIP("native build disabled");
  const auto original = fixture();
  for (const auto& [yaml, diagnostic] : std::vector<std::pair<std::string, std::string>>{
           {"frontend: [", "configuration"},
           {changed(original, "impl: External", "impl: SimpleO3"), "External"},
           {changed(original, "impl: GenericDRAM", "impl: Unknown"), "GenericDRAM"},
           {changed(original, "impl: CacheLineInterleave", "impl: PassThrough"), "CacheLineInterleave"},
           {changed(original, "count: [1, 1, 4, 4, 65536, 1024]", "count: [1]"), "organization dimensions"},
           {changed(original, "timing: [2400,", "timing: ["), "timing dimensions"},
           {changed(original, "command_cycles: [1, 1, 1, 1, 1, 1, 1, 1]", "command_cycles: [1]"), "command dimensions"},
           {changed(original, "- [0, [3, 5]", "- [99, [3, 5]"), "level out of range"},
           {changed(original, "- [0, [3, 5]", "- [0, [99, 5]"), "command out of range"},
           {changed(original, "read_buffer_size: 32", "read_buffer_size: 0"), "read_buffer_size"},
           {changed(original, "channel_width: 64", "channel_width: 24"), "transaction"},
           {changed(original, "channel_width: 64", "channel_width: 0"), "channel width"},
           {changed(original, "65536, 1024", "65536, 4"), "prefetch"},
           {changed(original, "count: [1, 1, 4, 4, 65536, 1024]", "count: [1, 1073741824, 1073741824, 4, 65536, 1024]"), "capacity overflow"},
           {changed(original, "interleave_bits: 0", "interleave_bits: 28"), "interleave"},
           {changed(original, "interleave_bits: 0", "interleave_bits: -1"), "interleave"},
           {changed(original, "count: [1, 1, 4, 4, 65536, 1024]", "count: [1, 1, 1, 1, 128, 1024]"), "reserved"}}) {
    CAPTURE(diagnostic);
    temporary_yaml file(yaml);
    REQUIRE_THROWS_WITH(champsim::make_ramulator2_driver(file.config()), Catch::Matchers::ContainsSubstring(diagnostic));
  }
}

// The native API does not check that a controller or address mapper can work
// with ChampSim's External shim: PassThroughAddrMapper leaves addr_vec empty
// and faults at the first tick, reserved RIT rows shift the top of the capacity
// the driver reports outside the device, and BlockHammer casts the frontend to
// a type it is not (undefined behaviour, so test_ramulator2_cli.py covers it in
// a subprocess).
TEST_CASE("Native driver rejects components that cannot serve the External shim")
{
  if (!champsim::ramulator2_available())
    SKIP("native build disabled");
  const auto original = fixture();
  const auto controller = original.substr(original.find("    - impl: GenericDDR"));
  const std::string flat_mapper = "      addr_mapper:\n        impl: RoBaRaCoCh\n";
  const auto rit = [&](const std::string& body) {
    return changed(original, flat_mapper, "      addr_mapper:\n        impl: RITAddrMapper\n" + body);
  };
  const std::string unsupported = " is not one of the components supported behind ChampSim's External frontend; supported: ";
  const std::string table = " must be a table with an impl key; supported: ";
  const std::string single = " impl must be a single name; supported: ";
  for (const auto& [yaml, diagnostic] : std::vector<std::pair<std::string, std::string>>{
           // A misspelled or unregistered name is not described as a registered component that does not fit.
           {changed(original, "impl: GenericDDR", "impl: DDR4Controller"),
            "controller impl 'DDR4Controller'" + unsupported + "GenericDDR, LPDDR5, LPDDR6, GDDR7, HBM12, HBM34, PRAC"},
           {changed(original, "    - impl: GenericDDR\n", "    - id: no_impl\n"), "controller impl is missing; supported: GenericDDR, LPDDR5"},
           {changed(original, "impl: RoBaRaCoCh", "impl: PassThroughAddrMapper"),
            "addr_mapper impl 'PassThroughAddrMapper'" + unsupported + "RoBaRaCoCh, ChRaBaRoCo, MOP4CLXOR"},
           {changed(original, "impl: RoBaRaCoCh", "impl: roBaRaCoCh"), "addr_mapper impl 'roBaRaCoCh'" + unsupported + "RoBaRaCoCh"},
           {changed(original, flat_mapper, ""), "addr_mapper impl is missing; supported: RoBaRaCoCh, ChRaBaRoCo, MOP4CLXOR, or RITAddrMapper"},
           {changed(original, flat_mapper, "      addr_mapper: RoBaRaCoCh\n"), "addr_mapper" + table + "RoBaRaCoCh, ChRaBaRoCo, MOP4CLXOR, or RITAddrMapper"},
           // An impl key is present, so it is not missing, but it names nothing.
           {changed(original, "impl: GenericDDR", "impl: [GenericDDR]"), "controller" + single + "GenericDDR, LPDDR5"},
           {changed(original, "impl: RoBaRaCoCh", "impl: [RoBaRaCoCh]"), "addr_mapper" + single + "RoBaRaCoCh, ChRaBaRoCo, MOP4CLXOR, or RITAddrMapper"},
           {changed(original, "impl: RoBaRaCoCh", "impl: {name: RoBaRaCoCh}"), "addr_mapper" + single + "RoBaRaCoCh, ChRaBaRoCo, MOP4CLXOR, or RITAddrMapper"},
           {rit("        addr_mapper:\n          impl: [MOP4CLXOR]\n"), "RITAddrMapper nested addr_mapper" + single + "RoBaRaCoCh"},
           {original + changed(controller, "impl: RoBaRaCoCh", "impl: PassThroughAddrMapper"), "addr_mapper impl 'PassThroughAddrMapper'" + unsupported},
           {rit("        reserved_rows_per_bank: 64\n        addr_mapper:\n          impl: RoBaRaCoCh\n"), "reserved_rows_per_bank 64 is not supported"},
           {rit("        reserved_rows_per_bank: 1024\n        addr_mapper:\n          impl: ChRaBaRoCo\n"), "reserved_rows_per_bank 1024 is not supported"},
           {rit("        reserved_rows_per_bank: -1\n        addr_mapper:\n          impl: RoBaRaCoCh\n"),
            "RITAddrMapper reserved_rows_per_bank -1 is invalid; it must be absent or 0"},
           {rit("        addr_mapper:\n          impl: PassThroughAddrMapper\n"),
            "RITAddrMapper nested addr_mapper impl 'PassThroughAddrMapper'" + unsupported},
           {rit("        addr_mapper:\n          impl: RITAddrMapper\n"), "RITAddrMapper nested addr_mapper impl 'RITAddrMapper'" + unsupported},
           {rit("        addr_mapper: MOP4CLXOR\n"), "RITAddrMapper nested addr_mapper" + table + "RoBaRaCoCh"},
           {rit("        reserved_rows_per_bank: 0\n"), "RITAddrMapper nested addr_mapper impl is missing; supported: RoBaRaCoCh"}}) {
    CAPTURE(diagnostic);
    temporary_yaml file(yaml);
    CHECK_THROWS_WITH(champsim::make_ramulator2_driver(file.config()), Catch::Matchers::ContainsSubstring(diagnostic));
  }
  // A negative reservation is not a row shift.
  temporary_yaml negative(rit("        reserved_rows_per_bank: -1\n        addr_mapper:\n          impl: RoBaRaCoCh\n"));
  CHECK_THROWS_WITH(champsim::make_ramulator2_driver(negative.config()), !Catch::Matchers::ContainsSubstring("shifted"));
}

namespace
{
// A controller with this address mapper serves reads at both ends of the fixture's capacity.
void require_mapper_serves_capacity(const std::string& mapper)
{
  CAPTURE(mapper);
  temporary_yaml file(changed(fixture(), "      addr_mapper:\n        impl: RoBaRaCoCh\n", mapper));
  auto driver = champsim::make_ramulator2_driver(file.config());
  REQUIRE(driver->size().count() == 8589934592LL);
  unsigned completed = 0;
  REQUIRE(driver->send(false, 0x100000, 0, 64, [&] { ++completed; }));
  REQUIRE(driver->send(false, 8589934592ULL - 64, 0, 64, [&] { ++completed; }));
  for (int i = 0; i < 10000 && completed != 2; ++i)
    driver->tick();
  REQUIRE(completed == 2);
}
} // namespace

TEST_CASE("Native driver serves every admitted flat address mapper")
{
  if (!champsim::ramulator2_available())
    SKIP("native build disabled");
  for (const auto* mapper : {"      addr_mapper:\n        impl: ChRaBaRoCo\n", "      addr_mapper:\n        impl: MOP4CLXOR\n"})
    require_mapper_serves_capacity(mapper);
}

// Cases tagged [rit-addr-mapper] construct RITAddrMapper controllers. The
// pinned native RITAddrMapper never frees the nested mapper it creates, so
// LeakSanitizer fails any process that runs one; the instrumented CI job runs
// them in a step of their own (see test/ramulator2/README.md).
TEST_CASE("Native driver serves every admitted row-indirection address mapper", "[rit-addr-mapper]")
{
  if (!champsim::ramulator2_available())
    SKIP("native build disabled");
  for (const auto* mapper :
       {"      addr_mapper:\n        impl: RITAddrMapper\n        addr_mapper:\n          impl: RoBaRaCoCh\n",
        "      addr_mapper:\n        impl: RITAddrMapper\n        reserved_rows_per_bank: 0\n        addr_mapper:\n          impl: MOP4CLXOR\n"})
    require_mapper_serves_capacity(mapper);
}

TEST_CASE("Native input identity is effective and detects replay drift")
{
  if (!champsim::ramulator2_available())
    SKIP("native build disabled");
  temporary_yaml file(fixture());
  auto cfg = file.config();
  auto driver = champsim::make_ramulator2_driver(cfg);
  const auto record = driver->config_record();
  REQUIRE(record.path == std::filesystem::canonical(file.path).string());
  REQUIRE(record.yaml == fixture());
  REQUIRE(record.revision == "72427a1bba3771564c4fb0e494ba02242fd1eaa7");
  REQUIRE(record.hash.size() == 16);
  REQUIRE(record.library_hash.size() == 16);
  REQUIRE_FALSE(record.build.empty());
  const auto consulted = cfg.consulted();
  REQUIRE(std::find(consulted.begin(), consulted.end(), std::pair<std::string, std::string>{"ramulator2.config_hash", '"' + record.hash + '"'})
          != consulted.end());
  cfg.set("ramulator2.config_hash=" + record.hash);
  cfg.set("ramulator2.revision=" + record.revision);
  REQUIRE_NOTHROW(champsim::make_ramulator2_driver(cfg));
  {
    std::ofstream output(file.path, std::ios::app);
    output << "\n# changed configuration identity\n";
  }
  REQUIRE_THROWS_WITH(champsim::make_ramulator2_driver(cfg), Catch::Matchers::ContainsSubstring("config_hash mismatch"));
  cfg.set("ramulator2.config_hash=");
  cfg.set("ramulator2.revision=stale");
  REQUIRE_THROWS_WITH(champsim::make_ramulator2_driver(cfg), Catch::Matchers::ContainsSubstring("revision mismatch"));
  cfg.set("ramulator2.config=/missing/champsim-native-fixture.yaml");
  REQUIRE_THROWS_WITH(champsim::make_ramulator2_driver(cfg), Catch::Matchers::ContainsSubstring("cannot open"));
}

TEST_CASE("Payload capacity and channel geometry match the reachable address range")
{
  if (!champsim::ramulator2_available())
    SKIP("native build disabled");
  const auto original = fixture();
  const auto controller = original.substr(original.find("    - impl: GenericDDR"));
  SECTION("payload override counts addressable bytes rather than wire width")
  {
    temporary_yaml file(changed(original, "channel_width: 64", "channel_width: 64\n        data_payload_bytes: 32"));
    auto driver = champsim::make_ramulator2_driver(file.config());
    REQUIRE(driver->size().count() == 4294967296LL);
    REQUIRE(driver->transaction_bytes() == 32);
  }
  SECTION("two equal channels include all storage with channel-sized interleave")
  {
    temporary_yaml file(changed(original + controller, "interleave_bits: 0", "interleave_bits: 27"));
    auto driver = champsim::make_ramulator2_driver(file.config());
    REQUIRE(driver->size().count() == 17179869184LL);
    unsigned completed = 0;
    REQUIRE(driver->send(false, 0x100000, 0, 64, [&] { ++completed; }));
    REQUIRE(driver->send(false, 8589934592ULL, 0, 64, [&] { ++completed; }));
    for (int i = 0; i < 10000 && completed != 2; ++i)
      driver->tick();
    REQUIRE(completed == 2);
    const auto stats = driver->statistics();
    REQUIRE(counter(stats, {"memory_system", "controller", "channel0", "num_read_reqs"}) == 1);
    REQUIRE(counter(stats, {"memory_system", "controller", "channel1", "num_read_reqs"}) == 1);
  }
  SECTION("unequal periods cannot share the memory operable")
  {
    temporary_yaml file(original + changed(controller, "2, 833]", "2, 834]"));
    REQUIRE_THROWS_WITH(champsim::make_ramulator2_driver(file.config()), Catch::Matchers::ContainsSubstring("heterogeneous"));
  }
  SECTION("unequal transactions cannot share the channel mapper")
  {
    temporary_yaml file(original + changed(controller, "channel_width: 64", "channel_width: 32"));
    REQUIRE_THROWS_WITH(champsim::make_ramulator2_driver(file.config()), Catch::Matchers::ContainsSubstring("heterogeneous"));
  }
}

TEST_CASE("Driver validates requests and preserves callbacks across statistics resets")
{
  if (!champsim::ramulator2_available())
    SKIP("native build disabled");
  temporary_yaml file(fixture());
  auto driver = champsim::make_ramulator2_driver(file.config());
  for (const auto address : {8589934592ULL, std::numeric_limits<unsigned long long>::max()})
    REQUIRE_THROWS(driver->send(false, address, 0, 64, {}));
  REQUIRE_THROWS(driver->send(false, 0, static_cast<uint32_t>(champsim::defs::num_cpus), 64, {}));
  REQUIRE_THROWS(driver->send(false, 0, 0, 0, {}));
  REQUIRE_THROWS(driver->send(false, 0, 0, 65, {}));
  REQUIRE_THROWS(driver->send(false, 63, 0, 2, {}));
  unsigned completed = 0;
  REQUIRE(driver->send(false, 0x100000, 0, 64, [&] { ++completed; }));
  const auto first = driver->statistics();
  REQUIRE(counter(first, {"memory_system", "total_num_read_requests"}) == 1);
  driver->reset_stats();
  for (int i = 0; i < 10000 && completed == 0; ++i)
    driver->tick();
  REQUIRE(completed == 1);
  REQUIRE(counter(first, {"memory_system", "total_num_read_requests"}) == 1); // owned snapshot
  REQUIRE(driver->send(true, 0x200000, 0, 64, [&] { ++completed; }));
  for (int i = 0; i < 10000 && completed != 2; ++i)
    driver->tick();
  REQUIRE(completed == 2);
  driver->finalize();
  REQUIRE_NOTHROW(driver->finalize());
  REQUIRE_THROWS(driver->tick());
  REQUIRE_THROWS(driver->send(false, 0, 0, 64, {}));
}

// Ramulator 2.1 keeps GenericDRAM's accepted-request totals and the tick
// counters of four controller plugins in signed int, where passing the maximum
// is undefined behaviour. The driver refuses the operation that could do so;
// these tests lower the limits so they can reach them.
TEST_CASE("Native signed counter limits default to the native int maximum")
{
  const champsim::ramulator2_native_limits limits;
  REQUIRE(limits.accepted_requests_per_statistics_phase == static_cast<uint64_t>(std::numeric_limits<int>::max()));
  REQUIRE(limits.plugin_ticks == static_cast<uint64_t>(std::numeric_limits<int>::max()));
}

namespace
{
champsim::ramulator2_native_limits request_limit(uint64_t accepted)
{
  champsim::ramulator2_native_limits limits;
  limits.accepted_requests_per_statistics_phase = accepted;
  return limits;
}
champsim::channel::request_type feeder_read(uint64_t address)
{
  champsim::channel::request_type request;
  request.address = champsim::address{address};
  request.cpu = 0;
  return request;
}
int64_t native_reads(const champsim::memory_backend& backend)
{
  return counter(backend.statistics().sim_ramulator2.value().native, {"memory_system", "total_num_read_requests"});
}
} // namespace

TEST_CASE("The driver refuses a native send that could pass a signed request total")
{
  if (!champsim::ramulator2_available())
    SKIP("native build disabled");
  temporary_yaml file(fixture());
  auto driver = champsim::make_ramulator2_driver(file.config(), request_limit(3));
  unsigned completed = 0;
  const auto done = [&] {
    ++completed;
  };
  for (uint64_t block = 0; block < 3; ++block)
    REQUIRE(driver->send(false, 0x100000 + 64 * block, 0, 64, done));
  REQUIRE_THROWS_WITH(driver->send(false, 0x200000, 0, 64, done), Catch::Matchers::ContainsSubstring("native GenericDRAM total_num_read_requests")
                                                                      && Catch::Matchers::ContainsSubstring("limit of 3 accepted read requests"));
  // Writes have their own total, and a duplicate write the controller absorbs
  // at send() counts toward it exactly as native counts it.
  REQUIRE(driver->send(true, 0x300000, 0, 64, done));
  REQUIRE(driver->send(true, 0x300040, 0, 64, done));
  REQUIRE(driver->send(true, 0x300000, 0, 64, done));
  REQUIRE_THROWS_WITH(driver->send(true, 0x400000, 0, 64, done), Catch::Matchers::ContainsSubstring("native GenericDRAM total_num_write_requests")
                                                                     && Catch::Matchers::ContainsSubstring("limit of 3 accepted write requests"));
  for (int i = 0; i < 10000 && completed != 6; ++i)
    driver->tick();
  REQUIRE(completed == 6);
  // Nothing refused reached native: its totals hold the limit, and no further callback arrives.
  for (int i = 0; i < 1000; ++i)
    driver->tick();
  REQUIRE(completed == 6);
  auto stats = driver->statistics();
  REQUIRE(counter(stats, {"memory_system", "total_num_read_requests"}) == 3);
  REQUIRE(counter(stats, {"memory_system", "total_num_write_requests"}) == 3);
  REQUIRE(counter(stats, {"memory_system", "controller", "channel0", "num_read_reqs"}) == 3);
  REQUIRE(counter(stats, {"memory_system", "controller", "channel0", "num_write_reqs_coalesced"}) == 1);
  // Native resets both totals with its statistics, and the limit restarts with them.
  driver->reset_stats();
  for (uint64_t block = 0; block < 3; ++block) {
    REQUIRE(driver->send(false, 0x200000 + 64 * block, 0, 64, done));
    REQUIRE(driver->send(true, 0x400000 + 64 * block, 0, 64, done));
  }
  REQUIRE_THROWS_WITH(driver->send(false, 0x500000, 0, 64, done), Catch::Matchers::ContainsSubstring("total_num_read_requests"));
  stats = driver->statistics();
  REQUIRE(counter(stats, {"memory_system", "total_num_read_requests"}) == 3);
  REQUIRE(counter(stats, {"memory_system", "total_num_write_requests"}) == 3);
}

TEST_CASE("Rejected native sends do not count toward the request limit")
{
  if (!champsim::ramulator2_available())
    SKIP("native build disabled");
  temporary_yaml file(changed(fixture(), "read_buffer_size: 32", "read_buffer_size: 1"));
  auto driver = champsim::make_ramulator2_driver(file.config(), request_limit(2));
  REQUIRE(driver->send(false, 0x100000, 0, 64, {}));
  for (int attempt = 0; attempt < 5; ++attempt)
    REQUIRE_FALSE(driver->send(false, 0x200000, 0, 64, {}));
  bool accepted = false;
  for (int i = 0; i < 10000 && !accepted; ++i) {
    driver->tick();
    accepted = driver->send(false, 0x200000, 0, 64, {});
  }
  REQUIRE(accepted);
  REQUIRE(counter(driver->statistics(), {"memory_system", "total_num_read_requests"}) == 2);
  REQUIRE_THROWS_WITH(driver->send(false, 0x300000, 0, 64, {}), Catch::Matchers::ContainsSubstring("limit of 2 accepted read requests"));
}

TEST_CASE("Fast warmup does not use the native request limit, and each phase begin restarts it")
{
  if (!champsim::ramulator2_available())
    SKIP("native build disabled");
  temporary_yaml file(fixture());
  champsim::channel feeder;
  auto backend = champsim::make_ramulator2_memory_backend(champsim::make_ramulator2_driver(file.config(), request_limit(1)), {&feeder});
  auto& memory = backend->clocked_component();
  memory.warmup = true;
  memory.begin_phase();
  for (uint64_t block = 0; block < 4; ++block)
    REQUIRE(feeder.add_rq(feeder_read(0x100000 + 64 * block)));
  REQUIRE_NOTHROW(memory._operate());
  REQUIRE(feeder.RQ.empty());
  REQUIRE(feeder.returned.size() == 4);
  REQUIRE(native_reads(*backend) == 0);

  memory.warmup = false;
  memory.begin_phase();
  feeder.returned.clear();
  REQUIRE(feeder.add_rq(feeder_read(0x200000)));
  REQUIRE(feeder.add_rq(feeder_read(0x200040)));
  REQUIRE_THROWS_WITH(memory._operate(), Catch::Matchers::ContainsSubstring("limit of 1 accepted read requests"));
  REQUIRE(feeder.RQ.size() == 1);
  REQUIRE(native_reads(*backend) == 1);

  // begin_phase resets native statistics, so the refused read now fits.
  memory.begin_phase();
  REQUIRE_NOTHROW(memory._operate());
  REQUIRE(feeder.RQ.empty());
  REQUIRE(native_reads(*backend) == 1);
  REQUIRE(backend->statistics().sim_ramulator2.value().accepted_reads == 1);
}

// Native counts transactions, not cache blocks: each 64-byte block is two
// LPDDR5 sends, so the limit can fall between a block's fragments.
TEST_CASE("The native request limit can stop a split cache block between its fragments")
{
  if (!champsim::ramulator2_available())
    SKIP("native build disabled");
  temporary_yaml file(fixture("lpddr5"));
  champsim::channel feeder;
  auto backend = champsim::make_ramulator2_memory_backend(champsim::make_ramulator2_driver(file.config(), request_limit(3)), {&feeder});
  auto& memory = backend->clocked_component();
  memory.warmup = false;
  memory.begin_phase();
  REQUIRE(feeder.add_rq(feeder_read(0x100000)));
  REQUIRE(feeder.add_rq(feeder_read(0x100040)));
  REQUIRE_THROWS_WITH(memory._operate(), Catch::Matchers::ContainsSubstring("limit of 3 accepted read requests"));
  REQUIRE(feeder.RQ.size() == 1); // the second block stays at the head with one fragment admitted
  REQUIRE(native_reads(*backend) == 3);
  auto stats = backend->statistics().sim_ramulator2.value();
  REQUIRE(stats.accepted_reads == 2);
  REQUIRE(stats.accepted_fragments == 3);

  // After the phase begins again, only the unaccepted fragment is submitted.
  memory.begin_phase();
  REQUIRE_NOTHROW(memory._operate());
  REQUIRE(feeder.RQ.empty());
  REQUIRE(native_reads(*backend) == 1);
  stats = backend->statistics().sim_ramulator2.value();
  REQUIRE(stats.accepted_reads == 0);
  REQUIRE(stats.accepted_fragments == 1);
  REQUIRE(stats.outstanding_parents == 2);
}

namespace
{
std::string with_plugin(const std::string& text, const std::string& plugin)
{
  return changed(text, "      row_policy:\n", "      controller_plugins:\n" + plugin + "      row_policy:\n");
}
// A three-tick plugin limit stops the memory clock at its fourth tick, and a statistics reset does not restart it.
void require_plugin_tick_limit(const std::string& name, const std::string& yaml)
{
  CAPTURE(name);
  champsim::ramulator2_native_limits limits;
  limits.plugin_ticks = 3;
  temporary_yaml file(yaml);
  auto driver = champsim::make_ramulator2_driver(file.config(), limits);
  for (int i = 0; i < 3; ++i)
    driver->tick();
  REQUIRE_THROWS_WITH(driver->tick(),
                      Catch::Matchers::ContainsSubstring("native controller plugin " + name) && Catch::Matchers::ContainsSubstring("limit of 3 ticks"));
  REQUIRE(counter(driver->statistics(), {"memory_system", "controller", "channel0", "cycles"}) == 3);
  // Native never resets the plugin counter, so a statistics reset does not restart the limit.
  driver->reset_stats();
  REQUIRE_THROWS_WITH(driver->tick(), Catch::Matchers::ContainsSubstring("native controller plugin " + name));
  REQUIRE(counter(driver->statistics(), {"memory_system", "controller", "channel0", "cycles"}) == 0);
}
} // namespace

// AQUA and RRS require a RITAddrMapper; see the row-indirection mapper case for the tag.
TEST_CASE("Native plugins over RITAddrMapper with a never-reset signed tick counter stop the memory clock at its limit", "[rit-addr-mapper]")
{
  if (!champsim::ramulator2_available())
    SKIP("native build disabled");
  const auto rit = changed(fixture(), "      addr_mapper:\n        impl: RoBaRaCoCh\n",
                           "      addr_mapper:\n        impl: RITAddrMapper\n        addr_mapper:\n          impl: RoBaRaCoCh\n");
  require_plugin_tick_limit(
      "AQUA", with_plugin(rit, "        - impl: AQUA\n          num_art_entries: 16\n          num_fpt_entries: 16\n          num_qrows_per_bank: 64\n"
                               "          art_threshold: 1000\n"));
  require_plugin_tick_limit(
      "RRS", with_plugin(rit, "        - impl: RRS\n          num_hrt_entries: 16\n          num_rit_entries: 16\n          rss_threshold: 1000\n"));
}

TEST_CASE("Native plugins with a never-reset signed tick counter stop the memory clock at its limit")
{
  if (!champsim::ramulator2_available())
    SKIP("native build disabled");
  const auto original = fixture();
  // DDR4_VRR is DDR4 with a VRR command and an nVRR timing appended. Native
  // AllBank refresh has no scope for it, and refresh is irrelevant here.
  const auto vrr_channel = [&] {
    auto text = changed(original, "impl: AllBank\n        scatter_interval: 0\n        debug: false\n", "impl: NoRefresh\n");
    text = changed(changed(text, "impl: DDR4\n", "impl: DDR4_VRR\n"), "2, 833]", "2, 833, 1]");
    return changed(text, "command_cycles: [1, 1, 1, 1, 1, 1, 1, 1]", "command_cycles: [1, 1, 1, 1, 1, 1, 1, 1, 1]");
  }();
  const std::string hydra = "        - impl: Hydra\n          hydra_tracking_threshold: 1000\n          hydra_group_threshold: 800\n";
  require_plugin_tick_limit(
      "Graphene",
      with_plugin(vrr_channel,
                  "        - impl: Graphene\n          num_table_entries: 16\n          activation_threshold: 1000\n          reset_period_ns: 64000000\n"));
  require_plugin_tick_limit("Hydra", with_plugin(vrr_channel, hydra));

  champsim::ramulator2_native_limits limits;
  limits.plugin_ticks = 3;
  // Controllers need not list the same plugins, so the one that does can be
  // any of them. Hydra over DDR4_VRR, not a RITAddrMapper plugin, keeps this
  // case free of the pinned RITAddrMapper leak.
  const auto vrr_controller = vrr_channel.substr(vrr_channel.find("    - impl: GenericDDR"));
  for (const auto& [where, yaml] : std::vector<std::pair<std::string, std::string>>{{"first", with_plugin(vrr_channel, hydra) + vrr_controller},
                                                                                    {"second", vrr_channel + with_plugin(vrr_controller, hydra)}}) {
    CAPTURE(where);
    temporary_yaml file(changed(yaml, "interleave_bits: 0", "interleave_bits: 27"));
    auto driver = champsim::make_ramulator2_driver(file.config(), limits);
    for (int i = 0; i < 3; ++i)
      driver->tick();
    REQUIRE_THROWS_WITH(driver->tick(),
                        Catch::Matchers::ContainsSubstring("native controller plugin Hydra") && Catch::Matchers::ContainsSubstring("limit of 3 ticks"));
    for (const auto* channel : {"channel0", "channel1"})
      REQUIRE(counter(driver->statistics(), {"memory_system", "controller", channel, "cycles"}) == 3);
  }
  // A plugin whose counters are 64-bit, or no plugin, leaves the memory clock unlimited.
  for (const auto& yaml :
       {original, with_plugin(original, "        - impl: CommandCounter\n          commands_to_count: [ACT, RD]\n          path: unused.csv\n")}) {
    temporary_yaml file(yaml);
    auto driver = champsim::make_ramulator2_driver(file.config(), limits);
    for (int i = 0; i < 10; ++i)
      REQUIRE_NOTHROW(driver->tick());
  }
}
