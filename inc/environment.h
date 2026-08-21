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
#include <string_view>
#include <vector>

#include "cache.h"
#include "dram_controller.h"
#include "ooo_cpu.h"
#include "operable.h"
#include "ptw.h"

namespace champsim
{
struct environment {
  virtual std::vector<std::reference_wrapper<O3_CPU>> cpu_view() = 0;
  virtual std::vector<std::reference_wrapper<CACHE>> cache_view() = 0;
  virtual std::vector<std::reference_wrapper<PageTableWalker>> ptw_view() = 0;
  virtual MEMORY_CONTROLLER& dram_view() = 0;
  virtual std::vector<std::reference_wrapper<operable>> operable_view() = 0;
};

namespace configured
{
template <unsigned long long ID>
struct generated_environment;

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
