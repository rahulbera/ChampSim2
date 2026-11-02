#include <catch.hpp>
#include <cstdint>

#include "defs.h"        // champsim::defs::num_cpus
#include "instruction.h" // BRANCH_* constants
#include "ooo_cpu.h"     // O3_CPU, champsim::core_builder

#include "../../../btb/ittage_32kb/ittage_32kb.h"
#include "../../../btb/ittage_64kb/ittage_64kb.h"

// Stage 6.5b, ittage cross-core isolation. 186 proves that context 0 and
// context 1 of ONE core (intern_ == nullptr -> core_of == 0) keep separate
// direct BTBs. This test proves the OTHER axis: two cores driven at the SAME
// context index do not alias. That axis is exactly what 6.2 fixed -- the
// per-(core, context) storage (shared()[core][context]) was context-only
// before, so core 0's ctx 0 and core 1's ctx 0 collided (Stage 6 hazard B).
//
// Each module is bound to a real O3_CPU with a distinct cpu index via the
// inherited btb(O3_CPU*) constructor, so core_of(intern_) returns 0 and 1.
// The direct-BTB path is driven (BRANCH_DIRECT_JUMP, taken=true): it touches
// only the direct predictor and the path history, never the core's input
// queue, so a minimally constructed core is safe here (see 186's rationale).

namespace
{
// Distinct ip/ASID from 186 (which uses ip 0x110000, asid {0,0}): the
// predictor storage is a function-local static shared by every instance of
// this module type across the whole test binary, and tests run in random
// order, so a fresh key keeps this test independent of 186.
constexpr champsim::address ip{0x220000};
constexpr champsim::address target_a{0xccc000};
constexpr champsim::address target_b{0xddd000};
constexpr champsim::address_space_id asid{2, 2};

template <typename Btb>
void check_cross_core_isolation()
{
  O3_CPU core0{champsim::core_builder{}.index(0)};
  O3_CPU core1{champsim::core_builder{}.index(1)};

  Btb btb0{&core0}; // intern_ -> core0 (core_of == 0)
  Btb btb1{&core1}; // intern_ -> core1 (core_of == 1)

  // Train each core's ctx 0 toward a different target. If the storage were
  // context-only, the second write would win on BOTH reads.
  btb0.update_btb(ip, target_a, true, BRANCH_DIRECT_JUMP, champsim::context_index{0}, asid);
  btb1.update_btb(ip, target_b, true, BRANCH_DIRECT_JUMP, champsim::context_index{0}, asid);

  const auto [pred_a, taken_a] = btb0.btb_prediction(ip, champsim::context_index{0}, asid);
  const auto [pred_b, taken_b] = btb1.btb_prediction(ip, champsim::context_index{0}, asid);

  REQUIRE(pred_a == target_a);
  REQUIRE(taken_a);
  REQUIRE(pred_b == target_b);
  REQUIRE(taken_b);
}
} // namespace

TEST_CASE("ittage_32kb keeps core 0 and core 1 direct BTBs separate")
{
  if constexpr (champsim::defs::num_cpus >= 2) {
    check_cross_core_isolation<ittage_32kb>();
  } else {
    SKIP("requires num_cpus >= 2");
  }
}

TEST_CASE("ittage_64kb keeps core 0 and core 1 direct BTBs separate")
{
  if constexpr (champsim::defs::num_cpus >= 2) {
    check_cross_core_isolation<ittage_64kb>();
  } else {
    SKIP("requires num_cpus >= 2");
  }
}
