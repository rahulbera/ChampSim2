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

#ifndef ENVIRONMENT_H
#define ENVIRONMENT_H

#include <functional>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

#include "cache.h"
#include "memory_backend.h"
#include "ooo_cpu.h"
#include "operable.h"
#include "ptw.h"

namespace champsim
{
struct environment {
  virtual std::vector<std::reference_wrapper<O3_CPU>> cpu_view() = 0;
  virtual std::vector<std::reference_wrapper<CACHE>> cache_view() = 0;
  virtual std::vector<std::reference_wrapper<PageTableWalker>> ptw_view() = 0;
  virtual memory_backend& memory_view() = 0;
  virtual std::vector<std::reference_wrapper<operable>> operable_view() = 0;

  // For each operable_view() entry, the core whose private state it reads and writes, or
  // no_lane if it touches state other cores share. A multi-threaded tick runs neighbouring
  // operables of different lanes at once, so two operables in different lanes must never
  // touch the same state; operables in no lane run alone. The default, no lane for anything,
  // keeps a machine serial at every thread count.
  static constexpr std::size_t no_lane = std::numeric_limits<std::size_t>::max();
  virtual std::vector<std::size_t> operable_lanes() { return std::vector<std::size_t>(std::size(operable_view()), no_lane); }

  // For each operable_view() entry, the entries it may interact with: share a channel, or any
  // other state. A multi-threaded tick runs a no-lane operable ahead of a whole run of laned
  // operables it interacts with none of, which can merge two runs into one. The default,
  // unknown, keeps every operable where the order puts it.
  virtual std::optional<std::vector<std::vector<std::size_t>>> operable_interactions() { return std::nullopt; }

  virtual ~environment() = default;
};

namespace configured
{
// The runtime module registry: per-kind name arrays and factories producing
// the type-erased module pimpls, emitted by config.sh
// (config/module_registry.py) from the modules it discovers and defined in the
// generated registry translation unit. Lets a runtime configuration key select
// any compiled module without rebuilding. One configuration per binary, so it
// is a plain struct rather than a per-build specialization.
struct module_registry;

template <typename R, typename... PTWs>
auto build(PTWs... builders)
{
  std::vector<R> retval{};
  retval.reserve(sizeof...(builders));
  (..., retval.emplace_back(builders));
  return retval;
}
} // namespace configured
} // namespace champsim

#endif
