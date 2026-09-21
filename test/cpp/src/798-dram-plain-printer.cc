#include <catch.hpp>
#include <limits>
#include <sstream>
#include <nlohmann/json.hpp>

#include "dram_stats.h"
#include "stats_printer.h"

TEST_CASE("An empty DRAM stats prints zero")
{
  dram_stats given{};
  given.name = "test_channel";

  std::vector<std::string> expected{"test_channel RQ ROW_BUFFER_HIT:          0",
                                    "  ROW_BUFFER_MISS:          0",
                                    "  AVG DBUS CONGESTED CYCLE: -",
                                    "test_channel WQ ROW_BUFFER_HIT:          0",
                                    "  ROW_BUFFER_MISS:          0",
                                    "  FULL:          0",
                                    "test_channel REFRESHES ISSUED: -"};

  REQUIRE_THAT(champsim::plain_printer::format(given), Catch::Matchers::RangeEquals(expected));
}

TEST_CASE("The DRAM RQ row buffer hit counter increments the printed stats")
{
  dram_stats given{};
  given.name = "test_channel";
  given.RQ_ROW_BUFFER_HIT = 255;

  std::vector<std::string> expected{"test_channel RQ ROW_BUFFER_HIT:        255",
                                    "  ROW_BUFFER_MISS:          0",
                                    "  AVG DBUS CONGESTED CYCLE: -",
                                    "test_channel WQ ROW_BUFFER_HIT:          0",
                                    "  ROW_BUFFER_MISS:          0",
                                    "  FULL:          0",
                                    "test_channel REFRESHES ISSUED: -"};

  REQUIRE_THAT(champsim::plain_printer::format(given), Catch::Matchers::RangeEquals(expected));
}

TEST_CASE("The DRAM RQ row buffer miss counter increments the printed stats")
{
  dram_stats given{};
  given.name = "test_channel";
  given.RQ_ROW_BUFFER_MISS = 255;

  std::vector<std::string> expected{"test_channel RQ ROW_BUFFER_HIT:          0",
                                    "  ROW_BUFFER_MISS:        255",
                                    "  AVG DBUS CONGESTED CYCLE: -",
                                    "test_channel WQ ROW_BUFFER_HIT:          0",
                                    "  ROW_BUFFER_MISS:          0",
                                    "  FULL:          0",
                                    "test_channel REFRESHES ISSUED: -"};

  REQUIRE_THAT(champsim::plain_printer::format(given), Catch::Matchers::RangeEquals(expected));
}

TEST_CASE("The DRAM WQ row buffer hit counter increments the printed stats")
{
  dram_stats given{};
  given.name = "test_channel";
  given.WQ_ROW_BUFFER_HIT = 255;

  std::vector<std::string> expected{"test_channel RQ ROW_BUFFER_HIT:          0",
                                    "  ROW_BUFFER_MISS:          0",
                                    "  AVG DBUS CONGESTED CYCLE: -",
                                    "test_channel WQ ROW_BUFFER_HIT:        255",
                                    "  ROW_BUFFER_MISS:          0",
                                    "  FULL:          0",
                                    "test_channel REFRESHES ISSUED: -"};

  REQUIRE_THAT(champsim::plain_printer::format(given), Catch::Matchers::RangeEquals(expected));
}

TEST_CASE("The DRAM WQ row buffer miss counter increments the printed stats")
{
  dram_stats given{};
  given.name = "test_channel";
  given.WQ_ROW_BUFFER_MISS = 255;

  std::vector<std::string> expected{"test_channel RQ ROW_BUFFER_HIT:          0",
                                    "  ROW_BUFFER_MISS:          0",
                                    "  AVG DBUS CONGESTED CYCLE: -",
                                    "test_channel WQ ROW_BUFFER_HIT:          0",
                                    "  ROW_BUFFER_MISS:        255",
                                    "  FULL:          0",
                                    "test_channel REFRESHES ISSUED: -"};

  REQUIRE_THAT(champsim::plain_printer::format(given), Catch::Matchers::RangeEquals(expected));
}

TEST_CASE("The DRAM WQ full counter increments the printed stats")
{
  dram_stats given{};
  given.name = "test_channel";
  given.WQ_FULL = 255;

  std::vector<std::string> expected{"test_channel RQ ROW_BUFFER_HIT:          0",
                                    "  ROW_BUFFER_MISS:          0",
                                    "  AVG DBUS CONGESTED CYCLE: -",
                                    "test_channel WQ ROW_BUFFER_HIT:          0",
                                    "  ROW_BUFFER_MISS:          0",
                                    "  FULL:        255",
                                    "test_channel REFRESHES ISSUED: -"};

  REQUIRE_THAT(champsim::plain_printer::format(given), Catch::Matchers::RangeEquals(expected));
}

