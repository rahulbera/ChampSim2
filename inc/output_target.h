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

#ifndef OUTPUT_TARGET_H
#define OUTPUT_TARGET_H

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Where a named --toml statistics document goes, decided before the run and
// written after it. Nothing here writes to the target until write().
namespace champsim::output
{
enum class write_mode {
  // A finished document in a new '.champsim-toml-<16 hex>.tmp' sibling is
  // renamed over the target; if that rename fails, the target is written in
  // place instead.
  replace_by_rename,
  // The target is opened, truncated and written only once the run is over:
  // anything but a regular file, a hard-linked file (a rename would split its
  // links), or a file whose directory refuses a new sibling.
  in_place,
};

struct target {
  // The name as given on the command line, for messages.
  std::string name;
  // The file the kernel reaches through that name: every symbolic link
  // followed, so a replacement lands where the link points.
  std::filesystem::path path;
  write_mode mode{write_mode::replace_by_rename};
};

struct plan_result {
  std::optional<target> planned;
  // Why there is no target, as one line without the "ERROR: " prefix.
  std::string error;
};

// Checks everything about a named output before a trace is opened, and
// writes, truncates or creates nothing that remains.
plan_result plan(const std::string& name, const std::vector<std::string>& traces);

// The filesystem calls whose failure the final write has to survive, as a
// seam for tests. Each returns 0 or an errno value. write_in_place opens with
// O_TRUNC, so it is only ever called after the run.
struct operations {
  std::function<int(const std::filesystem::path& from, const std::filesystem::path& to)> rename;
  std::function<int(const std::filesystem::path& path, std::string_view document)> write_in_place;
};
operations system_operations();

struct write_result {
  bool written{false};
  // Complete lines for stderr, warnings and errors alike.
  std::vector<std::string> messages;
};

write_result write(const target& destination, std::string_view document, const operations& ops = system_operations());
} // namespace champsim::output

#endif
