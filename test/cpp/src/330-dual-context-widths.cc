#include <algorithm>
#include <catch.hpp>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

#include "address.h"
#include "instr.h"
#include "mocks.hpp"
#include "ooo_cpu.h"

// The pipeline widths are one shared per-core budget, not a per-context
// budget. Each case backlogs both contexts past one width and makes a single call;
// the call must move exactly the width in total, so a width that silently became
// per-context (each context getting the full budget) would move twice as much and
// fail here.

namespace
{
constexpr long W_FETCH = 4;  // fetch and promotion share FETCH_WIDTH
constexpr long W_L1I = 3;    // L1I issue
constexpr long W_DECODE = 2; // decode of not-yet-decoded instructions
constexpr long W_DIB = 5;    // the already-decoded DIB-hit route
constexpr long W_SQ = 2;     // store issue
constexpr long W_LQ = 4;     // load issue
constexpr long W_EXEC = 3;   // execution completion

struct dual_width_fixture {
  do_nothing_MRC l1i, l1d;
  O3_CPU dual;

  dual_width_fixture()
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
                 .fetch_width(champsim::bandwidth::maximum_type{W_FETCH})
                 .decode_width(champsim::bandwidth::maximum_type{W_DECODE})
                 .dib_inorder_width(champsim::bandwidth::maximum_type{W_DIB})
                 .execute_width(champsim::bandwidth::maximum_type{W_EXEC})
                 .lq_width(champsim::bandwidth::maximum_type{W_LQ})
                 .sq_width(champsim::bandwidth::maximum_type{W_SQ})
                 .dispatch_width(champsim::bandwidth::maximum_type{64})
                 .schedule_width(champsim::bandwidth::maximum_type{64})
                 .retire_width(champsim::bandwidth::maximum_type{64})
                 .l1i_bandwidth(champsim::bandwidth::maximum_type{W_L1I})
                 .execute_latency(0)
                 .schedule_latency(0)}
  {
    dual.warmup = true;
    dual.current_time = champsim::chrono::clock::time_point{champsim::chrono::picoseconds{1000}};
  }
};
} // namespace

TEST_CASE_METHOD(dual_width_fixture, "Fetch moves FETCH_WIDTH instructions total across both contexts")
{
  for (std::size_t ctx : {0u, 1u})
    for (int i = 0; i < 8; ++i)
      dual.contexts[ctx].input_queue.push_back(champsim::test::instruction_with_ip(0x1000 * (ctx + 1) + 4 * i));

  dual.fetch_round_robin = 0;
  dual.initialize_instruction();

  const auto total = dual.contexts[0].IFETCH_BUFFER.size() + dual.contexts[1].IFETCH_BUFFER.size();
  CHECK(total == W_FETCH);
  CHECK(dual.contexts[1].IFETCH_BUFFER.empty()); // one shared budget, not one per context
}

TEST_CASE_METHOD(dual_width_fixture, "L1I issue sends L1I_BANDWIDTH requests total across both contexts")
{
  for (std::size_t ctx : {0u, 1u})
    for (int i = 0; i < 4; ++i) {
      auto instr = champsim::test::instruction_with_ip(0x1000 * (ctx + 1) + 0x40 * i); // one block per request
      instr.dib_checked = true;
      dual.contexts[ctx].IFETCH_BUFFER.push_back(std::move(instr));
    }

  dual.fetch_round_robin = 0;
  CHECK(dual.fetch_instruction() == W_L1I);
}

TEST_CASE_METHOD(dual_width_fixture, "Promotion moves FETCH_WIDTH instructions total across both contexts")
{
  for (std::size_t ctx : {0u, 1u})
    for (int i = 0; i < 8; ++i) {
      auto instr = champsim::test::instruction_with_ip(0x3000 * (ctx + 1) + 4 * i);
      instr.fetch_completed = true;
      instr.ready_time = champsim::chrono::clock::time_point::min();
      dual.contexts[ctx].IFETCH_BUFFER.push_back(std::move(instr));
    }

  dual.fetch_round_robin = 0;
  CHECK(dual.promote_to_decode() == W_FETCH);
}

