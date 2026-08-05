#include <catch.hpp>

#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "tracereader.h"

namespace
{
// v2 records are 512 bytes, so spelling them as byte literals (as the v1 tests
// do) would be unreadable. Build them structurally and serialise instead --
// this also exercises the exact reinterpretation the reader performs.
std::string serialize(const std::vector<input_instr_v2>& records)
{
  std::string out(std::size(records) * sizeof(input_instr_v2), '\0');
  std::memcpy(std::data(out), std::data(records), std::size(out));
  return out;
}

std::vector<input_instr_v2> sample_program()
{
  std::vector<input_instr_v2> prog(3);

  // 0: a load of 8 bytes from 0xdeadbe00 into register 11
  prog[0].ip = 0x1000;
  prog[0].source_registers[0] = 10;
  prog[0].destination_registers[0] = 11;
  prog[0].source_memory[0] = 0xdeadbe00;
  prog[0].source_memory_size[0] = 8;

  // 1: a taken conditional branch -- reads IP and FLAGS, writes IP
  prog[1].ip = 0x1004;
  prog[1].is_branch = 1;
  prog[1].branch_taken = 1;
  prog[1].destination_registers[0] = champsim::REG_INSTRUCTION_POINTER;
  prog[1].source_registers[0] = champsim::REG_INSTRUCTION_POINTER;
  prog[1].source_registers[1] = champsim::REG_FLAGS;

  // 2: the branch target
  prog[2].ip = 0x2000;

  return prog;
}
} // namespace

TEST_CASE("A tracereader can read the byte representation of an input_instr_v2")
{
  champsim::bulk_tracereader<input_instr_v2, std::istringstream> uut{0, std::istringstream{serialize(sample_program())}};

  auto inst0 = uut();
  REQUIRE(inst0.ip == champsim::address{0x1000});
  REQUIRE_FALSE(inst0.is_branch);
  REQUIRE(std::size(inst0.source_memory) == 1);
  REQUIRE(inst0.source_memory.at(0) == champsim::address{0xdeadbe00});
  REQUIRE(std::size(inst0.destination_memory) == 0);
}

TEST_CASE("A v2 record is classified into branch types identically to v1")
{
  champsim::bulk_tracereader<input_instr_v2, std::istringstream> uut{0, std::istringstream{serialize(sample_program())}};

  uut();
  auto branch = uut();
  REQUIRE(branch.ip == champsim::address{0x1004});
  REQUIRE(branch.is_branch);
  REQUIRE(branch.branch_taken);
  REQUIRE(branch.branch == BRANCH_CONDITIONAL);
}

TEST_CASE("A v2 record resolves its branch target from the following instruction")
{
  champsim::bulk_tracereader<input_instr_v2, std::istringstream> uut{0, std::istringstream{serialize(sample_program())}};

  uut();
  auto branch = uut();
  REQUIRE(branch.branch_target == champsim::address{0x2000});
}

TEST_CASE("A v1 and a v2 record describing the same instruction yield the same instruction")
{
  // The first 64 bytes are layout-identical, so the two readers must agree on
  // everything the core model consumes.
  auto prog = sample_program();

  std::vector<input_instr> v1_prog(std::size(prog));
  for (std::size_t i = 0; i < std::size(prog); ++i) {
    std::memcpy(&v1_prog.at(i), &prog.at(i), sizeof(input_instr));
  }
  std::string v1_bytes(std::size(v1_prog) * sizeof(input_instr), '\0');
  std::memcpy(std::data(v1_bytes), std::data(v1_prog), std::size(v1_bytes));

  champsim::bulk_tracereader<input_instr, std::istringstream> v1_reader{0, std::istringstream{v1_bytes}};
  champsim::bulk_tracereader<input_instr_v2, std::istringstream> v2_reader{0, std::istringstream{serialize(prog)}};

  for (int i = 0; i < 3; ++i) {
    auto a = v1_reader();
    auto b = v2_reader();
    REQUIRE(a.ip == b.ip);
    REQUIRE(a.is_branch == b.is_branch);
    REQUIRE(a.branch_taken == b.branch_taken);
    REQUIRE(a.branch == b.branch);
    REQUIRE(a.branch_target == b.branch_target);
    REQUIRE(a.source_memory == b.source_memory);
    REQUIRE(a.destination_memory == b.destination_memory);
  }
}
