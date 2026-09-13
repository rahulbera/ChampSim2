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
#include <cmath>
#include <type_traits>
#include <utility>
#include <fmt/core.h>
#include <nlohmann/json.hpp>

#include "stats_printer.h"

void to_json(nlohmann::json& j, const O3_CPU::stats_type& stats)
{
  constexpr std::array types{branch_type::BRANCH_DIRECT_JUMP, branch_type::BRANCH_INDIRECT,      branch_type::BRANCH_CONDITIONAL,
                             branch_type::BRANCH_DIRECT_CALL, branch_type::BRANCH_INDIRECT_CALL, branch_type::BRANCH_RETURN};

  auto total_mispredictions = std::ceil(
      std::accumulate(std::begin(types), std::end(types), 0LL, [btm = stats.branch_type_misses](auto acc, auto next) { return acc + btm.value_or(next, 0); }));

  std::map<std::string, std::size_t> mpki{};
  for (auto type : types) {
    mpki.emplace(branch_type_names.at(champsim::to_underlying(type)), stats.branch_type_misses.value_or(type, 0));
  }

  j = nlohmann::json{{"instructions", stats.instrs()},
                     {"cycles", stats.cycles()},
                     {"Avg ROB occupancy at mispredict", std::ceil(stats.total_rob_occupancy_at_branch_mispredict) / std::ceil(total_mispredictions)},
                     {"mispredict", mpki},
                     {"cycles on wrong path", stats.cycles_on_wrong_path},
                     {"CycWPKI", std::kilo::num * std::ceil(stats.cycles_on_wrong_path) / std::ceil(stats.instrs())}};
}

void to_json(nlohmann::json& j, const CACHE::stats_type& stats)
{
  using hits_value_type = typename decltype(stats.hits)::value_type;
  using misses_value_type = typename decltype(stats.misses)::value_type;
  using miss_merge_value_type = typename decltype(stats.miss_merge)::value_type;
  using fill_value_type = typename decltype(stats.fill)::value_type;

  std::map<std::string, nlohmann::json> statsmap;
  statsmap.emplace("prefetch requested", stats.pf_requested);
  statsmap.emplace("prefetch issued", stats.pf_issued);
  statsmap.emplace("useful prefetch", stats.pf_useful);
  statsmap.emplace("useless prefetch", stats.pf_useless);

  uint64_t total_downstream_demands = stats.fill.total();
  for (std::size_t cpu = 0; cpu < NUM_CPUS; ++cpu)
    total_downstream_demands -= stats.fill.value_or(std::pair{access_type::PREFETCH, cpu}, fill_value_type{});

  statsmap.emplace("miss latency", std::ceil(stats.total_miss_latency_cycles) / std::ceil(total_downstream_demands));
  for (const auto type : {access_type::LOAD, access_type::RFO, access_type::PREFETCH, access_type::WRITE, access_type::TRANSLATION}) {
    std::vector<hits_value_type> hits;
    std::vector<misses_value_type> misses;
    std::vector<miss_merge_value_type> miss_merges;

    for (std::size_t cpu = 0; cpu < NUM_CPUS; ++cpu) {
      hits.push_back(stats.hits.value_or(std::pair{type, cpu}, hits_value_type{}));
      misses.push_back(stats.misses.value_or(std::pair{type, cpu}, misses_value_type{}));
      miss_merges.push_back(stats.miss_merge.value_or(std::pair{type, cpu}, miss_merge_value_type{}));
    }

    statsmap.emplace(access_type_names.at(champsim::to_underlying(type)), nlohmann::json{{"hit", hits}, {"miss", misses}, {"miss_merge", miss_merges}});
  }

  j = statsmap;
}

void to_json(nlohmann::json& j, const dram_stats stats)
{
  j = nlohmann::json{{"RQ ROW_BUFFER_HIT", stats.RQ_ROW_BUFFER_HIT},
                     {"RQ ROW_BUFFER_MISS", stats.RQ_ROW_BUFFER_MISS},
                     {"WQ ROW_BUFFER_HIT", stats.WQ_ROW_BUFFER_HIT},
                     {"WQ ROW_BUFFER_MISS", stats.WQ_ROW_BUFFER_MISS},
                     {"AVG DBUS CONGESTED CYCLE", (std::ceil(stats.dbus_cycle_congested) / std::ceil(stats.dbus_count_congested))},
                     {"REFRESHES ISSUED", stats.refresh_cycles}};
}

namespace champsim
{
void to_json(nlohmann::json& j, const ramulator2_statistics& stats)
{
  j = {{"adapter",
        {{"accepted_reads", stats.accepted_reads},
         {"accepted_writes", stats.accepted_writes},
         {"completed_reads", stats.completed_reads},
         {"completed_writes", stats.completed_writes},
         {"accepted_fragments", stats.accepted_fragments},
         {"completed_fragments", stats.completed_fragments},
         {"rejected_submissions", stats.rejected_submissions},
         {"out_of_range_prefetches", stats.out_of_range_prefetches},
         {"outstanding_parents", stats.outstanding_parents},
         {"outstanding_fragments", stats.outstanding_fragments},
         {"total_read_latency_ps", stats.total_read_latency_ps},
         {"read_latency_samples", stats.read_latency_samples}}},
       {"native_yaml", stats.native.yaml},
       {"native", nlohmann::json::object()}};
  for (const auto& statistic : stats.native.values) {
    auto* node = &j["native"];
    for (const auto& component : statistic.path) {
      node = &(*node)[component];
    }
    std::visit(
        [node](const auto& value) {
          using type = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<type, double>) {
            // JSON has no nonfinite number literals. Preserve their identity as
            // strings instead of collapsing NaN and both infinities to null.
            if (!std::isfinite(value)) {
              *node = fmt::format("{}", value);
              return;
            }
          }
          *node = value;
        },
        statistic.value);
  }
}

void to_json(nlohmann::json& j, const champsim::phase_stats stats)
{
  std::map<std::string, nlohmann::json> roi_stats;
  roi_stats.emplace("cores", stats.roi_cpu_stats);
  if (stats.roi_ramulator2) {
    roi_stats.emplace("ramulator2", *stats.roi_ramulator2);
  } else {
    roi_stats.emplace("DRAM", stats.roi_dram_stats);
  }
  for (auto x : stats.roi_cache_stats) {
    roi_stats.emplace(x.name, x);
  }

  std::map<std::string, nlohmann::json> sim_stats;
  sim_stats.emplace("cores", stats.sim_cpu_stats);
  if (stats.sim_ramulator2) {
    sim_stats.emplace("ramulator2", *stats.sim_ramulator2);
  } else {
    sim_stats.emplace("DRAM", stats.sim_dram_stats);
  }
  for (auto x : stats.sim_cache_stats) {
    sim_stats.emplace(x.name, x);
  }

  std::map<std::string, nlohmann::json> statsmap{{"name", stats.name}, {"traces", stats.trace_names}};
  statsmap.emplace("roi", roi_stats);
  statsmap.emplace("sim", sim_stats);
  j = statsmap;
}
} // namespace champsim

void champsim::json_printer::print(std::vector<phase_stats>& stats) { stream << nlohmann::json::array_t{std::begin(stats), std::end(stats)}; }
