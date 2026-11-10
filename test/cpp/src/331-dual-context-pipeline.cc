#include <catch.hpp>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <vector>

#include "address.h"
#include "cache.h"
#include "cache_builder.h"
#include "channel.h"
#include "instr.h"
#include "mocks.hpp"
#include "ooo_cpu.h"

// The live-sibling paths that a single-context replay cannot
// touch. Each case drives both hardware contexts at once and asserts the
// per-context architectural result, so a shared structure that silently served
// only one context (or one context that leaked into the other's pipeline) fails
// here rather than passing on a replay against an empty sibling.
//
//   (1) a store behind its unretired head drains to L1D only once its own
//       context retires, under neither back-end round-robin start;
//   (2) an L1D release carrying one address space completes only the load that
//       lives in that address space, leaving the sibling's same-address load in
//       flight;
//   (3) a returned L1I line naming one instr_id completes only that fetch, not
//       the sibling's same-block fetch under a different instr_id;
//   (4) with warmup off, a misprediction freezes only the mispredicting
//       context's fetch while its sibling keeps fetching and retiring;
//   (5) two operate() calls let both contexts fetch, dispatch and retire.

namespace
{
struct dual_pipeline_fixture {
  release_MRC l1i, l1d;
  O3_CPU dual;

  dual_pipeline_fixture()
      : dual{champsim::core_builder{}
                 .fetch_queues(&l1i.queues)
                 .data_queues(&l1d.queues)
                 .num_contexts(2)
                 .ifetch_buffer_size(16)
                 .decode_buffer_size(16)
                 .dib_hit_buffer_size(16)
                 .dispatch_buffer_size(16)
                 .rob_size(16)
                 .lq_size(16)
                 .sq_size(16)
                 .register_file_size(128)
                 .fetch_width(champsim::bandwidth::maximum_type{1})
                 .decode_width(champsim::bandwidth::maximum_type{8})
                 .dib_inorder_width(champsim::bandwidth::maximum_type{8})
                 .dispatch_width(champsim::bandwidth::maximum_type{8})
                 .schedule_width(champsim::bandwidth::maximum_type{8})
                 .execute_width(champsim::bandwidth::maximum_type{8})
                 .lq_width(champsim::bandwidth::maximum_type{8})
                 .sq_width(champsim::bandwidth::maximum_type{8})
                 .retire_width(champsim::bandwidth::maximum_type{8})
                 .l1i_bandwidth(champsim::bandwidth::maximum_type{8})
                 .l1d_bandwidth(champsim::bandwidth::maximum_type{8})
                 .execute_latency(0)
                 .schedule_latency(0)}
  {
    dual.warmup = true;
    dual.current_time = champsim::chrono::clock::time_point{champsim::chrono::picoseconds{1000}};
  }
};

// A completed, no-memory instruction already sitting in a context's structures.
ooo_model_instr completed_nop(uint64_t id)
{
  auto instr = champsim::test::instruction_with_ip(4 * id);
  instr.instr_id = id;
  instr.executed = true;
  instr.completed = true;
  return instr;
}

// A not-yet-executed, no-memory instruction ready to be acted on this cycle.
ooo_model_instr ready_nop(uint64_t id)
{
  auto instr = champsim::test::instruction_with_ip(4 * id);
  instr.instr_id = id;
  instr.ready_time = champsim::chrono::clock::time_point::min();
  return instr;
}
} // namespace

