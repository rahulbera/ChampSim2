#include <catch.hpp>
#include <cstddef>

#include "ooo_cpu.h"

// BMT B1: the virtual-context layer is a pure skeleton -- a C_virt virtual-thread
// pool plus a C_phys slot->virtual map, with the identity binding (slot i ->
// virtual i) and zero swap behavior. These tests pin the structural invariants
// that make C_virt == C_phys byte-identical and C_virt > C_phys "extra virtual
// threads idle". There is no swap in B1, so the state machine is exercised only
// through its construction defaults and the distinctness of its states.

TEST_CASE("B1 virtual-context layer defaults to the identity binding")
{
  O3_CPU cpu{champsim::core_builder{}.index(0).num_contexts(2)};

  // C_virt defaults to C_phys: one virtual thread per physical slot.
  REQUIRE(cpu.virtual_threads.size() == 2);
  REQUIRE(cpu.contexts.size() == 2);
  REQUIRE(cpu.slot_map.size() == 2);

  // Mapping table is the identity: slot i -> virtual i.
  for (std::size_t i = 0; i < cpu.slot_map.size(); ++i) {
    CHECK(cpu.slot_map[i] == static_cast<long>(i));
  }

  // Every virtual thread is bound to its own trace slot (identity) and starts
  // running -- nothing is suspended or finished in a no-swap skeleton.
  for (std::size_t v = 0; v < cpu.virtual_threads.size(); ++v) {
    CHECK(cpu.virtual_threads[v].virtual_id == v);
    CHECK(cpu.virtual_threads[v].trace_slot == v);
    CHECK(cpu.virtual_threads[v].st == champsim::virtual_thread::state::running);
  }
}

TEST_CASE("B1 C_virt > C_phys leaves the tail virtual threads idle and unbound")
{
  O3_CPU cpu{champsim::core_builder{}.index(0).num_contexts(2).num_virtual_threads(4)};

  REQUIRE(cpu.virtual_threads.size() == 4);
  REQUIRE(cpu.contexts.size() == 2);
  REQUIRE(cpu.slot_map.size() == 2);

  // The two physical slots bind the first two virtual threads, identity.
  CHECK(cpu.slot_map[0] == 0);
  CHECK(cpu.slot_map[1] == 1);

  // Virtual threads 2 and 3 exist (their traces and records are preserved and
  // rebindable later) but no slot maps to them -- they are idle, not finished.
  for (std::size_t v = 0; v < cpu.virtual_threads.size(); ++v) {
    CHECK(cpu.virtual_threads[v].virtual_id == v);
    CHECK(cpu.virtual_threads[v].trace_slot == v);
    CHECK(cpu.virtual_threads[v].st == champsim::virtual_thread::state::running);
  }
}

TEST_CASE("B1 state machine keeps suspended and finished distinct")
{
  // B1.2 / B0.5 risk pin: V3.3 taught the phase-completion predicate to treat a
  // *suspended* context as *finished*. BMT must not inherit that conflation -- a
  // suspended virtual thread is recoverable and rebindable, and only trace EOF
  // is finished. The enum is the B1 skeleton of that distinction; the three
  // states being pairwise distinct is the guard that "suspended == finished"
  // cannot sneak back in silently.
  using st = champsim::virtual_thread::state;
  CHECK(st::running != st::suspended);
  CHECK(st::running != st::finished);
  CHECK(st::suspended != st::finished);
}
