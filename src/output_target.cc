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

#include "output_target.h"

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <random>
#include <system_error>
#include <unistd.h>
#include <fmt/core.h>
#include <sys/stat.h>

#include "stats_printer.h"

namespace
{
namespace fs = std::filesystem;

bool same_file(const struct stat& lhs, const struct stat& rhs) { return lhs.st_dev == rhs.st_dev && lhs.st_ino == rhs.st_ino; }

// `name` resolved as open() resolves it: one symbolic link at a time, a
// relative link from the directory that holds it, and each directory
// canonicalized by the kernel. Nothing is folded by text, so a '..' after a
// missing or non-directory component fails here exactly as it does in open().
// The last component of the result is not a symbolic link; it may not exist.
// Empty when the kernel could not reach that far either.
std::optional<fs::path> resolve(const fs::path& name)
{
  constexpr int max_links = 40; // Linux's own limit
  auto path = name;
  for (int hop = 0; hop <= max_links; ++hop) {
    const auto last = path.filename();
    if (last.empty() || last == "." || last == "..") {
      return std::nullopt;
    }
    std::error_code error;
    const auto parent = path.parent_path();
    const auto directory = fs::canonical(parent.empty() ? fs::path{"."} : parent, error);
    if (error || !fs::is_directory(directory, error)) {
      return std::nullopt;
    }
    const auto candidate = directory / last;
    const auto status = fs::symlink_status(candidate, error);
    if (status.type() == fs::file_type::none) {
      return std::nullopt;
    }
    if (!fs::is_symlink(status)) {
      return candidate;
    }
    const auto link = fs::read_symlink(candidate, error);
    if (error) {
      return std::nullopt;
    }
    path = link.is_absolute() ? link : directory / link;
  }
  return std::nullopt;
}

// Up to `size` leading bytes of an open file, or empty on a read error.
std::optional<std::string> read_head(int descriptor, std::size_t size)
{
  std::string head(size, '\0');
  std::size_t filled = 0;
  while (filled < size) {
    const auto got = ::read(descriptor, std::data(head) + filled, size - filled);
    if (got < 0 && errno == EINTR) {
      continue;
    }
    if (got < 0) {
      return std::nullopt;
    }
    if (got == 0) {
      break;
    }
    filled += static_cast<std::size_t>(got);
  }
  head.resize(filled);
  return head;
}

// A new, empty, uniquely named file beside `target`, created exclusively so it
// can never be someone else's file. Created with the usual permissions of a
// new file. Empty if the directory will not take it.
std::optional<fs::path> create_sibling(const fs::path& target)
{
  constexpr int max_attempts = 100;
  std::random_device entropy;
  for (int attempt = 0; attempt < max_attempts; ++attempt) {
    const auto candidate = target.parent_path() / fmt::format(".{}.{:08x}{:08x}.tmp", target.filename().string(), entropy(), entropy());
    errno = 0;
    if (std::FILE* created = std::fopen(candidate.string().c_str(), "wx"); created != nullptr) {
      std::fclose(created);
      return candidate;
    }
    if (errno != EEXIST) {
      return std::nullopt;
    }
  }
  return std::nullopt;
}

int write_all(int descriptor, std::string_view bytes)
{
  while (!std::empty(bytes)) {
    const auto wrote = ::write(descriptor, std::data(bytes), std::size(bytes));
    if (wrote < 0 && errno == EINTR) {
      continue;
    }
    if (wrote < 0) {
      return errno;
    }
    bytes.remove_prefix(static_cast<std::size_t>(wrote));
  }
  return 0;
}
} // namespace

