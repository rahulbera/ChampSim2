#include <catch.hpp>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "instr.h"
#include "mocks.hpp"
#include "ooo_cpu.h"

// Branch predictors and BTBs written before the asid hook existed keep their
// legacy signatures: predict_branch/last_branch_result/branch_execute_resolve
// take the same arguments minus the trailing address_space_id, and
// btb_prediction takes an ip and a branch type. The dispatcher must still reach
// those modules, forwarding every argument unchanged, and must call a module
// exactly once when it offers both an asid (or context-index) and a legacy
// overload of one hook.

namespace
{
int calls = 0; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

struct legacy_address_predictor : champsim::modules::branch_predictor {
  using branch_predictor::branch_predictor;
  bool predict_branch(champsim::address, champsim::address, bool, uint8_t)
  {
    ++calls;
    return true;
  }
  void last_branch_result(champsim::address, champsim::address, bool, uint8_t) { ++calls; }
  void branch_execute_resolve(uint64_t, champsim::address, champsim::address, bool, uint8_t) { ++calls; }
};

struct legacy_integer_predictor : champsim::modules::branch_predictor {
  using branch_predictor::branch_predictor;
  bool predict_branch(uint64_t, uint64_t, bool, uint8_t)
  {
    ++calls;
    return true;
  }
  void last_branch_result(uint64_t, uint64_t, bool, uint8_t) { ++calls; }
};

struct both_forms_predictor : champsim::modules::branch_predictor {
  using branch_predictor::branch_predictor;
  bool predict_branch(champsim::address, champsim::address, bool, uint8_t, champsim::address_space_id)
  {
    ++calls;
    return true;
  }
  bool predict_branch(champsim::address, champsim::address, bool, uint8_t) { return false; }
  void last_branch_result(champsim::address, champsim::address, bool, uint8_t, champsim::address_space_id) { ++calls; }
  void last_branch_result(champsim::address, champsim::address, bool, uint8_t) { ++calls; }
  void branch_execute_resolve(uint64_t, champsim::address, champsim::address, bool, uint8_t, champsim::address_space_id) { ++calls; }
  void branch_execute_resolve(uint64_t, champsim::address, champsim::address, bool, uint8_t) { ++calls; }
};

struct legacy_address_btb : champsim::modules::btb {
  using btb::btb;
  std::pair<champsim::address, bool> btb_prediction(champsim::address, uint8_t)
  {
    ++calls;
    return {champsim::address{0x4400}, true};
  }
  void update_btb(champsim::address, champsim::address, bool, uint8_t) { ++calls; }
};

struct legacy_integer_btb : champsim::modules::btb {
  using btb::btb;
  std::pair<champsim::address, bool> btb_prediction(uint64_t, uint8_t)
  {
    ++calls;
    return {champsim::address{0x4400}, true};
  }
  void update_btb(uint64_t, uint64_t, bool, uint8_t) { ++calls; }
};

struct both_forms_btb : champsim::modules::btb {
  using btb::btb;
  std::pair<champsim::address, bool> btb_prediction(champsim::address, uint8_t, champsim::address_space_id)
  {
    ++calls;
    return {champsim::address{0x4400}, true};
  }
  std::pair<champsim::address, bool> btb_prediction(champsim::address, uint8_t) { return {}; }
  void update_btb(champsim::address, champsim::address, bool, uint8_t, champsim::address_space_id) { ++calls; }
  void update_btb(champsim::address, champsim::address, bool, uint8_t) { ++calls; }
};

struct context_and_legacy_btb : champsim::modules::btb {
  using btb::btb;
  std::pair<champsim::address, bool> btb_prediction(champsim::address, champsim::context_index, champsim::address_space_id)
  {
    ++calls;
    return {champsim::address{0x4400}, true};
  }
  std::pair<champsim::address, bool> btb_prediction(champsim::address, uint8_t) { return {}; }
  void update_btb(champsim::address, champsim::address, bool, uint8_t, champsim::context_index, champsim::address_space_id) { ++calls; }
  void update_btb(champsim::address, champsim::address, bool, uint8_t) { ++calls; }
};
} // namespace

TEST_CASE("A legacy address-typed branch predictor is still dispatched")
{
  calls = 0;
  do_nothing_MRC mock_L1I, mock_L1D;
  O3_CPU uut{champsim::core_builder{}.fetch_queues(&mock_L1I.queues).data_queues(&mock_L1D.queues).branch_predictor<legacy_address_predictor>()};

  const champsim::address ip{0x4000};
  const champsim::address target{0x5000};
  const champsim::address_space_id asid{3, 4};

  CHECK(uut.impl_predict_branch(ip, target, true, BRANCH_CONDITIONAL, asid));
  CHECK(calls == 1);

  uut.impl_last_branch_result(ip, target, true, BRANCH_CONDITIONAL, asid);
  CHECK(calls == 2);

  uut.impl_branch_execute_resolve(42, ip, target, true, BRANCH_CONDITIONAL, asid);
  CHECK(calls == 3);
}

