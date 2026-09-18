#include <catch.hpp>

#include "instr.h"
#include "mocks.hpp"
#include "ooo_cpu.h"

namespace
{
auto ready_instruction(uint64_t id)
{
  auto instr = champsim::test::instruction_with_ip(id * 4);
  instr.instr_id = id;
  instr.scheduled = true;
  instr.ready_time = champsim::chrono::clock::time_point{};
  return instr;
}

auto blocked_memory_instruction(uint64_t id)
{
  auto instr = ready_instruction(id);
  instr.executed = true;
  instr.source_memory = {champsim::address{0x1000}};
  return instr;
}

struct rob_fixture {
  do_nothing_MRC l1i, l1d;
  O3_CPU cpu{champsim::core_builder{}
                 .fetch_queues(&l1i.queues)
                 .data_queues(&l1d.queues)
                 .lq_size(8)
                 .sq_size(8)
                 .register_file_size(128)
                 .execute_width(champsim::bandwidth::maximum_type{2})
                 .retire_width(champsim::bandwidth::maximum_type{0})
                 .execute_latency(3)};

  rob_fixture()
  {
    cpu.warmup = false;
    cpu.current_time = champsim::chrono::clock::time_point{champsim::chrono::picoseconds{10000}};
  }
};

struct resetting_predictor : champsim::modules::branch_predictor {
  using branch_predictor::branch_predictor;

  void branch_execute_resolve(uint64_t instr_id, champsim::address, champsim::address, bool, uint8_t)
  {
    // The callback's current instruction remains alive and its deque iterator
    // remains valid when a different, older entry is removed from the front.
    if (instr_id == 4) {
      intern_->ROB.pop_front();
    }
    intern_->ROB.front().executed = false;
    intern_->ROB.front().ready_time = intern_->current_time;
  }
};
} // namespace

TEST_CASE_METHOD(rob_fixture, "ROB execution leaves empty and memory-blocked windows unchanged")
{
  SECTION("Empty") { CHECK(cpu.operate() == 0); }
  SECTION("All executed with pending memory")
  {
    cpu.ROB = {blocked_memory_instruction(1), blocked_memory_instruction(2)};
    const auto old_time = cpu.ROB.front().ready_time;
    CHECK(cpu.operate() == 0);
    for (const auto& instr : cpu.ROB) {
      CHECK(instr.executed);
      CHECK_FALSE(instr.completed);
      CHECK(instr.ready_time == old_time);
    }
  }
}

TEST_CASE_METHOD(rob_fixture, "ROB execution selects the oldest ready instructions after a blocked memory prefix")
{
  cpu.ROB = {blocked_memory_instruction(1), blocked_memory_instruction(2), ready_instruction(3), ready_instruction(4), ready_instruction(5)};
  CHECK(cpu.operate() == 2);
  const auto ready = cpu.current_time + cpu.EXEC_LATENCY;
  CHECK(cpu.ROB[2].executed);
  CHECK(cpu.ROB[3].executed);
  CHECK_FALSE(cpu.ROB[4].executed);
  CHECK(cpu.ROB[2].ready_time == ready);
  CHECK(cpu.ROB[3].ready_time == ready);
  CHECK(cpu.ROB[4].ready_time == champsim::chrono::clock::time_point{});
  CHECK_FALSE(cpu.ROB[2].completed);
  CHECK_FALSE(cpu.ROB[3].completed);
}

