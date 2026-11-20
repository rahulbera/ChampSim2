#include <algorithm>
#include <array>
#include <catch.hpp>
#include <cstddef>
#include <cstdint>

#include "instr.h"
#include "mocks.hpp"
#include "ooo_cpu.h"

// The shared scheduler gates on O3_CPU::scheduler_occupancy, a running count of
// instructions scheduled and not yet executed that do_scheduling and
// do_execution keep, instead of walking every context's ROB each cycle. The
// first case drives two contexts through schedule, execute, complete and retire
// and compares the count with count_scheduled_not_executed() after every stage.
// The second pins the two-context execute and complete stages to global age when
// they take a full width. With one context those stages walk the ROB once
// instead of running the global-age loop; the third case pins that a core with
// one context issues and completes what a two-context core with an idle sibling
// does.

namespace
{
struct occupancy_fixture {
  do_nothing_MRC solo_l1i, solo_l1d;
  O3_CPU solo{champsim::core_builder{}
                  .fetch_queues(&solo_l1i.queues)
                  .data_queues(&solo_l1d.queues)
                  .num_contexts(1)
                  .rob_size(16)
                  .register_file_size(128)
                  .lq_size(8)
                  .sq_size(8)
                  .schedule_width(champsim::bandwidth::maximum_type{3})
                  .execute_width(champsim::bandwidth::maximum_type{2})
                  .retire_width(champsim::bandwidth::maximum_type{4})
                  .execute_latency(0)
                  .schedule_latency(0)};

  do_nothing_MRC l1i, l1d;
  O3_CPU dual{champsim::core_builder{}
                  .fetch_queues(&l1i.queues)
                  .data_queues(&l1d.queues)
                  .num_contexts(2)
                  .rob_size(16)
                  .register_file_size(128)
                  .lq_size(8)
                  .sq_size(8)
                  .ifetch_buffer_size(2)
                  .decode_buffer_size(2)
                  .dib_hit_buffer_size(2)
                  .dispatch_buffer_size(2)
                  .schedule_width(champsim::bandwidth::maximum_type{3})
                  .execute_width(champsim::bandwidth::maximum_type{2})
                  .retire_width(champsim::bandwidth::maximum_type{4})
                  .execute_latency(0)
                  .schedule_latency(0)};

  occupancy_fixture()
  {
    for (auto* cpu : {&solo, &dual}) {
      cpu->warmup = true;
      cpu->current_time = champsim::chrono::clock::time_point{champsim::chrono::picoseconds{1000}};
    }
  }

  // A dispatched, not yet scheduled instruction with the given core-wide age.
  // It writes one architectural register, so scheduling renames it and
  // retirement frees the previous mapping.
  static ooo_model_instr dispatched(uint64_t id, uint64_t seq, uint8_t reg)
  {
    auto instr = champsim::test::instruction_with_ip(id * 4);
    instr.instr_id = id;
    instr.dispatch_sequence = seq;
    instr.destination_registers = {static_cast<PHYSICAL_REGISTER_ID>(reg)};
    instr.ready_time = champsim::chrono::clock::time_point::min();
    return instr;
  }

  const ooo_model_instr& by_seq(uint64_t seq) const
  {
    for (const auto& c : dual.contexts)
      for (const auto& instr : c.ROB)
        if (instr.dispatch_sequence == seq)
          return instr;
    FAIL("no ROB entry with dispatch_sequence " << seq);
    return dual.contexts.front().ROB.front();
  }
};
} // namespace

TEST_CASE_METHOD(occupancy_fixture, "The shared scheduler's running occupancy equals a recount across schedule, execute, complete and retire")
{
  // Five instructions per context, ages interleaved, against a three-entry
  // reservation station and two issue slots: the cap binds on the first cycles.
  for (uint64_t i = 0; i < 5; ++i) {
    dual.contexts[0].ROB.push_back(dispatched(i, 2 * i, static_cast<uint8_t>(i + 1)));
    dual.contexts[1].ROB.push_back(dispatched(100 + i, 2 * i + 1, static_cast<uint8_t>(i + 1)));
  }
  REQUIRE(dual.scheduler_occupancy == 0);

  bool cap_reached = false;
  for (int cycle = 0; cycle < 20; ++cycle) {
    CAPTURE(cycle);
    dual.retire_rob();
    CHECK(dual.scheduler_occupancy == dual.count_scheduled_not_executed());
    dual.complete_inflight_instruction();
    CHECK(dual.scheduler_occupancy == dual.count_scheduled_not_executed());
    dual.execute_instruction();
    CHECK(dual.scheduler_occupancy == dual.count_scheduled_not_executed());
    dual.schedule_instruction();
    CHECK(dual.scheduler_occupancy == dual.count_scheduled_not_executed());
    CHECK(dual.scheduler_occupancy <= 3);
    cap_reached = cap_reached || dual.scheduler_occupancy == 3;
    dual.backend_round_robin = (dual.backend_round_robin + 1) % dual.contexts.size();
  }

  CHECK(cap_reached);
  CHECK(dual.contexts[0].num_retired == 5);
  CHECK(dual.contexts[1].num_retired == 5);
  CHECK(dual.scheduler_occupancy == 0);
}