TEST_CASE_METHOD(dual_pipeline_fixture, "A store behind its unretired head drains only once its context retires")
{
  // Context 0's ROB is a completed head (id 10) followed by a completed store
  // (id 11) whose data sits in the store queue already issued. The store's SQ
  // entry is younger than the ROB front, so the drain gate (strictly older than
  // the ROB head) holds it back under either back-end round-robin start.
  auto head = completed_nop(10);
  auto store = completed_nop(11);
  store.destination_memory = {champsim::address{0x8000}};

  dual.contexts[0].ROB = {head, store};

  LSQ_ENTRY sq_entry{champsim::address{0x8000}, 11, champsim::address{0x1000}, champsim::address_space_id{}};
  sq_entry.fetch_issued = true; // already translated/issued; only the drain phase remains
  sq_entry.ready_time = champsim::chrono::clock::time_point::min();
  dual.contexts[0].SQ.push_back(sq_entry);

  dual.backend_round_robin = 0;
  dual.operate_lsq();
  CHECK(dual.contexts[0].SQ.size() == 1); // the drain gate is not the round-robin start

  dual.backend_round_robin = 1;
  dual.operate_lsq();
  CHECK(dual.contexts[0].SQ.size() == 1); // nor the other start
  CHECK(l1d.queues.WQ.empty());

  dual.retire_rob(); // retires both the head and the store
  CHECK(dual.contexts[0].num_retired == 2);
  CHECK(dual.contexts[0].ROB.empty());

  dual.operate_lsq(); // now the ROB head is gone, so the store's data drains
  CHECK(dual.contexts[0].SQ.empty());
  CHECK(l1d.queues.WQ.size() == 1);
}

TEST_CASE_METHOD(dual_pipeline_fixture, "An L1D release completes only the load in the released address space")
{
  // One load address (0x4000) in two address spaces. Each release names one
  // address space and must complete only the load that lives there.
  auto load0 = champsim::test::instruction_with_ip_and_asid(0x2000, champsim::address_space_id{1, 1});
  load0.instr_id = 10;
  load0.source_memory = {champsim::address{0x4000}};
  load0.scheduled = true;
  load0.executed = true;
  load0.completed_mem_ops = 0;
  load0.ready_time = champsim::chrono::clock::time_point::min();

  auto load1 = champsim::test::instruction_with_ip_and_asid(0x3000, champsim::address_space_id{2, 2});
  load1.instr_id = 20;
  load1.source_memory = {champsim::address{0x4000}};
  load1.scheduled = true;
  load1.executed = true;
  load1.completed_mem_ops = 0;
  load1.ready_time = champsim::chrono::clock::time_point::min();

  dual.contexts[0].ROB = {load0};
  dual.contexts[1].ROB = {load1};

  dual.contexts[0].LQ[0].emplace(champsim::address{0x4000}, 10, champsim::address{0x2000}, champsim::address_space_id{1, 1});
  dual.contexts[0].LQ[0]->ready_time = champsim::chrono::clock::time_point::min();
  dual.contexts[1].LQ[0].emplace(champsim::address{0x4000}, 20, champsim::address{0x3000}, champsim::address_space_id{2, 2});
  dual.contexts[1].LQ[0]->ready_time = champsim::chrono::clock::time_point::min();

  dual.backend_round_robin = 0;
  dual.operate_lsq(); // issues both loads to the L1D

  CHECK(dual.contexts[0].LQ[0]->fetch_issued);
  CHECK(dual.contexts[1].LQ[0]->fetch_issued);

  l1d.operate();
  l1d.release(champsim::address{0x4000}, champsim::address_space_id{1, 1});
  dual.handle_memory_return();

  CHECK_FALSE(dual.contexts[0].LQ[0].has_value()); // context 0's load completed
  CHECK(dual.contexts[1].LQ[0].has_value());       // context 1's identical-address load did not
  CHECK(dual.contexts[0].ROB.front().completed_mem_ops == 1);
  CHECK(dual.contexts[1].ROB.front().completed_mem_ops == 0);

  l1d.release(champsim::address{0x4000}, champsim::address_space_id{2, 2});
  dual.handle_memory_return();

  CHECK_FALSE(dual.contexts[1].LQ[0].has_value()); // now context 1's load completes
  CHECK(dual.contexts[1].ROB.front().completed_mem_ops == 1);
}

