#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <sys/stat.h>

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

bool is_temporary_name(const std::string& name) { return std::regex_match(name, std::regex{R"(\.champsim-toml-[0-9a-f]{16}\.tmp)"}); }

const std::string old_document{"# ChampSim statistics. An earlier run.\n"};
const std::string new_document{"# ChampSim statistics. This run.\n"};

champsim::output::target planned_target(const fs::path& name)
{
  const auto planned = champsim::output::plan(name.string(), {});
  INFO(planned.error);
  REQUIRE(planned.planned.has_value());
  return *planned.planned;
}
} // namespace

TEST_CASE("A --toml document is replaced by a renamed sibling that already holds its bytes and permissions")
{
  scratch_directory scratch;
  const auto document = scratch.path / "run.toml";
  put(document, old_document);
  fs::permissions(document, fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read);

  const auto destination = planned_target(document);
  REQUIRE(destination.mode == write_mode::replace_by_rename);
  REQUIRE(names_in(scratch.path) == std::vector<std::string>{"run.toml"});

  auto ops = champsim::output::system_operations();
  const auto rename = ops.rename;
  std::vector<std::string> renamed;
  ops.rename = [&](const fs::path& from, const fs::path& to) {
    // A fixed-length name that does not embed the target's, so a long target
    // name cannot make it too long.
    renamed.push_back(from.filename().string());
    CHECK(from.parent_path() == document.parent_path());
    CHECK(contents(from) == new_document);
    CHECK((fs::status(from).permissions() & fs::perms::all) == (fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read));
    return rename(from, to);
  };
  const auto result = champsim::output::write(destination, new_document, ops);
  REQUIRE(result.written);
  REQUIRE(result.messages.empty());
  REQUIRE(std::size(renamed) == 1);
  REQUIRE(is_temporary_name(renamed.front()));
  REQUIRE(contents(document) == new_document);
  REQUIRE(names_in(scratch.path) == std::vector<std::string>{"run.toml"});
}

TEST_CASE("A --toml document whose rename fails is written in place instead, created only where nothing was")
{
  scratch_directory scratch;
  const auto document = scratch.path / "run.toml";
  // fs.protected_regular refuses O_CREAT on another user's existing file in a
  // sticky directory, even one that may be written.
  const bool existed = GENERATE(true, false);
  CAPTURE(existed);
  if (existed) {
    put(document, old_document);
  }
  const auto destination = planned_target(document);
  REQUIRE(destination.mode == write_mode::replace_by_rename);

  auto ops = champsim::output::system_operations();
  ops.rename = [](const fs::path&, const fs::path&) {
    return EBUSY;
  };
  std::vector<bool> created;
  ops.write_in_place = [&, write_in_place = ops.write_in_place](const fs::path& path, std::string_view bytes, bool create) {
    created.push_back(create);
    return write_in_place(path, bytes, create);
  };
  const auto result = champsim::output::write(destination, new_document, ops);
  REQUIRE(result.written);
  REQUIRE(created == std::vector<bool>{!existed});
  REQUIRE(contents(document) == new_document);
  REQUIRE(names_in(scratch.path) == std::vector<std::string>{"run.toml"});
  REQUIRE(std::size(result.messages) == 1);
  CHECK_THAT(result.messages.front(), Catch::Matchers::StartsWith("WARNING:") && Catch::Matchers::ContainsSubstring("in place"));
}

