#include <array>
#include <catch.hpp>
#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>

#include "runtime_config.h"

// The runtime configuration store: a flat key -> scalar map populated from
// TOML files (--config) and command-line assignments (--set), applied in argv
// order so the LAST definition of a key wins regardless of which source it
// came from. The store names no generated symbol, which is what lets it link
// into this test binary and be pinned directly.

namespace
{
// A self-removing temporary file.
struct temp_toml {
  std::string path;
  explicit temp_toml(const std::string& text)
  {
    char name[] = "/tmp/champsim-test-XXXXXX";
    const int fd = ::mkstemp(name);
    REQUIRE(fd >= 0);
    ::close(fd);
    path = name;
    std::ofstream out{path};
    out << text;
  }
  ~temp_toml() { std::remove(path.c_str()); }
  temp_toml(const temp_toml&) = delete;
  temp_toml& operator=(const temp_toml&) = delete;
};
} // namespace

TEST_CASE("An empty store returns every fallback")
{
  champsim::runtime_config cfg{};
  REQUIRE(cfg.value<long>("ooo_cpu.cpu0.rob_size", 352) == 352);
  REQUIRE(cfg.value<double>("pmem.frequency", 1600.0) == 1600.0);
  REQUIRE(cfg.value<bool>("cache.llc.prefetch_as_load", false) == false);
}

TEST_CASE("A set assignment overrides the fallback")
{
  champsim::runtime_config cfg{};
  cfg.set("ooo_cpu.cpu0.rob_size=512");
  REQUIRE(cfg.value<long>("ooo_cpu.cpu0.rob_size", 352) == 512);
}

TEST_CASE("set parses booleans, integers, floats, and strings")
{
  champsim::runtime_config cfg{};
  cfg.set("a.flag=true");
  cfg.set("a.count=42");
  cfg.set("a.rate=2.5");
  REQUIRE(cfg.value<bool>("a.flag", false) == true);
  REQUIRE(cfg.value<long>("a.count", 0) == 42);
  REQUIRE(cfg.value<double>("a.rate", 0.0) == 2.5);
}

TEST_CASE("A malformed set assignment is rejected")
{
  champsim::runtime_config cfg{};
  REQUIRE_THROWS(cfg.set("no_equals_sign"));
  REQUIRE_THROWS(cfg.set("=value_without_key"));
}

TEST_CASE("A TOML file loads nested tables as dotted keys")
{
  temp_toml file{"[ooo_cpu.cpu0]\nrob_size = 512\n\n[cache.cpu0_l1d]\nsets = 128\nprefetch_as_load = true\n"};
  champsim::runtime_config cfg{};
  cfg.load_file(file.path);
  REQUIRE(cfg.value<long>("ooo_cpu.cpu0.rob_size", 352) == 512);
  REQUIRE(cfg.value<long>("cache.cpu0_l1d.sets", 64) == 128);
  REQUIRE(cfg.value<bool>("cache.cpu0_l1d.prefetch_as_load", false) == true);
}

TEST_CASE("The last definition wins across interleaved files and assignments")
{
  temp_toml first{"[ooo_cpu.cpu0]\nrob_size = 100\nlq_size = 32\n"};
  temp_toml second{"[ooo_cpu.cpu0]\nrob_size = 300\n"};
  champsim::runtime_config cfg{};

  cfg.load_file(first.path); // rob_size=100, lq_size=32
  cfg.set("ooo_cpu.cpu0.rob_size=200");
  cfg.load_file(second.path); // rob_size=300 -- the file loaded after --set wins
  REQUIRE(cfg.value<long>("ooo_cpu.cpu0.rob_size", 0) == 300);
  REQUIRE(cfg.value<long>("ooo_cpu.cpu0.lq_size", 0) == 32);

  cfg.set("ooo_cpu.cpu0.rob_size=400"); // and a later --set wins again
  REQUIRE(cfg.value<long>("ooo_cpu.cpu0.rob_size", 0) == 400);
}

