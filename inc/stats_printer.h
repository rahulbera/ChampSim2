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
  explicit toml_printer(std::ostream& str, bool include_sim = false) : stream(str), include_sim_stats(include_sim) {}
  void print(std::vector<phase_stats>& stats);

  static std::vector<std::string> format(O3_CPU::stats_type stats, std::string_view path);
  static std::vector<std::string> format(CACHE::stats_type stats, std::string_view path);
  static std::vector<std::string> format(DRAM_CHANNEL::stats_type stats, std::string_view path);
  static std::vector<std::string> format(phase_stats& stats, bool include_sim = false);
  static std::vector<std::string> format(std::vector<phase_stats>& stats, bool include_sim = false);
};
} // namespace champsim
