#include <catch.hpp>

#include <cstddef>
#include <type_traits>

#include "trace_instruction.h"

// The v2 trace record is a 512-byte extension of the classic ChampSim record.
// Its first 64 bytes are layout-identical to input_instr so that everything
// downstream of the reader can consume a v2 record unchanged; the remaining
// 448 bytes carry physical addresses, access sizes, privilege, instruction
// type, and the memory values that motivated the format.
//
// These are byte-for-byte guarantees against traces produced by an external
// tracer, so they are asserted explicitly rather than inferred.

TEST_CASE("The v2 trace record is exactly 512 bytes")
{
  REQUIRE(sizeof(input_instr_v2) == 512);
}

TEST_CASE("The v2 trace record is a trivial standard-layout type")
{
  // bulk_tracereader static_asserts both of these, and the reader memcpy's
  // raw trace bytes directly over the record.
  STATIC_REQUIRE(std::is_trivial_v<input_instr_v2>);
  STATIC_REQUIRE(std::is_standard_layout_v<input_instr_v2>);
}

TEST_CASE("The first 64 bytes of a v2 record are layout-identical to input_instr")
{
  REQUIRE(offsetof(input_instr_v2, ip) == offsetof(input_instr, ip));
  REQUIRE(offsetof(input_instr_v2, is_branch) == offsetof(input_instr, is_branch));
  REQUIRE(offsetof(input_instr_v2, branch_taken) == offsetof(input_instr, branch_taken));
  REQUIRE(offsetof(input_instr_v2, destination_registers) == offsetof(input_instr, destination_registers));
  REQUIRE(offsetof(input_instr_v2, source_registers) == offsetof(input_instr, source_registers));
  REQUIRE(offsetof(input_instr_v2, destination_memory) == offsetof(input_instr, destination_memory));
  REQUIRE(offsetof(input_instr_v2, source_memory) == offsetof(input_instr, source_memory));

  // ...and the shared prefix ends exactly where input_instr does.
  REQUIRE(sizeof(input_instr) == 64);
  REQUIRE(offsetof(input_instr_v2, destination_memory_pa) == sizeof(input_instr));
}

TEST_CASE("The v2 metadata block occupies bytes 64 through 127")
{
  REQUIRE(offsetof(input_instr_v2, destination_memory_pa) == 64);
  REQUIRE(offsetof(input_instr_v2, source_memory_pa) == 80);
  REQUIRE(offsetof(input_instr_v2, source_memory_size) == 112);
  REQUIRE(offsetof(input_instr_v2, destination_memory_size) == 116);
  REQUIRE(offsetof(input_instr_v2, privilege) == 118);
  REQUIRE(offsetof(input_instr_v2, instr_type) == 119);
}

TEST_CASE("The v2 memory-value block occupies bytes 128 through 511")
{
  REQUIRE(MAX_MEM_VALUE_SIZE == 64);
  REQUIRE(offsetof(input_instr_v2, source_memory_value) == 128);
  REQUIRE(offsetof(input_instr_v2, destination_memory_value) == 384);
  REQUIRE(sizeof(input_instr_v2{}.source_memory_value) == NUM_INSTR_SOURCES * MAX_MEM_VALUE_SIZE);
  REQUIRE(sizeof(input_instr_v2{}.destination_memory_value) == NUM_INSTR_DESTINATIONS * MAX_MEM_VALUE_SIZE);
}