TEST_CASE("An unreadable or unparseable file is rejected")
{
  champsim::runtime_config cfg{};
  REQUIRE_THROWS(cfg.load_file("/nonexistent/definitely_not_here.toml"));
  temp_toml bad{"this is [not valid toml\n"};
  REQUIRE_THROWS(cfg.load_file(bad.path));
}

TEST_CASE("An array value in the file is rejected, matching the document's no-arrays rule")
{
  temp_toml file{"[cache.cpu0_l1d]\nprefetch_activate = [\"LOAD\"]\n"};
  champsim::runtime_config cfg{};
  REQUIRE_THROWS(cfg.load_file(file.path));
}

TEST_CASE("An integer is coerced to double, but nothing else converts")
{
  champsim::runtime_config cfg{};
  cfg.set("pmem.frequency=1600");
  REQUIRE(cfg.value<double>("pmem.frequency", 0.0) == 1600.0);

  cfg.set("a.pi=3.14");
  REQUIRE_THROWS(cfg.value<long>("a.pi", 0)); // double -> int would truncate silently
  cfg.set("a.word=hello");
  REQUIRE_THROWS(cfg.value<long>("a.word", 0));        // string -> int never
  REQUIRE(cfg.value<bool>("a.count", false) == false); // absent -> fallback
  cfg.set("a.count=1");
  REQUIRE_THROWS(cfg.value<bool>("a.count", false)); // int -> bool never (1 is not true)
}

TEST_CASE("An out-of-range integer does not narrow silently")
{
  champsim::runtime_config cfg{};
  cfg.set("a.big=5000000000"); // > uint32 max
  REQUIRE(cfg.value<long long>("a.big", 0) == 5000000000LL);
  REQUIRE_THROWS(cfg.value<int>("a.big", 0));
}

TEST_CASE("Validation reports keys absent from the manifest, with a case suggestion")
{
  champsim::runtime_config cfg{};
  cfg.set("pmem.tCAS=24");
  cfg.set("ooo_cpu.cpu0.rob_size=512");
  constexpr std::array<std::string_view, 2> manifest{"pmem.tcas", "ooo_cpu.cpu0.rob_size"};

  const auto unknown = cfg.unknown_keys(std::begin(manifest), std::end(manifest));
  REQUIRE(std::size(unknown) == 1);
  REQUIRE_THAT(unknown.front(), Catch::Matchers::ContainsSubstring("pmem.tCAS"));
  REQUIRE_THAT(unknown.front(), Catch::Matchers::ContainsSubstring("pmem.tcas")); // the suggestion
}

TEST_CASE("The store records its provenance")
{
  temp_toml file{"[ooo_cpu.cpu0]\nrob_size = 512\n"};
  champsim::runtime_config cfg{};
  cfg.load_file(file.path);
  cfg.set("cache.cpu0_l1d.sets=128");

  REQUIRE(cfg.files() == std::vector<std::string>{file.path});

  const auto applied = cfg.applied();
  REQUIRE(std::size(applied) == 2);
  // Rendered in the document's own value syntax, ready for [config_override].
  REQUIRE(applied.at(0) == std::pair<std::string, std::string>{"cache.cpu0_l1d.sets", "128"});
  REQUIRE(applied.at(1) == std::pair<std::string, std::string>{"ooo_cpu.cpu0.rob_size", "512"});

  // A string value renders as a quoted, escaped TOML string -- the store
  // speaks the same syntax it reads, so the rendered pair can be spliced into
  // a document verbatim.
  cfg.set("a.name=with \"quotes\"");
  const auto with_string = cfg.applied();
  REQUIRE(with_string.at(0) == std::pair<std::string, std::string>{"a.name", "\"with \\\"quotes\\\"\""});
}

