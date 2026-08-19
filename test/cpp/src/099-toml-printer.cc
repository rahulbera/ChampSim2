#include <catch.hpp>
#include <fmt/core.h>

#include "cache_stats.h"
#include "core_stats.h"
#include "dram_stats.h"
#include "stats_printer.h"

// The TOML statistics printer. These tests pin the exact emitted text, in the
// same spirit as 198/498/798 for the plain printer: the document is a public
// interface that analysis scripts parse, so a silent key rename is a breaking
// change and must fail here.
//
// Two rules the whole format rests on, asserted throughout:
//   * every ratio is emitted at two decimal places, AND the exact integer
//     numerator and denominator it came from are emitted alongside it, so
//     rounding never destroys information;
//   * a ratio with a zero denominator is `nan` -- a real TOML float literal --
//     rather than an omitted key, so the schema never varies between runs.

TEST_CASE("An empty core emits every key with a nan for each undefined ratio")
{
  cpu_stats given{};
  given.name = "test_cpu";

  std::vector<std::string> expected{"[core.cpu0]",
                                    "name = \"test_cpu\"",
                                    "instructions = 0",
                                    "cycles = 0",
                                    "ipc = nan",
                                    "total_branches = 0",
                                    "total_mispredicts = 0",
                                    "mpki = nan",
                                    "branch_prediction_accuracy = nan",
                                    "total_rob_occupancy_at_mispredict = 0",
                                    "avg_rob_occupancy_at_mispredict = nan",
                                    "cycles_on_wrong_path = 0",
                                    "cyc_wpki = nan",
                                    "avg_cycles_per_mispredict = nan",
                                    "",
                                    "[core.cpu0.mispredict]",
                                    "branch_direct_jump = 0",
                                    "branch_indirect = 0",
                                    "branch_conditional = 0",
                                    "branch_direct_call = 0",
                                    "branch_indirect_call = 0",
                                    "branch_return = 0",
                                    "",
                                    "[core.cpu0.executed]",
                                    "branch_direct_jump = 0",
                                    "branch_indirect = 0",
                                    "branch_conditional = 0",
                                    "branch_direct_call = 0",
                                    "branch_indirect_call = 0",
                                    "branch_return = 0"};

  REQUIRE_THAT(champsim::toml_printer::format(given, "core.cpu0"), Catch::Matchers::RangeEquals(expected));
}

TEST_CASE("A core ratio is rounded to two decimals and keeps its exact operands")
{
  cpu_stats given{};
  given.name = "test_cpu";
  given.end_instrs = 300;
  given.end_cycles = 700;

  const auto lines = champsim::toml_printer::format(given, "core.cpu0");

  // 300/700 = 0.428571... -- the rounded value is a convenience, and both
  // operands survive exactly so the full-precision ratio is recoverable.
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"ipc = 0.43"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"instructions = 300"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"cycles = 700"}));
}

TEST_CASE("Executed and mispredicted branches are counted in separate tables")
{
  // The executed census is the stat the JSON printer never carried: without it
  // the per-type miss RATE can only be approximated from a census of the trace,
  // which covers a different instruction window than the ROI.
  cpu_stats given{};
  given.name = "test_cpu";
  given.end_instrs = 1000;
  given.end_cycles = 1000;
  given.total_branch_types.set(branch_type::BRANCH_INDIRECT, 400);
  given.branch_type_misses.set(branch_type::BRANCH_INDIRECT, 100);

  const auto lines = champsim::toml_printer::format(given, "core.cpu0");

  const auto mispredict_at = std::distance(std::begin(lines), std::find(std::begin(lines), std::end(lines), "[core.cpu0.mispredict]"));
  const auto executed_at = std::distance(std::begin(lines), std::find(std::begin(lines), std::end(lines), "[core.cpu0.executed]"));
  REQUIRE(mispredict_at < executed_at);

  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"total_branches = 400"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"total_mispredicts = 100"}));
  // 100 misses in 1000 instructions
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"mpki = 100.00"}));
  // 300 of 400 branches correct
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"branch_prediction_accuracy = 75.00"}));

  // The counts land in their own tables, so the same key name appears twice
  // with different values -- once under each header.
  REQUIRE(std::count(std::begin(lines), std::end(lines), "branch_indirect = 400") == 1);
  REQUIRE(std::count(std::begin(lines), std::end(lines), "branch_indirect = 100") == 1);
}

