#include <catch.hpp>
#include <type_traits>

#include "instr.h"
#include "mocks.hpp"
#include "ooo_cpu.h"

// An SQ entry refers to its own context's LQ slots (lq_depend_on_me). A copied
// context would keep referring to the LQ it was copied from, so core_context is
// move-only: a move takes the LQ's buffer along, and the references stay valid.
static_assert(!std::is_copy_constructible_v<champsim::core_context>);
static_assert(!std::is_copy_assignable_v<champsim::core_context>);
static_assert(std::is_move_constructible_v<champsim::core_context>);
static_assert(std::is_move_assignable_v<champsim::core_context>);

SCENARIO("Relocating the contexts keeps a store's waiting load on the live LQ slot")
{
  GIVEN("A load that waits on an older, unexecuted store to the same address")
  {
    do_nothing_MRC mock_L1I, mock_L1D;
    O3_CPU uut{champsim::core_builder{}
                   .fetch_queues(&mock_L1I.queues)
                   .data_queues(&mock_L1D.queues)
                   .dispatch_width(champsim::bandwidth::maximum_type{2})
                   .rob_size(2)
                   .lq_size(2)};

    auto store = champsim::test::instruction_with_ip(champsim::address{2000});
    store.destination_memory.push_back(champsim::address{0xcafe0000});
    store.instr_id = 1;
    auto load = champsim::test::instruction_with_ip_and_source_memory(champsim::address{2004}, champsim::address{0xcafe0000});
    load.instr_id = 2;

    uut.ctx().DISPATCH_BUFFER.push_back(store);
    uut.ctx().DISPATCH_BUFFER.push_back(load);
    for (auto& instr : uut.ctx().DISPATCH_BUFFER)
      instr.ready_time = champsim::chrono::clock::time_point{};

    for (int i = 0; i < 100 && (std::empty(uut.ctx().SQ) || std::empty(uut.ctx().SQ.front().lq_depend_on_me)); ++i) {
      for (auto op : std::array<champsim::operable*, 3>{{&uut, &mock_L1I, &mock_L1D}})
        op->_operate();
    }

    REQUIRE(std::size(uut.ctx().SQ) == 1);
    REQUIRE_FALSE(uut.ctx().SQ.front().fetch_issued);
    REQUIRE(std::size(uut.ctx().SQ.front().lq_depend_on_me) == 1);
    auto slot = std::find_if(std::begin(uut.ctx().LQ), std::end(uut.ctx().LQ), [](const auto& entry) { return entry.has_value(); });
    REQUIRE(slot != std::end(uut.ctx().LQ));
    REQUIRE((*slot)->producer_id == store.instr_id);
    const auto slot_index = static_cast<std::size_t>(std::distance(std::begin(uut.ctx().LQ), slot));
    const std::optional<LSQ_ENTRY>* live_slot = &*slot;
    REQUIRE(&uut.ctx().SQ.front().lq_depend_on_me.front().get() == live_slot);

    WHEN("The vector of contexts reallocates")
    {
      const auto* old_buffer = std::data(uut.contexts);
      uut.contexts.reserve(uut.contexts.capacity() + 1);
      REQUIRE(std::data(uut.contexts) != old_buffer);

      THEN("The LQ slot did not move and the store still refers to it")
      {
        REQUIRE(&uut.ctx().LQ.at(slot_index) == live_slot);
        REQUIRE(&uut.ctx().SQ.front().lq_depend_on_me.front().get() == live_slot);
      }

      AND_WHEN("The store executes")
      {
        for (int i = 0; i < 100 && uut.ctx().ROB.back().completed_mem_ops == 0; ++i) {
          for (auto op : std::array<champsim::operable*, 3>{{&uut, &mock_L1I, &mock_L1D}})
            op->_operate();
        }

        THEN("It forwards to the waiting load and frees its LQ slot")
        {
          REQUIRE(uut.ctx().ROB.back().instr_id == load.instr_id);
          REQUIRE(uut.ctx().ROB.back().completed_mem_ops == 1);
          REQUIRE_FALSE(uut.ctx().LQ.at(slot_index).has_value());
          REQUIRE(mock_L1D.packet_count() == 0);
        }
      }
    }
  }
}
