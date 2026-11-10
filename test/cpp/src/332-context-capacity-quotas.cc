#include <algorithm>
#include <catch.hpp>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

#include "address.h"
#include "defs.h"
#include "instr.h"
#include "mocks.hpp"
#include "ooo_cpu.h"

// Capacity quotas. The builder still names whole-core totals
// (rob_size, lq_size, ...); O3_CPU::make_quota divides each by the context count
// and refuses a total that does not divide equally, and the pipeline gates read
// the per-context quota rather than the whole-core *_SIZE constant. The one DIB
// is shared, so it is sized on the core, not per context.
//
// The file pins four things: (1) the quota equals the total at one context and
// half of it at two; (2) a total that does not divide by the context count, and
// a context count outside 1..max_contexts, are refused naming the offending key;
// (3) a full backlog fills each context's ROB, LQ and SQ -- and its front-end
// buffers -- to exactly its quota, not the total; and (4) the single 1x1 DIB is
// shared: a fill in one context hits in the other within one asid, and misses
// across asids.

TEST_CASE("A one-context core keeps the whole size as its quota; two split it equally")
{
  do_nothing_MRC l1i, l1d;
  O3_CPU one{champsim::core_builder{}
                 .fetch_queues(&l1i.queues)
                 .data_queues(&l1d.queues)
                 .num_contexts(1)
                 .ifetch_buffer_size(4)
                 .decode_buffer_size(8)
                 .dib_hit_buffer_size(6)
                 .dispatch_buffer_size(10)
                 .rob_size(16)
                 .lq_size(12)
                 .sq_size(14)};

  REQUIRE(one.quota.ifetch_buffer == 4);
  REQUIRE(one.quota.decode_buffer == 8);
  REQUIRE(one.quota.dib_hit_buffer == 6);
  REQUIRE(one.quota.dispatch_buffer == 10);
  REQUIRE(one.quota.rob == 16);
  REQUIRE(one.quota.lq == 12);
  REQUIRE(one.quota.sq == 14);

  O3_CPU two{champsim::core_builder{}
                 .fetch_queues(&l1i.queues)
                 .data_queues(&l1d.queues)
                 .num_contexts(2)
                 .ifetch_buffer_size(4)
                 .decode_buffer_size(8)
                 .dib_hit_buffer_size(6)
                 .dispatch_buffer_size(10)
                 .rob_size(16)
                 .lq_size(12)
                 .sq_size(14)};

  REQUIRE(two.quota.ifetch_buffer == 2);
  REQUIRE(two.quota.decode_buffer == 4);
  REQUIRE(two.quota.dib_hit_buffer == 3);
  REQUIRE(two.quota.dispatch_buffer == 5);
  REQUIRE(two.quota.rob == 8);
  REQUIRE(two.quota.lq == 6);
  REQUIRE(two.quota.sq == 7);
}

TEST_CASE("An indivisible size is refused, naming the key")
{
  using Catch::Matchers::ContainsSubstring;
  do_nothing_MRC l1i, l1d;

  auto build = [&](std::size_t ifetch, std::size_t decode, std::size_t dib_hit, std::size_t dispatch, std::size_t rob, std::size_t lq, std::size_t sq) {
    return O3_CPU{champsim::core_builder{}
                      .fetch_queues(&l1i.queues)
                      .data_queues(&l1d.queues)
                      .num_contexts(2)
                      .ifetch_buffer_size(ifetch)
                      .decode_buffer_size(decode)
                      .dib_hit_buffer_size(dib_hit)
                      .dispatch_buffer_size(dispatch)
                      .rob_size(rob)
                      .lq_size(lq)
                      .sq_size(sq)};
  };

  REQUIRE_THROWS_WITH(build(3, 4, 4, 4, 4, 4, 4), ContainsSubstring("ifetch_buffer_size"));
  REQUIRE_THROWS_WITH(build(4, 3, 4, 4, 4, 4, 4), ContainsSubstring("decode_buffer_size"));
  REQUIRE_THROWS_WITH(build(4, 4, 3, 4, 4, 4, 4), ContainsSubstring("dib.hit_buffer_size"));
  REQUIRE_THROWS_WITH(build(4, 4, 4, 3, 4, 4, 4), ContainsSubstring("dispatch_buffer_size"));
  REQUIRE_THROWS_WITH(build(4, 4, 4, 4, 3, 4, 4), ContainsSubstring("rob_size"));
  REQUIRE_THROWS_WITH(build(4, 4, 4, 4, 4, 3, 4), ContainsSubstring("lq_size"));
  REQUIRE_THROWS_WITH(build(4, 4, 4, 4, 4, 4, 3), ContainsSubstring("sq_size"));
}

