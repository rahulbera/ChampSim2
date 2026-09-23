#include <algorithm>
#include <catch.hpp>
#include <cstdint>
#include <initializer_list>

#include "instr.h"
#include "mocks.hpp"
#include "ooo_cpu.h"

// The scheduler renames in program order, and stops at the first instruction
// the free register pool cannot cover. That check belongs only to instructions
// being renamed now. do_scheduling() overwrites an instruction's operands with
// physical register IDs in place, so a scheduled instruction's operands are no
// longer architectural IDs: checking it again charges it for registers it
// already holds, and looks its physical IDs up in the architectural RAT, which
// has only 256 entries.
//
// Upstream ChampSim fixed both in f46c1ff0, by running the check only for the
// instruction being renamed. Its test of the same name is the first scenario
// here, ported to core_builder.

namespace
{
ooo_model_instr ready_instruction(uint64_t id, std::initializer_list<int16_t> sources, std::initializer_list<int16_t> destinations)
{
  auto instr = champsim::test::instruction_with_ip(id);
  instr.instr_id = id;
  instr.source_registers.assign(sources);
  instr.destination_registers.assign(destinations);
  instr.ready_time = champsim::chrono::clock::time_point{};
  return instr;
}

auto core_with_registers(do_nothing_MRC& mock_L1I, do_nothing_MRC& mock_L1D, std::size_t register_file_size, unsigned schedule_width = 128)
{
  return champsim::core_builder{}
      .schedule_width(champsim::bandwidth::maximum_type{schedule_width})
      .register_file_size(register_file_size)
      .fetch_queues(&mock_L1I.queues)
      .data_queues(&mock_L1D.queues);
}
} // namespace

SCENARIO("An exhausted register file does not block scheduling behind in-flight writers")
{
  GIVEN("A core whose 32 registers are all taken by scheduled writers")
  {
    do_nothing_MRC mock_L1I, mock_L1D;
    O3_CPU uut{core_with_registers(mock_L1I, mock_L1D, 32)};

    for (uint64_t id = 1; id <= 32; ++id)
      uut.ROB.push_back(ready_instruction(id, {}, {10}));
    uut.schedule_instruction();

    REQUIRE(std::all_of(std::begin(uut.ROB), std::end(uut.ROB), [](const auto& instr) { return instr.scheduled; }));
    REQUIRE(uut.reg_allocator.count_free_registers() == 0);

    WHEN("A ready instruction that needs no registers arrives behind them")
    {
      uut.ROB.push_back(ready_instruction(1000, {}, {}));
      uut.schedule_instruction();

      THEN("It is scheduled") { REQUIRE(uut.ROB.back().scheduled); }
    }
  }
}

SCENARIO("A scheduled instruction's physical registers are not looked up as architectural ones")
{
  GIVEN("A core with 4 registers, 3 of them taken by one scheduled instruction")
  {
    do_nothing_MRC mock_L1I, mock_L1D;
    O3_CPU uut{core_with_registers(mock_L1I, mock_L1D, 4)};

    // Its sources become physical registers 0 and 1. Architectural registers 0
    // and 1 are unmapped, so reading those IDs as architectural makes this
    // instruction look like it needs two more registers.
    uut.ROB.push_back(ready_instruction(1, {10, 11}, {13}));
    uut.schedule_instruction();

    REQUIRE(uut.ROB.front().scheduled);
    REQUIRE(uut.ROB.front().source_registers == std::vector<int16_t>{0, 1});
    REQUIRE(uut.reg_allocator.count_free_registers() == 1);

    WHEN("An instruction that needs the last register arrives behind it")
    {
      uut.ROB.push_back(ready_instruction(2, {}, {12}));
      uut.schedule_instruction();

      THEN("It is scheduled and takes that register")
      {
        REQUIRE(uut.ROB.back().scheduled);
        REQUIRE(uut.reg_allocator.count_free_registers() == 0);
      }
    }
  }
}

SCENARIO("Physical register IDs of 256 and above are not used to index the architectural RAT")
{
  GIVEN("A core with 260 registers and a scheduled instruction whose sources are physical registers 256 and 257")
  {
    do_nothing_MRC mock_L1I, mock_L1D;
    O3_CPU uut{core_with_registers(mock_L1I, mock_L1D, 260, 1024)};

    for (uint64_t id = 1; id <= 256; ++id)
      uut.ROB.push_back(ready_instruction(id, {}, {200}));
    uut.ROB.push_back(ready_instruction(257, {10, 11}, {13}));
    uut.schedule_instruction();

    REQUIRE(uut.ROB.back().scheduled);
    REQUIRE(uut.ROB.back().source_registers == std::vector<int16_t>{256, 257});
    REQUIRE(uut.reg_allocator.count_free_registers() == 1);

    WHEN("An instruction that needs the last register arrives behind it")
    {
      uut.ROB.push_back(ready_instruction(258, {}, {12}));
      uut.schedule_instruction();

      THEN("It is scheduled") { REQUIRE(uut.ROB.back().scheduled); }
    }
  }
}

SCENARIO("Renaming stays in program order under register pressure")
{
  GIVEN("A core with 1 free register, an instruction needing 2, and a younger one needing none")
  {
    do_nothing_MRC mock_L1I, mock_L1D;
    O3_CPU uut{core_with_registers(mock_L1I, mock_L1D, 1)};

    uut.ROB.push_back(ready_instruction(1, {}, {10, 11}));
    uut.ROB.push_back(ready_instruction(2, {}, {}));

    WHEN("The scheduler runs")
    {
      uut.schedule_instruction();

      THEN("Neither is scheduled, because the younger may not rename before the older")
      {
        REQUIRE_FALSE(uut.ROB.at(0).scheduled);
        REQUIRE_FALSE(uut.ROB.at(1).scheduled);
        REQUIRE(uut.reg_allocator.count_free_registers() == 1);
      }
    }
  }
}

SCENARIO("An instruction's unmapped sources count against the free registers")
{
  GIVEN("A core with 2 registers and an instruction with two unmapped sources and a destination")
  {
    do_nothing_MRC mock_L1I, mock_L1D;
    O3_CPU uut{core_with_registers(mock_L1I, mock_L1D, 2)};

    uut.ROB.push_back(ready_instruction(1, {10, 11}, {12}));

    WHEN("The scheduler runs")
    {
      uut.schedule_instruction();

      THEN("It is not scheduled, and no register is taken")
      {
        REQUIRE_FALSE(uut.ROB.front().scheduled);
        REQUIRE(uut.reg_allocator.count_free_registers() == 2);
      }
    }
  }
}

SCENARIO("A repeated unmapped source is counted once")
{
  GIVEN("A core with 2 registers and an instruction reading one unmapped register twice and writing another")
  {
    do_nothing_MRC mock_L1I, mock_L1D;
    O3_CPU uut{core_with_registers(mock_L1I, mock_L1D, 2)};

    // Renaming maps the source on its first read, so the second read allocates
    // nothing: the instruction needs 2 registers, not 3.
    uut.ROB.push_back(ready_instruction(1, {10, 10}, {11}));

    WHEN("The scheduler runs")
    {
      uut.schedule_instruction();

      THEN("It is renamed, taking both registers")
      {
        REQUIRE(uut.ROB.front().scheduled);
        REQUIRE(uut.reg_allocator.count_free_registers() == 0);
      }
    }
  }
}
