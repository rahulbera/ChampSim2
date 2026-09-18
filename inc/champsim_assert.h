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

#ifndef CHAMPSIM_ASSERT_H
#define CHAMPSIM_ASSERT_H

#include <cstdio>
#include <cstdlib>

#ifndef CHAMPSIM_ENABLE_ASSERTIONS
#define CHAMPSIM_ENABLE_ASSERTIONS 1
#endif

namespace champsim
{
namespace detail
{
[[noreturn]]
#if defined(__clang__) || defined(__GNUC__)
__attribute__((cold))
#endif
inline void assertion_failed(const char* expression, const char* file, int line)
{
  std::fprintf(stderr, "ChampSim assertion failed: %s (%s:%d)\n", expression, file, line);
  std::abort();
}
} // namespace detail
} // namespace champsim

#if CHAMPSIM_ENABLE_ASSERTIONS
#define CHAMPSIM_ASSERT(expression) ((expression) ? static_cast<void>(0) : ::champsim::detail::assertion_failed(#expression, __FILE__, __LINE__))
#else
#define CHAMPSIM_ASSERT(expression) static_cast<void>(0)
#endif

#endif