TEST_CASE("Zero contexts and more than max_contexts are refused")
{
  using Catch::Matchers::ContainsSubstring;
  do_nothing_MRC l1i, l1d;

  REQUIRE_THROWS_WITH(O3_CPU{champsim::core_builder{}.fetch_queues(&l1i.queues).data_queues(&l1d.queues).num_contexts(0)},
                      ContainsSubstring("hardware contexts requested"));
  REQUIRE_THROWS_WITH(O3_CPU{champsim::core_builder{}.fetch_queues(&l1i.queues).data_queues(&l1d.queues).num_contexts(champsim::defs::max_contexts + 1)},
                      ContainsSubstring("hardware contexts requested"));
}

TEST_CASE("A full backlog fills each context's ROB, LQ and SQ to exactly its quota")
{
  do_nothing_MRC l1i, l1d;
  O3_CPU dual{champsim::core_builder{}
                  .fetch_queues(&l1i.queues)
                  .data_queues(&l1d.queues)
                  .num_contexts(2)
                  .rob_size(8)
                  .lq_size(4)
                  .sq_size(4)
                  .ifetch_buffer_size(2)
                  .decode_buffer_size(2)
                  .dib_hit_buffer_size(2)
                  .dispatch_buffer_size(2)
                  .register_file_size(128)
                  .dispatch_width(champsim::bandwidth::maximum_type{64})
                  .execute_latency(0)
                  .schedule_latency(0)};
  dual.warmup = true;
  dual.current_time = champsim::chrono::clock::time_point{champsim::chrono::picoseconds{1000}};

  auto ready_instr = [](uint64_t id) {
    auto instr = champsim::test::instruction_with_ip(id * 4);
    instr.instr_id = id;
    instr.ready_time = champsim::chrono::clock::time_point::min();
    return instr;
  };

  auto dispatch_both = [&] {
    dual.dispatch_owner = 0;
    dual.dispatch_instruction();
    dual.dispatch_owner = 1;
    dual.dispatch_instruction();
  };

  auto lq_used = [&](std::size_t ctx) {
    return static_cast<std::size_t>(
        std::count_if(std::begin(dual.contexts[ctx].LQ), std::end(dual.contexts[ctx].LQ), [](const auto& e) { return e.has_value(); }));
  };

  // No memory operands: the ROB quota is what binds, not the LQ or SQ.
  for (std::size_t ctx : {0u, 1u})
    for (int i = 0; i < 8; ++i)
      dual.contexts[ctx].DISPATCH_BUFFER.push_back(ready_instr(100 * ctx + i));
  dispatch_both();
  REQUIRE(dual.contexts[0].ROB.size() == 4);
  REQUIRE(dual.contexts[1].ROB.size() == 4);
  REQUIRE(dual.contexts[0].DISPATCH_BUFFER.size() == 4); // the leftover proves the ROB bound it
  REQUIRE(dual.contexts[1].DISPATCH_BUFFER.size() == 4);

  // Loads fill the LQ to its quota; the ROB has room to spare.
  for (auto& c : dual.contexts) {
    c.ROB.clear();
    c.DISPATCH_BUFFER.clear();
    for (auto& e : c.LQ)
      e.reset();
  }
  for (std::size_t ctx : {0u, 1u})
    for (int i = 0; i < 4; ++i) {
      auto instr = ready_instr(200 * ctx + i);
      instr.source_memory = {champsim::address{0x4000 + 8 * (4 * ctx + i)}};
      dual.contexts[ctx].DISPATCH_BUFFER.push_back(std::move(instr));
    }
  dispatch_both();
  REQUIRE(lq_used(0) == 2);
  REQUIRE(lq_used(1) == 2);
  REQUIRE(dual.contexts[0].ROB.size() == 2); // the LQ, not the ROB, bound it
  REQUIRE(dual.contexts[1].ROB.size() == 2);

  // Stores fill the SQ to its quota; again the ROB has room to spare.
  for (auto& c : dual.contexts) {
    c.ROB.clear();
    c.DISPATCH_BUFFER.clear();
    c.SQ.clear();
    for (auto& e : c.LQ)
      e.reset();
  }
  for (std::size_t ctx : {0u, 1u})
    for (int i = 0; i < 4; ++i) {
      auto instr = ready_instr(300 * ctx + i);
      instr.destination_memory = {champsim::address{0x8000 + 8 * (4 * ctx + i)}};
      dual.contexts[ctx].DISPATCH_BUFFER.push_back(std::move(instr));
    }
  dispatch_both();
  REQUIRE(dual.contexts[0].SQ.size() == 2);
  REQUIRE(dual.contexts[1].SQ.size() == 2);
  REQUIRE(dual.contexts[0].ROB.size() == 2); // the SQ, not the ROB, bound it
  REQUIRE(dual.contexts[1].ROB.size() == 2);
}

