#include <algorithm>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <sys/resource.h>

#include "output_target.h"

namespace
{
namespace fs = std::filesystem;
using champsim::output::write_mode;

struct scratch_directory {
  fs::path path;
  scratch_directory()
  {
    auto pattern = (fs::temp_directory_path() / "champsim-097-XXXXXX").string();
    if (::mkdtemp(std::data(pattern)) == nullptr) {
      throw std::runtime_error{"mkdtemp failed"};
    }
    path = pattern;
  }
  scratch_directory(const scratch_directory&) = delete;
  scratch_directory& operator=(const scratch_directory&) = delete;
  ~scratch_directory()
  {
    std::error_code error;
    for (const auto& entry : fs::recursive_directory_iterator{path, error}) {
      if (entry.is_directory(error)) {
        fs::permissions(entry.path(), fs::perms::owner_all, fs::perm_options::add, error);
      }
    }
    fs::remove_all(path, error);
  }
};

std::string contents(const fs::path& path)
{
  std::ifstream file{path, std::ios::binary};
  return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

void put(const fs::path& path, const std::string& text) { std::ofstream{path, std::ios::binary} << text; }

std::vector<std::string> names_in(const fs::path& directory)
{
  std::vector<std::string> names;
  for (const auto& entry : fs::directory_iterator{directory}) {
    names.push_back(entry.path().filename().string());
  }
  std::sort(std::begin(names), std::end(names));
  return names;
}

// While it lives, a write that would grow a regular file fails with EFBIG, as
// on a full disk; opening and truncating still succeed. That includes this
// process's own output, so everything buffered is flushed first, and nothing
// may be reported while it lives: check its results after it is gone.
class files_cannot_grow
{
  rlimit previous{};
  void (*previous_handler)(int){};
  bool lowered{false};

public:
  files_cannot_grow()
  {
    std::cout.flush();
    std::clog.flush();
    std::cerr.flush();
    std::fflush(stdout);
    std::fflush(stderr);
    if (::getrlimit(RLIMIT_FSIZE, &previous) == 0) {
      previous_handler = std::signal(SIGXFSZ, SIG_IGN);
      const rlimit none{0, previous.rlim_max};
      lowered = ::setrlimit(RLIMIT_FSIZE, &none) == 0;
      if (!lowered) {
        std::signal(SIGXFSZ, previous_handler);
      }
    }
  }
  files_cannot_grow(const files_cannot_grow&) = delete;
  files_cannot_grow& operator=(const files_cannot_grow&) = delete;
  ~files_cannot_grow()
  {
    if (lowered) {
      ::setrlimit(RLIMIT_FSIZE, &previous);
      std::signal(SIGXFSZ, previous_handler);
    }
  }
  [[nodiscard]] bool active() const { return lowered; }
};

const std::string old_document{"# ChampSim statistics. An earlier run.\n"};
const std::string new_document{"# ChampSim statistics. This run.\n"};

champsim::output::target planned_target(const fs::path& name)
{
  const auto planned = champsim::output::plan(name.string(), {});
  INFO(planned.error);
  REQUIRE(planned.planned.has_value());
  return *planned.planned;
}

champsim::output::write_result write_new_document(const fs::path& name) { return champsim::output::write(name.string(), {}, new_document); }
} // namespace

TEST_CASE("--toml names are resolved as the kernel opens them")
{
  scratch_directory scratch;
  const auto notes = scratch.path / "notes.txt";
  put(notes, "precious\n");
  fs::create_symlink("missing/../notes.txt", scratch.path / "dangling.toml");
  for (const auto& spelling : {scratch.path / "missing/../notes.txt", scratch.path / "notes.txt/../notes.txt", scratch.path / "dangling.toml",
                               scratch.path / "notes.txt/", scratch.path / ".."}) {
    CAPTURE(spelling);
    const auto planned = champsim::output::plan(spelling.string(), {});
    REQUIRE_FALSE(planned.planned.has_value());
    REQUIRE_THAT(planned.error, Catch::Matchers::ContainsSubstring("cannot open"));
  }
  REQUIRE(contents(notes) == "precious\n");

  // A relative link is followed from the directory that holds it.
  fs::create_directory(scratch.path / "results");
  fs::create_symlink("results/run.toml", scratch.path / "latest.toml");
  const auto destination = planned_target(scratch.path / "latest.toml");
  REQUIRE(destination.path == fs::canonical(scratch.path) / "results" / "run.toml");
  REQUIRE(destination.mode == write_mode::regular_file);
  REQUIRE_FALSE(destination.exists);
  REQUIRE(names_in(scratch.path / "results").empty());
  REQUIRE(write_new_document(scratch.path / "latest.toml").written);
  REQUIRE(fs::is_symlink(scratch.path / "latest.toml"));
  REQUIRE(contents(scratch.path / "results" / "run.toml") == new_document);

  const auto trace = scratch.path / "trace.champsim2";
  put(trace, "not a statistics document");
  const auto alias = champsim::output::plan((scratch.path / "results/../trace.champsim2").string(), {trace.string()});
  REQUIRE_THAT(alias.error, Catch::Matchers::ContainsSubstring("aliases an input trace"));
}

TEST_CASE("Checking a --toml target writes, truncates and leaves nothing")
{
  scratch_directory scratch;
  const auto document = scratch.path / "run.toml";
  put(document, old_document);
  const auto existing = planned_target(document);
  REQUIRE(existing.exists);
  REQUIRE(contents(document) == old_document);

  const auto created = planned_target(scratch.path / "new.toml");
  REQUIRE_FALSE(created.exists);
  REQUIRE(names_in(scratch.path) == std::vector<std::string>{"run.toml"});
}

TEST_CASE("An existing --toml file that may be written but not read")
{
  if (::geteuid() == 0) {
    SKIP("permissions do not bind the superuser");
  }
  scratch_directory scratch;
  const auto document = scratch.path / "run.toml";

  SECTION("is written when it is empty, which needs no read")
  {
    put(document, "");
    fs::permissions(document, fs::perms::owner_write);
    planned_target(document);
    REQUIRE(write_new_document(document).written);
    REQUIRE((fs::status(document).permissions() & fs::perms::all) == fs::perms::owner_write);
    fs::permissions(document, fs::perms::owner_read, fs::perm_options::add);
    REQUIRE(contents(document) == new_document);
  }

  SECTION("is refused when it has contents that cannot be checked")
  {
    put(document, old_document);
    fs::permissions(document, fs::perms::owner_write);
    const auto planned = champsim::output::plan(document.string(), {});
    REQUIRE_FALSE(planned.planned.has_value());
    REQUIRE_THAT(planned.error, Catch::Matchers::Equals("cannot read '" + document.string() + "' to check that it is a ChampSim statistics document."));
    fs::permissions(document, fs::perms::owner_read, fs::perm_options::add);
    REQUIRE(contents(document) == old_document);
  }
}

TEST_CASE("Existing --toml files are written in place")
{
  scratch_directory scratch;

  SECTION("A document with a second hard link keeps both names")
  {
    const auto document = scratch.path / "run.toml";
    const auto other = scratch.path / "latest.toml";
    put(document, old_document);
    fs::create_hard_link(document, other);
    planned_target(document);
    REQUIRE(write_new_document(document).written);
    REQUIRE(contents(document) == new_document);
    REQUIRE(contents(other) == new_document);
    REQUIRE(fs::hard_link_count(document) == 2);
  }

  SECTION("A 245-byte name")
  {
    const auto document = scratch.path / (std::string(240, 'r') + ".toml");
    for (const auto existed : {false, true}) {
      CAPTURE(existed);
      REQUIRE(planned_target(document).exists == existed);
      REQUIRE(write_new_document(document).written);
      REQUIRE(contents(document) == new_document);
    }
    REQUIRE(names_in(scratch.path) == std::vector<std::string>{document.filename().string()});
  }

  SECTION("Files in a directory that refuses new entries")
  {
    if (::geteuid() == 0) {
      SKIP("permissions do not bind the superuser");
    }
    const auto directory = scratch.path / "read-only";
    fs::create_directory(directory);
    const auto empty = directory / "empty.toml";
    const auto document = directory / "run.toml";
    put(empty, "");
    put(document, old_document);
    fs::permissions(directory, fs::perms::owner_read | fs::perms::owner_exec | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read
                                   | fs::perms::others_exec);
    for (const auto& path : {empty, document}) {
      CAPTURE(path);
      planned_target(path);
      REQUIRE(write_new_document(path).written);
      REQUIRE(contents(path) == new_document);
    }
    REQUIRE(names_in(directory) == std::vector<std::string>{"empty.toml", "run.toml"});

    const auto missing = champsim::output::plan((directory / "new.toml").string(), {});
    REQUIRE_FALSE(missing.planned.has_value());
    REQUIRE_THAT(missing.error, Catch::Matchers::ContainsSubstring("cannot open"));
  }
}

TEST_CASE("A --toml target is checked again after the run")
{
  scratch_directory scratch;
  const auto document = scratch.path / "run.toml";
  put(document, old_document);
  planned_target(document);

  SECTION("A document overwritten by something else during the run is refused and left alone")
  {
    put(document, "notes\n");
    const auto result = write_new_document(document);
    REQUIRE_FALSE(result.written);
    REQUIRE(std::size(result.messages) == 1);
    CHECK_THAT(result.messages.front(), Catch::Matchers::StartsWith("ERROR: TOML output '" + document.string() + "' is not a ChampSim statistics document")
                                            && Catch::Matchers::EndsWith("The run completed, but the TOML statistics were not written."));
    REQUIRE(contents(document) == "notes\n");
  }

  SECTION("A document removed during the run is created again")
  {
    const auto other = scratch.path / "latest.toml";
    fs::create_hard_link(document, other);
    fs::remove(document);
    const auto result = write_new_document(document);
    REQUIRE(result.written);
    REQUIRE(result.messages.empty());
    REQUIRE(contents(document) == new_document);
    REQUIRE(fs::hard_link_count(document) == 1);
    REQUIRE(contents(other) == old_document);
  }

  SECTION("A document that cannot be opened by then is not touched, and the error says so")
  {
    if (::geteuid() == 0) {
      SKIP("permissions do not bind the superuser");
    }
    fs::permissions(document, fs::perms::owner_read);
    const auto result = write_new_document(document);
    REQUIRE_FALSE(result.written);
    REQUIRE(std::size(result.messages) == 1);
    CHECK_THAT(result.messages.front(),
               Catch::Matchers::Equals("ERROR: cannot open '" + document.string()
                                       + "' to write the TOML statistics (Permission denied); its earlier contents, if any, were not touched."));
    REQUIRE(contents(document) == old_document);
  }
}

TEST_CASE("A --toml write that fails after truncating says the target may now be empty or partial")
{
  scratch_directory scratch;
  const auto document = scratch.path / "run.toml";
  put(document, old_document);
  planned_target(document);

  bool lowered = false;
  champsim::output::write_result result;
  {
    const files_cannot_grow full_disk;
    lowered = full_disk.active();
    result = write_new_document(document);
  }
  REQUIRE(lowered);
  REQUIRE_FALSE(result.written);
  REQUIRE(contents(document).empty());
  REQUIRE(std::size(result.messages) == 1);
  CHECK_THAT(result.messages.front(), Catch::Matchers::Equals("ERROR: failed to write the TOML statistics to '" + document.string()
                                                              + "' (File too large); it may now be empty or partial."));
}