TEST_CASE("A legacy integer-typed branch predictor is still dispatched")
{
  calls = 0;
  do_nothing_MRC mock_L1I, mock_L1D;
  O3_CPU uut{champsim::core_builder{}.fetch_queues(&mock_L1I.queues).data_queues(&mock_L1D.queues).branch_predictor<legacy_integer_predictor>()};

  const champsim::address ip{0x4000};
  const champsim::address target{0x5000};
  const champsim::address_space_id asid{3, 4};

  CHECK(uut.impl_predict_branch(ip, target, true, BRANCH_CONDITIONAL, asid));
  CHECK(calls == 1);

  uut.impl_last_branch_result(ip, target, true, BRANCH_CONDITIONAL, asid);
  CHECK(calls == 2);
}

TEST_CASE("A branch predictor with asid and legacy overloads is called once per hook")
{
  calls = 0;
  do_nothing_MRC mock_L1I, mock_L1D;
  O3_CPU uut{champsim::core_builder{}.fetch_queues(&mock_L1I.queues).data_queues(&mock_L1D.queues).branch_predictor<both_forms_predictor>()};

  const champsim::address ip{0x4000};
  const champsim::address target{0x5000};
  const champsim::address_space_id asid{3, 4};

  CHECK(uut.impl_predict_branch(ip, target, true, BRANCH_CONDITIONAL, asid));
  CHECK(calls == 1);

  uut.impl_last_branch_result(ip, target, true, BRANCH_CONDITIONAL, asid);
  CHECK(calls == 2);

  uut.impl_branch_execute_resolve(42, ip, target, true, BRANCH_CONDITIONAL, asid);
  CHECK(calls == 3);
}

TEST_CASE("A legacy address-typed BTB is still dispatched")
{
  calls = 0;
  do_nothing_MRC mock_L1I, mock_L1D;
  O3_CPU uut{champsim::core_builder{}.fetch_queues(&mock_L1I.queues).data_queues(&mock_L1D.queues).btb<legacy_address_btb>()};

  const champsim::address ip{0x4000};
  const champsim::address target{0x5000};
  const champsim::address_space_id asid{3, 4};

  const auto [pred, taken] = uut.impl_btb_prediction(ip, BRANCH_CONDITIONAL, champsim::context_index{}, asid);
  CHECK(pred == champsim::address{0x4400});
  CHECK(taken);
  CHECK(calls == 1);

  uut.impl_update_btb(ip, target, true, BRANCH_CONDITIONAL, champsim::context_index{}, asid);
  CHECK(calls == 2);
}

TEST_CASE("A legacy integer-typed BTB is still dispatched")
{
  calls = 0;
  do_nothing_MRC mock_L1I, mock_L1D;
  O3_CPU uut{champsim::core_builder{}.fetch_queues(&mock_L1I.queues).data_queues(&mock_L1D.queues).btb<legacy_integer_btb>()};

  const champsim::address ip{0x4000};
  const champsim::address target{0x5000};
  const champsim::address_space_id asid{3, 4};

  const auto [pred, taken] = uut.impl_btb_prediction(ip, BRANCH_CONDITIONAL, champsim::context_index{}, asid);
  CHECK(pred == champsim::address{0x4400});
  CHECK(taken);
  CHECK(calls == 1);

  uut.impl_update_btb(ip, target, true, BRANCH_CONDITIONAL, champsim::context_index{}, asid);
  CHECK(calls == 2);
}

TEST_CASE("A BTB with asid and legacy overloads is called once per hook")
{
  calls = 0;
  do_nothing_MRC mock_L1I, mock_L1D;
  O3_CPU uut{champsim::core_builder{}.fetch_queues(&mock_L1I.queues).data_queues(&mock_L1D.queues).btb<both_forms_btb>()};

  const champsim::address ip{0x4000};
  const champsim::address target{0x5000};
  const champsim::address_space_id asid{3, 4};

  const auto [pred, taken] = uut.impl_btb_prediction(ip, BRANCH_CONDITIONAL, champsim::context_index{}, asid);
  CHECK(pred == champsim::address{0x4400});
  CHECK(taken);
  CHECK(calls == 1);

  uut.impl_update_btb(ip, target, true, BRANCH_CONDITIONAL, champsim::context_index{}, asid);
  CHECK(calls == 2);
}

TEST_CASE("A BTB with context-index and legacy overloads is called once per hook")
{
  calls = 0;
  do_nothing_MRC mock_L1I, mock_L1D;
  O3_CPU uut{champsim::core_builder{}.fetch_queues(&mock_L1I.queues).data_queues(&mock_L1D.queues).btb<context_and_legacy_btb>()};

  const champsim::address ip{0x4000};
  const champsim::address target{0x5000};
  const champsim::address_space_id asid{3, 4};

  const auto [pred, taken] = uut.impl_btb_prediction(ip, BRANCH_CONDITIONAL, champsim::context_index{1}, asid);
  CHECK(pred == champsim::address{0x4400});
  CHECK(taken);
  CHECK(calls == 1);

  uut.impl_update_btb(ip, target, true, BRANCH_CONDITIONAL, champsim::context_index{1}, asid);
  CHECK(calls == 2);
}
