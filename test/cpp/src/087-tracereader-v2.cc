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
template <typename T>
std::string serialize(const std::vector<T>& records)
{
  std::string out(std::size(records) * sizeof(T), '\0');
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

template <typename T>
void check_trailing_partial_record()
{
  std::vector<T> records(2);
  records[0].ip = 0x1000;
  records[0].is_branch = 1;
  records[0].branch_taken = 1;
  records[0].destination_registers[0] = champsim::REG_INSTRUCTION_POINTER;
  records[0].source_registers[0] = champsim::REG_INSTRUCTION_POINTER;
  records[0].source_registers[1] = champsim::REG_FLAGS;
  records[1].ip = 0x2000;

  auto bytes = serialize(records);
  bytes.append(sizeof(T) - 1, '\xff');
  champsim::bulk_tracereader<T, std::istringstream> reader{0, std::istringstream{bytes}};

  CHECK_FALSE(reader.eof());
  const auto branch = reader();
  CHECK(branch.ip == champsim::address{0x1000});
  CHECK(branch.branch_target == champsim::address{0x2000});
  CHECK(reader.eof());
  CHECK(reader().ip == champsim::address{0x2000});
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

TEST_CASE("A v2 branch at a refill boundary resolves its target from the next refill")
{
  std::vector<input_instr_v2> prog(128);
  for (std::size_t i = 0; i < std::size(prog); ++i)
    prog[i].ip = 0x1000 + 4 * i;

  prog[126].is_branch = 1;
  prog[126].branch_taken = 1;
  prog[126].destination_registers[0] = champsim::REG_INSTRUCTION_POINTER;
  prog[126].source_registers[0] = champsim::REG_INSTRUCTION_POINTER;
  prog[126].source_registers[1] = champsim::REG_FLAGS;
  prog[127].ip = 0x2000;

  champsim::bulk_tracereader<input_instr_v2, std::istringstream> uut{0, std::istringstream{serialize(prog)}};
  for (int i = 0; i < 126; ++i)
    uut();

  auto branch = uut();
  REQUIRE(branch.ip == champsim::address{0x11f8});
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

TEST_CASE("Constructing from a sparse v2 record preserves raw slots and owns compacted operands")
{
  auto record = sample_program().front();
  record.source_memory[0] = 0x11110000;
  record.source_memory[2] = 0x33330000;
  record.destination_memory[1] = 0x44440000;
  const input_instr_v2 raw_record = record;

  const ooo_model_instr instr{0, raw_record};

  REQUIRE(raw_record.source_memory[0] == 0x11110000);
  REQUIRE(raw_record.source_memory[1] == 0);
  REQUIRE(raw_record.source_memory[2] == 0x33330000);
  REQUIRE(raw_record.destination_memory[0] == 0);
  REQUIRE(raw_record.destination_memory[1] == 0x44440000);
  REQUIRE_THAT(instr.source_memory, Catch::Matchers::RangeEquals(std::vector<champsim::address>{champsim::address{0x11110000}, champsim::address{0x33330000}}));
  REQUIRE_THAT(instr.destination_memory, Catch::Matchers::RangeEquals(std::vector<champsim::address>{champsim::address{0x44440000}}));

  const ooo_model_instr owned_instr{0, record};
  record.source_memory[0] = 0;
  record.source_memory[2] = 0;
  record.destination_memory[1] = 0;
  record.source_memory[1] = 0x22220000;
  REQUIRE_THAT(owned_instr.source_memory,
               Catch::Matchers::RangeEquals(std::vector<champsim::address>{champsim::address{0x11110000}, champsim::address{0x33330000}}));
}

TEST_CASE("v1 and CloudSuite constructors compact sparse records and own their operands")
{
  SECTION("v1")
  {
    input_instr record{};
    record.source_memory[2] = 0x33330000;
    record.destination_memory[1] = 0x44440000;
    const ooo_model_instr instr{2, record};
    record.source_memory[2] = 0;
    record.destination_memory[1] = 0;

    REQUIRE(instr.asid == std::array<uint8_t, 2>{2, 2});
    REQUIRE_THAT(instr.source_memory, Catch::Matchers::RangeEquals(std::vector<champsim::address>{champsim::address{0x33330000}}));
    REQUIRE_THAT(instr.destination_memory, Catch::Matchers::RangeEquals(std::vector<champsim::address>{champsim::address{0x44440000}}));
  }

  SECTION("CloudSuite")
  {
    cloudsuite_instr record{};
    record.asid[0] = 3;
    record.asid[1] = 7;
    record.source_memory[2] = 0x33330000;
    record.destination_memory[3] = 0x55550000;
    const ooo_model_instr instr{2, record};
    record.source_memory[2] = 0;
    record.destination_memory[3] = 0;

    REQUIRE(instr.asid == std::array<uint8_t, 2>{3, 7});
    REQUIRE_THAT(instr.source_memory, Catch::Matchers::RangeEquals(std::vector<champsim::address>{champsim::address{0x33330000}}));
    REQUIRE_THAT(instr.destination_memory, Catch::Matchers::RangeEquals(std::vector<champsim::address>{champsim::address{0x55550000}}));
  }
}

TEST_CASE("Trailing partial records retain complete-record lookahead and EOF behavior")
{
  SECTION("v1") { check_trailing_partial_record<input_instr>(); }
  SECTION("v2") { check_trailing_partial_record<input_instr_v2>(); }
  SECTION("CloudSuite") { check_trailing_partial_record<cloudsuite_instr>(); }
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
