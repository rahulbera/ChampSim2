#include <catch.hpp>
#include <cstdint>
#include <initializer_list>
#include <stdexcept>

#include "instr.h"
#include "mocks.hpp"
#include "ooo_cpu.h"

// Every architectural register a trace uses keeps one committed physical
// register for the rest of the run: registers are mapped on first use and a
// register is freed only when a newer write of the same architectural register
// retires. So a register file smaller than the trace's architectural footprint,
// plus what one instruction renames, eventually cannot rename the oldest
// instruction. With nothing older in flight, nothing can ever free a register.
// A single-core run used to spin until the generic DEADLOCK guard aborted it; a
// multi-core run could instead finish with that core frozen, or hit the
// livelock panic, and a run without -i could end at trace EOF as a success.

namespace
{
ooo_model_instr instruction(uint64_t id, std::initializer_list<int16_t> sources, std::initializer_list<int16_t> destinations)
{
  auto instr = champsim::test::instruction_with_ip(id);
  instr.instr_id = id;
  instr.source_registers.assign(sources);
  instr.destination_registers.assign(destinations);
  instr.ready_time = champsim::chrono::clock::time_point{};
  return instr;
}

auto core(do_nothing_MRC& mock_L1I, do_nothing_MRC& mock_L1D, std::size_t register_file_size, uint32_t cpu = 0)
{
  // A width of 128 lets one call reach every instruction in these ROBs; the default of 1 stops after the first.
  return champsim::core_builder{}
      .index(cpu)
      .schedule_width(champsim::bandwidth::maximum_type{128})
      .register_file_size(register_file_size)
      .fetch_queues(&mock_L1I.queues)
      .data_queues(&mock_L1D.queues);
}
} // namespace

SCENARIO("A register file that cannot rename the oldest instruction is a named error")
{
  GIVEN("A core with 3 registers, 2 of them holding committed architectural registers")
  {
    do_nothing_MRC mock_L1I, mock_L1D;
    O3_CPU uut{core(mock_L1I, mock_L1D, 3, 1)};
    uut.reg_allocator.rename_src_register(10);
    uut.reg_allocator.rename_src_register(11);
    REQUIRE(uut.reg_allocator.count_free_registers() == 1);

    WHEN("The oldest instruction needs 2 registers and nothing older is in flight")
    {
      uut.ROB.push_back(instruction(42, {}, {12, 13}));

      THEN("Scheduling reports the key, the size, and what the instruction needed")
      {
        REQUIRE_THROWS_AS(uut.schedule_instruction(), std::runtime_error);
        REQUIRE_THROWS_WITH(
            uut.schedule_instruction(),
            Catch::Matchers::ContainsSubstring("ooo_cpu.cpu1.register_file_size = 3")
                && Catch::Matchers::ContainsSubstring("the oldest instruction (instr_id 42) needs 2 registers to rename, with 1 free; the other 2 hold"));
        REQUIRE_FALSE(uut.ROB.front().scheduled);
        REQUIRE(uut.reg_allocator.count_free_registers() == 1);
      }
    }
  }
}

SCENARIO("An instruction short of registers waits while an older one is in flight")
{
  GIVEN("A core with 3 registers and an older writer the scheduler renames first")
  {
    do_nothing_MRC mock_L1I, mock_L1D;
    O3_CPU uut{core(mock_L1I, mock_L1D, 3)};
    uut.ROB.push_back(instruction(1, {}, {10}));
    uut.ROB.push_back(instruction(2, {}, {11, 12, 13}));

    WHEN("The scheduler runs")
    {
      REQUIRE_NOTHROW(uut.schedule_instruction());

      THEN("The older instruction is renamed and the younger waits for it, without an error")
      {
        REQUIRE(uut.ROB.at(0).scheduled);
        REQUIRE_FALSE(uut.ROB.at(1).scheduled);
        REQUIRE(uut.reg_allocator.count_free_registers() == 2);
      }
    }
  }
}

SCENARIO("With enough registers the same pair renames together")
{
  // The control for the scenario above: one call reaches the younger instruction.
  GIVEN("A core with 4 registers, an older writer and a younger instruction needing 3")
  {
    do_nothing_MRC mock_L1I, mock_L1D;
    O3_CPU uut{core(mock_L1I, mock_L1D, 4)};
    uut.ROB.push_back(instruction(1, {}, {10}));
    uut.ROB.push_back(instruction(2, {}, {11, 12, 13}));

    WHEN("The scheduler runs")
    {
      uut.schedule_instruction();

      THEN("Both are renamed")
      {
        REQUIRE(uut.ROB.at(0).scheduled);
        REQUIRE(uut.ROB.at(1).scheduled);
        REQUIRE(uut.reg_allocator.count_free_registers() == 0);
      }
    }
  }
}

SCENARIO("An oldest instruction that is not ready yet is not judged")
{
  GIVEN("A core with 1 register and an oldest instruction needing 2 that is not ready")
  {
    do_nothing_MRC mock_L1I, mock_L1D;
    O3_CPU uut{core(mock_L1I, mock_L1D, 1)};
    uut.ROB.push_back(instruction(1, {}, {10, 11}));
    uut.ROB.front().ready_time = champsim::chrono::clock::time_point{} + std::chrono::hours{1};

    THEN("The scheduler neither renames it nor reports an error")
    {
      REQUIRE_NOTHROW(uut.schedule_instruction());
      REQUIRE_FALSE(uut.ROB.front().scheduled);
    }
  }
}