TEST_CASE("Front-end buffers fill to exactly their per-context quota")
{
  do_nothing_MRC l1i, l1d;
  O3_CPU dual{champsim::core_builder{}
                  .fetch_queues(&l1i.queues)
                  .data_queues(&l1d.queues)
                  .num_contexts(2)
                  .ifetch_buffer_size(4)
                  .decode_buffer_size(4)
                  .dib_hit_buffer_size(4)
                  .dispatch_buffer_size(4)
                  .rob_size(8)
                  .lq_size(4)
                  .sq_size(4)
                  .register_file_size(128)
                  .fetch_width(champsim::bandwidth::maximum_type{64})
                  .decode_width(champsim::bandwidth::maximum_type{64})
                  .dib_inorder_width(champsim::bandwidth::maximum_type{64})};
  dual.warmup = true;
  dual.current_time = champsim::chrono::clock::time_point{champsim::chrono::picoseconds{1000}};

  // IFETCH: initialize_instruction stops at the per-context quota.
  for (std::size_t ctx : {0u, 1u})
    for (int i = 0; i < 4; ++i)
      dual.contexts[ctx].input_queue.push_back(champsim::test::instruction_with_ip(0x1000 * (ctx + 1) + 4 * i));
  dual.fetch_round_robin = 0;
  dual.initialize_instruction();
  REQUIRE(dual.contexts[0].IFETCH_BUFFER.size() == 2);
  REQUIRE(dual.contexts[1].IFETCH_BUFFER.size() == 2);

  // DECODE: promote_to_decode sends fetch-completed, not-yet-decoded instructions
  // to the decode buffer, stopping at the per-context quota.
  for (std::size_t ctx : {0u, 1u}) {
    for (auto& instr : dual.contexts[ctx].IFETCH_BUFFER)
      instr.fetch_completed = true;
    for (int i = 0; i < 2; ++i) {
      auto instr = champsim::test::instruction_with_ip(0x3000 * (ctx + 1) + 4 * i);
      instr.fetch_completed = true;
      instr.ready_time = champsim::chrono::clock::time_point::min();
      dual.contexts[ctx].IFETCH_BUFFER.push_back(std::move(instr));
    }
  }
  dual.fetch_round_robin = 0;
  dual.promote_to_decode();
  REQUIRE(dual.contexts[0].DECODE_BUFFER.size() == 2);
  REQUIRE(dual.contexts[1].DECODE_BUFFER.size() == 2);

  // DIB-hit: decoded instructions fill the DIB-hit buffer to its quota.
  for (std::size_t ctx : {0u, 1u}) {
    dual.contexts[ctx].IFETCH_BUFFER.clear();
    dual.contexts[ctx].DECODE_BUFFER.clear();
    for (int i = 0; i < 4; ++i) {
      auto instr = champsim::test::instruction_with_ip(0x5000 * (ctx + 1) + 4 * i);
      instr.fetch_completed = true;
      instr.decoded = true;
      instr.ready_time = champsim::chrono::clock::time_point::min();
      dual.contexts[ctx].IFETCH_BUFFER.push_back(std::move(instr));
    }
  }
  dual.fetch_round_robin = 0;
  dual.promote_to_decode();
  REQUIRE(dual.contexts[0].DIB_HIT_BUFFER.size() == 2);
  REQUIRE(dual.contexts[1].DIB_HIT_BUFFER.size() == 2);

  // DISPATCH: decode stops at the per-context dispatch quota.
  for (std::size_t ctx : {0u, 1u}) {
    dual.contexts[ctx].DIB_HIT_BUFFER.clear();
    dual.contexts[ctx].DECODE_BUFFER.clear();
    for (int i = 0; i < 4; ++i) {
      auto instr = champsim::test::instruction_with_ip(0x7000 * (ctx + 1) + 4 * i);
      instr.ready_time = champsim::chrono::clock::time_point::min();
      dual.contexts[ctx].DECODE_BUFFER.push_back(std::move(instr));
    }
  }
  dual.fetch_round_robin = 0;
  dual.decode_instruction();
  REQUIRE(dual.contexts[0].DISPATCH_BUFFER.size() == 2);
  REQUIRE(dual.contexts[1].DISPATCH_BUFFER.size() == 2);
}

