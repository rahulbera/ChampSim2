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
#include <iostream>
#include <system_error>
#include <unistd.h>
#include <fmt/core.h>
#include <sys/stat.h>

#include "stats_printer.h"

namespace
{
namespace fs = std::filesystem;
using file_status = struct stat;

bool same_file(const file_status& lhs, const file_status& rhs) { return lhs.st_dev == rhs.st_dev && lhs.st_ino == rhs.st_ino; }

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

std::string describe(int error) { return std::generic_category().message(error); }

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

using champsim::output::plan_result;
using champsim::output::target;
using champsim::output::write_mode;

// Every check on a named output. At startup (`probe`), whatever is to be
// opened after the run is also opened now -- a new name by creating it and
// removing it again -- so that a target that cannot be opened costs no run.
// After the run the real open does that instead, and reports its own failure.
plan_result check(const std::string& name, const std::vector<std::string>& traces, bool probe)
{
  const auto refuse = [](std::string message) {
    return plan_result{std::nullopt, std::move(message)};
  };
  const auto cannot_open = [&] {
    return refuse(fmt::format("cannot open '{}' to receive the TOML statistics.", name));
  };

  // What open() reaches through the name, if anything. Every decision below
  // is about that file, never about the spelling.
  file_status reached{};
  const bool exists = ::stat(name.c_str(), &reached) == 0;
  if (!exists && errno != ENOENT) {
    return cannot_open();
  }

  // Where it is, which a new file is created at.
  auto located = resolve(name);
  file_status at_location{};
  const bool location_exists = located && ::lstat(located->c_str(), &at_location) == 0;
  const int location_errno = errno;
  if (exists) {
    // A name only the kernel can follow -- /proc/self/fd/N naming a pipe --
    // is opened as it was given.
    if (!location_exists || !same_file(at_location, reached)) {
      located.reset();
    }
  } else if (!located || location_exists || location_errno != ENOENT) {
    return cannot_open();
  }
  const fs::path reachable = located ? *located : fs::path{name};
  const auto planned = [&](write_mode mode, int stream = -1) {
    return plan_result{target{reachable, mode, stream, exists}, {}};
  };

  for (const auto& trace : traces) {
    std::error_code equivalent_error, canonical_error;
    const bool same_file_as_trace = exists && fs::equivalent(reachable, trace, equivalent_error);
    const auto input = fs::canonical(trace, canonical_error);
    if (same_file_as_trace || (located && !canonical_error && input == *located)) {
      return refuse(fmt::format("TOML output '{}' aliases an input trace '{}'.", name, trace));
    }
  }

  if (!exists) {
    if (probe) {
      // O_EXCL: the file removed again is the one created here.
      const int created = ::open(reachable.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOCTTY | O_CLOEXEC, 0666);
      if (created < 0) {
        return cannot_open();
      }
      ::close(created);
      ::unlink(reachable.c_str());
    }
    return planned(write_mode::regular_file);
  }
  if (S_ISDIR(reached.st_mode)) {
    return cannot_open();
  }
  // The file the shell redirected stdout or stderr to is not checked or
  // truncated: that would destroy everything the run printed.
  for (const int stream : {STDOUT_FILENO, STDERR_FILENO}) {
    if (file_status open_stream{}; ::fstat(stream, &open_stream) == 0 && same_file(open_stream, reached)) {
      return planned(write_mode::standard_stream, stream);
    }
  }
  if (S_ISFIFO(reached.st_mode)) {
    // A named FIFO or a process substitution's pipe, not opened now: that
    // would consume the reader waiting for the document.
    return planned(write_mode::special_file);
  }
  if (!S_ISREG(reached.st_mode)) {
    // A device (/dev/null, a terminal) or a socket, opened now without
    // blocking or acquiring a controlling terminal.
    if (probe) {
      const int descriptor = ::open(reachable.c_str(), O_WRONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
      if (descriptor < 0) {
        return cannot_open();
      }
      ::close(descriptor);
    }
    return planned(write_mode::special_file);
  }

  if (probe) {
    // Opening it for writing, without truncation, is what refuses a read-only
    // document.
    const int writable = ::open(reachable.c_str(), O_WRONLY | O_NOCTTY | O_CLOEXEC);
    if (writable < 0) {
      return cannot_open();
    }
    ::close(writable);
  }
  // A trace the optional value consumed, a --config source, the native YAML:
  // none of them begins like a statistics document, and overwriting any of
  // them would be unrecoverable. So its first bytes are read, through a
  // separate open; only an empty file, with nothing to check, may refuse it.
  const auto cannot_read = [&] {
    return refuse(fmt::format("cannot read '{}' to check that it is a ChampSim statistics document.", name));
  };
  const auto& signature = champsim::toml_printer::document_signature;
  std::optional<std::string> head{std::string{}};
  if (const int readable = ::open(reachable.c_str(), O_RDONLY | O_NOCTTY | O_CLOEXEC); readable >= 0) {
    head = read_head(readable, std::size(signature));
    ::close(readable);
  } else if (reached.st_size != 0) {
    return cannot_read();
  }
  if (!head) {
    return cannot_read();
  }
  if (!std::empty(*head) && *head != signature) {
    // /dev/fd/N, /dev/stdin and the like name an open descriptor, not a
    // trace path the optional value swallowed. (/dev/shm can hold traces.)
    const bool descriptor_name =
        name.rfind("/dev/fd/", 0) == 0 || name.rfind("/proc/", 0) == 0 || name == "/dev/stdin" || name == "/dev/stdout" || name == "/dev/stderr";
    return refuse(fmt::format("TOML output '{}' is not a ChampSim statistics document; refusing to replace it.{}", name,
                              descriptor_name ? ""
                                              : " If it is a trace, --toml took it as the output filename: use --toml=FILE, or put -- before the "
                                                "trace paths."));
  }
  return planned(write_mode::regular_file);
}
} // namespace

namespace champsim::output
{
plan_result plan(const std::string& name, const std::vector<std::string>& traces) { return check(name, traces, true); }

write_result write(const std::string& name, const std::vector<std::string>& traces, std::string_view document)
{
  write_result result;
  const auto fail = [&](std::string message) {
    result.messages.push_back(std::move(message));
    return result;
  };

  // A name created by someone else between the check and the create is
  // checked like any other existing file, once.
  for (int attempt = 0;; ++attempt) {
    const auto checked = check(name, traces, false);
    if (!checked.planned) {
      return fail(fmt::format("ERROR: {} The run completed, but the TOML statistics were not written.", checked.error));
    }
    const auto& destination = *checked.planned;

    if (destination.mode == write_mode::standard_stream) {
      // After the plain report, which is still buffered in stdout.
      std::cout.flush();
      std::fflush(stdout);
      std::FILE* const stream = destination.stream == STDERR_FILENO ? stderr : stdout;
      const bool complete = std::fwrite(std::data(document), 1, std::size(document), stream) == std::size(document);
      if (std::fflush(stream) != 0 || !complete) {
        return fail(fmt::format("ERROR: failed to write the TOML statistics to '{}'.", name));
      }
      result.written = true;
      return result;
    }

    const bool regular = destination.mode == write_mode::regular_file;
    int descriptor = -1;
    int open_error = ENOENT;
    if (destination.exists) {
      // Not O_CREAT on a file that exists: fs.protected_regular refuses that
      // for another user's file in a sticky directory, even one that may be
      // written.
      descriptor = ::open(destination.path.c_str(), O_WRONLY | O_NOCTTY | O_CLOEXEC | (regular ? O_TRUNC : 0));
      open_error = descriptor < 0 ? errno : 0;
    }
    if (descriptor < 0 && regular && open_error == ENOENT) {
      // Nothing there, or removed during the run. Mode 0666, so that the umask
      // or the directory's default ACL decides, as for any new file.
      descriptor = ::open(destination.path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOCTTY | O_CLOEXEC, 0666);
      open_error = descriptor < 0 ? errno : 0;
      if (descriptor < 0 && open_error == EEXIST && attempt == 0) {
        continue;
      }
    }
    if (descriptor < 0) {
      return fail(fmt::format("ERROR: cannot open '{}' to write the TOML statistics ({}); its earlier contents, if any, were not touched.", name,
                              describe(open_error)));
    }

    const int write_error = write_all(descriptor, document);
    const int close_error = ::close(descriptor) == 0 ? 0 : errno;
    if (write_error != 0 || close_error != 0) {
      return fail(fmt::format("ERROR: failed to write the TOML statistics to '{}' ({}); it may now be empty or partial.", name,
                              describe(write_error != 0 ? write_error : close_error)));
    }
    result.written = true;
    return result;
  }
}
} // namespace champsim::output
