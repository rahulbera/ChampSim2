#include <catch.hpp>

#include "instr.h"
#include "ooo_cpu.h"

TEST_CASE("Execution timestamps only its own loads and stores in either phase")
{
  const auto warmup = GENERATE(false, true);
  const auto has_loads = GENERATE(false, true);
  const auto has_stores = GENERATE(false, true);
  CAPTURE(warmup, has_loads, has_stores);
  O3_CPU cpu{champsim::core_builder{}.lq_size(8).sq_size(8).execute_latency(3)};
  cpu.warmup = warmup;
  cpu.current_time = champsim::chrono::clock::time_point{champsim::chrono::picoseconds{1000}};

  auto other = champsim::test::instruction_with_ip(champsim::address{0x1000});
  other.instr_id = 1;
  other.source_memory = {champsim::address{0x2000}};
  other.destination_memory = {champsim::address{0x3000}};
  cpu.do_memory_scheduling(other);

  auto target = champsim::test::instruction_with_ip(champsim::address{0x1004});
  target.instr_id = 2;
  if (has_loads) {
    target.source_memory = {champsim::address{0x4000}, champsim::address{0x5000}};
  }
  if (has_stores) {
    target.destination_memory = {champsim::address{0x6000}, champsim::address{0x7000}};
  }
  cpu.do_memory_scheduling(target);
  REQUIRE(std::count_if(std::begin(cpu.LQ), std::end(cpu.LQ), [](const auto& entry) { return entry.has_value(); }) == 1 + (has_loads ? 2 : 0));
  REQUIRE(cpu.SQ.size() == 1u + (has_stores ? 2u : 0u));

  cpu.do_execution(target);
  const auto ready = cpu.current_time + (warmup ? champsim::chrono::clock::duration{} : cpu.EXEC_LATENCY);
  CHECK(target.executed);
  CHECK(target.ready_time == ready);
  for (const auto& entry : cpu.LQ) {
    if (entry) {
      CHECK(entry->ready_time == (entry->instr_id == target.instr_id ? ready : champsim::chrono::clock::time_point::max()));
    }
  }
  for (const auto& entry : cpu.SQ) {
    CHECK(entry.ready_time == (entry.instr_id == target.instr_id ? ready : champsim::chrono::clock::time_point::max()));
  }
}