TEST_CASE_METHOD(rob_fixture, "An unready first ROB candidate does not block later independent execution")
{
  cpu.ROB = {blocked_memory_instruction(1), ready_instruction(2), ready_instruction(3)};
  PHYSICAL_REGISTER_ID dependency{};
  enum class blockage { unscheduled, future, reg };
  const auto blocked_by = GENERATE(blockage::unscheduled, blockage::future, blockage::reg);
  if (blocked_by == blockage::unscheduled) {
    cpu.ROB[1].scheduled = false;
  } else if (blocked_by == blockage::future) {
    cpu.ROB[1].ready_time = cpu.current_time + cpu.clock_period;
  } else {
    dependency = cpu.reg_allocator.rename_dest_register(7, 0);
    cpu.ROB[1].source_registers = {dependency};
  }
  cpu.operate();
  CHECK_FALSE(cpu.ROB[1].executed);
  CHECK(cpu.ROB[2].executed);
  CHECK(cpu.ROB[2].ready_time == cpu.current_time + cpu.EXEC_LATENCY);
  cpu.current_time += cpu.clock_period;
  if (blocked_by == blockage::reg) {
    cpu.reg_allocator.complete_dest_register(dependency);
  }
  cpu.operate();
  CHECK(cpu.ROB[1].executed);
  CHECK(cpu.ROB[1].ready_time == cpu.current_time + cpu.EXEC_LATENCY);
}

TEST_CASE_METHOD(rob_fixture, "Completion and execution each use their full width in ROB order")
{
  for (uint64_t id = 1; id <= 3; ++id) {
    auto instr = ready_instruction(id);
    instr.executed = true;
    cpu.ROB.push_back(instr);
  }
  for (uint64_t id = 4; id <= 6; ++id) {
    cpu.ROB.push_back(ready_instruction(id));
  }
  CHECK(cpu.operate() == 4);
  CHECK(cpu.ROB[0].completed);
  CHECK(cpu.ROB[1].completed);
  CHECK_FALSE(cpu.ROB[2].completed);
  CHECK(cpu.ROB[3].executed);
  CHECK(cpu.ROB[4].executed);
  CHECK_FALSE(cpu.ROB[5].executed);
  CHECK(cpu.ROB[3].ready_time == cpu.current_time + cpu.EXEC_LATENCY);
  CHECK(cpu.ROB[4].ready_time == cpu.current_time + cpu.EXEC_LATENCY);
}

TEST_CASE_METHOD(rob_fixture, "Producer completion in the final completion slot wakes same-cycle execution")
{
  const auto width = GENERATE(1L, 2L);
  cpu.EXEC_WIDTH = champsim::bandwidth::maximum_type{width};
  auto producer = ready_instruction(2);
  producer.destination_registers = {7};
  cpu.do_scheduling(producer);
  producer.executed = true;
  auto dependent = ready_instruction(3);
  dependent.source_registers = {7};
  cpu.do_scheduling(dependent);
  if (width == 2) {
    auto other = ready_instruction(1);
    other.executed = true;
    cpu.ROB.push_back(other);
  }
  cpu.ROB.push_back(producer);
  cpu.ROB.push_back(dependent);
  REQUIRE_FALSE(cpu.reg_allocator.isValid(dependent.source_registers.front()));
  CHECK(cpu.operate() == width + 1);
  CHECK(cpu.ROB[cpu.ROB.size() - 2].completed);
  CHECK(cpu.ROB.back().executed);
  CHECK_FALSE(cpu.ROB.back().completed);
  CHECK(cpu.ROB.back().ready_time == cpu.current_time + cpu.EXEC_LATENCY);
}

TEST_CASE("Completion callbacks can make an older ROB entry win execution")
{
  const auto pop_front = GENERATE(false, true);
  const auto has_competitor = GENERATE(false, true);
  CAPTURE(pop_front, has_competitor);
  do_nothing_MRC l1i, l1d;
  O3_CPU cpu{champsim::core_builder{}
                 .fetch_queues(&l1i.queues)
                 .data_queues(&l1d.queues)
                 .lq_size(8)
                 .sq_size(8)
                 .register_file_size(128)
                 .execute_width(champsim::bandwidth::maximum_type{1})
                 .retire_width(champsim::bandwidth::maximum_type{0})
                 .branch_predictor<resetting_predictor>()};
  cpu.warmup = false;
  if (pop_front) {
    cpu.ROB.push_back(blocked_memory_instruction(1));
  }
  auto earlier = ready_instruction(2);
  earlier.executed = true;
  earlier.ready_time = champsim::chrono::clock::time_point::max();
  cpu.ROB.push_back(earlier);
  if (has_competitor) {
    cpu.ROB.push_back(ready_instruction(3)); // The first unexecuted entry before the callback.
  }
  // Without a competitor, a prefix measured before pop_front is the old ROB
  // size and exceeds the new size. It must not even be converted to an iterator.
  auto branch = ready_instruction(pop_front ? 4 : 5);
  branch.executed = true;
  branch.is_branch = true;
  cpu.ROB.push_back(branch);
  CHECK(cpu.operate() == 2);
  REQUIRE(cpu.ROB.size() == (has_competitor ? 3u : 2u));
  CHECK(cpu.ROB.front().instr_id == 2);
  CHECK(cpu.ROB.front().executed);
  CHECK(cpu.ROB.front().ready_time == cpu.current_time + cpu.EXEC_LATENCY);
  if (has_competitor) {
    CHECK_FALSE(cpu.ROB[1].executed);
  }
  CHECK(cpu.ROB.back().completed);
}

