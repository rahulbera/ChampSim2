#include <catch.hpp>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "cache.h"
#include "instr.h"
#include "mocks.hpp"
#include "ooo_cpu.h"

// Branch predictors and BTBs written before the asid hook existed keep their
// legacy signatures: predict_branch/last_branch_result/branch_execute_resolve
// take the same arguments minus the trailing address_space_id, and
// btb_prediction takes an ip and a branch type. The dispatcher must still reach
// those modules, forwarding every argument unchanged, and must call a module
// exactly once when it offers both an asid (or context-index) and a legacy
// overload of one hook. A module that offers a context-index overload is
// called through it alone, whatever asid or legacy overloads it also offers.

namespace
{
int calls = 0; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
champsim::context_index last_context{}; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

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

struct context_forms_predictor : champsim::modules::branch_predictor {
  using branch_predictor::branch_predictor;
  bool predict_branch(champsim::address, champsim::address, bool, uint8_t, champsim::context_index context, champsim::address_space_id)
  {
    ++calls;
    last_context = context;
    return true;
  }
  bool predict_branch(champsim::address, champsim::address, bool, uint8_t) { return false; }
  void last_branch_result(champsim::address, champsim::address, bool, uint8_t, champsim::context_index context, champsim::address_space_id)
  {
    ++calls;
    last_context = context;
  }
  void last_branch_result(champsim::address, champsim::address, bool, uint8_t) { ++calls; }
  void branch_execute_resolve(uint64_t, champsim::address, champsim::address, bool, uint8_t, champsim::context_index context, champsim::address_space_id)
  {
    ++calls;
    last_context = context;
  }
  void branch_execute_resolve(uint64_t, champsim::address, champsim::address, bool, uint8_t) { ++calls; }
};

struct context_forms_btb : champsim::modules::btb {
  using btb::btb;
  std::pair<champsim::address, bool> btb_prediction(champsim::address, champsim::context_index context, champsim::address_space_id)
  {
    ++calls;
    last_context = context;
    return {champsim::address{0x4400}, true};
  }
  std::pair<champsim::address, bool> btb_prediction(champsim::address, uint8_t) { return {}; }
  void update_btb(champsim::address, champsim::address, bool, uint8_t, champsim::context_index context, champsim::address_space_id)
  {
    ++calls;
    last_context = context;
  }
  void update_btb(champsim::address, champsim::address, bool, uint8_t) { ++calls; }
};

struct context_and_asid_btb : champsim::modules::btb {
  using btb::btb;
  std::pair<champsim::address, bool> btb_prediction(champsim::address, champsim::context_index, champsim::address_space_id)
  {
    ++calls;
    return {champsim::address{0x4400}, true};
  }
  std::pair<champsim::address, bool> btb_prediction(champsim::address, champsim::address_space_id) { return {}; }
  void update_btb(champsim::address, champsim::address, bool, uint8_t, champsim::context_index context, champsim::address_space_id)
  {
    ++calls;
    last_context = context;
  }
  void update_btb(champsim::address, champsim::address, bool, uint8_t, champsim::address_space_id) { ++calls; }
  void update_btb(uint64_t, uint64_t, bool, uint8_t, champsim::address_space_id) { ++calls; }
};

struct context_and_integer_btb : champsim::modules::btb {
  using btb::btb;
  std::pair<champsim::address, bool> btb_prediction(champsim::address, champsim::context_index, champsim::address_space_id)
  {
    ++calls;
    return {champsim::address{0x4400}, true};
  }
  std::pair<champsim::address, bool> btb_prediction(uint64_t, uint8_t) { return {}; }
  void update_btb(champsim::address, champsim::address, bool, uint8_t, champsim::context_index, champsim::address_space_id) { ++calls; }
  void update_btb(uint64_t, uint64_t, bool, uint8_t) { ++calls; }
};

// Each hook records 10 * hook + the context it was given.
std::vector<int> seen; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

struct context_recording_predictor : champsim::modules::branch_predictor {
  using branch_predictor::branch_predictor;
  bool predict_branch(champsim::address, champsim::address, bool, uint8_t, champsim::context_index context, champsim::address_space_id)
  {
    seen.push_back(10 + context.value);
    return true;
  }
  void last_branch_result(champsim::address, champsim::address, bool, uint8_t, champsim::context_index context, champsim::address_space_id)
  {
    seen.push_back(20 + context.value);
  }
  void branch_execute_resolve(uint64_t, champsim::address, champsim::address, bool, uint8_t, champsim::context_index context, champsim::address_space_id)
  {
    seen.push_back(30 + context.value);
  }
};

struct context_recording_btb : champsim::modules::btb {
  using btb::btb;
  std::pair<champsim::address, bool> btb_prediction(champsim::address, champsim::context_index context, champsim::address_space_id)
  {
    seen.push_back(40 + context.value);
    return {champsim::address{0x9000}, true};
  }
  void update_btb(champsim::address, champsim::address, bool, uint8_t, champsim::context_index context, champsim::address_space_id)
  {
    seen.push_back(50 + context.value);
  }
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