TEST_CASE("An empty cache emits the full per-CPU access matrix")
{
  // Unlike the plain printer -- which emits nothing at all for a cache that saw
  // no traffic -- every key is always present, so a parser can index the
  // document without first checking which caches happened to be used.
  cache_stats given{};
  given.name = "test_cache";

  std::vector<std::string> expected{"[cache.test_cache]",
                                    "name = \"test_cache\"",
                                    "total_miss_latency_cycles = 0",
                                    "miss_latency = nan",
                                    "",
                                    "[cache.test_cache.prefetch]",
                                    "requested = 0",
                                    "issued = 0",
                                    "useful = 0",
                                    "useless = 0",
                                    "fill = 0",
                                    "",
                                    "[cache.test_cache.cpu0]",
                                    "load_hit = 0",
                                    "load_miss = 0",
                                    "load_miss_merge = 0",
                                    "load_fill = 0",
                                    "rfo_hit = 0",
                                    "rfo_miss = 0",
                                    "rfo_miss_merge = 0",
                                    "rfo_fill = 0",
                                    "prefetch_hit = 0",
                                    "prefetch_miss = 0",
                                    "prefetch_miss_merge = 0",
                                    "prefetch_fill = 0",
                                    "write_hit = 0",
                                    "write_miss = 0",
                                    "write_miss_merge = 0",
                                    "write_fill = 0",
                                    "translation_hit = 0",
                                    "translation_miss = 0",
                                    "translation_miss_merge = 0",
                                    "translation_fill = 0"};

  REQUIRE_THAT(champsim::toml_printer::format(given, "cache.test_cache"), Catch::Matchers::RangeEquals(expected));
}

TEST_CASE("Average miss latency excludes prefetch fills from its denominator")
{
  // The denominator is downstream DEMAND fills, i.e. every fill except the ones
  // the prefetcher caused. Emitting the fills per access type is what makes
  // that denominator reconstructible -- neither the plain nor the JSON printer
  // ever exposed it.
  cache_stats given{};
  given.name = "test_cache";
  given.fill.set({access_type::LOAD, 0}, 10);
  given.fill.set({access_type::PREFETCH, 0}, 5);
  given.total_miss_latency_cycles = 100;

  const auto lines = champsim::toml_printer::format(given, "cache.test_cache");

  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"total_miss_latency_cycles = 100"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"load_fill = 10"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"prefetch_fill = 5"}));
  // 100 cycles over the 10 demand fills, with the 5 prefetch fills excluded
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"miss_latency = 10.00"}));
}

TEST_CASE("Prefetcher counters are separated from PREFETCH-type accesses")
{
  // Both are legitimately called "prefetch", so they live in different tables:
  // [<cache>.prefetch] counts what the prefetcher did, while the per-CPU table
  // counts accesses whose type was PREFETCH. Conflating them would silently
  // double-report.
  cache_stats given{};
  given.name = "test_cache";
  given.pf_requested = 7;
  given.pf_useful = 3;
  given.hits.set({access_type::PREFETCH, 0}, 11);

  const auto lines = champsim::toml_printer::format(given, "cache.test_cache");

  const auto prefetch_table = std::find(std::begin(lines), std::end(lines), "[cache.test_cache.prefetch]");
  const auto cpu_table = std::find(std::begin(lines), std::end(lines), "[cache.test_cache.cpu0]");
  REQUIRE(prefetch_table != std::end(lines));
  REQUIRE(cpu_table != std::end(lines));

  REQUIRE(std::find(prefetch_table, cpu_table, "requested = 7") != cpu_table);
  REQUIRE(std::find(prefetch_table, cpu_table, "useful = 3") != cpu_table);
  REQUIRE(std::find(cpu_table, std::end(lines), "prefetch_hit = 11") != std::end(lines));
}