namespace champsim::output
{
plan_result plan(const std::string& name, const std::vector<std::string>& traces)
{
  const auto refuse = [](std::string message) {
    return plan_result{std::nullopt, std::move(message)};
  };
  const auto cannot_open = [&] {
    return refuse(fmt::format("cannot open '{}' to receive the TOML statistics.", name));
  };

  // What open() reaches through the name, if anything. Every decision below
  // is about that file, never about the spelling.
  struct stat reached {
  };
  const bool exists = ::stat(name.c_str(), &reached) == 0;
  if (!exists && errno != ENOENT) {
    return cannot_open();
  }

  // Where it is, so that a replacement can be made in its directory.
  auto located = resolve(name);
  struct stat at_location {
  };
  const bool location_exists = located && ::lstat(located->c_str(), &at_location) == 0;
  const int location_errno = errno;
  if (exists) {
    // A name only the kernel can follow -- /proc/self/fd/N naming a pipe --
    // has no directory to put a replacement in.
    if (!location_exists || !same_file(at_location, reached)) {
      located.reset();
    }
  } else if (!located || location_exists || location_errno != ENOENT) {
    return cannot_open();
  }
  const fs::path reachable = located ? *located : fs::path{name};

  for (const auto& trace : traces) {
    std::error_code equivalent_error, canonical_error;
    const bool same_file_as_trace = exists && fs::equivalent(reachable, trace, equivalent_error);
    const auto input = fs::canonical(trace, canonical_error);
    if (same_file_as_trace || (located && !canonical_error && input == *located)) {
      return refuse(fmt::format("TOML output '{}' aliases an input trace '{}'.", name, trace));
    }
  }

  if (exists && S_ISDIR(reached.st_mode)) {
    return cannot_open();
  }
  if (exists && !S_ISREG(reached.st_mode)) {
    // /dev/null, the pipe or terminal behind /dev/stdout, a FIFO, a process
    // substitution's /dev/fd entry: written in place, unprobed, because
    // opening a FIFO here would consume the reader waiting for the document.
    return plan_result{target{name, reachable, write_mode::in_place}, {}};
  }

  if (exists) {
    // A trace the optional value consumed, a --config source, the native
    // YAML: none of them begins like a statistics document, and replacing any
    // of them would be unrecoverable. Opening it for writing (without
    // truncation) is also what refuses a read-only document.
    const int descriptor = ::open(reachable.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (descriptor < 0) {
      return cannot_open();
    }
    const auto& signature = champsim::toml_printer::document_signature;
    const auto head = read_head(descriptor, std::size(signature));
    ::close(descriptor);
    if (!head) {
      return cannot_open();
    }
    if (!std::empty(*head) && *head != signature) {
      return refuse(fmt::format("TOML output '{}' is not a ChampSim statistics document; refusing to replace it. If it is a trace, --toml took it as the "
                                "output filename: use --toml=FILE, or put -- before the trace paths.",
                                name));
    }
  }

  // The replacement is created beside the target, so that directory must
  // accept a new file.
  if (!located) {
    return cannot_open();
  }
  const auto probe = create_sibling(*located);
  if (!probe) {
    return cannot_open();
  }
  std::error_code remove_error;
  fs::remove(*probe, remove_error);
  return plan_result{target{name, *located, write_mode::replace_by_rename}, {}};
}

operations system_operations()
{
  operations ops;
  ops.rename = [](const fs::path& from, const fs::path& to) {
    return ::rename(from.c_str(), to.c_str()) == 0 ? 0 : errno;
  };
  ops.write_in_place = [](const fs::path& path, std::string_view document) {
    const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOCTTY | O_CLOEXEC, 0666);
    if (descriptor < 0) {
      return errno;
    }
    const int write_error = write_all(descriptor, document);
    const int close_error = ::close(descriptor) == 0 ? 0 : errno;
    return write_error != 0 ? write_error : close_error;
  };
  return ops;
}

write_result write(const target& destination, std::string_view document, const operations& ops)
{
  write_result result;
  const auto failed = [&] {
    result.messages.push_back(fmt::format("ERROR: failed to write the TOML statistics to '{}'.", destination.name));
    return result;
  };

  if (destination.mode == write_mode::in_place) {
    if (ops.write_in_place(destination.path, document) != 0) {
      return failed();
    }
    result.written = true;
    return result;
  }

  // A regular target receives a finished document by rename, so a write that
  // fails here -- or a run killed while writing -- leaves whatever the target
  // held before exactly as it was.
  const auto temporary = create_sibling(destination.path);
  if (!temporary) {
    return failed();
  }
  bool written = false;
  {
    std::ofstream file{*temporary, std::ios::binary};
    file.write(std::data(document), static_cast<std::streamsize>(std::size(document)));
    file.flush();
    written = static_cast<bool>(file);
    file.close();
    written = written && !file.fail();
  }
  std::error_code replace_error;
  if (written) {
    // rename substitutes a new file: carry over an existing document's
    // permissions rather than the defaults it was created with.
    if (const auto previous = fs::status(destination.path, replace_error); fs::is_regular_file(previous)) {
      fs::permissions(*temporary, previous.permissions(), replace_error);
    }
    written = ops.rename(*temporary, destination.path) == 0;
  }
  if (!written) {
    fs::remove(*temporary, replace_error);
    return failed();
  }
  result.written = true;
  return result;
}
} // namespace champsim::output
