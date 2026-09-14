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

TEST_CASE("Trace operands and targets survive multiple refill boundaries")
{
  std::vector<input_instr_v2> program(260);
  for (std::size_t i = 0; i < program.size(); ++i) {
    auto& record = program.at(i);
    record.ip = 0x1000 + 4 * i;
    record.is_branch = static_cast<unsigned char>(i % 3 != 0);
    record.branch_taken = static_cast<unsigned char>(i % 3 == 2);
    // These lookahead records acquire their targets from the next refill.
    if (i == 126 || i == 253) {
      record.is_branch = 1;
      record.branch_taken = 1;
    }
    record.source_registers[0] = record.is_branch ? champsim::REG_INSTRUCTION_POINTER : 10;
    record.source_registers[2] = record.is_branch ? champsim::REG_FLAGS : 12;
    record.destination_registers[1] = record.is_branch ? champsim::REG_INSTRUCTION_POINTER : 11;
    record.source_memory[0] = 0x100000 + 64 * i;
    record.source_memory[2] = 0x200000 + 64 * i;
    record.destination_memory[1] = 0x300000 + 64 * i;
    record.source_memory_pa[0] = 0x400000 + 64 * i;
    record.source_memory_pa[2] = 0x500000 + 64 * i;
    record.destination_memory_pa[1] = 0x600000 + 64 * i;
    record.source_memory_size[0] = 8;
    record.source_memory_size[2] = 4;
    record.destination_memory_size[1] = 2;
    record.privilege = static_cast<unsigned char>(i % 2);
    record.instr_type = static_cast<unsigned char>(i % 3);
    for (std::size_t byte = 0; byte < MAX_MEM_VALUE_SIZE; ++byte) {
      record.source_memory_value[0][byte] = static_cast<unsigned char>(i + byte);
      record.source_memory_value[2][byte] = static_cast<unsigned char>(i ^ byte);
      record.destination_memory_value[1][byte] = static_cast<unsigned char>(i - byte);
    }
  }

  champsim::bulk_tracereader<input_instr_v2, std::istringstream> reader{0, std::istringstream{serialize(program)}};
  // Keep the trailing lookahead record, matching the reader's EOF contract for
  // a partial final read. This crosses the 127- and 254-record refill boundaries.
  for (std::size_t i = 0; i + 1 < program.size(); ++i) {
    CAPTURE(i);
    REQUIRE_FALSE(reader.eof());
    const auto actual = reader();
    const ooo_model_instr expected{0, program.at(i)};
    CHECK(actual.ip == expected.ip);
    CHECK(actual.asid == expected.asid);
    CHECK(actual.branch == expected.branch);
    CHECK(actual.is_branch == expected.is_branch);
    CHECK(actual.branch_taken == expected.branch_taken);
    CHECK(actual.branch_target == (expected.is_branch && expected.branch_taken ? champsim::address{program.at(i + 1).ip} : champsim::address{}));
    CHECK(actual.source_registers == expected.source_registers);
    CHECK(actual.destination_registers == expected.destination_registers);
    CHECK(actual.source_memory == expected.source_memory);
    CHECK(actual.destination_memory == expected.destination_memory);
#if CHAMPSIM_TRACE_MEMORY_VALUES
    CHECK(actual.source_memory_pa == expected.source_memory_pa);
    CHECK(actual.destination_memory_pa == expected.destination_memory_pa);
    CHECK(actual.source_memory_size == expected.source_memory_size);
    CHECK(actual.destination_memory_size == expected.destination_memory_size);
    CHECK(actual.source_memory_value == expected.source_memory_value);
    CHECK(actual.destination_memory_value == expected.destination_memory_value);
    CHECK(actual.privilege == expected.privilege);
    CHECK(actual.instr_type == expected.instr_type);
#endif
  }
  CHECK(reader.eof());
}