TEST_CASE("A DRAM channel emits the write-queue-full count the JSON printer dropped")
{
  // WQ_FULL is reported by the plain printer and was absent from the JSON
  // entirely; dbus congestion is emitted as both the raw pair and the ratio.
  dram_stats given{};
  given.name = "test_dram";
  given.RQ_ROW_BUFFER_HIT = 5;
  given.RQ_ROW_BUFFER_MISS = 7;
  given.WQ_FULL = 3;
  given.dbus_cycle_congested = 90;
  given.dbus_count_congested = 4;
  given.refresh_cycles = 11;

  std::vector<std::string> expected{
      "[dram.channel0]",        "name = \"test_dram\"", "rq_row_buffer_hit = 5",     "rq_row_buffer_miss = 7",   "wq_row_buffer_hit = 0",
      "wq_row_buffer_miss = 0", "wq_full = 3",          "dbus_cycle_congested = 90", "dbus_count_congested = 4", "avg_dbus_congested_cycle = 22.50",
      "refreshes_issued = 11"};

  REQUIRE_THAT(champsim::toml_printer::format(given, "dram.channel0"), Catch::Matchers::RangeEquals(expected));
}

TEST_CASE("A DRAM channel that never congested reports nan, not a zero")
{
  // The plain printer writes "-" here. Zero would be a lie -- it would claim
  // the bus was congested for zero cycles on average, when in fact it never
  // congested and there is no average to report.
  dram_stats given{};
  given.name = "test_dram";

  const auto lines = champsim::toml_printer::format(given, "dram.channel0");

  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"avg_dbus_congested_cycle = nan"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"dbus_count_congested = 0"}));
}

namespace
{
champsim::phase_stats one_of_everything()
{
  champsim::phase_stats stats{};
  stats.name = "Simulation";
  stats.trace_names = {"/traces/a.champsimtrace.xz"};

  cpu_stats core{};
  core.name = "CPU 0";
  core.end_instrs = 10;
  core.end_cycles = 20;
  stats.roi_cpu_stats = {core};
  stats.sim_cpu_stats = {core};

  cache_stats cache{};
  cache.name = "cpu0_L1D";
  stats.roi_cache_stats = {cache};
  stats.sim_cache_stats = {cache};

  dram_stats channel{};
  channel.name = "Channel 0";
  stats.roi_dram_stats = {channel};
  stats.sim_dram_stats = {channel};

  return stats;
}
} // namespace

TEST_CASE("The document opens with a meta table declaring the schema version")
{
  // A parser that meets a future revision of this format needs a way to tell
  // which one it is holding without guessing from the keys.
  auto phase = one_of_everything();
  std::vector<champsim::phase_stats> given{phase};

  const auto lines = champsim::toml_printer::format(given);

  REQUIRE_THAT(lines.at(0), Catch::Matchers::StartsWith("#"));
  REQUIRE(std::find(std::begin(lines), std::end(lines), "[meta]") != std::end(lines));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"schema_version = 1"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"num_cpus = 1"}));
}

TEST_CASE("A phase is keyed by its lower-cased name but preserves the original")
{
  auto phase = one_of_everything();
  std::vector<champsim::phase_stats> given{phase};

  const auto lines = champsim::toml_printer::format(given);

  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"[phase.simulation]"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"name = \"Simulation\""}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"[phase.simulation.trace]"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"cpu0 = \"/traces/a.champsimtrace.xz\""}));
}

TEST_CASE("The region of interest is emitted and the whole-run section is not, by default")
{
  // A run with a single region of interest -- which is the usual case -- makes
  // the whole-run section a copy of the region-of-interest one, so it is off
  // unless asked for. [meta] records which, so a consumer can tell a suppressed
  // section from one this ChampSim never produced.
  auto phase = one_of_everything();
  std::vector<champsim::phase_stats> given{phase};

  const auto lines = champsim::toml_printer::format(given);

  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"[phase.simulation.roi.core.cpu0]"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"[phase.simulation.roi.cache.cpu0_l1d]"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"[phase.simulation.roi.dram.channel0]"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"sim_stats = false"}));

  const auto is_sim_table = [](const auto& line) {
    return line.rfind("[phase.simulation.sim.", 0) == 0;
  };
  REQUIRE(std::none_of(std::begin(lines), std::end(lines), is_sim_table));
}