TEST_CASE("The DRAM dbus congestion counters increment the printed stats")
{
  dram_stats given{};
  given.name = "test_channel";
  given.dbus_cycle_congested = 100;
  given.dbus_count_congested = 100;

  std::vector<std::string> expected{"test_channel RQ ROW_BUFFER_HIT:          0",
                                    "  ROW_BUFFER_MISS:          0",
                                    "  AVG DBUS CONGESTED CYCLE: 1",
                                    "test_channel WQ ROW_BUFFER_HIT:          0",
                                    "  ROW_BUFFER_MISS:          0",
                                    "  FULL:          0",
                                    "test_channel REFRESHES ISSUED: -"};

  REQUIRE_THAT(champsim::plain_printer::format(given), Catch::Matchers::RangeEquals(expected));
}

TEST_CASE("The DRAM refresh counters increment the printed stats")
{
  dram_stats given{};
  given.name = "test_channel";
  given.refresh_cycles = 100;

  std::vector<std::string> expected{"test_channel RQ ROW_BUFFER_HIT:          0", "  ROW_BUFFER_MISS:          0", "  AVG DBUS CONGESTED CYCLE: -",
                                    "test_channel WQ ROW_BUFFER_HIT:          0", "  ROW_BUFFER_MISS:          0", "  FULL:          0",
                                    "test_channel REFRESHES ISSUED:        100"};

  REQUIRE_THAT(champsim::plain_printer::format(given), Catch::Matchers::RangeEquals(expected));
}

TEST_CASE("Native plain statistics identify the backend and keep independent adapter counts")
{
  champsim::phase_stats phase{};
  phase.name = "Simulation";
  phase.roi_ramulator2.emplace();
  phase.roi_ramulator2->accepted_reads = 2;
  phase.roi_ramulator2->accepted_fragments = 4;
  phase.roi_ramulator2->outstanding_parents = 1;
  phase.roi_ramulator2->out_of_range_prefetches = 5;
  phase.roi_ramulator2->native.values = {{{"memory_system", "controller", "channel0", "read_hits"}, int64_t{3}}};
  const auto lines = champsim::plain_printer::format(phase);
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"Ramulator2 Statistics"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"accepted_reads = 2"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"accepted_fragments = 4"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"outstanding_parents = 1"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"out_of_range_prefetches = 5"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"read_hits = 3"}));
  REQUIRE(std::find(lines.begin(), lines.end(), "DRAM Statistics") == lines.end());
  REQUIRE(std::none_of(lines.begin(), lines.end(), [](const auto& line) { return line.find("DBUS") != std::string::npos; }));
}

TEST_CASE("Native JSON preserves independent counters, typed leaves and escaped path components")
{
  champsim::phase_stats phase{};
  phase.name = "Simulation";
  phase.roi_ramulator2.emplace();
  phase.roi_ramulator2->accepted_reads = 2;
  phase.roi_ramulator2->accepted_fragments = 4;
  phase.roi_ramulator2->outstanding_parents = 1;
  phase.roi_ramulator2->out_of_range_prefetches = 5;
  phase.roi_ramulator2->native.yaml = "count: 42\n";
  phase.roi_ramulator2->native.values = {{{"memory_system", "controller", "channel0", "count"}, int64_t{42}},
                                         {{"plugin.with.dots", "0", "enabled"}, true},
                                         {{"unsigned_max"}, std::numeric_limits<uint64_t>::max()},
                                         {{"nan"}, std::numeric_limits<double>::quiet_NaN()}};
  phase.sim_ramulator2 = phase.roi_ramulator2;
  phase.sim_ramulator2->completed_reads = 1;
  std::vector<champsim::phase_stats> phases{phase};
  std::ostringstream output;
  champsim::json_printer{output}.print(phases);
  const auto doc = nlohmann::json::parse(output.str());
  const auto& roi = doc.at(0).at("roi");
  REQUIRE(roi.at("ramulator2").at("adapter").at("accepted_reads") == 2);
  REQUIRE(roi.at("ramulator2").at("adapter").at("accepted_fragments") == 4);
  REQUIRE(roi.at("ramulator2").at("adapter").at("outstanding_parents") == 1);
  REQUIRE(roi.at("ramulator2").at("adapter").at("out_of_range_prefetches") == 5);
  REQUIRE(doc.at(0).at("sim").at("ramulator2").at("adapter").at("completed_reads") == 1);
  REQUIRE_FALSE(roi.contains("DRAM"));
  const auto& native = roi.at("ramulator2").at("native");
  REQUIRE(native.at("memory_system").at("controller").at("channel0").at("count") == 42);
  REQUIRE(native.at("plugin.with.dots").at("0").at("enabled") == true);
  REQUIRE(native.at("unsigned_max").get<uint64_t>() == std::numeric_limits<uint64_t>::max());
  REQUIRE(native.at("nan") == "nan");
  REQUIRE(roi.at("ramulator2").at("native_yaml") == "count: 42\n");
}
