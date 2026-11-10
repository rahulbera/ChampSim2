#include <catch.hpp>
#include <utility>

#include "instr.h"
#include "mocks.hpp"
#include "ooo_cpu.h"
#include "register_allocator.h"

// Stage 1 splits register renaming in two: the physical register file and its
// free list stay on the core (one RegisterAllocator), while each hardware
// context owns its own frontend and backend alias tables (champsim::core_context)
// and its own context index. The four scenarios below bind two hand-built
// contexts to one allocator, giving the second a distinct context index; the
// final case builds a two-context core and checks that the builder wires both
// contexts to the core's shared physical register file with indices 0 and 1.
// Physical registers leave the free list in index order, which the exact free
// counts rely on.

namespace
{
ooo_model_instr reader_of(uint64_t id, PHYSICAL_REGISTER_ID physical_source)
{
  auto instr = champsim::test::instruction_with_ip(id);
  instr.instr_id = id;
  instr.source_registers.push_back(physical_source);
  return instr;
}
} // namespace

SCENARIO("Two contexts sharing a physical register file rename the same architectural register independently")
{
  GIVEN("Two contexts bound to one physical register file")
  {
    constexpr int PHYSICALREGS = 8;
    RegisterAllocator prf{PHYSICALREGS};
    champsim::core_context a{&prf}, b{&prf};
    b.ctx_index = champsim::context_index{1};

    WHEN("Context A renames architectural register 5 as a destination")
    {
      const auto a_dest = a.rename_dest_register(5, 1);

      THEN("Only context A maps register 5")
      {
        REQUIRE(a.frontend_RAT[5] == a_dest);
        REQUIRE(a.backend_RAT[5] == -1);
        REQUIRE(b.frontend_RAT[5] == -1);
        REQUIRE(b.backend_RAT[5] == -1);
        REQUIRE(prf.count_free_registers() == PHYSICALREGS - 1);
      }

      AND_WHEN("Context B reads architectural register 5")
      {
        const auto b_source = b.rename_src_register(5);

        THEN("B gets its own committed live-in, not A's in-flight destination")
        {
          REQUIRE(b_source != a_dest);
          REQUIRE(b.frontend_RAT[5] == b_source);
          REQUIRE(b.backend_RAT[5] == b_source);
          REQUIRE(a.frontend_RAT[5] == a_dest);
          REQUIRE(a.backend_RAT[5] == -1);
          REQUIRE(prf.count_free_registers() == PHYSICALREGS - 2);
        }

        THEN("B's read waits on nothing while A's own read waits on A's write")
        {
          REQUIRE(prf.isValid(b_source));
          REQUIRE_FALSE(prf.isValid(a_dest));
          REQUIRE(prf.count_reg_dependencies(reader_of(2, b_source)) == 0);
          REQUIRE(a.rename_src_register(5) == a_dest);
          REQUIRE(prf.count_reg_dependencies(reader_of(3, a.rename_src_register(5))) == 1);
        }
      }

      AND_WHEN("Context B also renames architectural register 5 as a destination")
      {
        const auto b_dest = b.rename_dest_register(5, 2);

        THEN("The two in-flight destinations are distinct physical registers")
        {
          REQUIRE(b_dest != a_dest);
          REQUIRE(a.frontend_RAT[5] == a_dest);
          REQUIRE(b.frontend_RAT[5] == b_dest);
          REQUIRE(prf.count_free_registers() == PHYSICALREGS - 2);
        }
      }
    }
  }
}

SCENARIO("Retiring a write in one context frees only that context's committed mapping")
{
  GIVEN("Two contexts that each hold a committed live-in for architectural register 5")
  {
    constexpr int PHYSICALREGS = 8;
    RegisterAllocator prf{PHYSICALREGS};
    champsim::core_context a{&prf}, b{&prf};
    b.ctx_index = champsim::context_index{1};
    const auto b_live_in = b.rename_src_register(5);
    const auto a_live_in = a.rename_src_register(5);
    REQUIRE(a_live_in != b_live_in);
    REQUIRE(prf.count_free_registers() == PHYSICALREGS - 2);

    WHEN("Context A writes register 5, completes and retires the write")
    {
      const auto a_dest = a.rename_dest_register(5, 1);
      REQUIRE(prf.count_free_registers() == PHYSICALREGS - 3);
      prf.complete_dest_register(a_dest);
      a.retire_dest_register(a_dest);

      THEN("Exactly A's old mapping is freed and B's mapping is untouched")
      {
        REQUIRE(prf.count_free_registers() == PHYSICALREGS - 2);
        REQUIRE_FALSE(prf.isValid(a_live_in));
        REQUIRE(a.backend_RAT[5] == a_dest);
        REQUIRE(b.frontend_RAT[5] == b_live_in);
        REQUIRE(b.backend_RAT[5] == b_live_in);
        REQUIRE(prf.isValid(b_live_in));
        REQUIRE(b.rename_src_register(5) == b_live_in);
        REQUIRE(prf.count_free_registers() == PHYSICALREGS - 2);
      }

      AND_WHEN("Context B then writes register 5, completes and retires the write")
      {
        const auto b_dest = b.rename_dest_register(5, 2);
        prf.complete_dest_register(b_dest);
        b.retire_dest_register(b_dest);

        THEN("Exactly B's live-in is freed and A's committed mapping is untouched")
        {
          REQUIRE(prf.count_free_registers() == PHYSICALREGS - 2);
          REQUIRE(b.backend_RAT[5] == b_dest);
          REQUIRE(a.backend_RAT[5] == a_dest);
          REQUIRE(a.frontend_RAT[5] == a_dest);
          REQUIRE(prf.isValid(a_dest));
        }
      }
    }
  }
}