  CHECK(uut.impl_predict_branch(ip, target, true, BRANCH_CONDITIONAL, champsim::context_index{}, asid));
  CHECK(calls == 1);

  uut.impl_last_branch_result(ip, target, true, BRANCH_CONDITIONAL, champsim::context_index{}, asid);
  CHECK(calls == 2);

  uut.impl_branch_execute_resolve(42, ip, target, true, BRANCH_CONDITIONAL, champsim::context_index{}, asid);
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

  CHECK(uut.impl_predict_branch(ip, target, true, BRANCH_CONDITIONAL, champsim::context_index{}, asid));
  CHECK(calls == 1);

  uut.impl_last_branch_result(ip, target, true, BRANCH_CONDITIONAL, champsim::context_index{}, asid);
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

  CHECK(uut.impl_predict_branch(ip, target, true, BRANCH_CONDITIONAL, champsim::context_index{}, asid));
  CHECK(calls == 1);

  uut.impl_last_branch_result(ip, target, true, BRANCH_CONDITIONAL, champsim::context_index{}, asid);
  CHECK(calls == 2);

  uut.impl_branch_execute_resolve(42, ip, target, true, BRANCH_CONDITIONAL, champsim::context_index{}, asid);
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

TEST_CASE("A branch predictor with context-index and legacy overloads is called once per hook")
{
  calls = 0;
  last_context = champsim::context_index{};
  do_nothing_MRC mock_L1I, mock_L1D;
  O3_CPU uut{champsim::core_builder{}.fetch_queues(&mock_L1I.queues).data_queues(&mock_L1D.queues).branch_predictor<context_forms_predictor>()};

  const champsim::address ip{0x4000};
  const champsim::address target{0x5000};
  const champsim::address_space_id asid{3, 4};

  CHECK(uut.impl_predict_branch(ip, target, true, BRANCH_CONDITIONAL, champsim::context_index{1}, asid));
  CHECK(calls == 1);
  CHECK(last_context.value == 1);

  uut.impl_last_branch_result(ip, target, true, BRANCH_CONDITIONAL, champsim::context_index{1}, asid);
  CHECK(calls == 2);
  CHECK(last_context.value == 1);

  uut.impl_branch_execute_resolve(42, ip, target, true, BRANCH_CONDITIONAL, champsim::context_index{1}, asid);
  CHECK(calls == 3);
  CHECK(last_context.value == 1);
}

TEST_CASE("A BTB with context-index and legacy overloads forwards the context once per hook")
{
  calls = 0;
  last_context = champsim::context_index{};
  do_nothing_MRC mock_L1I, mock_L1D;
  O3_CPU uut{champsim::core_builder{}.fetch_queues(&mock_L1I.queues).data_queues(&mock_L1D.queues).btb<context_forms_btb>()};

  const champsim::address ip{0x4000};
  const champsim::address target{0x5000};
  const champsim::address_space_id asid{3, 4};

  const auto [pred, taken] = uut.impl_btb_prediction(ip, BRANCH_CONDITIONAL, champsim::context_index{1}, asid);
  CHECK(pred == champsim::address{0x4400});
  CHECK(taken);
  CHECK(calls == 1);
  CHECK(last_context.value == 1);

  uut.impl_update_btb(ip, target, true, BRANCH_CONDITIONAL, champsim::context_index{1}, asid);
  CHECK(calls == 2);
  CHECK(last_context.value == 1);
}

TEST_CASE("A BTB with context-index and asid overloads is trained through its context form alone")
{
  calls = 0;
  last_context = champsim::context_index{};
  do_nothing_MRC mock_L1I, mock_L1D;
  O3_CPU uut{champsim::core_builder{}.fetch_queues(&mock_L1I.queues).data_queues(&mock_L1D.queues).btb<context_and_asid_btb>()};

  const champsim::address ip{0x4000};
  const champsim::address target{0x5000};
  const champsim::address_space_id asid{3, 4};

  const auto [pred, taken] = uut.impl_btb_prediction(ip, BRANCH_CONDITIONAL, champsim::context_index{1}, asid);
  CHECK(pred == champsim::address{0x4400});
  CHECK(taken);
  CHECK(calls == 1);

  // The strong-asid and raw-asid update_btb overloads must not run beside the
  // context form: one update is one call.
  uut.impl_update_btb(ip, target, true, BRANCH_CONDITIONAL, champsim::context_index{1}, asid);
  CHECK(calls == 2);
  CHECK(last_context.value == 1);
}

TEST_CASE("A BTB with context-index and integer legacy overloads is called once per hook")
{
  calls = 0;
  do_nothing_MRC mock_L1I, mock_L1D;
  O3_CPU uut{champsim::core_builder{}.fetch_queues(&mock_L1I.queues).data_queues(&mock_L1D.queues).btb<context_and_integer_btb>()};

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

TEST_CASE("The core passes each context's index to every branch predictor and BTB hook")
{
  // The cases above call O3_CPU::impl_* directly, so they check only the
  // forwarding. This one drives the core's own call sites: do_predict_branch
  // (btb_prediction, predict_branch, update_btb, last_branch_result) and
  // do_complete_execution (branch_execute_resolve), for a branch in context 1 and
  // then in context 0.
  do_nothing_MRC mock_L1I, mock_L1D;
  CACHE branch_l1i{champsim::cache_builder{}.sets(1).ways(1)};
  O3_CPU uut{champsim::core_builder{}
                 .fetch_queues(&mock_L1I.queues)
                 .data_queues(&mock_L1D.queues)
                 .l1i(&branch_l1i)
                 .num_contexts(2)
                 .ifetch_buffer_size(16)
                 .decode_buffer_size(16)
                 .dib_hit_buffer_size(16)
                 .dispatch_buffer_size(16)
                 .rob_size(16)
                 .lq_size(16)
                 .sq_size(16)
                 .branch_predictor<context_recording_predictor>()
                 .btb<context_recording_btb>()};
  uut.warmup = true;

  for (uint8_t i : {uint8_t{1}, uint8_t{0}}) {
    CAPTURE(static_cast<int>(i));
    seen.clear();
    auto instr = champsim::test::branch_instruction_with_ip(0x8000);
    instr.branch = BRANCH_DIRECT_JUMP;
    instr.branch_target = champsim::address{0x9000};
    instr.destination_registers.clear();
    instr.source_registers.clear();
    (void)uut.do_predict_branch(uut.contexts.at(i), champsim::context_index{i}, instr);
    uut.do_complete_execution(uut.contexts.at(i), instr);
    CHECK(seen == std::vector<int>{40 + i, 10 + i, 50 + i, 20 + i, 30 + i});
  }
}
