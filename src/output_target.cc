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
#include <random>
#include <system_error>
#include <unistd.h>
#include <fmt/core.h>
#include <sys/stat.h>
#ifdef __linux__
#include <sys/xattr.h>
#endif

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

// A temporary file in `directory`, created exclusively so that it can never be
// someone else's file, and with mode 0600 so that nobody else can open it
// before its permissions are set. The name has a fixed length and does not
// embed the target's, so any target name that fits the directory leaves room
// for it. The descriptor is open for writing; on failure it is -1 and `error`
// holds errno.
struct temporary_file {
  fs::path path;
  int descriptor{-1};
  int error{0};
};

temporary_file create_temporary(const fs::path& directory)
{
  constexpr int max_attempts = 100;
  std::random_device entropy;
  temporary_file created;
  for (int attempt = 0; attempt < max_attempts; ++attempt) {
    created.path = directory / fmt::format(".champsim-toml-{:08x}{:08x}.tmp", entropy(), entropy());
    created.descriptor = ::open(created.path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOCTTY | O_CLOEXEC, 0600);
    if (created.descriptor >= 0) {
      return created;
    }
    created.error = errno;
    if (created.error != EEXIST) {
      break;
    }
  }
  return created;
}

// The status of a new file created in `directory` and removed again, so its
// owner and group are the ones a replacement would get there; empty when the
// directory refuses one. Nothing is left behind.
std::optional<file_status> probe_new_file(const fs::path& directory)
{
  const auto probe = create_temporary(directory);
  if (probe.descriptor < 0) {
    return std::nullopt;
  }
  file_status created{};
  const bool described = ::fstat(probe.descriptor, &created) == 0;
  ::close(probe.descriptor);
  ::unlink(probe.path.c_str());
  if (!described) {
    return std::nullopt;
  }
  return created;
}

// Whether `path` may carry a POSIX access ACL, which a rename would replace
// with a new file's: only a definite absence counts as none.
bool may_have_access_acl(const fs::path& path)
{
#ifdef __linux__
  if (::getxattr(path.c_str(), "system.posix_acl_access", nullptr, 0) >= 0) {
    return true;
  }
  return errno != ENODATA && errno != ENOTSUP && errno != EOPNOTSUPP;
#else
  (void)path;
  return false;
#endif
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
  // The umask cannot be read without being set, so it is read once, here.
  const mode_t mask = ::umask(0);
  ::umask(mask);

  // What open() reaches through the name, if anything. Every decision below
  // is about that file, never about the spelling.
  file_status reached{};
  const bool exists = ::stat(name.c_str(), &reached) == 0;
  if (!exists && errno != ENOENT) {
    return cannot_open();
  }

  // Where it is, so that a replacement can be made in its directory.
  auto located = resolve(name);
  file_status at_location{};
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
  const auto planned = [&](const fs::path& path, write_mode mode, int stream = -1) {
    return plan_result{target{name, path, mode, stream, exists, 0666U & ~static_cast<unsigned int>(mask)}, {}};
  };

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
  // The file the shell redirected stdout or stderr to is not replaced, checked
  // or truncated: renaming over a log would unlink everything the run printed.
  for (const int stream : {STDOUT_FILENO, STDERR_FILENO}) {
    if (file_status open_stream{}; exists && ::fstat(stream, &open_stream) == 0 && same_file(open_stream, reached)) {
      return planned(reachable, write_mode::standard_stream, stream);
    }
  }
  if (exists && S_ISFIFO(reached.st_mode)) {
    // A named FIFO or a process substitution's pipe: written in place, and
    // not opened now, because that would consume the reader waiting for the
    // document.
    return planned(reachable, write_mode::in_place);
  }
  if (exists && !S_ISREG(reached.st_mode)) {
    // A device (/dev/null, a terminal) or a socket: written in place, but
    // opened and closed now -- without truncating, blocking or acquiring a
    // controlling terminal -- so one that cannot be opened costs no run.
    const int descriptor = ::open(reachable.c_str(), O_WRONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (descriptor < 0) {
      return cannot_open();
    }
    ::close(descriptor);
    return planned(reachable, write_mode::in_place);
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
      // /dev/fd/N, /dev/stdin and the like name an open descriptor, not a
      // trace path the optional value swallowed. (/dev/shm can hold traces.)
      const bool descriptor_name =
          name.rfind("/dev/fd/", 0) == 0 || name.rfind("/proc/", 0) == 0 || name == "/dev/stdin" || name == "/dev/stdout" || name == "/dev/stderr";
      return refuse(fmt::format("TOML output '{}' is not a ChampSim statistics document; refusing to replace it.{}", name,
                                descriptor_name ? ""
                                                : " If it is a trace, --toml took it as the output filename: use --toml=FILE, or put -- before the "
                                                  "trace paths."));
    }
  }

  if (!located) {
    // Reachable only through the kernel, so there is no directory to put a
    // replacement in.
    return planned(reachable, write_mode::in_place);
  }
  // A replacement is created beside the target and renamed over it. An
  // existing file is written in place instead wherever the rename would not
  // leave the same file behind: its directory refuses new files, it has a
  // second hard link (which a rename would split), or a new file there would
  // not carry its owner, its group or its access ACL.
  if (!exists || (reached.st_nlink <= 1 && !may_have_access_acl(*located))) {
    const auto created = probe_new_file(located->parent_path());
    if (created && (!exists || (created->st_uid == reached.st_uid && created->st_gid == reached.st_gid))) {
      return planned(*located, write_mode::replace_by_rename);
    }
  }
  if (!exists) {
    return cannot_open();
  }
  return planned(*located, write_mode::in_place);
}