TEST_CASE_METHOD(rob_fixture, "Standalone ROB stages inspect mutations between public calls")
{
  cpu.ROB = {blocked_memory_instruction(1), ready_instruction(2)};
  CHECK(cpu.complete_inflight_instruction() == 0);
  SECTION("Reset the old head")
  {
    cpu.ROB.front() = ready_instruction(1);
    CHECK(cpu.execute_instruction() == 2);
    CHECK(cpu.ROB.front().executed);
  }
  SECTION("Clear and repopulate")
  {
    cpu.ROB.clear();
    cpu.ROB.push_back(ready_instruction(3));
    CHECK(cpu.execute_instruction() == 1);
    CHECK(cpu.ROB.front().executed);
  }
  SECTION("Append after completion")
  {
    cpu.ROB.push_back(ready_instruction(3));
    CHECK(cpu.execute_instruction() == 2);
    CHECK(cpu.ROB.back().executed);
  }
}

TEST_CASE_METHOD(rob_fixture, "ROB execution has no state surviving direct mutations between cycles")
{
  cpu.ROB = {blocked_memory_instruction(1), blocked_memory_instruction(2)};
  CHECK(cpu.operate() == 0);
  const auto check_execution = [&] {
    CHECK(cpu.operate() == 1);
    if (cpu.ROB.front().instr_id == 1 && cpu.ROB.size() == 3) {
      CHECK(cpu.ROB.back().executed);
      CHECK(cpu.ROB.back().ready_time == cpu.current_time + cpu.EXEC_LATENCY);
    } else {
      CHECK(cpu.ROB.front().executed);
      CHECK(cpu.ROB.front().ready_time == cpu.current_time + cpu.EXEC_LATENCY);
    }
  };
  SECTION("Reset the head")
  {
    cpu.ROB.front() = ready_instruction(1);
    check_execution();
  }
  SECTION("Clear and repopulate")
  {
    cpu.ROB.clear();
    cpu.ROB.push_back(ready_instruction(3));
    check_execution();
  }
  SECTION("Append")
  {
    cpu.ROB.push_back(ready_instruction(3));
    check_execution();
  }
}

TEST_CASE_METHOD(rob_fixture, "Zero execution width holds both stages until width is restored")
{
  auto completing = ready_instruction(1);
  completing.executed = true;
  cpu.ROB = {completing, ready_instruction(2)};
  cpu.EXEC_WIDTH = champsim::bandwidth::maximum_type{0};
  CHECK(cpu.operate() == 0);
  CHECK_FALSE(cpu.ROB[0].completed);
  CHECK_FALSE(cpu.ROB[1].executed);
  CHECK(cpu.complete_inflight_instruction() == 0);
  CHECK(cpu.execute_instruction() == 0);
  cpu.EXEC_WIDTH = champsim::bandwidth::maximum_type{1};
  CHECK(cpu.operate() == 2);
  CHECK(cpu.ROB[0].completed);
  CHECK(cpu.ROB[1].executed);
}