TEST_CASE_METHOD(dual_pipeline_fixture, "A returned L1I line completes only the fetch it names, not a sibling's same-block fetch")
{
  // Context 1 holds fetch id 107 and context 0 holds fetch id 7, both in block
  // 0x8000 and address space {2,2}. A returned line naming only id 107 must
  // complete id 107 and leave id 7 in flight: L1I completion is keyed by
  // instr_id, not by a block+asid broadcast like L1D.
  auto fetch1 = champsim::test::instruction_with_ip_and_asid(0x8000, champsim::address_space_id{2, 2});
  fetch1.instr_id = 107;
  fetch1.fetch_issued = true;
  dual.contexts[1].IFETCH_BUFFER.push_back(fetch1);

  auto fetch0 = champsim::test::instruction_with_ip_and_asid(0x8000, champsim::address_space_id{2, 2});
  fetch0.instr_id = 7;
  fetch0.fetch_issued = true;
  dual.contexts[0].IFETCH_BUFFER.push_back(fetch0);

  CHECK_FALSE(dual.contexts[1].IFETCH_BUFFER.front().fetch_completed);
  CHECK_FALSE(dual.contexts[0].IFETCH_BUFFER.front().fetch_completed);

  l1i.queues.returned.push_back(champsim::channel::response_type{champsim::address{0x8000}, champsim::address{0x8000}, champsim::address{}, 0, 0, 2, 2, {107}});
  dual.handle_memory_return();

  CHECK(dual.contexts[1].IFETCH_BUFFER.front().fetch_completed);
  CHECK_FALSE(dual.contexts[0].IFETCH_BUFFER.front().fetch_completed);
  CHECK(l1i.queues.returned.empty());
}

namespace
{
// Case (4) drives real fetch with warmup off, so it needs its own fixture: a
// do_nothing_MRC for the fetch/data buses plus a real one-set CACHE standing in
// as l1i, because a fetched branch calls l1i's prefetcher branch hook and a
// null l1i would dereference null.
struct mispredict_fixture {
  do_nothing_MRC l1i_mrc, l1d_mrc;
  CACHE branch_l1i{champsim::cache_builder{}.sets(1).ways(1)};
  O3_CPU dual{champsim::core_builder{}
                  .fetch_queues(&l1i_mrc.queues)
                  .data_queues(&l1d_mrc.queues)
                  .l1i(&branch_l1i)
                  .num_contexts(2)
                  .ifetch_buffer_size(16)
                  .decode_buffer_size(16)
                  .dib_hit_buffer_size(16)
                  .dispatch_buffer_size(16)
                  .rob_size(64)
                  .lq_size(16)
                  .sq_size(16)
                  .register_file_size(128)
                  .fetch_width(champsim::bandwidth::maximum_type{1})
                  .mispredict_penalty(8)
                  .execute_latency(0)
                  .schedule_latency(0)};

  mispredict_fixture() { dual.warmup = false; }
};

ooo_model_instr make_branch(uint64_t ip, branch_type type, bool taken, uint64_t target)
{
  auto instr = champsim::test::instruction_with_ip(ip);
  instr.is_branch = true;
  instr.branch_taken = taken;
  instr.branch = type;
  instr.branch_target = champsim::address{target};
  return instr;
}
} // namespace

