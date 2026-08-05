#include <catch.hpp>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <zstd.h>

#include "inf_stream.h"
#include "tracereader.h"

namespace
{
std::vector<input_instr_v2> sample_program()
{
  std::vector<input_instr_v2> prog(3);
  prog[0].ip = 0x1000;
  prog[0].source_memory[0] = 0xdeadbe00;

  prog[1].ip = 0x1004;
  prog[1].is_branch = 1;
  prog[1].branch_taken = 1;
  prog[1].destination_registers[0] = champsim::REG_INSTRUCTION_POINTER;
  prog[1].source_registers[0] = champsim::REG_INSTRUCTION_POINTER;
  prog[1].source_registers[1] = champsim::REG_FLAGS;

  prog[2].ip = 0x2000;
  return prog;
}

std::string raw_bytes(const std::vector<input_instr_v2>& prog)
{
  std::string out(std::size(prog) * sizeof(input_instr_v2), '\0');
  std::memcpy(std::data(out), std::data(prog), std::size(out));
  return out;
}

std::string zstd_compress(const std::string& plain)
{
  std::string out(::ZSTD_compressBound(std::size(plain)), '\0');
  const std::size_t n = ::ZSTD_compress(std::data(out), std::size(out), std::data(plain), std::size(plain), 3);
  REQUIRE(::ZSTD_isError(n) == 0U);
  out.resize(n);
  return out;
}
} // namespace

TEST_CASE("A zstd-compressed stream of v2 records can be read")
{
  auto compressed = zstd_compress(raw_bytes(sample_program()));

  champsim::bulk_tracereader<input_instr_v2, champsim::inf_istream<champsim::decomp_tags::zstd_tag_t, std::istringstream>> uut{
      0, champsim::inf_istream<champsim::decomp_tags::zstd_tag_t, std::istringstream>{std::istringstream{compressed}}};

  auto inst0 = uut();
  REQUIRE(inst0.ip == champsim::address{0x1000});
  REQUIRE(inst0.source_memory.at(0) == champsim::address{0xdeadbe00});

  auto branch = uut();
  REQUIRE(branch.ip == champsim::address{0x1004});
  REQUIRE(branch.branch == BRANCH_CONDITIONAL);
  REQUIRE(branch.branch_target == champsim::address{0x2000});
}

TEST_CASE("get_tracereader reads a .zst file of v2 records end to end")
{
  const std::string path{"champsim-test-v2.champsim2.zst"};
  {
    std::ofstream out{path, std::ios::binary};
    auto compressed = zstd_compress(raw_bytes(sample_program()));
    out.write(std::data(compressed), static_cast<std::streamsize>(std::size(compressed)));
  }

  {
    auto uut = get_tracereader(path, 0, false, false, 2);

    auto inst0 = uut();
    REQUIRE(inst0.ip == champsim::address{0x1000});
    REQUIRE(inst0.source_memory.at(0) == champsim::address{0xdeadbe00});

    auto branch = uut();
    REQUIRE(branch.ip == champsim::address{0x1004});
    REQUIRE(branch.branch == BRANCH_CONDITIONAL);
    REQUIRE(branch.branch_target == champsim::address{0x2000});
  }

  std::remove(path.c_str());
}

TEST_CASE("get_tracereader rejects the cloudsuite/v2 combination")
{
  // There is no v2 cloudsuite record, so this must fail loudly rather than
  // silently reading one format as the other.
  REQUIRE_THROWS_AS(get_tracereader("unused.champsimtrace.zst", 0, true, false, 2), std::invalid_argument);
}

TEST_CASE("get_tracereader refuses to read a champsim2-named trace as v1")
{
  // The trace format is headerless, so a v2 file read as v1 does not error --
  // it yields plausible-looking but entirely wrong statistics. Catch the
  // likely mistake via the naming convention rather than letting it through.
  REQUIRE_THROWS_AS(get_tracereader("somewhere/723.llvm_r.champsim2.zst", 0, false, false, 1), std::invalid_argument);
  REQUIRE_NOTHROW(get_tracereader("somewhere/723.llvm_r.champsim2.zst", 0, false, false, 2));
}

TEST_CASE("get_tracereader still defaults to the v1 record format")
{
  const std::string path{"champsim-test-v1.champsimtrace.zst"};

  std::vector<input_instr> prog(2);
  prog[0].ip = 0x4000;
  prog[1].ip = 0x4004;
  std::string plain(std::size(prog) * sizeof(input_instr), '\0');
  std::memcpy(std::data(plain), std::data(prog), std::size(plain));

  {
    std::ofstream out{path, std::ios::binary};
    auto compressed = zstd_compress(plain);
    out.write(std::data(compressed), static_cast<std::streamsize>(std::size(compressed)));
  }

  {
    auto uut = get_tracereader(path, 0, false, false);
    REQUIRE(uut().ip == champsim::address{0x4000});
    REQUIRE(uut().ip == champsim::address{0x4004});
  }

  std::remove(path.c_str());
}

TEST_CASE("get_tracereader refuses to read a v1-named trace as v2")
{
  // The mirror of the champsim2 guard. A v1 file read as v2 consumes eight
  // 64-byte records per 512-byte read and is just as silently wrong.
  REQUIRE_THROWS_AS(get_tracereader("traces/400.perlbench.champsimtrace.xz", 0, false, false, 2), std::invalid_argument);
}

TEST_CASE("The naming guard looks at the file name, not the directory")
{
  // A v1 trace stored under a directory called champsim2-traces must not trip
  // the v2 guard.
  REQUIRE_NOTHROW(get_tracereader("champsim2-traces/400.perlbench.champsimtrace.xz", 0, false, false, 1));
}
