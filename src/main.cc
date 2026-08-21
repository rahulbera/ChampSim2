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
#include <fstream>
#include <iterator>
#include <numeric>
#include <optional>
#include <string>
#include <vector>
#include <CLI/CLI.hpp>
#include <fmt/core.h>
#include <fmt/ranges.h>

#include "cache.h" // for CACHE
#include "champsim.h"
#ifndef CHAMPSIM_TEST_BUILD
#include "core_inst.inc"
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
#ifdef CHAMPSIM_STATIC_ENV
// Phase C step 1: the hand-written machine, selectable so it can be compared
// against the generated one before the generated path is removed.
using configured_environment = champsim::static_environment;
#else
using configured_environment = champsim::configured::generated_environment<CHAMPSIM_BUILD>;
#endif

#ifdef CHAMPSIM_STATIC_ENV
constexpr uint64_t configured_heartbeat_frequency = champsim::defs::heartbeat_frequency;
const std::size_t NUM_CPUS = champsim::defs::num_cpus;
const unsigned BLOCK_SIZE = champsim::defs::block_size;
const unsigned PAGE_SIZE = champsim::defs::page_size;
#else
constexpr uint64_t configured_heartbeat_frequency = configured_environment::heartbeat_frequency;
const std::size_t NUM_CPUS = configured_environment::num_cpus;

const unsigned BLOCK_SIZE = configured_environment::block_size;
const unsigned PAGE_SIZE = configured_environment::page_size;
#endif
#endif
const unsigned LOG2_BLOCK_SIZE = champsim::lg2(BLOCK_SIZE);
const unsigned LOG2_PAGE_SIZE = champsim::lg2(PAGE_SIZE);