TEST_CASE("A consulted-key recording supports --knobs")
{
  champsim::runtime_config cfg{};
  cfg.value<long>("ooo_cpu.cpu0.rob_size", 352);
  cfg.value<double>("pmem.frequency", 1600.0);
  cfg.value<bool>("cache.llc.prefetch_as_load", false);

  // Sorted by key, with the fallback rendered in TOML value syntax.
  const auto consulted = cfg.consulted();
  REQUIRE(std::size(consulted) == 3);
  REQUIRE(consulted.at(0) == std::pair<std::string, std::string>{"cache.llc.prefetch_as_load", "false"});
  REQUIRE(consulted.at(1) == std::pair<std::string, std::string>{"ooo_cpu.cpu0.rob_size", "352"});
  REQUIRE(consulted.at(2) == std::pair<std::string, std::string>{"pmem.frequency", "1600.0"});
}

TEST_CASE("An out-of-range --set number is rejected, not clamped")
{
  // strtoll clamps on overflow and signals only through errno; without the
  // check, --set cache.llc.latency=18446744073709551615 silently became
  // INT64_MAX while the same value in a --config file was a fatal parse error.
  champsim::runtime_config cfg{};
  REQUIRE_THROWS(cfg.set("a.big=18446744073709551615"));
  REQUIRE_THROWS(cfg.set("a.small=-99999999999999999999"));
  REQUIRE_THROWS(cfg.set("a.huge=1e999"));
}

TEST_CASE("A consulted unsigned fallback above int64 max renders exactly")
{
  champsim::runtime_config cfg{};
  REQUIRE(cfg.value<uint64_t>("a.k", 18446744073709551615ULL) == 18446744073709551615ULL);
  const auto consulted = cfg.consulted();
  REQUIRE(consulted.at(0) == std::pair<std::string, std::string>{"a.k", "18446744073709551615"});
}

TEST_CASE("positive_value rejects zero and negative values")
{
  // Frequencies and rates divide into a clock period: zero would be a
  // division by zero whose inf is undefined behavior to cast, and a negative
  // clock runs time backwards. The generated frequency lookups use this.
  champsim::runtime_config cfg{};
  REQUIRE(cfg.positive_value<double>("a.f", 4000.0) == 4000.0);

  cfg.set("a.f=0");
  REQUIRE_THROWS(cfg.positive_value<double>("a.f", 4000.0));
  cfg.set("a.f=-250");
  REQUIRE_THROWS(cfg.positive_value<double>("a.f", 4000.0));
  cfg.set("a.f=3200");
  REQUIRE(cfg.positive_value<double>("a.f", 4000.0) == 3200.0);
}

TEST_CASE("A string value round-trips for module selection")
{
  champsim::runtime_config cfg{};
  REQUIRE(cfg.value<std::string>("ooo_cpu.cpu0.btb", "basic_btb") == "basic_btb");

  cfg.set("ooo_cpu.cpu0.btb=blbp_64kb_tuned");
  REQUIRE(cfg.value<std::string>("ooo_cpu.cpu0.btb", "basic_btb") == "blbp_64kb_tuned");

  // A quoted TOML string from a file is the same value.
  temp_toml file{"[ooo_cpu.cpu0]\nbtb = \"ittage_64kb\"\n"};
  cfg.load_file(file.path);
  REQUIRE(cfg.value<std::string>("ooo_cpu.cpu0.btb", "basic_btb") == "ittage_64kb");
}

TEST_CASE("A string lookup on a non-string value is a type error")
{
  champsim::runtime_config cfg{};
  cfg.set("a.k=42");
  REQUIRE_THROWS(cfg.value<std::string>("a.k", "x"));
  cfg.set("a.b=true");
  REQUIRE_THROWS(cfg.value<std::string>("a.b", "x"));
}

TEST_CASE("A consulted string fallback renders as a quoted TOML string")
{
  champsim::runtime_config cfg{};
  cfg.value<std::string>("ooo_cpu.cpu0.btb", "basic_btb");
  const auto consulted = cfg.consulted();
  REQUIRE(consulted.at(0) == std::pair<std::string, std::string>{"ooo_cpu.cpu0.btb", "\"basic_btb\""});
}