TEST_CASE_METHOD(rob_fixture, "Retirement exposes the next ROB head before execution")
{
  auto retired = ready_instruction(1);
  retired.executed = true;
  retired.completed = true;
  cpu.ROB = {retired, ready_instruction(2)};
  cpu.RETIRE_WIDTH = champsim::bandwidth::maximum_type{1};
  CHECK(cpu.operate() == 2);
  REQUIRE(cpu.ROB.size() == 1);
  CHECK(cpu.num_retired == 1);
  CHECK(cpu.ROB.front().instr_id == 2);
  CHECK(cpu.ROB.front().executed);
  CHECK_FALSE(cpu.ROB.front().completed);
}

TEST_CASE_METHOD(rob_fixture, "Execution latency and completion ordering survive warmup transitions")
{
  cpu.warmup = GENERATE(false, true);
  const auto starting_warmup = cpu.warmup;
  cpu.ROB = {blocked_memory_instruction(1), ready_instruction(2)};
  CHECK(cpu.operate() == 1);
  const auto first_ready = cpu.current_time + (starting_warmup ? champsim::chrono::clock::duration{} : cpu.EXEC_LATENCY);
  CHECK(cpu.ROB[1].ready_time == first_ready);
  CHECK_FALSE(cpu.ROB[1].completed);
  cpu.warmup = !starting_warmup;
  cpu.ROB.push_back(ready_instruction(3));
  cpu.operate();
  CHECK(cpu.ROB[1].completed == starting_warmup);
  CHECK(cpu.ROB[1].ready_time == first_ready);
  CHECK(cpu.ROB[2].executed);
  CHECK_FALSE(cpu.ROB[2].completed);
  CHECK(cpu.ROB[2].ready_time == cpu.current_time + (cpu.warmup ? champsim::chrono::clock::duration{} : cpu.EXEC_LATENCY));
  cpu.current_time += cpu.EXEC_LATENCY;
  cpu.operate();
  CHECK(cpu.ROB[1].completed);
  CHECK(cpu.ROB[2].completed);
}

TEST_CASE_METHOD(rob_fixture, "ROB execution timestamps only the owning LSQ entries through operate")
{
  cpu.warmup = GENERATE(false, true);
  cpu.LQ_WIDTH = champsim::bandwidth::maximum_type{0};
  cpu.SQ_WIDTH = champsim::bandwidth::maximum_type{0};
  auto target = ready_instruction(2);
  target.source_memory = {champsim::address{0x2000}};
  target.destination_memory = {champsim::address{0x3000}};
  auto other = ready_instruction(3);
  other.scheduled = false;
  other.ready_time = champsim::chrono::clock::time_point::max();
  other.source_memory = {champsim::address{0x4000}};
  other.destination_memory = {champsim::address{0x5000}};
  cpu.do_memory_scheduling(target);
  cpu.do_memory_scheduling(other);
  const auto require_lsq_owners = [this] {
    for (uint64_t id : {2u, 3u}) {
      REQUIRE(std::count_if(cpu.LQ.begin(), cpu.LQ.end(), [id](const auto& entry) { return entry && entry->instr_id == id; }) == 1);
      REQUIRE(std::count_if(cpu.SQ.begin(), cpu.SQ.end(), [id](const auto& entry) { return entry.instr_id == id; }) == 1);
    }
  };
  require_lsq_owners();
  cpu.ROB = {blocked_memory_instruction(1), target, other};
  CHECK(cpu.operate() == 1);
  require_lsq_owners();
  const auto ready = cpu.current_time + (cpu.warmup ? champsim::chrono::clock::duration{} : cpu.EXEC_LATENCY);
  CHECK(cpu.ROB[1].ready_time == ready);
  CHECK_FALSE(cpu.ROB[1].completed);
  for (const auto& entry : cpu.LQ) {
    if (entry) {
      CHECK(entry->ready_time == (entry->instr_id == 2 ? ready : champsim::chrono::clock::time_point::max()));
    }
  }
  for (const auto& entry : cpu.SQ) {
    CHECK(entry.ready_time == (entry.instr_id == 2 ? ready : champsim::chrono::clock::time_point::max()));
  }
}
