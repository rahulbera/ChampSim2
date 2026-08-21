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

#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cache.h"
#include "dram_controller.h"
#include "ooo_cpu.h"
#include "phase_info.h"

namespace champsim
{
class plain_printer
{
  std::ostream& stream;

public:
  plain_printer(std::ostream& str) : stream(str) {}
  void print(phase_stats& stats);
  void print(std::vector<phase_stats>& stats);

  static std::vector<std::string> format(O3_CPU::stats_type stats);
  static std::vector<std::string> format(CACHE::stats_type stats);
  static std::vector<std::string> format(DRAM_CHANNEL::stats_type stats);
  static std::vector<std::string> format(phase_stats& stats);
};

class json_printer
{
  std::ostream& stream;

public:
  json_printer(std::ostream& str) : stream(str) {}
  void print(std::vector<phase_stats>& stats);
};

// The machine-readable statistics format. Each component formats into the
// dotted TOML table path it is given, which is what makes the pieces testable
// in isolation -- json_printer has no such seam, which is why it has no tests.
class toml_printer
{
  std::ostream& stream;

  // The whole-run section is a copy of the region-of-interest one whenever
  // there is a single region of interest, which is the usual case, so it is
  // written only on request.
  bool include_sim_stats;

public:
  // What produced this document, as opposed to what it measured. Everything
  // here is supplied by the caller rather than read from a generated symbol,
  // which is what lets this printer be linked into the test binary at all --
  // and what keeps the static format() seam, the only reason it can be pinned
  // by exact output.
  struct run_info {
    // The build id config.sh derived from the parsed configuration, rendered
    // as it appears in the generated source (e.g. "0x2989172160dc027f").
    std::string build_id{};
    // argv, joined. Recorded verbatim rather than prettified: a shell has
    // already expanded process substitution and globs by the time ChampSim
    // sees them, so a re-runnable line cannot be reconstructed honestly.
    std::string command_line{};
    long long warmup_instructions{0};
    long long simulation_instructions{0};
    int trace_version{0};
    // The [config] section, already rendered as TOML by format_config().
    // Spliced verbatim; the printer does not re-parse it.
    std::string_view config_toml{};
    // The runtime configuration files loaded, in order, comma-joined (the
    // document holds no arrays).
    std::string config_files{};
    // The keys the runtime store applied, with values already rendered in TOML
    // syntax. [config] records what config.sh generated from; this records
    // what the run changed on top of it.
    std::vector<std::pair<std::string, std::string>> overrides{};
  };

  // `run_info` cannot appear in a default argument of this class: its default
  // member initializers are not required until the end of the enclosing class,
  // which is after a default argument would need them. Hence overloads.
  explicit toml_printer(std::ostream& str, bool include_sim = false) : stream(str), include_sim_stats(include_sim) {}
  toml_printer(std::ostream& str, bool include_sim, run_info info) : stream(str), include_sim_stats(include_sim), info_(std::move(info)) {}
  void print(std::vector<phase_stats>& stats);

  static std::vector<std::string> format(O3_CPU::stats_type stats, std::string_view path);
  static std::vector<std::string> format(CACHE::stats_type stats, std::string_view path);
  static std::vector<std::string> format(DRAM_CHANNEL::stats_type stats, std::string_view path);
  static std::vector<std::string> format(phase_stats& stats, bool include_sim = false);
  // The effective configuration -- flat dotted keys with values already in
  // TOML syntax -- rendered as the nested [config] table tree.
  static std::vector<std::string> format_config(const std::vector<std::pair<std::string, std::string>>& effective);

  // A stable content hash of the effective configuration: the identity of the
  // MACHINE, not of a build. Two runs simulating the same thing share it
  // however their configuration was expressed.
  static std::string config_id(const std::vector<std::pair<std::string, std::string>>& effective);

  static std::vector<std::string> format(std::vector<phase_stats>& stats, bool include_sim, const run_info& info);
  static std::vector<std::string> format(std::vector<phase_stats>& stats, bool include_sim = false);

private:
  run_info info_{};
};
} // namespace champsim