TEST_CASE_METHOD(occupancy_fixture, "Two-context execute and complete take a full width by global age")
{
  // Ages 0, 1, 4 in context 0 and 2, 3, 5 in context 1. Age 3 reads a register
  // its context has in flight, so it cannot issue; age 1 waits on a memory
  // operand, so it can issue but not complete. Each blocked entry sits right
  // after one the stage takes from the same context and is older than the
  // sibling's next candidate, so a stage that took the next entry without
  // testing it would take the blocked one.
  for (uint64_t seq : {0u, 1u, 4u})
    dual.contexts[0].ROB.push_back(dispatched(seq, seq, static_cast<uint8_t>(seq + 1)));
  for (uint64_t seq : {2u, 3u, 5u})
    dual.contexts[1].ROB.push_back(dispatched(seq, seq, static_cast<uint8_t>(seq + 1)));
  dual.contexts[1].rename_dest_register(9, 999); // register 9 is in flight in context 1
  dual.contexts[1].ROB[1].source_registers = {9};
  dual.contexts[0].ROB[1].source_memory = {champsim::address{0x4000}};

  dual.SCHEDULER_SIZE = champsim::bandwidth::maximum_type{8};
  REQUIRE(dual.schedule_instruction() == 6);
  REQUIRE(dual.scheduler_occupancy == 6);

  // Four issue slots, six scheduled instructions, one not ready: the four
  // oldest ready ones issue, three from context 0 and one from context 1.
  dual.EXEC_WIDTH = champsim::bandwidth::maximum_type{4};
  CHECK(dual.execute_instruction() == 4);
  for (uint64_t seq : {0u, 1u, 2u, 4u}) {
    CAPTURE(seq);
    CHECK(by_seq(seq).executed);
  }
  CHECK_FALSE(by_seq(3).executed);
  CHECK_FALSE(by_seq(5).executed);
  CHECK(dual.scheduler_occupancy == 2);
  CHECK(dual.scheduler_occupancy == dual.count_scheduled_not_executed());

  // Two writeback slots, three completable instructions (0, 2, 4): the two
  // oldest complete, and age 1's memory operand keeps it from completing.
  dual.EXEC_WIDTH = champsim::bandwidth::maximum_type{2};
  CHECK(dual.complete_inflight_instruction() == 2);
  CHECK(by_seq(0).completed);
  CHECK(by_seq(2).completed);
  CHECK_FALSE(by_seq(1).completed);
  CHECK_FALSE(by_seq(4).completed);
}

TEST_CASE_METHOD(occupancy_fixture, "A one-context core executes and completes what a two-context core with an idle sibling does")
{
  // Ages 0 to 5 in context 0: age 1 waits on a memory operand and age 2 reads a
  // register in flight. The one-context core walks its ROB once per stage; the
  // two-context core runs the global-age loop over a busy and an idle context.
  for (auto* cpu : {&solo, &dual}) {
    for (uint64_t seq = 0; seq < 6; ++seq)
      cpu->contexts[0].ROB.push_back(dispatched(seq, seq, static_cast<uint8_t>(seq + 1)));
    cpu->contexts[0].rename_dest_register(9, 999);
    cpu->contexts[0].ROB[2].source_registers = {9};
    cpu->contexts[0].ROB[1].source_memory = {champsim::address{0x4000}};
    cpu->SCHEDULER_SIZE = champsim::bandwidth::maximum_type{8};
    REQUIRE(cpu->schedule_instruction() == 6);

    cpu->EXEC_WIDTH = champsim::bandwidth::maximum_type{4};
    CHECK(cpu->execute_instruction() == 4);
    CHECK(cpu->scheduler_occupancy == 2);
    cpu->EXEC_WIDTH = champsim::bandwidth::maximum_type{2};
    CHECK(cpu->complete_inflight_instruction() == 2);
  }

  constexpr std::array<bool, 6> executed{true, true, false, true, true, false};
  constexpr std::array<bool, 6> completed{true, false, false, true, false, false};
  REQUIRE(solo.contexts[0].ROB.size() == 6);
  REQUIRE(dual.contexts[0].ROB.size() == 6);
  for (std::size_t i = 0; i < 6; ++i) {
    CAPTURE(i);
    CHECK(solo.contexts[0].ROB[i].executed == executed[i]);
    CHECK(solo.contexts[0].ROB[i].completed == completed[i]);
    CHECK(dual.contexts[0].ROB[i].executed == executed[i]);
    CHECK(dual.contexts[0].ROB[i].completed == completed[i]);
  }
}