#ifndef CHAMPSIM_TEST_BUILD
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

  // Same reasoning as the guard above: a statistics path that cannot be written
  // should cost nothing, but finding that out after the run costs the run. The
  // probe truncates the file, which the run would do anyway.
  if (toml_option->count() > 0 && !std::empty(toml_file_name)) {
    if (const std::ofstream probe{toml_file_name}; !probe) {
      fmt::print(stderr, "ERROR: cannot open '{}' to receive the TOML statistics.\n", toml_file_name);
      return 1;
    }
  }

  // The simulation-level knobs live outside the generated constructor, so
  // their keys are appended to the generated manifest by hand.
  constexpr std::array<std::string_view, 3> sim_knob_keys{"sim.deadlock_cycle", "sim.heartbeat_frequency", "sim.livelock_period"};

  using registry = champsim::configured::module_registry<CHAMPSIM_BUILD>;

  // A module-knob key names a registered module's table on a component:
  // <component>.<registered module name>.<knob...>. Knob names inside the
  // table are the module's to validate (the consumed-key check below); the
  // module-name segment is checked here so a typo'd module still fails fast.
  const auto is_module_knob_key = [](std::string_view key) {
    const auto component_prefixes = std::array<std::pair<std::string_view, std::vector<std::string_view>>, 2>{
        std::pair{std::string_view{"ooo_cpu."},
                  [] {
                    std::vector<std::string_view> names(std::begin(registry::branch_predictor), std::end(registry::branch_predictor));
                    names.insert(std::end(names), std::begin(registry::btb), std::end(registry::btb));
                    return names;
                  }()},
        std::pair{std::string_view{"cache."},
                  [] {
                    std::vector<std::string_view> names(std::begin(registry::prefetcher), std::end(registry::prefetcher));
                    names.insert(std::end(names), std::begin(registry::replacement), std::end(registry::replacement));
                    return names;
                  }()},
    };
    for (const auto& [kind_prefix, module_names] : component_prefixes) {
      if (key.substr(0, std::size(kind_prefix)) != kind_prefix) {
        continue;
      }
      // <kind>.<component>.<module>.<knob...>: split off the component name,
      // then require a registered module name followed by at least one knob
      // segment.
      const auto after_kind = key.substr(std::size(kind_prefix));
      const auto component_end = after_kind.find('.');
      if (component_end == std::string_view::npos) {
        return false;
      }
      const auto after_component = after_kind.substr(component_end + 1);
      const auto module_end = after_component.find('.');
      if (module_end == std::string_view::npos) {
        return false;
      }
      const auto module_name = after_component.substr(0, module_end);
      return std::find(std::begin(module_names), std::end(module_names), module_name) != std::end(module_names);
    }
    return false;
  };

  // Fail before construction: an unknown key would otherwise be silently
  // ignored, and a sweep that misspells a knob would sweep nothing.
  std::vector<std::string> module_knob_keys{};
  {
    std::vector<std::string_view> valid{std::begin(champsim::configured::config_record<CHAMPSIM_BUILD>::runtime_keys),
                                        std::end(champsim::configured::config_record<CHAMPSIM_BUILD>::runtime_keys)};
    valid.insert(std::end(valid), std::begin(sim_knob_keys), std::end(sim_knob_keys));
    // Collect fully before taking views: growing the vector would reallocate
    // and dangle any view already handed to `valid`.
    for (const auto& [applied_key, rendered] : runtime_cfg.applied()) {
      if (is_module_knob_key(applied_key)) {
        module_knob_keys.push_back(applied_key);
      }
    }
    valid.insert(std::end(valid), std::begin(module_knob_keys), std::end(module_knob_keys));
    const auto complaints = runtime_cfg.unknown_keys(std::begin(valid), std::end(valid));
    if (!std::empty(complaints)) {
      for (const auto& complaint : complaints) {
        fmt::print(stderr, "ERROR: {}\n", complaint);
      }
      fmt::print(stderr, "Run with --knobs to list every key this binary accepts.\n");
      return 1;
    }
  }

  if (list_knobs) {
    // Construct a throwaway environment against the ACTUAL store: the
    // (key, fallback) pairs it consults are this binary's knobs under the
    // given configuration -- including the knob tables of runtime-SELECTED
    // modules, which an empty-store probe could never reach.
    try {
      configured_environment knob_probe{runtime_cfg};
      runtime_cfg.value<int>("sim.deadlock_cycle", champsim::simulation_knobs{}.deadlock_cycle);
      runtime_cfg.value<uint64_t>("sim.heartbeat_frequency", configured_heartbeat_frequency);
      runtime_cfg.value<uint64_t>("sim.livelock_period", champsim::simulation_knobs{}.livelock_period);
    } catch (const std::runtime_error& err) {
      fmt::print(stderr, "ERROR: {}\n", err.what());
      return 1;
    }
    for (const auto& [knob_key, fallback] : runtime_cfg.consulted()) {
      fmt::print("{} = {}\n", knob_key, fallback);
    }
    fmt::print("\nSelectable modules (per component, via the keys above):\n");
    const auto print_names = [](std::string_view kind, const auto& names) {
      fmt::print("{}: {}\n", kind, fmt::join(names, ", "));
    };
    print_names("branch_predictor", registry::branch_predictor);
    print_names("btb", registry::btb);
    print_names("prefetcher", registry::prefetcher);
    print_names("replacement", registry::replacement);
    return 0;
  }

  // --knobs is the only invocation that may omit the traces.
  if (std::size(trace_names) != NUM_CPUS) {
    fmt::print(stderr, "ERROR: expected {} trace(s), got {}.\n", NUM_CPUS, std::size(trace_names));
    return 1;
  }

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
    if (heartbeat_option->count() == 0) {
      heartbeat_frequency = runtime_cfg.positive_value<uint64_t>("sim.heartbeat_frequency", configured_heartbeat_frequency);
    }
    sim_knobs.deadlock_cycle = runtime_cfg.positive_value<int>("sim.deadlock_cycle", sim_knobs.deadlock_cycle);
    sim_knobs.livelock_period = runtime_cfg.positive_value<uint64_t>("sim.livelock_period", sim_knobs.livelock_period);

    built_environment.emplace(runtime_cfg);
  } catch (const std::runtime_error& err) {
    fmt::print(stderr, "ERROR: {}\n", err.what());
    return 1;
  }
  configured_environment& gen_environment = *built_environment;

  // The hook contract: a module consults every knob it owns, unconditionally,
  // during configure(). A module-knob key never consulted therefore targets a
  // module that is not selected on that component, or misspells a knob.
  {
    const auto consulted = runtime_cfg.consulted();
    std::vector<std::string> unconsumed{};
    std::copy_if(std::begin(module_knob_keys), std::end(module_knob_keys), std::back_inserter(unconsumed), [&consulted](const auto& knob_key) {
      return std::none_of(std::begin(consulted), std::end(consulted), [&knob_key](const auto& entry) { return entry.first == knob_key; });
    });
    if (!std::empty(unconsumed)) {
      for (const auto& knob_key : unconsumed) {
        fmt::print(stderr,
                   "ERROR: no selected module consumed configuration key '{}' -- is that module selected on that component, and is the knob name right?\n",
                   knob_key);
      }
      return 1;
    }
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
  // which run it came from. The configuration itself is compiled away into
  // literals by config.sh, so it is carried as a pre-rendered TOML fragment on
  // the generated environment -- see config/config_record.py. Both this and the
  // build id may only be named inside `#ifndef CHAMPSIM_TEST_BUILD`, which is
  // why they are read here and passed to the printer as plain data.
  champsim::toml_printer::run_info run{};
  // Sixteen digits, zero-padded, to match the id in the generated source and in
  // _configuration.mk exactly -- a shake_128 digest may begin with a zero, and
  // "{:#x}" would silently drop it.
  run.build_id = fmt::format("0x{:016x}", static_cast<unsigned long long>(CHAMPSIM_BUILD));
  run.config_toml = champsim::configured::config_record<CHAMPSIM_BUILD>::toml;
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
      std::ofstream toml_file{toml_file_name};
      champsim::toml_printer{toml_file, toml_sim_stats, run}.print(phase_stats);
      toml_file.flush();

      // A full disk here would otherwise discard the run's only
      // machine-readable output and still report success.
      if (!toml_file) {
        fmt::print(stderr, "ERROR: failed to write the TOML statistics to '{}'.\n", toml_file_name);
        return 1;
      }
    }
  }

  return 0;
}
#endif
