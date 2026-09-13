/*
 *    Copyright 2023 The ChampSim Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numeric>
#include <optional>
#include <random>
#include <string>
#include <vector>
#include <CLI/CLI.hpp>
#include <fmt/core.h>
#include <fmt/ranges.h>

#include "cache.h" // for CACHE
#include "champsim.h"
#include "ramulator2_driver.h"
#ifndef CHAMPSIM_TEST_BUILD
#include "registry.inc"
#endif
#include "defaults.hpp"
#include "environment.h"
#include "event_listeners.h"
#include "ooo_cpu.h" // for O3_CPU
#include "phase_info.h"
#include "runtime_config.h"
#include "static_environment.h"
#include "stats_printer.h"
#include "tracereader.h"
#include "vmem.h"

namespace champsim
{
std::vector<phase_stats> main(environment& env, std::vector<phase_info>& phases, std::vector<tracereader>& traces, const simulation_knobs& knobs);
}

#ifndef CHAMPSIM_TEST_BUILD
using configured_environment = champsim::static_environment;

constexpr uint64_t configured_heartbeat_frequency = champsim::defs::heartbeat_frequency;
const std::size_t NUM_CPUS = champsim::defs::num_cpus;
const unsigned BLOCK_SIZE = champsim::defs::block_size;
const unsigned PAGE_SIZE = champsim::defs::page_size;
#endif
const unsigned LOG2_BLOCK_SIZE = champsim::lg2(BLOCK_SIZE);
const unsigned LOG2_PAGE_SIZE = champsim::lg2(PAGE_SIZE);

#ifndef CHAMPSIM_TEST_BUILD
namespace
{
// The file a named --toml document replaces, with every symbolic link
// followed -- a dangling one included -- so that the rename lands where the
// link points and leaves the link itself alone. Empty on any error.
std::optional<std::filesystem::path> resolve_output_target(const std::filesystem::path& name)
{
  namespace fs = std::filesystem;
  constexpr int max_links = 40; // Linux's own limit
  std::error_code error;
  fs::path path = name;
  for (int hop = 0; hop < max_links; ++hop) {
    const auto followed = fs::status(path, error);
    if (followed.type() == fs::file_type::none) {
      return std::nullopt;
    }
    if (fs::exists(followed) || !fs::is_symlink(fs::symlink_status(path, error))) {
      const auto resolved = fs::weakly_canonical(path, error);
      return error ? std::nullopt : std::optional{resolved};
    }
    const auto link = fs::read_symlink(path, error);
    if (error) {
      return std::nullopt;
    }
    path = link.is_absolute() ? link : fs::absolute(path, error).parent_path() / link;
  }
  return std::nullopt;
}

// A new, empty, uniquely named file beside `target`, created exclusively so it
// can never be someone else's file. Created with the usual permissions of a
// new file. Empty if the directory will not take it.
std::optional<std::filesystem::path> create_sibling(const std::filesystem::path& target)
{
  constexpr int max_attempts = 100;
  std::random_device entropy;
  for (int attempt = 0; attempt < max_attempts; ++attempt) {
    const auto candidate = target.parent_path() / fmt::format(".{}.{:08x}{:08x}.tmp", target.filename().string(), entropy(), entropy());
    errno = 0;
    if (std::FILE* created = std::fopen(candidate.string().c_str(), "wx"); created != nullptr) {
      std::fclose(created);
      return candidate;
    }
    if (errno != EEXIST) {
      return std::nullopt;
    }
  }
  return std::nullopt;
}
} // namespace

int main(int argc, char** argv) // NOLINT(bugprone-exception-escape)
{
  // The runtime configuration store. --config and --set apply to it in argv
  // order (trigger_on_parse), so the LAST definition of a key wins regardless
  // of which source it came from. The environment is constructed only after
  // the store is complete and validated.
  champsim::runtime_config runtime_cfg{};

  CLI::App app{"A microarchitecture simulator for research and education"};

  bool knob_cloudsuite{false};
  unsigned trace_version{1};
  // Default from the configuration's heartbeat_frequency (config.sh bakes it
  // into the generated environment); --heartbeat-frequency still overrides.
  uint64_t heartbeat_frequency{configured_heartbeat_frequency};
  long long warmup_instructions = 0;
  long long simulation_instructions = std::numeric_limits<long long>::max();
  std::string json_file_name;
  std::string toml_file_name;
  bool toml_sim_stats{false};
  std::vector<std::string> requested_listeners;
  std::vector<std::string> trace_names;

  // Applied after the environment exists; a parse-time callback would touch
  // an environment that is no longer constructed before CLI11_PARSE.
  bool hide_heartbeat{false};
  bool list_knobs{false};

  app.add_flag("-c,--cloudsuite", knob_cloudsuite, "Read all traces using the cloudsuite format");

  app.add_option_function<std::string>(
         "--config",
         [&runtime_cfg](const std::string& path) {
           try {
             runtime_cfg.load_file(path);
           } catch (const std::runtime_error& err) {
             throw CLI::ValidationError{"--config", err.what()};
           }
         },
         "A TOML file of runtime configuration values. May repeat; applied in command-line order, interleaved with --set, and the last definition of a key "
         "wins")
      ->trigger_on_parse();
  app.add_option_function<std::string>(
         "--set",
         [&runtime_cfg](const std::string& assignment) {
           try {
             runtime_cfg.set(assignment);
           } catch (const std::runtime_error& err) {
             throw CLI::ValidationError{"--set", err.what()};
           }
         },
         "One runtime configuration value, as key=value (e.g. --set ooo_cpu.cpu0.rob_size=512). May repeat; same ordering rule as --config")
      ->trigger_on_parse();
  app.add_flag("--knobs", list_knobs, "List every runtime configuration key this binary consults, with its baked default, then exit");
  app.add_option("--trace-version", trace_version, "The trace record format version: 1 (64-byte, default) or 2 (512-byte, with memory values)")
      ->check(CLI::IsMember({1U, 2U}));
  app.add_flag("--hide-heartbeat", hide_heartbeat, "Hide the heartbeat output");
  auto* heartbeat_option = app.add_option("--heartbeat-frequency", heartbeat_frequency,
                                          "Instructions retired between heartbeat lines (default: the configuration's heartbeat_frequency)")
                               ->check(CLI::PositiveNumber);
  auto* warmup_instr_option = app.add_option("-w,--warmup-instructions", warmup_instructions, "The number of instructions in the warmup phase");
  auto* deprec_warmup_instr_option =
      app.add_option("--warmup_instructions", warmup_instructions, "[deprecated] use --warmup-instructions instead")->excludes(warmup_instr_option);
  auto* sim_instr_option = app.add_option("-i,--simulation-instructions", simulation_instructions,
                                          "The number of instructions in the detailed phase. If not specified, run to the end of the trace.");
  auto* deprec_sim_instr_option =
      app.add_option("--simulation_instructions", simulation_instructions, "[deprecated] use --simulation-instructions instead")->excludes(sim_instr_option);

  // --json is retired in favour of --toml, but is still accepted so that a
  // stale script fails at once with an explanation rather than running for
  // hours and writing no machine-readable output at all.
  auto* json_option = app.add_option("--json", json_file_name, "[removed] the statistics document is now TOML -- use --toml")->expected(0, 1);

  auto* toml_option =
      app.add_option("--toml", toml_file_name, "The name of the file to receive TOML output. If no name is specified, stdout will be used")->expected(0, 1);

  app.add_flag("--toml-sim-stats", toml_sim_stats,
               "Also write the whole-run statistics to the TOML output. Off by default: with a single region of interest they merely repeat it");

  app.add_option("--listeners", requested_listeners, "A list of the listeners to be attached to the run");

  app.add_option("traces", trace_names, "The paths to the traces")->expected(NUM_CPUS)->check(CLI::ExistingFile);

  CLI11_PARSE(app, argc, argv);

  // Refuse before a single trace is opened: discovering this after a long run
  // would cost the whole run.
  if (json_option->count() > 0) {
    fmt::print(stderr,
               "ERROR: --json has been removed. The machine-readable statistics document is now TOML, with lower_snake_case keys and a different structure; "
               "pass --toml{} instead.\n",
               std::empty(json_file_name) ? std::string{} : fmt::format(" {}", json_file_name));
    return 1;
  }

  // An optional --toml argument can consume the next trace pathname. When that
  // leaves too few traces, this check catches it; when one path too many was
  // given, the count still matches, and only the output checks below stand
  // between the consumed trace and its replacement by statistics.
  if (!list_knobs && std::size(trace_names) != NUM_CPUS) {
    fmt::print(stderr, "ERROR: expected {} trace(s), got {}. Use -- before trace paths when omitting the --toml filename.\n", NUM_CPUS, std::size(trace_names));
    return 1;
  }

  // Everything about a named --toml output is checked here, before a trace is
  // opened, and nothing here writes to it: a statistics path that cannot be
  // written should cost nothing, and a startup error must not cost an earlier
  // document. A regular file (or a path with nothing there yet) is replaced
  // only at the end of a successful run, by renaming a finished document over
  // it; `toml_target` is that file, with its links followed. An existing
  // target that is not a regular file -- /dev/null, the pipe or terminal
  // behind /dev/stdout, a FIFO, a process substitution's /dev/fd entry -- is
  // written in place, unprobed: opening a FIFO here would consume the reader
  // waiting for the document. A directory is refused.
  std::optional<std::filesystem::path> toml_target{};
  if (!list_knobs && toml_option->count() > 0 && !std::empty(toml_file_name)) {
    std::error_code output_error;
    const auto output_path = std::filesystem::weakly_canonical(toml_file_name, output_error);
    for (const auto& trace : trace_names) {
      std::error_code input_error, equivalent_error;
      const auto input_path = std::filesystem::weakly_canonical(trace, input_error);
      const bool same_file = std::filesystem::equivalent(toml_file_name, trace, equivalent_error);
      if (same_file || (!input_error && !output_error && input_path == output_path)) {
        fmt::print(stderr, "ERROR: TOML output '{}' aliases an input trace '{}'.\n", toml_file_name, trace);
        return 1;
      }
    }

    const auto cannot_open = [&toml_file_name] {
      fmt::print(stderr, "ERROR: cannot open '{}' to receive the TOML statistics.\n", toml_file_name);
      return 1;
    };
    std::error_code status_error;
    const auto output_status = std::filesystem::status(toml_file_name, status_error);
    if (output_status.type() == std::filesystem::file_type::none || std::filesystem::is_directory(output_status)) {
      return cannot_open();
    }
    if (!std::filesystem::exists(output_status) || std::filesystem::is_regular_file(output_status)) {
      toml_target = resolve_output_target(toml_file_name);
      if (!toml_target) {
        return cannot_open();
      }

      if (std::filesystem::is_regular_file(output_status)) {
        // A trace the optional value consumed, a --config source, the native
        // YAML: none of them begins like a statistics document, and replacing
        // any of them would be unrecoverable.
        std::ifstream existing{*toml_target, std::ios::binary};
        if (!existing) {
          return cannot_open();
        }
        const auto& signature = champsim::toml_printer::document_signature;
        std::string head(std::size(signature), '\0');
        existing.read(std::data(head), static_cast<std::streamsize>(std::size(head)));
        head.resize(static_cast<std::size_t>(existing.gcount()));
        if (!std::empty(head) && head != signature) {
          fmt::print(stderr,
                     "ERROR: TOML output '{}' is not a ChampSim statistics document; refusing to replace it. If it is a trace, --toml took it as the "
                     "output filename: use --toml=FILE, or put -- before the trace paths.\n",
                     toml_file_name);
          return 1;
        }
        // Renaming would replace a read-only document; opening it for writing
        // (without truncation) is what refuses, as writing in place used to.
        if (const std::fstream writable{*toml_target, std::ios::in | std::ios::out | std::ios::binary}; !writable) {
          return cannot_open();
        }
      }

      // The replacement is created beside the target, so that directory must
      // accept a new file.
      const auto probe = create_sibling(*toml_target);
      if (!probe) {
        return cannot_open();
      }
      std::error_code remove_error;
      std::filesystem::remove(*probe, remove_error);
    }
  }

  // The simulation-level knobs live outside the generated constructor, so
  // their keys are appended to the generated manifest by hand.
  // The environment is constructed here -- after the CLI parse -- so that
  // every builder argument can consult the completed store. Every store read,
  // including the sim.* knobs, sits inside this try so that a type mismatch
  // anywhere is a clean diagnostic and exit 1, never an uncaught exception.
  champsim::simulation_knobs sim_knobs{};
  std::optional<configured_environment> built_environment{};
  try {
    // Heartbeat precedence: the explicit CLI flag beats the store, which
    // beats the configuration's baked default. positive_value gives the store
    // path the same zero-rejection the flag's PositiveNumber check gives the
    // CLI path.
    // Consulted unconditionally: post-construction validation treats an
    // unconsulted key as unknown, and a key that merely LOST a precedence
    // contest is not unknown.
    const auto stored_heartbeat = runtime_cfg.positive_value<uint64_t>("sim.heartbeat_frequency", configured_heartbeat_frequency);
    if (heartbeat_option->count() == 0) {
      heartbeat_frequency = stored_heartbeat;
    } else {
      // The flag won. [config] states what the run used, so it must report the
      // flag's value and not the key's -- otherwise a document that claims to
      // reproduce its run would replay it at a different heartbeat.
      runtime_cfg.override_effective("sim.heartbeat_frequency", heartbeat_frequency);
    }
    sim_knobs.livelock_period = runtime_cfg.positive_value<uint64_t>("sim.livelock_period", sim_knobs.livelock_period);

    built_environment.emplace(runtime_cfg);
    auto deadlock_default = sim_knobs.deadlock_cycle;
    // Native mode only: the 10 us allowance in simulator ticks, and the tick.
    std::optional<std::pair<long long, champsim::chrono::picoseconds>> native_allowance{};
    if (built_environment->memory_view().name() == "ramulator2") {
      // Native refresh can block all demand progress longer than the legacy
      // 500-tick guard. Allow 10 us, measured in the actual simulation quantum,
      // without treating idle native ticks as progress or changing explicit overrides.
      auto quantum = champsim::chrono::picoseconds::max();
      for (const champsim::operable& op : built_environment->operable_view()) {
        if (op.clock_period <= champsim::chrono::picoseconds::zero()) {
          throw std::runtime_error{"runtime config: ramulator2 requires a positive operable clock period"};
        }
        quantum = std::min(quantum, op.clock_period);
      }
      constexpr champsim::chrono::picoseconds allowance{10'000'000};
      const auto ticks = allowance / quantum + (allowance % quantum != champsim::chrono::picoseconds::zero());
      deadlock_default = std::max(deadlock_default, static_cast<int>(ticks));
      native_allowance.emplace(ticks, quantum);
    }
    sim_knobs.deadlock_cycle = runtime_cfg.positive_value<int>("sim.deadlock_cycle", deadlock_default);

    // An explicit value stays authoritative, but a short one is almost always
    // inherited rather than chosen: every legacy --knobs dump and statistics
    // document records sim.deadlock_cycle = 500, and 500 ticks at 250 ps abort
    // inside the first DDR4 refresh stall. Stderr, so --knobs stays TOML.
    const auto applied_settings = runtime_cfg.applied();
    const bool explicit_deadlock_cycle =
        std::any_of(std::cbegin(applied_settings), std::cend(applied_settings), [](const auto& setting) { return setting.first == "sim.deadlock_cycle"; });
    if (native_allowance && explicit_deadlock_cycle && sim_knobs.deadlock_cycle < native_allowance->first) {
      fmt::print(stderr,
                 "WARNING: sim.deadlock_cycle = {} allows only {} ps without progress, less than the 10 us ramulator2 allowance ({} ticks, this machine's "
                 "native default). The explicit value is kept. Legacy --knobs dumps and statistics documents record 500: remove sim.deadlock_cycle from "
                 "a converted configuration, or raise it.\n",
                 sim_knobs.deadlock_cycle, (native_allowance->second * sim_knobs.deadlock_cycle).count(), deadlock_default);
    }
  } catch (const std::runtime_error& err) {
    fmt::print(stderr, "ERROR: {}\n", err.what());
    return 1;
  }
  configured_environment& gen_environment = *built_environment;

  // Every key the machine understands has now been read, so a key nothing
  // consulted is one this binary has no use for: a typo, or a knob aimed at a
  // component or module that is not part of this machine. This is the whole
  // of key validation -- scalars, module selections and module knobs alike.
  //
  // Ahead of --knobs deliberately: --knobs is what a user reaches for to check
  // whether a key is real, so accepting a bad one there and exiting 0 answers
  // the question wrongly.
  {
    const auto complaints = runtime_cfg.unconsulted_keys();
    if (!std::empty(complaints)) {
      for (const auto& complaint : complaints) {
        fmt::print(stderr, "ERROR: {}\n", complaint);
      }
      fmt::print(stderr, "Run --knobs with no other configuration to list every key this binary accepts.\n");
      return 1;
    }
  }

  if (list_knobs) {
    // Reported after construction, so these are the keys this machine actually
    // consults with the values this invocation would use -- including the knob
    // tables of any runtime-selected module.
    for (const auto& [knob_key, effective] : runtime_cfg.consulted()) {
      fmt::print("{} = {}\n", knob_key, effective);
    }
    // Commented, so that the whole listing is a valid TOML document:
    //     bin/champsim --knobs > my.toml
    // gives a complete, editable starting configuration.
    fmt::print("\n# DRAM backends (dram-model): {}\n", champsim::ramulator2_available() ? "legacy, ramulator2" : "legacy");
    using registry = champsim::configured::module_registry;
    fmt::print("\n# Selectable modules (per component, via the keys above):\n");
    const auto print_names = [](std::string_view kind, const auto& names) {
      fmt::print("# {}: {}\n", kind, fmt::join(names, ", "));
    };
    print_names("branch_predictor", registry::branch_predictor);
    print_names("btb", registry::btb);
    print_names("prefetcher", registry::prefetcher);
    print_names("replacement", registry::replacement);
    return 0;
  }

  if (hide_heartbeat) {
    for (O3_CPU& cpu : gen_environment.cpu_view()) {
      cpu.show_heartbeat = false;
    }
  }

  init_event_listeners(requested_listeners);
  std::get<0>(listeners).instructions_between_printouts = heartbeat_frequency;

  const bool warmup_given = (warmup_instr_option->count() > 0) || (deprec_warmup_instr_option->count() > 0);
  const bool simulation_given = (sim_instr_option->count() > 0) || (deprec_sim_instr_option->count() > 0);

  if (deprec_warmup_instr_option->count() > 0) {
    fmt::print("WARNING: option --warmup_instructions is deprecated. Use --warmup-instructions instead.\n");
  }

  if (deprec_sim_instr_option->count() > 0) {
    fmt::print("WARNING: option --simulation_instructions is deprecated. Use --simulation-instructions instead.\n");
  }

  if (simulation_given && !warmup_given) {
    // Warmup is 20% by default
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
    warmup_instructions = simulation_instructions / 5;
  }

  std::vector<champsim::tracereader> traces;
  std::transform(std::begin(trace_names), std::end(trace_names), std::back_inserter(traces),
                 [knob_cloudsuite, trace_version, repeat = simulation_given, i = uint8_t(0)](auto name) mutable {
                   return get_tracereader(name, i++, knob_cloudsuite, repeat, trace_version);
                 });

  std::vector<champsim::phase_info> phases{
      {champsim::phase_info{"Warmup", true, warmup_instructions, std::vector<std::size_t>(std::size(trace_names), 0), trace_names},
       champsim::phase_info{"Simulation", false, simulation_instructions, std::vector<std::size_t>(std::size(trace_names), 0), trace_names}}};

  for (auto& p : phases) {
    std::iota(std::begin(p.trace_index), std::end(p.trace_index), 0);
  }

  fmt::print("\n*** ChampSim Multicore Out-of-Order Simulator ***\nWarmup Instructions: {}\nSimulation Instructions: {}\nNumber of CPUs: {}\nPage size: {}\n\n",
             phases.at(0).length, phases.at(1).length, std::size(gen_environment.cpu_view()), PAGE_SIZE);

  decltype(champsim::main(gen_environment, phases, traces, sim_knobs)) phase_stats{};
  try {
    phase_stats = champsim::main(gen_environment, phases, traces, sim_knobs);
  } catch (const std::runtime_error& err) {
    // A module may reject its placement at initialize() -- CBP6 tenants throw
    // when instantiated beyond cpu0 -- which under runtime selection is a
    // one-flag user error, not a programming error.
    fmt::print(stderr, "ERROR: {}\n", err.what());
    return 1;
  }

  fmt::print("\nChampSim completed all CPUs\n\n");

  champsim::plain_printer{std::cout}.print(phase_stats);

  for (O3_CPU& cpu : gen_environment.cpu_view()) {
    cpu.impl_branch_predictor_final_stats();
  }

  for (CACHE& cache : gen_environment.cache_view()) {
    cache.impl_prefetcher_final_stats();
  }

  for (CACHE& cache : gen_environment.cache_view()) {
    cache.impl_replacement_final_stats();
  }

  // What produced this document, so that a result file states which machine and
  // which run it came from.
  champsim::toml_printer::run_info run{};
  run.ramulator2 = gen_environment.memory_view().config_record();
  // Identifies the MACHINE rather than the build: a content hash of the
  // effective configuration, so two runs that simulate the same thing share it
  // however their configuration was expressed.
  run.build_id = champsim::toml_printer::config_id(runtime_cfg.consulted());
  // [config] is the EFFECTIVE configuration: every key the machine consulted
  // with the value it actually used. With no configure-time JSON there is no
  // "baked" configuration to record, and this is the more useful record --
  // one table with no overlay arithmetic, and directly feedable back as
  // --config.
  const auto effective_config = champsim::toml_printer::format_config(runtime_cfg.consulted());
  const auto effective_config_text = fmt::format("{}", fmt::join(effective_config, "\n"));
  run.config_toml = effective_config_text;
  run.warmup_instructions = warmup_instructions;
  run.simulation_instructions = simulation_instructions;
  run.trace_version = static_cast<int>(trace_version);
  // argv joined verbatim. The shell has already expanded process substitution,
  // globs and quotes by now, so a re-runnable command line cannot be honestly
  // reconstructed; recording what this process actually received is the only
  // claim that is true.
  run.command_line = fmt::format("{}", fmt::join(std::vector<std::string>(argv, std::next(argv, argc)), " "));
  run.config_files = fmt::format("{}", fmt::join(runtime_cfg.files(), ","));
  run.overrides = runtime_cfg.applied();
  // [config_override] records what took effect. A stored heartbeat that lost
  // to the explicit --heartbeat-frequency flag did not, so it must not be
  // recorded as if it had.
  if (heartbeat_option->count() > 0) {
    run.overrides.erase(
        std::remove_if(std::begin(run.overrides), std::end(run.overrides), [](const auto& entry) { return entry.first == "sim.heartbeat_frequency"; }),
        std::end(run.overrides));
  }

  // champsim::json_printer is still compiled and linked, so it cannot rot
  // silently, but it is unreachable at run time: --json is rejected above.
  if (toml_option->count() > 0) {
    if (toml_file_name.empty()) {
      champsim::toml_printer{std::cout, toml_sim_stats, run}.print(phase_stats);
    } else {
      // A regular target receives a finished document by rename, so a write
      // that fails here -- or a run killed while writing -- leaves whatever
      // the target held before exactly as it was. Other targets (/dev/stdout,
      // a FIFO) are written in place.
      const auto temporary = toml_target ? create_sibling(*toml_target) : std::optional<std::filesystem::path>{};
      bool written = !toml_target || temporary.has_value();
      if (written) {
        std::ofstream toml_file{toml_target ? *temporary : std::filesystem::path{toml_file_name}};
        champsim::toml_printer{toml_file, toml_sim_stats, run}.print(phase_stats);
        toml_file.flush();
        written = static_cast<bool>(toml_file);
        toml_file.close();
        written = written && !toml_file.fail();
      }

      std::error_code replace_error;
      if (written && toml_target) {
        // rename substitutes a new file: carry over an existing document's
        // permissions rather than the defaults it was created with.
        if (const auto previous = std::filesystem::status(*toml_target, replace_error); std::filesystem::is_regular_file(previous)) {
          std::filesystem::permissions(*temporary, previous.permissions(), replace_error);
        }
        std::filesystem::rename(*temporary, *toml_target, replace_error);
        written = !replace_error;
      }

      // A full disk here would otherwise discard the run's only
      // machine-readable output and still report success.
      if (!written) {
        if (temporary) {
          std::filesystem::remove(*temporary, replace_error);
        }
        fmt::print(stderr, "ERROR: failed to write the TOML statistics to '{}'.\n", toml_file_name);
        return 1;
      }
    }
  }

  return 0;
}
#endif