TEST_CASE("The consulted record holds the effective value, not the fallback")
{
  // [config] becomes the EFFECTIVE configuration once there is no baked JSON
  // to record, so a consulted key must report what the run actually used.
  champsim::runtime_config cfg{};
  cfg.set("ooo_cpu.cpu0.rob_size=512");

  REQUIRE(cfg.value<long>("ooo_cpu.cpu0.rob_size", 352) == 512);
  REQUIRE(cfg.value<long>("ooo_cpu.cpu0.lq_size", 128) == 128);

  const auto consulted = cfg.consulted();
  REQUIRE(consulted.at(1) == std::pair<std::string, std::string>{"ooo_cpu.cpu0.rob_size", "512"});
  REQUIRE(consulted.at(0) == std::pair<std::string, std::string>{"ooo_cpu.cpu0.lq_size", "128"});
}

TEST_CASE("An effective string value is recorded quoted, like any TOML string")
{
  champsim::runtime_config cfg{};
  cfg.set("ooo_cpu.cpu0.btb=ittage_64kb");
  REQUIRE(cfg.value<std::string>("ooo_cpu.cpu0.btb", "basic_btb") == "ittage_64kb");
  REQUIRE(cfg.consulted().at(0) == std::pair<std::string, std::string>{"ooo_cpu.cpu0.btb", "\"ittage_64kb\""});
}

TEST_CASE("An unsigned fallback above int64 max is recorded exactly when it is the effective value")
{
  // The store cannot HOLD such a value (set() rejects it as out of range), but
  // a baked default can be one, and the consulted record must not wrap it.
  champsim::runtime_config cfg{};
  REQUIRE(cfg.value<uint64_t>("a.k", 18446744073709551615ULL) == 18446744073709551615ULL);
  REQUIRE(cfg.consulted().at(0) == std::pair<std::string, std::string>{"a.k", "18446744073709551615"});
}

TEST_CASE("Keys nothing consulted are reported, with a suggestion from what was")
{
  // Post-construction validation: the machine reads every key it understands,
  // so a key left unread is one this binary has no use for -- a typo, or a
  // knob aimed at a component or module that is not part of this machine.
  champsim::runtime_config cfg{};
  cfg.set("ooo_cpu.cpu0.rob_size=512");
  cfg.set("ooo_cpu.cpu0.rob_sze=512");
  cfg.set("cache.cpu0_l1d.SETS=128");

  cfg.value<long>("ooo_cpu.cpu0.rob_size", 352);
  cfg.value<long>("cache.cpu0_l1d.sets", 64);

  const auto complaints = cfg.unconsulted_keys();
  REQUIRE(std::size(complaints) == 2);
  const auto joined = complaints.at(0) + " " + complaints.at(1);
  REQUIRE_THAT(joined, Catch::Matchers::ContainsSubstring("rob_sze"));
  REQUIRE_THAT(joined, Catch::Matchers::ContainsSubstring("cache.cpu0_l1d.SETS"));
  // The case-folded near miss gets a did-you-mean; the misspelling does not.
  REQUIRE_THAT(joined, Catch::Matchers::ContainsSubstring("did you mean 'cache.cpu0_l1d.sets'"));
}

TEST_CASE("A key that was consulted is never reported unconsulted")
{
  champsim::runtime_config cfg{};
  cfg.set("ooo_cpu.cpu0.rob_size=512");
  cfg.value<long>("ooo_cpu.cpu0.rob_size", 352);
  REQUIRE(std::empty(cfg.unconsulted_keys()));
}

TEST_CASE("Consulting a key the user never set does not make it a complaint")
{
  champsim::runtime_config cfg{};
  cfg.value<long>("ooo_cpu.cpu0.rob_size", 352);
  REQUIRE(std::empty(cfg.unconsulted_keys()));
}