TEST_CASE_METHOD(dual_width_fixture, "Decode moves DECODE_WIDTH instructions total across both contexts")
{
  for (std::size_t ctx : {0u, 1u})
    for (int i = 0; i < 8; ++i) {
      auto instr = champsim::test::instruction_with_ip(0x5000 * (ctx + 1) + 4 * i);
      instr.ready_time = champsim::chrono::clock::time_point::min();
      dual.contexts[ctx].DECODE_BUFFER.push_back(std::move(instr));
    }

  dual.fetch_round_robin = 0;
  CHECK(dual.decode_instruction() == W_DECODE);
}

TEST_CASE_METHOD(dual_width_fixture, "The DIB-hit route moves DIB_INORDER_WIDTH instructions total across both contexts")
{
  for (std::size_t ctx : {0u, 1u})
    for (int i = 0; i < 8; ++i) {
      auto instr = champsim::test::instruction_with_ip(0x7000 * (ctx + 1) + 4 * i);
      instr.decoded = true;
      instr.ready_time = champsim::chrono::clock::time_point::min();
      dual.contexts[ctx].DIB_HIT_BUFFER.push_back(std::move(instr));
    }

  dual.fetch_round_robin = 0;
  CHECK(dual.decode_instruction() == W_DIB);
}

TEST_CASE_METHOD(dual_width_fixture, "Store issue writes SQ_WIDTH stores total across both contexts")
{
  auto store = [](uint64_t id) {
    LSQ_ENTRY e{champsim::address{0x8000 + 8 * id}, id, champsim::address{0x1000}, champsim::address_space_id{}};
    e.fetch_issued = true; // already forwarded; only the issue-to-L1D phase remains
    e.ready_time = champsim::chrono::clock::time_point::min();
    return e;
  };

  for (std::size_t ctx : {0u, 1u})
    for (uint64_t i = 0; i < 4; ++i)
      dual.contexts[ctx].SQ.push_back(store(100 * ctx + i));

  dual.backend_round_robin = 0;
  CHECK(dual.operate_lsq() == W_SQ);
}

TEST_CASE_METHOD(dual_width_fixture, "Load issue reads LQ_WIDTH loads total across both contexts")
{
  auto load = [](uint64_t id) {
    LSQ_ENTRY e{champsim::address{0x1000 + 8 * id}, id, champsim::address{0x2000}, champsim::address_space_id{}};
    e.ready_time = champsim::chrono::clock::time_point::min();
    return e;
  };

  for (std::size_t ctx : {0u, 1u})
    for (uint64_t i = 0; i < 4; ++i)
      dual.contexts[ctx].LQ[static_cast<std::size_t>(i)].emplace(load(100 * ctx + i));

  dual.backend_round_robin = 0;
  CHECK(dual.operate_lsq() == W_LQ);
}

TEST_CASE_METHOD(dual_width_fixture, "Completion finalizes EXEC_WIDTH instructions total across both contexts")
{
  auto done = [](uint64_t id, uint64_t seq) {
    auto instr = champsim::test::instruction_with_ip(id * 4);
    instr.instr_id = id;
    instr.executed = true;
    instr.dispatch_sequence = seq;
    instr.ready_time = champsim::chrono::clock::time_point::min();
    return instr;
  };

  for (uint64_t i = 0; i < 8; ++i) {
    dual.contexts[0].ROB.push_back(done(i, i));
    dual.contexts[1].ROB.push_back(done(100 + i, 100 + i));
  }

  dual.backend_round_robin = 0;
  dual.complete_inflight_instruction();

  const auto completed = [](const auto& c) {
    return std::count_if(std::begin(c.ROB), std::end(c.ROB), [](const auto& i) { return i.completed; });
  };
  CHECK(completed(dual.contexts[0]) + completed(dual.contexts[1]) == W_EXEC);
  CHECK(completed(dual.contexts[1]) == 0); // all slots went to the older context
}