SCENARIO("Resetting one context's frontend alias table leaves its sibling untouched")
{
  GIVEN("Two contexts with a committed live-in each and a speculative write in B")
  {
    constexpr int PHYSICALREGS = 8;
    RegisterAllocator prf{PHYSICALREGS};
    champsim::core_context a{&prf}, b{&prf};
    b.ctx_index = champsim::context_index{1};
    const auto a_live_in = a.rename_src_register(5);
    const auto b_live_in = b.rename_src_register(5);
    const auto b_speculative = b.rename_dest_register(5, 2);

    WHEN("Context A renames register 5 speculatively and then resets its frontend alias table")
    {
      const auto a_speculative = a.rename_dest_register(5, 1);
      REQUIRE(a.frontend_RAT[5] == a_speculative);
      a.reset_frontend_RAT();

      THEN("A returns to its committed mapping and B keeps its speculative one")
      {
        REQUIRE(a.frontend_RAT[5] == a_live_in);
        REQUIRE(a.backend_RAT[5] == a_live_in);
        REQUIRE(b.frontend_RAT[5] == b_speculative);
        REQUIRE(b.backend_RAT[5] == b_live_in);
      }
    }
  }
}

SCENARIO("Whether an architectural register is allocated is answered per context")
{
  GIVEN("Two contexts bound to one physical register file")
  {
    RegisterAllocator prf{8};
    champsim::core_context a{&prf}, b{&prf};
    b.ctx_index = champsim::context_index{1};
    const auto& a_view = std::as_const(a);
    const auto& b_view = std::as_const(b);

    WHEN("Context A reads register 7 and context B writes register 9")
    {
      a.rename_src_register(7);
      b.rename_dest_register(9, 1);

      THEN("Each context reports only its own mappings")
      {
        REQUIRE(a_view.isAllocated(7));
        REQUIRE_FALSE(b_view.isAllocated(7));
        REQUIRE(b_view.isAllocated(9));
        REQUIRE_FALSE(a_view.isAllocated(9));
      }
    }
  }
}

TEST_CASE("A built core has one context bound to the core's shared physical register file")
{
  constexpr std::size_t PHYSICALREGS = 16;
  do_nothing_MRC mock_L1I, mock_L1D;
  O3_CPU uut{champsim::core_builder{}.register_file_size(PHYSICALREGS).fetch_queues(&mock_L1I.queues).data_queues(&mock_L1D.queues)};

  REQUIRE(uut.contexts.size() == 1);
  REQUIRE(&uut.ctx() == &uut.contexts.front());
  REQUIRE(uut.shared_prf != nullptr);
  REQUIRE(uut.ctx().reg_allocator == uut.shared_prf.get());

  const auto& view = std::as_const(uut);
  REQUIRE_FALSE(view.ctx().isAllocated(3));
  uut.ctx().rename_src_register(3);
  REQUIRE(view.ctx().isAllocated(3));
  REQUIRE(uut.shared_prf->count_free_registers() == PHYSICALREGS - 1);
}

TEST_CASE("A core built with two contexts binds both to the shared physical register file with indices 0 and 1")
{
  constexpr std::size_t PHYSICALREGS = 16;
  do_nothing_MRC mock_L1I, mock_L1D;
  O3_CPU uut{champsim::core_builder{}
                 .register_file_size(PHYSICALREGS)
                 .num_contexts(2)
                 .ifetch_buffer_size(2)
                 .decode_buffer_size(2)
                 .dib_hit_buffer_size(2)
                 .dispatch_buffer_size(2)
                 .rob_size(16)
                 .lq_size(8)
                 .sq_size(8)
                 .fetch_queues(&mock_L1I.queues)
                 .data_queues(&mock_L1D.queues)};

  REQUIRE(uut.contexts.size() == 2);
  REQUIRE(uut.shared_prf != nullptr);
  REQUIRE(uut.contexts[0].ctx_index.value == 0);
  REQUIRE(uut.contexts[1].ctx_index.value == 1);
  REQUIRE(uut.contexts[0].reg_allocator == uut.shared_prf.get());
  REQUIRE(uut.contexts[1].reg_allocator == uut.shared_prf.get());
}
