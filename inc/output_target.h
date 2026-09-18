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
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// A named --toml statistics document: checked before the run, without writing,
// truncating or leaving anything, and written in place after a successful run.
// An existing file is truncated and rewritten, so it keeps its inode, links,
// owner, group, ACLs and permissions; a new file is created with mode 0666,
// so it gets whatever the umask or a default ACL gives any new file there.
namespace champsim::output
{
enum class write_mode {
  // A regular file, or a name that reaches nothing yet.
  regular_file,
  // A FIFO, not opened until the run is over, or a device or socket, opened
  // non-blocking at startup to check it; each is written after the run.
  special_file,
  // The file behind standard output or standard error -- a log the shell
  // redirected to, the pipe or terminal behind /dev/stdout -- so the document
  // is written to that stream after everything already printed, as an
  // unnamed --toml writes it to stdout.
  standard_stream,
};

struct target {
  // The file the kernel reaches through the name: every symbolic link
  // followed, so the document lands where the link points.
  std::filesystem::path path;
  write_mode mode{write_mode::regular_file};
  // STDOUT_FILENO or STDERR_FILENO for write_mode::standard_stream.
  int stream{-1};
  // Whether the name reached a file when it was checked.
  bool exists{false};
};

struct plan_result {
  std::optional<target> planned;
  // Why there is no target, as one line without the "ERROR: " prefix.
  std::string error;
};

// Checks everything about a named output before a trace is opened. An
// existing regular file must open for writing and either be empty or be
// readable and begin like a statistics document; a device or socket must
// open; a new name must be creatable, and the file created to show it is
// removed again.
plan_result plan(const std::string& name, const std::vector<std::string>& traces);

struct write_result {
  bool written{false};
  // Complete lines for stderr.
  std::vector<std::string> messages;
};

// After a successful run: checks the name again as plan() does, since the
// filesystem may have changed during the run, and refuses without writing if
// it now fails; otherwise writes the document in place, creating the file
// only where nothing is.
write_result write(const std::string& name, const std::vector<std::string>& traces, std::string_view document);
} // namespace champsim::output

#endif
