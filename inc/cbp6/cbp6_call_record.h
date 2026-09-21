/*
 * The on-the-wire record for one adapter->tenant call.
 *
 * Deliberately free of ChampSim dependencies: the replay tool under
 * tools/cbp6_replay/ links this together with a vendored predictor and nothing
 * else, so a dumped call log can be re-driven outside the simulator.
 */

#ifndef CBP6_CALL_RECORD_H
#define CBP6_CALL_RECORD_H

#include <cstdint>

namespace champsim::cbp6
{
enum class call_kind : uint8_t { predict = 0, history_update = 1, track_other = 2, update = 3 };

struct call_record {
  call_kind kind;
  int32_t brtype;
  bool pred_dir;
  bool resolve_dir;
  uint64_t seq_no;
  uint64_t pc;
  uint64_t next_pc;
};

// The log is written and read as raw records, so the layout must not drift.
static_assert(sizeof(call_record) == 40, "call_record layout changed; dumped logs are no longer readable");
} // namespace champsim::cbp6

#endif
