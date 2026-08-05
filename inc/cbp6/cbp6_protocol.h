/*
 * Protocol validation for the CBP6 adapter.
 *
 * The unit tests in 177-cbp6-host-protocol pin down the adapter's call sequence
 * on a handful of synthetic branches. This header lets the same rules be checked
 * against a real run -- tens of millions of branches -- where the interesting
 * failures live: a checkpoint that is never released, a sequence number that
 * drifts, a fall-through address that arrives as zero.
 *
 * The checker is deliberately a pure consumer of records, so it can be driven
 * either live from the host or offline from a dumped call log, and so it can be
 * tested without a simulator.
 */

#ifndef CBP6_PROTOCOL_H
#define CBP6_PROTOCOL_H

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "cbp6/cbp6_call_record.h"
#include "cbp6/cbp6_types.h"

namespace champsim::cbp6
{
// Checks the CBP6 call protocol over a stream of records. Feed every call in
// order, then call finish(); it returns false if anything was violated.
class protocol_checker
{
  std::vector<std::string> violations_;
  std::size_t records_{0};
  std::size_t conditional_{0};
  std::size_t other_{0};

  // The conditional branch currently between predict() and update().
  bool outstanding_{false};
  uint64_t out_seq_{0};
  uint64_t out_pc_{0};
  bool out_pred_{false};

  uint64_t last_seq_{0};
  bool seen_seq_{false};

  void fail(const std::string& what) { violations_.push_back(what); }

public:
  void observe(const call_record& rec)
  {
    ++records_;

    switch (rec.kind) {
    case call_kind::predict:
      if (outstanding_) {
        // The previous branch's checkpoint was never released by update().
        fail("predict at pc=" + std::to_string(rec.pc) + " while seq=" + std::to_string(out_seq_) + " is still outstanding (checkpoint leak)");
      }
      if (seen_seq_ && rec.seq_no <= last_seq_) {
        fail("sequence number " + std::to_string(rec.seq_no) + " did not increase past " + std::to_string(last_seq_));
      }
      last_seq_ = rec.seq_no;
      seen_seq_ = true;
      outstanding_ = true;
      out_seq_ = rec.seq_no;
      out_pc_ = rec.pc;
      out_pred_ = rec.pred_dir;
      break;

    case call_kind::history_update:
    case call_kind::update:
      if (!outstanding_) {
        fail("update-class call for seq=" + std::to_string(rec.seq_no) + " with no matching predict");
        break;
      }
      if (rec.seq_no != out_seq_) {
        fail("seq mismatch: predict was " + std::to_string(out_seq_) + ", update is " + std::to_string(rec.seq_no));
      }
      if (rec.pc != out_pc_) {
        fail("pc mismatch for seq=" + std::to_string(rec.seq_no));
      }
      if (rec.pred_dir != out_pred_) {
        fail("predicted direction changed between predict and update for seq=" + std::to_string(rec.seq_no));
      }
      // A not-taken conditional must carry a real fall-through: tenants test
      // `nextPC < PC` to detect backward branches, so zero reads as a loop.
      if (!rec.resolve_dir && rec.next_pc == 0) {
        fail("not-taken conditional at pc=" + std::to_string(rec.pc) + " has next_pc == 0");
      }
      if (rec.kind == call_kind::update) {
        outstanding_ = false; // the tenant releases its checkpoint here
        ++conditional_;
      }
      break;

    case call_kind::track_other:
      if ((rec.brtype & BRTYPE_COND) != 0) {
        fail("TrackOtherInst used for a conditional branch at pc=" + std::to_string(rec.pc));
      }
      ++other_;
      break;
    }
  }

  // Returns true if the whole stream was well formed.
  bool finish()
  {
    if (outstanding_) {
      fail("run ended with seq=" + std::to_string(out_seq_) + " still outstanding (checkpoint leak)");
      outstanding_ = false;
    }
    return violations_.empty();
  }

  [[nodiscard]] const std::vector<std::string>& violations() const { return violations_; }
  [[nodiscard]] std::size_t records() const { return records_; }
  [[nodiscard]] std::size_t conditional_branches() const { return conditional_; }
  [[nodiscard]] std::size_t other_branches() const { return other_; }
};

// Writes the call stream to a file so it can be replayed outside the simulator
// (see tools/cbp6_replay/). Capped, because a full run produces millions of
// records per second of simulated time.
class call_log_writer
{
  std::ofstream out_;
  std::size_t remaining_;

public:
  call_log_writer(const std::string& path, std::size_t cap) : out_(path, std::ios::binary), remaining_(cap) {}

  void write(const call_record& rec)
  {
    if (remaining_ == 0) {
      return;
    }
    out_.write(reinterpret_cast<const char*>(&rec), sizeof(rec)); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    --remaining_;
  }

  [[nodiscard]] bool exhausted() const { return remaining_ == 0; }
};

// Returns a writer when CBP6_CALL_DUMP names a path, else null. The cap defaults
// to 4M records (~160 MB) and can be overridden with CBP6_CALL_DUMP_MAX.
inline std::unique_ptr<call_log_writer> writer_from_env()
{
  // NOLINTNEXTLINE(concurrency-mt-unsafe): called once, during initialization
  const char* path = std::getenv("CBP6_CALL_DUMP");
  if (path == nullptr || *path == '\0') {
    return nullptr;
  }

  std::size_t cap = 4000000;
  // NOLINTNEXTLINE(concurrency-mt-unsafe): called once, during initialization
  if (const char* cap_str = std::getenv("CBP6_CALL_DUMP_MAX"); cap_str != nullptr && *cap_str != '\0') {
    cap = static_cast<std::size_t>(std::strtoull(cap_str, nullptr, 10));
  }
  return std::make_unique<call_log_writer>(path, cap);
}

// Returns a checker when CBP6_PROTOCOL_CHECK is set in the environment, else
// null. Off by default: the check costs a branch per call and its value is in
// deliberate validation runs, not in every simulation.
inline std::unique_ptr<protocol_checker> checker_from_env()
{
  // NOLINTNEXTLINE(concurrency-mt-unsafe): called once, during initialization
  if (const char* enabled = std::getenv("CBP6_PROTOCOL_CHECK"); enabled != nullptr && *enabled != '\0' && *enabled != '0') {
    return std::make_unique<protocol_checker>();
  }
  return nullptr;
}
} // namespace champsim::cbp6

#endif