TEST_CASE_METHOD(mispredict_fixture, "A misprediction freezes only the mispredicting context's fetch")
{
  // Context 1 fetches a conditional branch and then a direct jump, both to
  // 0x9000. With no predictor or BTB the builder leaves both target-predicted to
  // nothing, so both mispredict; warmup is off, so each misprediction freezes
  // context 1's fetch. Context 0 fetches forty branch-free instructions and must
  // keep making progress through the freeze.
  auto branch_seq = [](uint64_t id, uint64_t ip) {
    auto instr = champsim::test::instruction_with_ip(ip);
    instr.instr_id = id;
    return instr;
  };

  dual.contexts[1].input_queue.push_back(make_branch(0x8000, BRANCH_CONDITIONAL, true, 0x9000));
  dual.contexts[1].input_queue.back().instr_id = 0;
  dual.contexts[1].input_queue.push_back(make_branch(0x8100, BRANCH_DIRECT_JUMP, true, 0x9000));
  dual.contexts[1].input_queue.back().instr_id = 1;
  for (uint64_t i = 0; i < 3; ++i) {
    dual.contexts[1].input_queue.push_back(branch_seq(2 + i, 0x8200 + 4 * i));
  }
  for (uint64_t i = 0; i < 40; ++i)
    dual.contexts[0].input_queue.push_back(branch_seq(i, 0x9000 + 4 * i));

  bool ctx1_hit_max_first10 = false;
  bool ctx0_ever_max = false;
  bool ctx0_fetched_while_stalled = false;
  bool freeze_started = false;
  std::size_t ctx0_iq_at_freeze = 0;

  const auto max_time = champsim::chrono::clock::time_point::max();
  for (int i = 0; i < 400; ++i) {
    dual._operate();
    l1i_mrc._operate();
    l1d_mrc._operate();

    if (i < 10 && dual.contexts[1].fetch_resume_time == max_time)
      ctx1_hit_max_first10 = true;

    if (dual.contexts[0].fetch_resume_time == max_time)
      ctx0_ever_max = true;

    const bool frozen = (dual.contexts[1].fetch_resume_time == max_time);
    if (frozen && !freeze_started) {
      freeze_started = true;
      ctx0_iq_at_freeze = dual.contexts[0].input_queue.size();
    }
    if (frozen && dual.contexts[0].input_queue.size() < ctx0_iq_at_freeze)
      ctx0_fetched_while_stalled = true;

    if (dual.contexts[0].num_retired == 40 && dual.contexts[1].num_retired == 5)
      break;
  }

  CHECK(ctx1_hit_max_first10);       // context 1's fetch froze almost immediately
  CHECK_FALSE(ctx0_ever_max);        // context 0's fetch never froze
  CHECK(ctx0_fetched_while_stalled); // context 0 kept fetching while context 1 was frozen

  CHECK(dual.contexts[1].num_retired == 5);
  CHECK(dual.contexts[0].num_retired == 40);
  CHECK(dual.contexts[1].sim_stats.cycles_on_wrong_path >= 8);
  CHECK(dual.contexts[0].sim_stats.cycles_on_wrong_path == 0);
  CHECK(dual.contexts[1].ROB.empty());
  CHECK(dual.contexts[0].ROB.empty());
  CHECK(dual.contexts[1].input_queue.empty());
  CHECK(dual.contexts[0].input_queue.empty());
}

TEST_CASE_METHOD(dual_pipeline_fixture, "Two operate() calls let both contexts fetch, dispatch and retire")
{
  // Pre-stage one instruction per context in each of three structures: a
  // completed ROB entry to retire, a ready DISPATCH_BUFFER entry to dispatch,
  // and an input-queue entry to fetch. One operate() call arbitrates to a single
  // owner for dispatch and a single round-robin start for fetch; two calls cover
  // both contexts.
  dual.contexts[0].ROB = {completed_nop(2)};
  dual.contexts[0].DISPATCH_BUFFER = {ready_nop(1)};
  dual.contexts[0].input_queue = {ready_nop(0)};

  dual.contexts[1].ROB = {completed_nop(102)};
  dual.contexts[1].DISPATCH_BUFFER = {ready_nop(101)};
  dual.contexts[1].input_queue = {ready_nop(100)};

  dual.fetch_round_robin = 0;
  dual.dispatch_owner = 0;
  dual.backend_round_robin = 0;

  dual.operate();
  dual.operate();

  CHECK(dual.contexts[0].num_retired == 1);
  CHECK(dual.contexts[1].num_retired == 1);
  CHECK(dual.contexts[0].input_queue.empty());
  CHECK(dual.contexts[1].input_queue.empty());
  CHECK(dual.contexts[0].IFETCH_BUFFER.size() == 1);
  CHECK(dual.contexts[1].IFETCH_BUFFER.size() == 1);
  CHECK(dual.contexts[0].ROB.size() == 1);
  CHECK(dual.contexts[1].ROB.size() == 1);
  CHECK(dual.contexts[0].DISPATCH_BUFFER.empty());
  CHECK(dual.contexts[1].DISPATCH_BUFFER.empty());
  CHECK(dual.next_dispatch_sequence == 2);
}
