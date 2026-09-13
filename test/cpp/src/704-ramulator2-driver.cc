#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "defs.h"
#include "ramulator2_driver.h"
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

TEST_CASE("Native driver serves every admitted flat and row-indirection address mapper")
{
  if (!champsim::ramulator2_available())
    SKIP("native build disabled");
  const auto original = fixture();
  const std::string flat_mapper = "      addr_mapper:\n        impl: RoBaRaCoCh\n";
  for (const auto& mapper : std::vector<std::string>{
           "      addr_mapper:\n        impl: ChRaBaRoCo\n", "      addr_mapper:\n        impl: MOP4CLXOR\n",
           "      addr_mapper:\n        impl: RITAddrMapper\n        addr_mapper:\n          impl: RoBaRaCoCh\n",
           "      addr_mapper:\n        impl: RITAddrMapper\n        reserved_rows_per_bank: 0\n        addr_mapper:\n          impl: MOP4CLXOR\n"}) {
    CAPTURE(mapper);
    temporary_yaml file(changed(original, flat_mapper, mapper));
    auto driver = champsim::make_ramulator2_driver(file.config());
    REQUIRE(driver->size().count() == 8589934592LL);
    unsigned completed = 0;
    REQUIRE(driver->send(false, 0x100000, 0, 64, [&] { ++completed; }));
    REQUIRE(driver->send(false, 8589934592ULL - 64, 0, 64, [&] { ++completed; }));
    for (int i = 0; i < 10000 && completed != 2; ++i)
      driver->tick();
    REQUIRE(completed == 2);
  }
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
