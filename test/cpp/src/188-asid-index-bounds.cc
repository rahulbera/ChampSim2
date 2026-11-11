#include <catch.hpp>

#include "../../../branch/bimodal/bimodal.h"
#include "../../../branch/gshare/gshare.h"
#include "../../../branch/hashed_perceptron/hashed_perceptron.h"
#include "../../../branch/perceptron/perceptron.h"
#include "../../../btb/basic_btb/basic_btb.h"
#include "instr.h"
#include "instruction.h"
#include "mocks.hpp"
#include "ooo_cpu.h"

// The predictors fold a 16-bit asid into a table index of fewer bits. Before
// this test, hashed_perceptron XOR-ed the whole asid_word into a 12-bit index,
// so any record with asid[1] >= 16 (e.g. {0,16} -> asid_word 4096) could index
// past the table and throw in table.at(); bimodal and perceptron stayed in
// range but changed slot sharing by XOR-ing before a non-power-of-two modulus.
// Every index form below must keep the asid in bounds for the whole range, and
// must be a no-op at {0,0}.

namespace
{
const std::array<champsim::address_space_id, 5> asids{{{0, 0}, {0, 16}, {16, 16}, {255, 0}, {255, 255}}};
}

TEMPLATE_TEST_CASE("A branch predictor keeps asid-folded indexes inside its tables", "", bimodal, gshare, perceptron, hashed_perceptron)
{
  do_nothing_MRC mock_L1I, mock_L1D;
  O3_CPU uut{champsim::core_builder{}.fetch_queues(&mock_L1I.queues).data_queues(&mock_L1D.queues).branch_predictor<TestType>()};
  const champsim::address ip{0xdeadbeef};

  for (const auto& asid : asids) {
    CAPTURE(static_cast<int>(asid[0]), static_cast<int>(asid[1]));
    for (int i = 0; i < 64; ++i) {
      REQUIRE_NOTHROW(uut.impl_predict_branch(ip, champsim::address{}, true, BRANCH_CONDITIONAL, champsim::context_index{}, asid));
      uut.impl_last_branch_result(ip, champsim::address{}, true, BRANCH_CONDITIONAL, champsim::context_index{}, asid);
    }
    CHECK(uut.impl_predict_branch(ip, champsim::address{}, true, BRANCH_CONDITIONAL, champsim::context_index{}, asid));
  }
}

TEST_CASE("The basic_btb keeps asid-folded indexes inside its tables")
{
  basic_btb uut;

  for (const auto& asid : asids) {
    CAPTURE(static_cast<int>(asid[0]), static_cast<int>(asid[1]));

    REQUIRE_NOTHROW(uut.update_btb(champsim::address{0x110000}, champsim::address{0x66b5f0}, true, BRANCH_DIRECT_JUMP, champsim::context_index{}, asid));
    CHECK(uut.btb_prediction(champsim::address{0x110000}, champsim::context_index{}, asid).first == champsim::address{0x66b5f0});

    REQUIRE_NOTHROW(uut.update_btb(champsim::address{0x220000}, champsim::address{0x66b5f0}, true, BRANCH_INDIRECT, champsim::context_index{}, asid));
    CHECK(uut.btb_prediction(champsim::address{0x220000}, champsim::context_index{}, asid).first == champsim::address{0x66b5f0});
  }
}

TEST_CASE("The DIB keys a decoded window by (asid, ip) for every asid")
{
  for (const auto& asid : asids) {
    CAPTURE(static_cast<int>(asid[0]), static_cast<int>(asid[1]));
    do_nothing_MRC mock_L1I, mock_L1D;
    O3_CPU uut{champsim::core_builder{}.dib_window(16).fetch_queues(&mock_L1I.queues).data_queues(&mock_L1D.queues)};

    auto instr = champsim::test::instruction_with_ip(0xdeadbeef);
    instr.asid = asid;
    uut.do_check_dib(instr);
    uut.do_dib_update(instr);
    uut.do_check_dib(instr);

    CHECK(uut.ctx().sim_stats.dib_misses == 1);
    CHECK(uut.ctx().sim_stats.dib_hits == 1);
  }
}