TEST_CASE("A --toml document that can be neither renamed nor written in place is kept beside the target")
{
  scratch_directory scratch;
  const auto document = scratch.path / "run.toml";
  put(document, old_document);
  const auto destination = planned_target(document);

  auto ops = champsim::output::system_operations();
  ops.rename = [](const fs::path&, const fs::path&) {
    return EBUSY;
  };
  ops.write_in_place = [](const fs::path&, std::string_view, bool) {
    return EIO;
  };
  const auto result = champsim::output::write(destination, new_document, ops);
  REQUIRE_FALSE(result.written);
  REQUIRE(contents(document) == old_document);

  const auto names = names_in(scratch.path);
  REQUIRE(std::size(names) == 2);
  REQUIRE(names.back() == "run.toml");
  REQUIRE(is_temporary_name(names.front()));
  const auto kept = scratch.path / names.front();
  REQUIRE(contents(kept) == new_document);
  REQUIRE(std::size(result.messages) == 1);
  CHECK_THAT(result.messages.front(), Catch::Matchers::StartsWith("ERROR:") && Catch::Matchers::ContainsSubstring(document.string())
                                          && Catch::Matchers::ContainsSubstring(kept.string()));
}

TEST_CASE("A --toml document whose directory stops taking new files during the run is written in place")
{
  scratch_directory scratch;
  const auto document = scratch.path / "run.toml";
  put(document, old_document);
  const auto destination = planned_target(document);

  // Removing the directory's write permission after planning makes creating the
  // temporary fail at the end; writing in place still delivers the document.
  auto ops = champsim::output::system_operations();
  bool fell_back = false;
  ops.write_in_place = [&, write_in_place = ops.write_in_place](const fs::path& path, std::string_view bytes, bool create) {
    fell_back = true;
    CHECK_FALSE(create);
    return write_in_place(path, bytes, create);
  };
  if (::geteuid() == 0) {
    SKIP("permissions do not bind the superuser");
  }
  fs::permissions(scratch.path, fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write, fs::perm_options::remove);
  const auto result = champsim::output::write(destination, new_document, ops);
  fs::permissions(scratch.path, fs::perms::owner_write, fs::perm_options::add);
  REQUIRE(result.written);
  REQUIRE(fell_back);
  REQUIRE(contents(document) == new_document);
  REQUIRE(names_in(scratch.path) == std::vector<std::string>{"run.toml"});
}

TEST_CASE("--toml targets that a rename would not replace as the same file are written in place")
{
  scratch_directory scratch;

  SECTION("A document with a second hard link keeps both names")
  {
    const auto document = scratch.path / "run.toml";
    const auto other = scratch.path / "latest.toml";
    put(document, old_document);
    fs::create_hard_link(document, other);
    const auto destination = planned_target(document);
    REQUIRE(destination.mode == write_mode::in_place);
    REQUIRE(champsim::output::write(destination, new_document).written);
    REQUIRE(contents(document) == new_document);
    REQUIRE(contents(other) == new_document);
    REQUIRE(fs::hard_link_count(document) == 2);
  }

  SECTION("A name too long to carry a suffix is still replaced by rename")
  {
    const auto document = scratch.path / (std::string(240, 'r') + ".toml");
    const auto destination = planned_target(document);
    REQUIRE(destination.mode == write_mode::replace_by_rename);
    REQUIRE(champsim::output::write(destination, new_document).written);
    REQUIRE(contents(document) == new_document);
    REQUIRE(names_in(scratch.path) == std::vector<std::string>{document.filename().string()});
  }

  SECTION("Existing files in a directory that refuses new entries")
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
      const auto destination = planned_target(path);
      REQUIRE(destination.mode == write_mode::in_place);
      REQUIRE(champsim::output::write(destination, new_document).written);
      REQUIRE(contents(path) == new_document);
    }
    REQUIRE(names_in(directory) == std::vector<std::string>{"empty.toml", "run.toml"});

    const auto missing = champsim::output::plan((directory / "new.toml").string(), {});
    REQUIRE_FALSE(missing.planned.has_value());
    REQUIRE_THAT(missing.error, Catch::Matchers::ContainsSubstring("cannot open"));
  }
}

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
  REQUIRE(destination.mode == write_mode::replace_by_rename);

  const auto trace = scratch.path / "trace.champsim2";
  put(trace, "not a statistics document");
  const auto alias = champsim::output::plan((scratch.path / "results/../trace.champsim2").string(), {trace.string()});
  REQUIRE_THAT(alias.error, Catch::Matchers::ContainsSubstring("aliases an input trace"));
}
