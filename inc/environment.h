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

// The parsed configuration this build was generated from, rendered as a TOML
// fragment by the Python configuration layer (config/config_record.py) and
// specialized per build id alongside generated_environment. It lets a
// statistics document state which machine produced it -- the JSON is otherwise
// compiled away into literals and unavailable at run time.
//
// Only src/main.cc reads this, and only inside `#ifndef CHAMPSIM_TEST_BUILD`:
// main.cc IS linked into the test binary, where CHAMPSIM_BUILD expands to the
// non-literal `0xTEST`, so nothing outside that guard may name a specialization.
template <unsigned long long ID>
struct config_record;

// The runtime module registry for this build: per-kind name arrays and
// factories producing the type-erased module pimpls, specialized alongside
// generated_environment by config.sh (config/module_registry.py) and defined
// in the generated registry translation unit. Lets a runtime configuration
// key select any compiled module without rebuilding.
template <unsigned long long ID>
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
