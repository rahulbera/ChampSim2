#include <catch.hpp>

#include <algorithm>
#include <cstdint>

#include "instruction.h"

// The v2 payload is opt-in (see inc/trace_instruction.h): carrying it costs
// roughly 2x wall-clock and nothing reads it yet. These cases therefore only
// build when it is enabled:
//
//     CPPFLAGS=-DCHAMPSIM_TRACE_MEMORY_VALUES=1 make test
//
// The macro must be the same for the whole build -- ChampSim shares core object
// files between the test binary and the simulator binaries, so varying a
// struct-layout macro per target is an ODR violation.
#if CHAMPSIM_TRACE_MEMORY_VALUES

namespace
{
// A v2 record whose source memory operands sit in slots 0 and 2, leaving slot 1
// empty. ooo_model_instr compacts source_memory (zeros are removed), so the
// carried payload must be compacted the same way or the two would silently
// disagree about which operand is which.
input_instr_v2 gappy_record()
{
  input_instr_v2 rec{};
  rec.ip = 0x1000;

  rec.source_memory[0] = 0xaaaa0000;
  rec.source_memory_pa[0] = 0x11110000;
  rec.source_memory_size[0] = 8;
  rec.source_memory_value[0][0] = 0xa1;

  // slot 1 deliberately empty

  rec.source_memory[2] = 0xcccc0000;
  rec.source_memory_pa[2] = 0x33330000;
  rec.source_memory_size[2] = 4;
  rec.source_memory_value[2][0] = 0xc3;

  // Slot 0 deliberately empty, so the destination half has a gap too. With the
  // operand in slot 0 the compacted index equals the raw slot index and the
  // compaction is not actually under test.
  rec.destination_memory[1] = 0xdddd0000;
  rec.destination_memory_pa[1] = 0x44440000;
  rec.destination_memory_size[1] = 2;
  rec.destination_memory_value[1][0] = 0xd4;

  rec.privilege = 1;
  rec.instr_type = 2;
  return rec;
}
} // namespace

TEST_CASE("A v2 instruction carries its source memory values, compacted to match source_memory")
{
  ooo_model_instr uut{0, gappy_record()};

  REQUIRE(std::size(uut.source_memory) == 2);
  REQUIRE(uut.source_memory.at(0) == champsim::address{0xaaaa0000});
  REQUIRE(uut.source_memory.at(1) == champsim::address{0xcccc0000});

  // The value at index i must belong to the operand at source_memory[i].
  REQUIRE(uut.source_memory_value.at(0).at(0) == 0xa1);
  REQUIRE(uut.source_memory_value.at(1).at(0) == 0xc3);
}

TEST_CASE("A v2 instruction carries physical addresses and access sizes, compacted alike")
{
  ooo_model_instr uut{0, gappy_record()};

  REQUIRE(uut.source_memory_pa.at(0) == champsim::address{0x11110000});
  REQUIRE(uut.source_memory_pa.at(1) == champsim::address{0x33330000});
  REQUIRE(uut.source_memory_size.at(0) == 8);
  REQUIRE(uut.source_memory_size.at(1) == 4);
}

TEST_CASE("A v2 instruction carries its destination memory payload")
{
  ooo_model_instr uut{0, gappy_record()};

  REQUIRE(std::size(uut.destination_memory) == 1);
  REQUIRE(uut.destination_memory_pa.at(0) == champsim::address{0x44440000});
  REQUIRE(uut.destination_memory_size.at(0) == 2);
  REQUIRE(uut.destination_memory_value.at(0).at(0) == 0xd4);
}

TEST_CASE("A v2 instruction carries privilege and instruction type")
{
  ooo_model_instr uut{0, gappy_record()};

  REQUIRE(uut.privilege == 1);
  REQUIRE(uut.instr_type == 2);
}

TEST_CASE("A v1 instruction leaves the v2 payload zeroed")
{
  input_instr rec{};
  rec.ip = 0x2000;
  rec.source_memory[0] = 0xbbbb0000;

  ooo_model_instr uut{0, rec};

  REQUIRE(std::size(uut.source_memory) == 1);
  REQUIRE(uut.source_memory_pa.at(0) == champsim::address{});
  REQUIRE(uut.source_memory_size.at(0) == 0);
  REQUIRE(std::all_of(std::begin(uut.source_memory_value.at(0)), std::end(uut.source_memory_value.at(0)), [](auto b) { return b == 0; }));
  REQUIRE(uut.privilege == 0);
  REQUIRE(uut.instr_type == 0);
}
#endif // CHAMPSIM_TRACE_MEMORY_VALUES