TEST_CASE("The one 1x1 DIB is shared: a fill in one context hits in the other within one asid, and misses across asids")
{
  do_nothing_MRC l1i, l1d;
  O3_CPU dual{champsim::core_builder{}
                  .fetch_queues(&l1i.queues)
                  .data_queues(&l1d.queues)
                  .num_contexts(2)
                  .ifetch_buffer_size(2)
                  .decode_buffer_size(2)
                  .dib_hit_buffer_size(2)
                  .dispatch_buffer_size(2)
                  .rob_size(2)
                  .lq_size(2)
                  .sq_size(2)
                  .register_file_size(128)}; // the DIB geometry stays the 1x1 default

  constexpr champsim::address IP{0xdeadbeef};

  // Context 0 fills the single entry with asid {1,1}.
  auto c0_fill = champsim::test::instruction_with_ip(IP);
  c0_fill.asid = champsim::address_space_id{1, 1};
  dual.do_dib_update(dual.contexts[0], c0_fill);

  // Context 1 with the same (ip, asid) hits the entry context 0 filled: the DIB
  // is shared across contexts.
  auto c1_hit = champsim::test::instruction_with_ip(IP);
  c1_hit.asid = champsim::address_space_id{1, 1};
  dual.do_check_dib(dual.contexts[1], c1_hit);
  CHECK(c1_hit.fetch_completed);
  CHECK(c1_hit.decoded);
  CHECK(dual.contexts[1].sim_stats.dib_hits == 1);

  // A different asid at the same ip misses: the DIB tag folds the asid.
  auto c0_miss = champsim::test::instruction_with_ip(IP);
  c0_miss.asid = champsim::address_space_id{2, 2};
  dual.do_check_dib(dual.contexts[0], c0_miss);
  CHECK_FALSE(c0_miss.fetch_completed);
  CHECK_FALSE(c0_miss.decoded);
  CHECK(dual.contexts[0].sim_stats.dib_misses == 1);

  // 1x1 holds one entry for both contexts: context 1's fill evicts the entry
  // context 0 filled, so context 0's former window now misses.
  auto c1_fill = champsim::test::instruction_with_ip(champsim::address{0x2000});
  c1_fill.asid = champsim::address_space_id{1, 1};
  dual.do_dib_update(dual.contexts[1], c1_fill);

  auto c0_evicted = champsim::test::instruction_with_ip(IP);
  c0_evicted.asid = champsim::address_space_id{1, 1};
  dual.do_check_dib(dual.contexts[0], c0_evicted);
  CHECK_FALSE(c0_evicted.fetch_completed);
}
