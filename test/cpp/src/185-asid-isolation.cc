#include <catch.hpp>

#include "../../../branch/bimodal/bimodal.h"
#include "../../../btb/basic_btb/basic_btb.h"
#include "instr.h"
#include "instruction.h"
#include "mocks.hpp"
#include "ooo_cpu.h"

// Step 2.5: the decoded-instruction buffer, the branch direction predictor and
// the BTB are all keyed by (asid, ip), so two address spaces reusing the same
// instruction pointer must not share a decoded window, a direction counter or a
// branch target. In the N=1 milestone asid is constant {0,0}, which is exactly
// why the ASID fold is a numeric no-op there; these scenarios prove the
// isolation is real once asid actually differs.

TEST_CASE("The DIB keys a decoded window by (asid, ip)")
{
  do_nothing_MRC mock_L1I, mock_L1D;
  O3_CPU uut{champsim::core_builder{}.dib_window(16).fetch_queues(&mock_L1I.queues).data_queues(&mock_L1D.queues)};

  auto first = champsim::test::instruction_with_ip(0xdeadbeef);
  first.asid = {0, 0};
  uut.do_check_dib(first);
  uut.do_dib_update(first);

  // Same IP, different address space: the window decoded under asid {0,0} is
  // not visible under asid {0,1}, so the lookup misses.
  auto second = champsim::test::instruction_with_ip(0xdeadbeef);
  second.asid = {0, 1};
  uut.do_check_dib(second);

  CHECK(uut.ctx().sim_stats.dib_hits == 0);
  CHECK(uut.ctx().sim_stats.dib_misses == 2);
}

TEST_CASE("The basic_btb direct predictor keys its table by (asid, ip)")
{
  basic_btb uut;

  const champsim::address ip{0x110000};
  const champsim::address target{0x66b5f0};
  const champsim::address_space_id asid_a{0, 0};
  const champsim::address_space_id asid_b{0, 1};

  uut.update_btb(ip, target, true, BRANCH_DIRECT_JUMP, champsim::context_index{}, asid_a);

  const auto [pred_a, taken_a] = uut.btb_prediction(ip, champsim::context_index{}, asid_a);
  CHECK(pred_a == target);
  CHECK(taken_a);

  const auto [pred_b, taken_b] = uut.btb_prediction(ip, champsim::context_index{}, asid_b);
  CHECK(pred_b == champsim::address{});
  CHECK_FALSE(taken_b);
}

TEST_CASE("The bimodal predictor keys its counters by (asid, ip)")
{
  bimodal uut{nullptr};

  const champsim::address ip{0xdeadbeef};
  const champsim::address_space_id asid_a{0, 0};
  const champsim::address_space_id asid_b{0, 1};

  for (int i = 0; i < 100; ++i)
    uut.last_branch_result(ip, champsim::address{}, true, BRANCH_CONDITIONAL, asid_a);

  CHECK(uut.predict_branch(ip, champsim::address{}, true, BRANCH_CONDITIONAL, asid_a));
  CHECK_FALSE(uut.predict_branch(ip, champsim::address{}, true, BRANCH_CONDITIONAL, asid_b));
}

TEST_CASE("The DIB compares the asid itself in its tag")
{
  do_nothing_MRC mock_L1I, mock_L1D;
  O3_CPU uut{champsim::core_builder{}.dib_set(32).dib_way(8).dib_window(16).fetch_queues(&mock_L1I.queues).data_queues(&mock_L1D.queues)};

  auto first = champsim::test::instruction_with_ip(0x401230);
  first.asid = {0, 0};
  uut.do_check_dib(first);
  uut.do_dib_update(first);

  // 0x401230>>4 (0x40123) equals 0x400230>>4 (0x40023) XOR the {0,1} asid_word (0x100), so an XOR tag would collide.
  auto second = champsim::test::instruction_with_ip(0x400230);
  second.asid = {0, 1};
  uut.do_check_dib(second);

  auto third = champsim::test::instruction_with_ip(0x401230);
  third.asid = {0, 0};
  uut.do_check_dib(third);

  CHECK(uut.ctx().sim_stats.dib_hits == 1);
  CHECK(uut.ctx().sim_stats.dib_misses == 2);
}

TEST_CASE("The basic_btb direct predictor compares the asid itself in its tag")
{
  basic_btb uut;

  // 0x405000>>2 (0x101400) equals 0x404000>>2 (0x101000) XOR the {0,4} asid_word (0x400), so an XOR tag would collide.
  uut.update_btb(champsim::address{0x405000}, champsim::address{0x409000}, true, BRANCH_DIRECT_JUMP, champsim::context_index{}, {0, 0});

  const auto [pred_b, taken_b] = uut.btb_prediction(champsim::address{0x404000}, champsim::context_index{}, {0, 4});
  CHECK(pred_b == champsim::address{});
  CHECK_FALSE(taken_b);

  const auto [pred_a, taken_a] = uut.btb_prediction(champsim::address{0x405000}, champsim::context_index{}, {0, 0});
  CHECK(pred_a == champsim::address{0x409000});
  CHECK(taken_a);
}