operations system_operations()
{
  operations ops;
  ops.rename = [](const fs::path& from, const fs::path& to) {
    return ::rename(from.c_str(), to.c_str()) == 0 ? 0 : errno;
  };
  ops.write_in_place = [](const fs::path& path, std::string_view document, bool create) {
    // Not O_CREAT on a file that exists: fs.protected_regular refuses that for
    // another user's file in a sticky directory, even one that may be written.
    const int descriptor = ::open(path.c_str(), O_WRONLY | O_TRUNC | O_NOCTTY | O_CLOEXEC | (create ? O_CREAT : 0), 0666);
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

  if (destination.mode == write_mode::standard_stream) {
    // After the plain report, which is still buffered in stdout.
    std::cout.flush();
    std::fflush(stdout);
    std::FILE* const stream = destination.stream == STDERR_FILENO ? stderr : stdout;
    const bool complete = std::fwrite(std::data(document), 1, std::size(document), stream) == std::size(document);
    if (std::fflush(stream) != 0 || !complete) {
      return failed();
    }
    result.written = true;
    return result;
  }

  if (destination.mode == write_mode::in_place) {
    if (ops.write_in_place(destination.path, document, !destination.existed) != 0) {
      return failed();
    }
    result.written = true;
    return result;
  }

  // The finished document is written to a new sibling and renamed over the
  // target, so a write that fails here -- a full disk -- or a run killed
  // before the rename leaves whatever the target held before as it was.
  auto temporary = create_temporary(destination.path.parent_path());
  if (temporary.descriptor < 0) {
    // The directory took a probe at startup but refuses a file now. Every
    // check writing in place needs has passed, and this is the only copy.
    if (ops.write_in_place(destination.path, document, !destination.existed) != 0) {
      return failed();
    }
    result.messages.push_back(fmt::format("WARNING: could not create a temporary file beside '{}' ({}); wrote the TOML statistics in place instead.",
                                          destination.name, describe(temporary.error)));
    result.written = true;
    return result;
  }

  // rename substitutes a new file, created private: before it holds any of
  // the document, give it an existing document's permissions, or a new file's.
  auto permissions = destination.new_file_permissions;
  if (file_status previous{}; ::lstat(destination.path.c_str(), &previous) == 0 && S_ISREG(previous.st_mode)) {
    permissions = static_cast<unsigned int>(previous.st_mode) & 07777U;
  }
  ::fchmod(temporary.descriptor, static_cast<mode_t>(permissions));
  const int write_error = write_all(temporary.descriptor, document);
  const int close_error = ::close(temporary.descriptor) == 0 ? 0 : errno;
  if (write_error != 0 || close_error != 0) {
    ::unlink(temporary.path.c_str());
    return failed();
  }

  const int rename_error = ops.rename(temporary.path, destination.path);
  if (rename_error == 0) {
    result.written = true;
    return result;
  }
  // rename can fail where writing does not, as EBUSY does for a file
  // bind-mounted into a container.
  const int in_place_error = ops.write_in_place(destination.path, document, !destination.existed);
  if (in_place_error == 0) {
    ::unlink(temporary.path.c_str());
    result.messages.push_back(
        fmt::format("WARNING: could not rename the TOML statistics over '{}' ({}); wrote them in place instead.", destination.name, describe(rename_error)));
    result.written = true;
    return result;
  }
  result.messages.push_back(fmt::format("ERROR: failed to write the TOML statistics to '{}': renaming failed ({}), and so did writing in place ({}). "
                                        "The complete document is kept in '{}'.",
                                        destination.name, describe(rename_error), describe(in_place_error), temporary.path.string()));
  return result;
}
} // namespace champsim::output