TEST_CASE("The whole-run section is emitted when it is asked for")
{
  auto phase = one_of_everything();
  std::vector<champsim::phase_stats> given{phase};

  const auto lines = champsim::toml_printer::format(given, true);

  for (const auto* section : {"roi", "sim"}) {
    REQUIRE_THAT(lines, Catch::Matchers::Contains(fmt::format("[phase.simulation.{}.core.cpu0]", section)));
    REQUIRE_THAT(lines, Catch::Matchers::Contains(fmt::format("[phase.simulation.{}.cache.cpu0_l1d]", section)));
    REQUIRE_THAT(lines, Catch::Matchers::Contains(fmt::format("[phase.simulation.{}.dram.channel0]", section)));
  }
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"sim_stats = true"}));
}

TEST_CASE("A component name that is not a bare TOML key is quoted in the path")
{
  // Cache names come straight from the configuration file and nothing
  // sanitises them. Upstream ChampSim names caches "cpu0->L1D"; '>' is not a
  // bare-key character, so an unquoted path would not parse.
  auto phase = one_of_everything();
  phase.roi_cache_stats.at(0).name = "cpu0->L1D";
  phase.sim_cache_stats.at(0).name = "cpu0->L1D";
  std::vector<champsim::phase_stats> given{phase};

  const auto lines = champsim::toml_printer::format(given);

  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"[phase.simulation.roi.cache.\"cpu0->l1d\"]"}));
  // and the original spelling survives inside the table
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"name = \"cpu0->L1D\""}));
}

TEST_CASE("A trace path containing a quote or a backslash is escaped")
{
  auto phase = one_of_everything();
  phase.trace_names = {"/tmp/od\"d\\path"};
  std::vector<champsim::phase_stats> given{phase};

  const auto lines = champsim::toml_printer::format(given);

  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"cpu0 = \"/tmp/od\\\"d\\\\path\""}));
}

TEST_CASE("Two cache names differing only in case do not collapse onto one table")
{
  // Lower-casing is not injective. TOML forbids defining a table twice, so a
  // collision does not corrupt one section -- it makes the WHOLE document
  // unparseable, and it does so silently at emit time. The configuration layer
  // keys caches on their exact name, so two separately-reachable caches really
  // can differ only in case; when the lower-cased form would collide, the exact
  // spelling is used instead, which is unique by construction.
  auto phase = one_of_everything();
  cache_stats other = phase.roi_cache_stats.at(0);
  phase.roi_cache_stats.at(0).name = "SharedL1";
  other.name = "sharedl1";
  phase.roi_cache_stats.push_back(other);
  phase.sim_cache_stats = phase.roi_cache_stats;
  std::vector<champsim::phase_stats> given{phase};

  const auto lines = champsim::toml_printer::format(given);

  std::vector<std::string> headers{};
  std::copy_if(std::begin(lines), std::end(lines), std::back_inserter(headers),
               [](const auto& line) { return line.rfind("[phase.simulation.roi.cache.", 0) == 0; });

  auto unique_headers = headers;
  std::sort(std::begin(unique_headers), std::end(unique_headers));
  REQUIRE(std::adjacent_find(std::begin(unique_headers), std::end(unique_headers)) == std::end(unique_headers));

  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"[phase.simulation.roi.cache.SharedL1]"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"[phase.simulation.roi.cache.sharedl1]"}));
}

TEST_CASE("Names that do not collide are still lower-cased")
{
  // The disambiguation must not leak into the ordinary case: a name only keeps
  // its original spelling when lower-casing it would actually collide.
  auto phase = one_of_everything();
  cache_stats other = phase.roi_cache_stats.at(0);
  other.name = "cpu0_L1I";
  phase.roi_cache_stats.push_back(other);
  phase.sim_cache_stats = phase.roi_cache_stats;
  std::vector<champsim::phase_stats> given{phase};

  const auto lines = champsim::toml_printer::format(given);

  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"[phase.simulation.roi.cache.cpu0_l1d]"}));
  REQUIRE_THAT(lines, Catch::Matchers::Contains(std::string{"[phase.simulation.roi.cache.cpu0_l1i]"}));
}
