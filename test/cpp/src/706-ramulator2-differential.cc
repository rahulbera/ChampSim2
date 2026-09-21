// Differential oracle for the Ramulator2 request adapter.
//
// The PRODUCTION adapter (src/ramulator2_memory_backend.cc) runs over the REAL
// native driver. A logging decorator records every native attempt, its
// accept/reject result, every callback and every tick. An independent model,
// written from the documented request contract
// (docs/ramulator-integration/ramulator2-integration.md, "A request's lifetime")
// and not from the adapter source, predicts each
// operate: which fragment is sent next, when a queue stops, which responses
// appear, operate()'s progress value and every adapter counter.
//
// Independence: native decisions (accept or reject, callback timing) are the
// oracle's inputs, taken from the decorator log. No expected value is read
// from the adapter; the model checks only its public outputs -- upstream
// responses, feeder queues, operate() progress and statistics().
//
// The smoke case below runs in every enabled `make test`. The campaigns are
// hidden and configured by environment variables; test/ramulator2/
// run_differential.py drives them over native YAML variants:
//
//   DIFF_YAML         exported native YAML (required)
//   DIFF_TX           expected native transaction bytes (0: do not check)
//   DIFF_PERIOD       expected native clock period in ps (0: do not check)
//   DIFF_SEEDS        number of seeds (default 10); DIFF_SEED0 first seed (1)
//   DIFF_PARENTS      measured parents per seed (default 3000)
//   DIFF_FEEDERS      number of feeder channels (default 1)
//   DIFF_LATE_WARMUP  1 = insert a later warmup phase before the drain (1)
//   DIFF_STATS_EVERY  compare adapter counters every N operates (61)
//   DIFF_MAX_BURST    maximum ordinary burst (24)
//   DIFF_BACKLOG      pause the producer above this feeder backlog (160)
//   DIFF_NO_DRAIN     1 = finalize with live native work instead of draining
//   DIFF_OOR_PERCENT  share of packets that are PREFETCHes at or above native
//                     capacity, in any queue (default 4). Nonzero also mixes
//                     in-range PREFETCH packets into RQ and WQ; 0 reproduces
//                     the first-wave evaluator's packet streams exactly.
//
// The recovery campaign ([.differential-recovery]) repeats: an overload burst
// of DIFF_BURST_OPS operates, each adding 1..DIFF_BURST_SIZE packets; then the
// producer stops and the adapter operates until the model is quiescent (at
// most DIFF_RECOVERY_LIMIT operates), for DIFF_CYCLES cycles (defaults 40, 48,
// 4000000, 20). A new measured phase begins at every other quiescent point.
//
// Both campaigns first check, once per run, the requests that must stop the
// run instead (check_invalid_requests): invalid cores and out-of-range
// non-PREFETCH requests, in warmup and measured phases and every queue.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <regex>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <fmt/core.h>

#include "access_type.h"
#include "channel.h"
#include "defs.h"
#include "operable.h"
#include "ramulator2_driver.h"
#include "ramulator2_memory_backend.h"
#include "runtime_config.h"
#include "util/to_underlying.h"

namespace oracle706
{
struct failure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

uint64_t env_u64(const char* name, uint64_t fallback)
{
  const char* text = std::getenv(name);
  return (text != nullptr && *text != '\0') ? std::stoull(text) : fallback;
}

// ---------------------------------------------------------------- decorator
enum class kind { attempt_begin, attempt_end, callback, tick_begin, tick_end, reset, stats, finalize };
struct event {
  kind what;
  uint64_t attempt = 0;
  bool flag = false; // accepted (attempt_end) or synchronous inside its own send (callback)
  std::optional<uint64_t> during_send{};
  uint64_t tick = 0;
};
struct attempt_record {
  bool write = false;
  uint64_t address = 0;
  uint32_t cpu = 0;
  std::size_t bytes = 0;
  bool accepted = false;
  bool returned = false;
  uint64_t tick = 0;
  uint64_t callbacks = 0;
};
struct driver_log {
  std::vector<attempt_record> attempts;
  std::vector<event> events;
  std::optional<uint64_t> in_send;
  bool in_tick = false;
  bool finalized = false;
  uint64_t ticks = 0, resets = 0, finalizes = 0, sync_callbacks = 0, callbacks_after_finalize = 0, callbacks_outside = 0;
  uint64_t accepted_total = 0, callbacks_total = 0;
  // Every callback closure handed to the native driver holds a copy of this
  // token, so use_count() - 1 is the number the native side still keeps.
  std::shared_ptr<int> closure_token = std::make_shared<int>(0);
  uint64_t live_closures() const { return static_cast<uint64_t>(closure_token.use_count() - 1); }
};

class logging_driver final : public champsim::ramulator2_driver
{
  std::unique_ptr<champsim::ramulator2_driver> real;
  std::shared_ptr<driver_log> log;

public:
  logging_driver(std::unique_ptr<champsim::ramulator2_driver> real_, std::shared_ptr<driver_log> log_) : real(std::move(real_)), log(std::move(log_)) {}
  champsim::chrono::picoseconds clock_period() const override { return real->clock_period(); }
  champsim::data::bytes size() const override { return real->size(); }
  std::size_t transaction_bytes() const override { return real->transaction_bytes(); }
  bool send(bool write, uint64_t address, uint32_t cpu, std::size_t bytes, std::function<void()> done) override
  {
    const uint64_t index = log->attempts.size();
    log->attempts.push_back({write, address, cpu, bytes, false, false, log->ticks, 0});
    log->events.push_back({kind::attempt_begin, index, false, log->in_send, log->ticks});
    log->in_send = index;
    std::weak_ptr<driver_log> weak = log;
    auto wrapped = [weak, index, token = log->closure_token, done = std::move(done)]() {
      static_cast<void>(token);
      if (auto l = weak.lock()) {
        ++l->callbacks_total;
        const bool sync = l->in_send && *l->in_send == index;
        l->events.push_back({kind::callback, index, sync, l->in_send, l->ticks});
        ++l->attempts.at(index).callbacks;
        l->sync_callbacks += sync;
        l->callbacks_after_finalize += l->finalized;
        l->callbacks_outside += (!l->in_send && !l->in_tick);
      }
      if (done)
        done();
    };
    bool accepted = false;
    try {
      accepted = real->send(write, address, cpu, bytes, wrapped);
    } catch (...) {
      log->in_send.reset();
      throw;
    }
    log->in_send.reset();
    log->attempts[index].accepted = accepted;
    log->accepted_total += accepted;
    log->attempts[index].returned = true;
    log->events.push_back({kind::attempt_end, index, accepted, std::nullopt, log->ticks});
    return accepted;
  }
  void tick() override
  {
    log->events.push_back({kind::tick_begin, 0, false, log->in_send, log->ticks});
    log->in_tick = true;
    real->tick();
    log->in_tick = false;
    ++log->ticks;
    log->events.push_back({kind::tick_end, 0, false, std::nullopt, log->ticks});
  }
  void reset_stats() override
  {
    ++log->resets;
    log->events.push_back({kind::reset, 0, false, log->in_send, log->ticks});
    real->reset_stats();
  }
  champsim::native_memory_statistics statistics() override
  {
    log->events.push_back({kind::stats, 0, false, log->in_send, log->ticks});
    return real->statistics();
  }
  champsim::ramulator2_config_record config_record() const override { return real->config_record(); }
  void finalize() override
  {
    ++log->finalizes;
    log->finalized = true;
    log->events.push_back({kind::finalize, 0, false, log->in_send, log->ticks});
    real->finalize();
  }
};

// Captures what enclosed code prints to stdout -- the adapter's deadlock
// diagnostics -- and restores stdout even if that code throws.
class stdout_capture
{
  std::FILE* file_ = std::tmpfile();
  int saved_ = -1;
  void restore()
  {
    if (saved_ >= 0) {
      std::fflush(stdout);
      dup2(saved_, fileno(stdout));
      close(saved_);
      saved_ = -1;
    }
  }

public:
  stdout_capture()
  {
    if (file_ == nullptr)
      throw std::runtime_error("stdout_capture: cannot create a temporary file");
    std::fflush(stdout);
    saved_ = dup(fileno(stdout));
    if (saved_ < 0 || dup2(fileno(file_), fileno(stdout)) < 0)
      throw std::runtime_error("stdout_capture: cannot redirect stdout");
  }
  stdout_capture(const stdout_capture&) = delete;
  stdout_capture& operator=(const stdout_capture&) = delete;
  ~stdout_capture()
  {
    restore();
    std::fclose(file_);
  }
  std::string text()
  {
    restore();
    std::rewind(file_);
    std::string result;
    char buffer[4096];
    for (std::size_t n; (n = std::fread(buffer, 1, sizeof buffer, file_)) > 0;)
      result.append(buffer, n);
    return result;
  }
};

// ------------------------------------------------------------ reference model
using request_type = champsim::channel::request_type;
struct counters {
  uint64_t accepted_reads = 0, accepted_writes = 0, completed_reads = 0, completed_writes = 0;
  uint64_t accepted_fragments = 0, completed_fragments = 0, rejected_submissions = 0, out_of_range_prefetches = 0;
  uint64_t outstanding_parents = 0, outstanding_fragments = 0, total_read_latency_ps = 0, read_latency_samples = 0;
  // Native-side cross-check: fragments accepted in this phase's window.
  uint64_t read_fragments = 0, write_fragments = 0;
};
struct parent_model {
  uint64_t id = 0;
  request_type packet{};
  std::size_t feeder = 0;
  std::size_t queue = 0; // 0 RQ, 1 PQ, 2 WQ
  bool write = false;
  uint64_t block = 0;
  std::size_t fragments = 1;
  std::size_t accepted = 0, completed = 0;
  bool created = false;      // reached the head of its queue in a measured phase
  bool out_of_range = false; // a PREFETCH whose cache block is not wholly inside capacity
  uint64_t first_accept_op = 0;
  uint64_t first_accept_phase = 0;
  std::vector<bool> fragment_done{};
};
struct expected_response {
  uint64_t id;
  request_type packet;
};
struct totals {
  uint64_t parents = 0, measured_parents = 0, attempts = 0, accepted = 0, rejected = 0, sync = 0, responses = 0, write_parents = 0, suppressed_reads = 0;
  uint64_t oor_packets = 0, oor_responses = 0, oor_dropped = 0, oor_in_warmup = 0;
  std::vector<uint64_t> oor_by_queue = std::vector<uint64_t>(3, 0);
  uint64_t warmup_responses = 0, warmup_dropped = 0, partial_rejects = 0, first_rejects = 0, carry_over_completions = 0, late_warmup_retained = 0;
  uint64_t max_backlog = 0, max_outstanding = 0, ops = 0, stats_checks = 0;
  uint64_t max_stall_ops_with_work = 0, stall_ops_current = 0;
  uint64_t boundary_partial_heads = 0, boundary_rejected_heads = 0, boundary_live_parents = 0, warmup_partial_head_visits = 0, teardown_live_parents = 0;
  uint64_t requested_reads = 0; // response-requested packets outside WQ
  uint64_t recovery_cycles = 0, overloaded_cycles = 0, recovery_ops = 0, max_recovery_ops = 0, max_backlog_at_pause = 0, max_live_at_pause = 0;
  uint64_t invalid_request_cases = 0;
  std::vector<uint64_t> accepted_by_cpu = std::vector<uint64_t>(champsim::defs::num_cpus, 0);
};

struct config {
  std::string yaml;
  std::size_t tx = 0; // 0: do not check
  int64_t period = 0; // 0: do not check
  std::size_t feeders = 1;
  uint64_t parents = 3000;
  bool late_warmup = true;
  uint64_t stats_every = 61;
  uint64_t max_burst = 24;
  uint64_t backlog = 160;
  bool drain = true;
  uint64_t oor_percent = 4;
  uint64_t cycles = 20;
  uint64_t burst_ops = 40;
  uint64_t burst_size = 48;
  uint64_t recovery_limit = 4000000;
};

class harness
{
public:
  const config cfg;
  const uint64_t seed;
  std::mt19937_64 rng;
  std::shared_ptr<driver_log> log = std::make_shared<driver_log>();
  std::vector<champsim::channel> feeders;
  std::unique_ptr<champsim::memory_backend> backend;
  uint64_t capacity = 0;
  std::size_t tx = 0;
  int64_t period = 0;

  std::map<uint64_t, parent_model> parents;
  std::vector<std::deque<uint64_t>> queues;            // three per feeder, in visiting order
  std::vector<std::pair<uint64_t, std::size_t>> owner; // attempt -> (parent, fragment)
  counters model{};
  totals tot{};
  uint64_t op = 0, phase = 0, next_id = 1;
  bool warmup = true;
  std::optional<champsim::ramulator2_statistics> roi_copy;
  std::string phase_name = "construction";
  uint64_t op_events = 0; // accepted fragments + completed fragments + queue pops in this operate

  // generator state
  std::vector<uint64_t> hot;
  uint64_t prev_address = 0, idle_until = 0;

  harness(const config& c, uint64_t s) : cfg(c), seed(s), rng(s), feeders(c.feeders), queues(3 * c.feeders)
  {
    champsim::runtime_config rc;
    rc.set("ramulator2.config=" + cfg.yaml);
    auto real = champsim::make_ramulator2_driver(rc);
    tx = real->transaction_bytes();
    period = real->clock_period().count();
    if (cfg.tx != 0 && tx != cfg.tx)
      throw failure(fmt::format("driver transaction {} != expected {}", tx, cfg.tx));
    if (cfg.period != 0 && period != cfg.period)
      throw failure(fmt::format("driver period {} != expected {}", period, cfg.period));
    capacity = static_cast<uint64_t>(real->size().count());
    std::vector<champsim::channel*> ptrs;
    for (auto& f : feeders)
      ptrs.push_back(&f);
    backend = champsim::make_ramulator2_memory_backend(std::make_unique<logging_driver>(std::move(real), log), ptrs);
    const uint64_t blocks = capacity / BLOCK_SIZE;
    std::uniform_int_distribution<uint64_t> any(0, blocks - 1);
    for (int i = 0; i < 6; ++i)
      hot.push_back(any(rng) * BLOCK_SIZE);
    hot.push_back(0);
    hot.push_back(capacity - BLOCK_SIZE); // top in-range block
  }

  champsim::operable& memory() { return backend->clocked_component(); }

  [[noreturn]] void fail(const std::string& what) const
  {
    throw failure(fmt::format("seed {} phase {} ({}) op {} (native ticks {}): {}", seed, phase, phase_name, op, log->ticks, what));
  }
  void require(bool condition, const std::function<std::string()>& what) const
  {
    if (!condition)
      fail(what());
  }

  // The documented fragmentation: a 64-byte cache block becomes BLOCK_SIZE/tx
  // adjacent transactions, or one block-sized request when tx >= BLOCK_SIZE.
  std::size_t fragments() const { return std::max<std::size_t>(1, BLOCK_SIZE / tx); }
  std::size_t fragment_bytes() const { return std::min<std::size_t>(BLOCK_SIZE, tx); }

  uint64_t backlog() const
  {
    uint64_t total = 0;
    for (const auto& q : queues)
      total += q.size();
    return total;
  }

  // ----------------------------------------------------------- generator
  uint64_t pick_address()
  {
    std::uniform_int_distribution<int> pct(0, 99);
    std::uniform_int_distribution<uint64_t> offset(0, BLOCK_SIZE - 1);
    const uint64_t blocks = capacity / BLOCK_SIZE;
    const int choice = pct(rng);
    uint64_t block;
    if (choice < 30) {
      block = hot[std::uniform_int_distribution<std::size_t>(0, hot.size() - 1)(rng)];
    } else if (choice < 50) {
      const uint64_t prev = prev_address - prev_address % BLOCK_SIZE;
      const int delta = std::uniform_int_distribution<int>(-2, 2)(rng);
      const int64_t candidate = static_cast<int64_t>(prev) + delta * static_cast<int64_t>(BLOCK_SIZE);
      block = (candidate < 0 || static_cast<uint64_t>(candidate) >= blocks * BLOCK_SIZE) ? prev : static_cast<uint64_t>(candidate);
    } else if (choice < 60) {
      return prev_address; // exact repeat
    } else {
      block = std::uniform_int_distribution<uint64_t>(0, blocks - 1)(rng) * BLOCK_SIZE;
    }
    return block + offset(rng);
  }

  // A physical prefetcher's request past the top of native capacity: the
  // first block past the end (next_line after the top frame), up to 256 blocks
  // ahead (va_ampm_lite), anywhere above, or at the top of the address space.
  uint64_t pick_out_of_range_address()
  {
    std::uniform_int_distribution<int> pct(0, 99);
    std::uniform_int_distribution<uint64_t> offset(0, BLOCK_SIZE - 1);
    const int choice = pct(rng);
    if (choice < 40)
      return capacity + offset(rng);
    if (choice < 70)
      return capacity + std::uniform_int_distribution<uint64_t>(1, 256)(rng) * BLOCK_SIZE + offset(rng);
    if (choice < 85)
      return std::uniform_int_distribution<uint64_t>(capacity, std::numeric_limits<uint64_t>::max())(rng);
    return std::numeric_limits<uint64_t>::max() - offset(rng);
  }

  void add_packet(bool measured)
  {
    std::uniform_int_distribution<int> pct(0, 99);
    const uint64_t id = next_id++;
    request_type r;
    const bool out_of_range = cfg.oor_percent != 0 && static_cast<uint64_t>(pct(rng)) < cfg.oor_percent;
    const auto address = out_of_range ? pick_out_of_range_address() : pick_address();
    if (!out_of_range)
      prev_address = address;
    r.address = champsim::address{address};
    r.v_address = champsim::address{0x7f0000000000ULL + id * 64};
    r.data = champsim::address{id * 0x9e3779b97f4a7c15ULL};
    r.pf_metadata = static_cast<uint32_t>(id);
    r.instr_id = id;
    r.instr_depend_on_me = {id, id ^ 0xabcdefULL};
    r.cpu = std::uniform_int_distribution<uint32_t>(0, static_cast<uint32_t>(champsim::defs::num_cpus - 1))(rng);
    r.response_requested = pct(rng) < 75;
    const int qsel = pct(rng);
    const std::size_t queue = qsel < 45 ? 0 : (qsel < 65 ? 1 : 2);
    r.type = queue == 2 ? access_type::WRITE : (queue == 1 ? access_type::PREFETCH : (pct(rng) < 20 ? access_type::RFO : access_type::LOAD));
    // The adapter's policy keys on the packet type, not the queue: a
    // prefetch-as-load cache sends PREFETCH packets through RQ.
    if (cfg.oor_percent != 0 && queue != 1 && pct(rng) < 10)
      r.type = access_type::PREFETCH;
    if (out_of_range)
      r.type = access_type::PREFETCH;
    const std::size_t feeder = std::uniform_int_distribution<std::size_t>(0, cfg.feeders - 1)(rng);
    const bool ok = queue == 0 ? feeders[feeder].add_rq(r) : (queue == 1 ? feeders[feeder].add_pq(r) : feeders[feeder].add_wq(r));
    require(ok, [] { return std::string{"unbounded feeder rejected a packet"}; });
    parent_model p;
    p.id = id;
    p.packet = r;
    p.feeder = feeder;
    p.queue = queue;
    p.write = queue == 2;
    p.block = address - address % BLOCK_SIZE;
    p.fragments = fragments();
    p.fragment_done.assign(p.fragments, false);
    // The documented range rule, applied by the model itself.
    p.out_of_range = r.type == access_type::PREFETCH && (p.block >= capacity || BLOCK_SIZE > capacity - p.block);
    parents.emplace(id, std::move(p));
    queues[3 * feeder + queue].push_back(id);
    ++tot.parents;
    tot.measured_parents += measured;
    tot.write_parents += queue == 2;
    tot.suppressed_reads += (queue != 2 && !r.response_requested);
    tot.requested_reads += (queue != 2 && r.response_requested);
  }

  void generate(bool measured)
  {
    if (op < idle_until || backlog() >= cfg.backlog)
      return;
    std::uniform_int_distribution<int> pct(0, 999);
    if (pct(rng) < 3) {
      idle_until = op + std::uniform_int_distribution<uint64_t>(20, 3000)(rng);
      return;
    }
    if (pct(rng) >= 250)
      return;
    uint64_t burst = std::uniform_int_distribution<uint64_t>(1, cfg.max_burst)(rng);
    if (pct(rng) < 40)
      burst = std::uniform_int_distribution<uint64_t>(40, 120)(rng); // exceeds native buffers
    for (uint64_t i = 0; i < burst; ++i)
      add_packet(measured);
  }

  // ------------------------------------------------------------- checker
  void complete(uint64_t attempt, std::vector<std::vector<expected_response>>& expected)
  {
    require(attempt < owner.size() && owner[attempt].first != 0, [&] { return fmt::format("callback for attempt {} that was never accepted", attempt); });
    // Not a structured binding: the lambdas below refer to these, which Clang
    // accepts only from 16 (C++20 allows it; C++17 does not).
    const auto pid = owner[attempt].first;
    const auto fragment = owner[attempt].second;
    auto found = parents.find(pid);
    require(found != parents.end(), [&] { return fmt::format("callback for attempt {} of already completed parent {}", attempt, pid); });
    auto& p = found->second;
    require(!p.fragment_done[fragment], [&] { return fmt::format("second callback for parent {} fragment {} (attempt {})", pid, fragment, attempt); });
    p.fragment_done[fragment] = true;
    ++op_events;
    ++p.completed;
    ++model.completed_fragments;
    if (p.completed == p.fragments) {
      if (p.first_accept_phase != phase)
        ++tot.carry_over_completions;
      if (p.write) {
        ++model.completed_writes;
      } else {
        // Latency runs from the operate that accepted the first fragment to the
        // operate whose callback completed the last one.
        ++model.completed_reads;
        model.total_read_latency_ps += (op - p.first_accept_op) * static_cast<uint64_t>(period);
        ++model.read_latency_samples;
        if (p.packet.response_requested)
          expected[p.feeder].push_back({pid, p.packet});
      }
      parents.erase(found);
    }
  }

  const event& next_event(std::size_t& e) const
  {
    require(e < log->events.size(), [&] { return fmt::format("missing driver event (expected more after event {})", e); });
    return log->events[e++];
  }

  // Submit the model's view of one queue: the FIFO prefix up to the first
  // rejected fragment, with synchronous callbacks processed after the head's
  // admission bookkeeping.
  void model_queue(std::size_t q, std::size_t& e, std::vector<std::vector<expected_response>>& expected)
  {
    auto& dq = queues[q];
    while (!dq.empty()) {
      auto& p = parents.at(dq.front());
      if (!p.created) {
        if (p.out_of_range) {
          // In either phase: a requested read is answered at once with the
          // original packet; a suppressed read or a write gets nothing. No
          // parent, native attempt or latency sample; counted and progress.
          if (!p.write && p.packet.response_requested) {
            expected[p.feeder].push_back({p.id, p.packet});
            ++tot.oor_responses;
          } else {
            ++tot.oor_dropped;
          }
          ++model.out_of_range_prefetches;
          ++tot.oor_packets;
          tot.oor_in_warmup += warmup;
          ++tot.oor_by_queue.at(p.queue);
          parents.erase(p.id);
          dq.pop_front();
          ++op_events;
          continue;
        }
        if (warmup) {
          // Fast warmup returns a requested read at once and drops the rest.
          if (!p.write && p.packet.response_requested) {
            expected[p.feeder].push_back({p.id, p.packet});
            ++tot.warmup_responses;
          } else {
            ++tot.warmup_dropped;
          }
          parents.erase(p.id);
          dq.pop_front();
          ++op_events;
          continue;
        }
        p.created = true;
      }
      if (warmup) {
        // A later warmup neither bypasses nor continues a retained head.
        ++tot.late_warmup_retained;
        tot.warmup_partial_head_visits += p.accepted > 0;
        break;
      }
      std::vector<uint64_t> sync;
      while (p.accepted < p.fragments) {
        const auto& begin = next_event(e);
        require(begin.what == kind::attempt_begin, [&] {
          return fmt::format("expected send for parent {} fragment {} (queue {}), got event kind {} attempt {}", p.id, p.accepted, q,
                             static_cast<int>(begin.what), begin.attempt);
        });
        const auto index = begin.attempt;
        const auto& a = log->attempts[index];
        const uint64_t address = p.block + p.accepted * tx;
        require(a.write == p.write && a.address == address && a.cpu == p.packet.cpu && a.bytes == fragment_bytes(), [&] {
          return fmt::format(
              "send mismatch for parent {} (queue {}, packet address {:#x}) fragment {}: expected (w={} {:#x} cpu{} {}B) got (w={} {:#x} cpu{} {}B)", p.id, q,
              p.packet.address.to<uint64_t>(), p.accepted, p.write, address, p.packet.cpu, fragment_bytes(), a.write, a.address, a.cpu, a.bytes);
        });
        bool accepted = false;
        for (;;) {
          const auto& x = next_event(e);
          if (x.what == kind::callback) {
            require(x.flag && x.attempt == index, [&] { return fmt::format("callback for attempt {} nested inside send {}", x.attempt, index); });
            sync.push_back(x.attempt);
            continue;
          }
          require(x.what == kind::attempt_end && x.attempt == index, [&] { return fmt::format("unexpected event inside send {}", index); });
          accepted = x.flag;
          break;
        }
        ++tot.attempts;
        if (!accepted) {
          require(sync.empty() || sync.back() != index, [&] { return fmt::format("rejected attempt {} produced a callback", index); });
          ++model.rejected_submissions;
          ++tot.rejected;
          (p.accepted == 0 ? tot.first_rejects : tot.partial_rejects) += 1;
          break;
        }
        ++tot.accepted;
        ++tot.accepted_by_cpu.at(a.cpu);
        if (p.accepted == 0) {
          p.first_accept_op = op;
          p.first_accept_phase = phase;
          ++(p.write ? model.accepted_writes : model.accepted_reads);
        }
        if (owner.size() <= index)
          owner.resize(index + 1, {0, 0});
        owner[index] = {p.id, p.accepted};
        ++p.accepted;
        ++op_events;
        ++model.accepted_fragments;
        ++(p.write ? model.write_fragments : model.read_fragments);
      }
      const bool all = p.accepted == p.fragments;
      if (all)
        dq.pop_front();
      tot.sync += sync.size();
      for (auto s : sync)
        complete(s, expected); // may erase p
      if (!all)
        break;
    }
  }

  void check_operate(std::size_t start)
  {
    op_events = 0;
    std::size_t e = start;
    std::vector<std::vector<expected_response>> expected(cfg.feeders);
    for (std::size_t q = 0; q < queues.size(); ++q)
      model_queue(q, e, expected);

    // Exactly one native tick follows the admissions; callbacks inside it complete.
    const auto& tb = next_event(e);
    require(tb.what == kind::tick_begin, [&] {
      return fmt::format("expected exactly the native tick after admissions; got event kind {} (attempt {})", static_cast<int>(tb.what), tb.attempt);
    });
    for (;;) {
      const auto& x = next_event(e);
      if (x.what == kind::callback) {
        require(!x.during_send, [&] { return std::string{"callback during send inside tick"}; });
        complete(x.attempt, expected);
        continue;
      }
      require(x.what == kind::tick_end, [&] { return fmt::format("unexpected event kind {} inside native tick", static_cast<int>(x.what)); });
      break;
    }
    require(e == log->events.size(), [&] {
      return fmt::format("{} unexpected driver events after the native tick (first kind {})", log->events.size() - e, static_cast<int>(log->events[e].what));
    });

    // Upstream responses: the exact sequence per feeder, with original packet contents.
    for (std::size_t f = 0; f < cfg.feeders; ++f) {
      auto& returned = feeders[f].returned;
      const auto& exp = expected[f];
      for (std::size_t i = 0; i < std::max(returned.size(), exp.size()); ++i) {
        require(i < returned.size(), [&] {
          return fmt::format("feeder {}: missing response #{} for parent {} (address {:#x})", f, i, exp[i].id, exp[i].packet.address.to<uint64_t>());
        });
        require(i < exp.size(), [&] {
          return fmt::format("feeder {}: unexpected extra response pf_metadata {} address {:#x}", f, returned[i].pf_metadata,
                             returned[i].address.to<uint64_t>());
        });
        const auto& got = returned[i];
        const auto& want = exp[i].packet;
        require(got.address == want.address && got.v_address == want.v_address && got.data == want.data && got.pf_metadata == want.pf_metadata
                    && got.instr_depend_on_me == want.instr_depend_on_me,
                [&] { return fmt::format("feeder {}: response #{} is pf_metadata {} but expected parent {}", f, i, got.pf_metadata, exp[i].id); });
      }
      tot.responses += returned.size();
      returned.clear();
    }
    // The real feeder queues mirror the model's.
    for (std::size_t f = 0; f < cfg.feeders; ++f) {
      const std::deque<request_type>* real[3] = {&feeders[f].RQ, &feeders[f].PQ, &feeders[f].WQ};
      for (std::size_t q = 0; q < 3; ++q) {
        const auto& mq = queues[3 * f + q];
        require(real[q]->size() == mq.size(), [&] { return fmt::format("feeder {} queue {} holds {} packets, model {}", f, q, real[q]->size(), mq.size()); });
        if (!mq.empty())
          require(real[q]->front().pf_metadata == static_cast<uint32_t>(mq.front()), [&] { return fmt::format("feeder {} queue {} head mismatch", f, q); });
      }
    }
  }

  counters gauges() const
  {
    counters c = model;
    for (const auto& [id, p] : parents) {
      if (p.accepted != 0) {
        ++c.outstanding_parents;
        c.outstanding_fragments += p.accepted - p.completed;
      }
    }
    return c;
  }

  static const champsim::native_statistic* find_native(const champsim::native_memory_statistics& n, const std::vector<std::string>& path)
  {
    for (const auto& v : n.values)
      if (v.path == path)
        return &v;
    return nullptr;
  }

  void compare(const champsim::ramulator2_statistics& s, const counters& c, const char* label) const
  {
    const auto mismatch = [&](const char* field, uint64_t got, uint64_t want) {
      if (got != want)
        fail(fmt::format("{}: adapter {} = {}, independent model = {}", label, field, got, want));
    };
    mismatch("accepted_reads", s.accepted_reads, c.accepted_reads);
    mismatch("accepted_writes", s.accepted_writes, c.accepted_writes);
    mismatch("completed_reads", s.completed_reads, c.completed_reads);
    mismatch("completed_writes", s.completed_writes, c.completed_writes);
    mismatch("accepted_fragments", s.accepted_fragments, c.accepted_fragments);
    mismatch("completed_fragments", s.completed_fragments, c.completed_fragments);
    mismatch("rejected_submissions", s.rejected_submissions, c.rejected_submissions);
    mismatch("out_of_range_prefetches", s.out_of_range_prefetches, c.out_of_range_prefetches);
    mismatch("outstanding_parents", s.outstanding_parents, c.outstanding_parents);
    mismatch("outstanding_fragments", s.outstanding_fragments, c.outstanding_fragments);
    mismatch("total_read_latency_ps", s.total_read_latency_ps, c.total_read_latency_ps);
    mismatch("read_latency_samples", s.read_latency_samples, c.read_latency_samples);
  }

  static counters snapshot_counters(const champsim::ramulator2_statistics& r)
  {
    counters frozen{};
    frozen.accepted_reads = r.accepted_reads;
    frozen.accepted_writes = r.accepted_writes;
    frozen.completed_reads = r.completed_reads;
    frozen.completed_writes = r.completed_writes;
    frozen.accepted_fragments = r.accepted_fragments;
    frozen.completed_fragments = r.completed_fragments;
    frozen.rejected_submissions = r.rejected_submissions;
    frozen.out_of_range_prefetches = r.out_of_range_prefetches;
    frozen.outstanding_parents = r.outstanding_parents;
    frozen.outstanding_fragments = r.outstanding_fragments;
    frozen.total_read_latency_ps = r.total_read_latency_ps;
    frozen.read_latency_samples = r.read_latency_samples;
    return frozen;
  }

  void check_stats(const char* label)
  {
    ++tot.stats_checks;
    const auto before = log->events.size();
    const auto stats = backend->statistics();
    require(stats.sim_ramulator2.has_value(), [] { return std::string{"no sim statistics"}; });
    const auto c = gauges();
    compare(*stats.sim_ramulator2, c, label);
    tot.max_outstanding = std::max(tot.max_outstanding, c.outstanding_parents);
    // GenericDRAM counts accepted fragments since its last reset.
    const auto* nr = find_native(stats.sim_ramulator2->native, {"memory_system", "total_num_read_requests"});
    const auto* nw = find_native(stats.sim_ramulator2->native, {"memory_system", "total_num_write_requests"});
    require(nr != nullptr && nw != nullptr, [] { return std::string{"native request counters missing"}; });
    require(std::get<int64_t>(nr->value) == static_cast<int64_t>(c.read_fragments) && std::get<int64_t>(nw->value) == static_cast<int64_t>(c.write_fragments),
            [&] {
              return fmt::format("{}: native accepted read/write fragments {}/{} != model {}/{}", label, std::get<int64_t>(nr->value),
                                 std::get<int64_t>(nw->value), c.read_fragments, c.write_fragments);
            });
    // A frozen ROI snapshot does not move until the next end_phase.
    if (roi_copy) {
      require(stats.roi_ramulator2.has_value(), [] { return std::string{"ROI snapshot disappeared"}; });
      compare(*stats.roi_ramulator2, snapshot_counters(*roi_copy), "roi snapshot changed");
      require(stats.roi_ramulator2->native.yaml == roi_copy->native.yaml, [] { return std::string{"ROI native YAML changed after freeze"}; });
    }
    // statistics() itself never submits, ticks or calls back.
    for (auto i = before; i < log->events.size(); ++i)
      require(log->events[i].what == kind::stats,
              [&] { return fmt::format("statistics() produced driver event kind {}", static_cast<int>(log->events[i].what)); });
  }

  void step(bool generate_traffic, bool measured)
  {
    if (generate_traffic)
      generate(measured);
    tot.max_backlog = std::max(tot.max_backlog, backlog());
    for (auto& f : feeders)
      require(f.returned.empty(), [] { return std::string{"response appeared outside operate"}; });
    const auto start = log->events.size();
    ++op;
    ++tot.ops;
    long progress = 0;
    const bool had_work = std::any_of(parents.begin(), parents.end(), [](const auto& kv) { return kv.second.accepted > 0; }) || backlog() != 0;
    try {
      progress = memory()._operate();
    } catch (const failure&) {
      throw;
    } catch (const std::exception& error) {
      fail(std::string{"adapter threw: "} + error.what());
    }
    require(memory().current_time.time_since_epoch().count() == static_cast<int64_t>(op) * period, [] { return std::string{"adapter time origin moved"}; });
    check_operate(start);
    require(progress == static_cast<long>(op_events),
            [&] { return fmt::format("operate reported progress {} but {} admissions/completions/queue pops happened", progress, op_events); });
    if (progress == 0 && had_work) {
      ++tot.stall_ops_current;
      tot.max_stall_ops_with_work = std::max(tot.max_stall_ops_with_work, tot.stall_ops_current);
    } else {
      tot.stall_ops_current = 0;
    }
    if (cfg.stats_every != 0 && op % cfg.stats_every == 0)
      check_stats("periodic");
  }

  void begin(bool is_warmup, const char* name)
  {
    for (const auto& q : queues) {
      if (!q.empty()) {
        const auto& head = parents.at(q.front());
        tot.boundary_partial_heads += head.created && head.accepted > 0;
        tot.boundary_rejected_heads += head.created && head.accepted == 0;
      }
    }
    for (const auto& [id, p] : parents)
      tot.boundary_live_parents += p.accepted > 0;
    ++phase;
    phase_name = name;
    const auto start = log->events.size();
    const auto resets = log->resets;
    warmup = is_warmup;
    memory().warmup = is_warmup;
    memory().begin_phase();
    require(log->resets == resets + 1, [] { return std::string{"begin_phase did not reset native statistics exactly once"}; });
    for (auto i = start; i < log->events.size(); ++i)
      require(log->events[i].what == kind::reset || log->events[i].what == kind::stats, [] { return std::string{"begin_phase submitted or ticked"}; });
    // Event counters reset; gauges (derived from live parents) carry over.
    model = counters{};
    for (auto& f : feeders)
      require(f.sim_stats.RQ_ACCESS == 0 && f.roi_stats.RQ_ACCESS == 0 && f.sim_stats.WQ_ACCESS == 0, [] { return std::string{"feeder stats not reset"}; });
    check_stats("after begin_phase");
  }

  void end(unsigned cpu)
  {
    const auto start = log->events.size();
    memory().end_phase(cpu);
    for (auto i = start; i < log->events.size(); ++i)
      require(log->events[i].what == kind::stats, [] { return std::string{"end_phase submitted or ticked"}; });
    const auto stats = backend->statistics();
    require(stats.roi_ramulator2.has_value(), [] { return std::string{"end_phase did not freeze ROI"}; });
    compare(*stats.roi_ramulator2, gauges(), "roi snapshot at end_phase");
    roi_copy = stats.roi_ramulator2;
    check_stats("after end_phase");
  }

  void end_all_cpus()
  {
    for (unsigned cpu = 0; cpu < champsim::defs::num_cpus; ++cpu)
      end(cpu);
  }

  void drain_to_quiescence(const char* what, uint64_t limit)
  {
    uint64_t guard = 0;
    while (backlog() != 0 || !parents.empty()) {
      step(false, false);
      require(++guard < limit,
              [&] { return fmt::format("{}: did not drain in {} operates: backlog {} live parents {}", what, limit, backlog(), parents.size()); });
    }
  }

  void check_teardown_invariants()
  {
    for (std::size_t i = 0; i < log->attempts.size(); ++i) {
      const auto& a = log->attempts[i];
      require(a.returned, [&] { return fmt::format("attempt {} never returned", i); });
      require(a.callbacks == (a.accepted ? 1U : 0U), [&] { return fmt::format("attempt {} (accepted={}) has {} callbacks", i, a.accepted, a.callbacks); });
    }
    require(log->callbacks_outside == 0, [] { return std::string{"callback outside send/tick"}; });
  }

  void finalize_and_destroy()
  {
    const auto ticks = log->ticks;
    backend->finalize();
    backend->finalize();
    require(log->finalizes == 1 && log->ticks == ticks, [] { return std::string{"finalize not once, or ticked"}; });
    for (auto& f : feeders)
      require(f.returned.empty(), [] { return std::string{"finalize produced responses"}; });
    bool threw = false;
    try {
      memory()._operate();
    } catch (const std::exception&) {
      threw = true;
    }
    require(threw && log->ticks == ticks, [] { return std::string{"operate after finalize did not fail cleanly"}; });
    backend.reset();
    require(log->callbacks_after_finalize == 0, [] { return std::string{"native callback after finalize"}; });
  }

  // The documented validation in front of every queue policy: a packet from an
  // invalid core, or a non-PREFETCH packet whose cache block is not wholly
  // inside capacity, stops the run with an error in warmup and measured phases
  // and from any queue. It is never submitted natively, never answered, and
  // never handled as an out-of-range prefetch: an invalid core stops even a
  // PREFETCH. The operate that throws is left unfinished, so this runs on a
  // quiescent adapter that is only finalized or destroyed afterwards.
  void expect_stopped(std::size_t queue, access_type type, uint64_t address, uint32_t cpu)
  {
    require(backlog() == 0 && parents.empty(), [] { return std::string{"an invalid request must be checked on a quiescent adapter"}; });
    request_type r;
    r.address = champsim::address{address};
    r.v_address = champsim::address{address};
    r.type = type;
    r.cpu = cpu;
    r.response_requested = true;
    const bool ok = queue == 0 ? feeders[0].add_rq(r) : (queue == 1 ? feeders[0].add_pq(r) : feeders[0].add_wq(r));
    require(ok, [] { return std::string{"unbounded feeder rejected a packet"}; });
    const auto what = [&] {
      return fmt::format("{} request in queue {} at {:#x} from core {}", access_type_names.at(champsim::to_underlying(type)), queue, address, cpu);
    };
    const auto start = log->events.size();
    std::string message;
    try {
      memory()._operate();
    } catch (const std::exception& error) {
      message = error.what();
    }
    require(!message.empty(), [&] { return fmt::format("{} did not stop the run", what()); });
    const std::string reason = cpu >= champsim::defs::num_cpus ? "core id" : "out of range";
    require(message.find(reason) != std::string::npos, [&] { return fmt::format("{} stopped the run for another reason: {}", what(), message); });
    for (auto i = start; i < log->events.size(); ++i)
      require(log->events[i].what != kind::attempt_begin,
              [&] { return fmt::format("{} reached the native driver as attempt {} before stopping: {}", what(), log->events[i].attempt, message); });
    require(feeders[0].returned.empty(), [&] { return fmt::format("{} was answered before stopping: {}", what(), message); });
  }

  // The differential scenario: warmup with traffic, two measured phases around
  // a zero-length phase, staggered per-CPU ROI ends, a later warmup that must
  // retain partial heads, then a drain (or finalization with live work).
  void run()
  {
    const auto measured_half = cfg.parents / 2;
    begin(true, "warmup0");
    for (int i = 0; i < 400; ++i)
      step(true, false);
    end_all_cpus();
    begin(false, "measured1");
    const auto base = tot.measured_parents;
    while (tot.measured_parents - base < measured_half)
      step(true, true);
    end_all_cpus();
    begin(false, "zero-length");
    end_all_cpus();
    begin(false, "measured2");
    bool first_cpu_done = false;
    while (tot.measured_parents - base < cfg.parents) {
      step(true, true);
      // CPU 0 freezes the ROI early; the rest keep running.
      if (!first_cpu_done && tot.measured_parents - base >= measured_half + cfg.parents / 4) {
        end(0);
        first_cpu_done = true;
      }
    }
    for (unsigned cpu = 1; cpu < champsim::defs::num_cpus; ++cpu)
      end(cpu);
    if (!first_cpu_done)
      end(0);
    if (cfg.late_warmup) {
      begin(true, "warmup-late");
      for (int i = 0; i < 300; ++i)
        step(true, false);
      end_all_cpus();
    }
    if (!cfg.drain) {
      for (const auto& [id, p] : parents)
        tot.teardown_live_parents += p.accepted > 0;
      finalize_and_destroy();
      return;
    }
    begin(false, "drain");
    drain_to_quiescence("drain", 2000000);
    for (int i = 0; i < 50; ++i)
      step(false, false);
    check_stats("drained");
    end_all_cpus();
    check_teardown_invariants();
    finalize_and_destroy();
  }

  champsim::ramulator2_statistics adapter_statistics()
  {
    const auto before = log->events.size();
    auto stats = backend->statistics().sim_ramulator2;
    require(stats.has_value(), [] { return std::string{"no sim statistics"}; });
    for (auto i = before; i < log->events.size(); ++i)
      require(log->events[i].what == kind::stats,
              [&] { return fmt::format("statistics() produced driver event kind {}", static_cast<int>(log->events[i].what)); });
    return *stats;
  }

  // At a quiescent point after the producer stopped: exact accounting for the
  // cycle, and nothing left anywhere a request could still live.
  void check_recovered(const champsim::ramulator2_statistics& s0, const totals& t0)
  {
    for (std::size_t f = 0; f < cfg.feeders; ++f) {
      const auto& feeder = feeders[f];
      require(feeder.RQ.empty() && feeder.PQ.empty() && feeder.WQ.empty() && feeder.returned.empty(), [&] {
        return fmt::format("recovery: feeder {} still holds RQ {} PQ {} WQ {} returned {}", f, feeder.RQ.size(), feeder.PQ.size(), feeder.WQ.size(),
                           feeder.returned.size());
      });
    }
    const auto s1 = adapter_statistics();
    require(s1.outstanding_parents == 0 && s1.outstanding_fragments == 0, [&] {
      return fmt::format("recovery: adapter still reports {} outstanding parents and {} outstanding fragments", s1.outstanding_parents,
                         s1.outstanding_fragments);
    });
    // The phase began at a quiescent point, so the adapter's own counters balance.
    require(s1.accepted_reads == s1.completed_reads && s1.accepted_writes == s1.completed_writes && s1.accepted_fragments == s1.completed_fragments, [&] {
      return fmt::format("recovery: adapter counters do not balance: reads {}/{} writes {}/{} fragments {}/{}", s1.accepted_reads, s1.completed_reads,
                         s1.accepted_writes, s1.completed_writes, s1.accepted_fragments, s1.completed_fragments);
    });
    const uint64_t added = tot.parents - t0.parents;
    const uint64_t completed = (s1.completed_reads + s1.completed_writes) - (s0.completed_reads + s0.completed_writes);
    const uint64_t out_of_range = s1.out_of_range_prefetches - s0.out_of_range_prefetches;
    require(completed + out_of_range == added, [&] {
      return fmt::format("recovery: {} packets added this cycle, adapter completed {} parents and popped {} out-of-range prefetches", added, completed,
                         out_of_range);
    });
    require(tot.responses - t0.responses == tot.requested_reads - t0.requested_reads, [&] {
      return fmt::format("recovery: {} upstream responses for {} response-requested reads", tot.responses - t0.responses,
                         tot.requested_reads - t0.requested_reads);
    });
    require(s1.accepted_fragments > s0.accepted_fragments, [] { return std::string{"recovery: the burst admitted no native fragment"}; });

    // Native side: every accepted fragment called back, and no callback
    // closure is still held anywhere below the driver interface.
    require(log->accepted_total == log->callbacks_total,
            [&] { return fmt::format("recovery: {} accepted native requests have not called back", log->accepted_total - log->callbacks_total); });
    require(log->live_closures() == 0, [&] { return fmt::format("recovery: the native side still holds {} callback closures", log->live_closures()); });

    // The adapter's own diagnostics: no live parent, fragment, queued packet
    // or retained queue head.
    const auto events = log->events.size();
    std::string report;
    {
      stdout_capture capture;
      memory().print_deadlock();
      report = capture.text();
    }
    require(log->events.size() == events, [] { return std::string{"print_deadlock reached the native driver"}; });
    std::smatch totals_line;
    require(std::regex_search(report, totals_line, std::regex{R"((\d+) outstanding parents, (\d+) outstanding fragments)"}) && totals_line[1] == "0"
                && totals_line[2] == "0",
            [&] { return fmt::format("recovery: adapter diagnostics report live work: {}", report); });
    const std::regex feeder_line{R"(Feeder (\d+) RQ: (\d+) PQ: (\d+) WQ: (\d+) pending heads: (\d+))"};
    std::size_t lines = 0;
    for (auto it = std::sregex_iterator(report.begin(), report.end(), feeder_line); it != std::sregex_iterator(); ++it, ++lines) {
      const auto& line = *it;
      require(line[2] == "0" && line[3] == "0" && line[4] == "0" && line[5] == "0",
              [&] { return fmt::format("recovery: adapter diagnostics: {}", line.str()); });
    }
    require(lines == cfg.feeders, [&] { return fmt::format("recovery: adapter diagnostics list {} feeders, expected {}: {}", lines, cfg.feeders, report); });
  }

  // The recovery campaign: overload bursts, each followed by a producer pause
  // that must return every queue, parent, fragment and callback to zero.
  void recover()
  {
    begin(true, "warmup0");
    for (int i = 0; i < 200; ++i)
      step(true, false);
    end_all_cpus();
    std::uniform_int_distribution<uint64_t> burst(1, std::max<uint64_t>(1, cfg.burst_size));
    for (uint64_t cycle = 0; cycle < cfg.cycles; ++cycle) {
      if (cycle % 2 == 0)
        begin(false, "recovery");
      phase_name = fmt::format("recovery cycle {}", cycle);
      const auto s0 = adapter_statistics();
      const auto t0 = tot;
      for (uint64_t b = 0; b < cfg.burst_ops; ++b) {
        const auto n = burst(rng);
        for (uint64_t i = 0; i < n; ++i)
          add_packet(true);
        step(false, true);
      }
      tot.overloaded_cycles += tot.rejected > t0.rejected;
      tot.max_backlog_at_pause = std::max(tot.max_backlog_at_pause, backlog());
      const auto live = static_cast<uint64_t>(std::count_if(parents.begin(), parents.end(), [](const auto& kv) { return kv.second.accepted > 0; }));
      tot.max_live_at_pause = std::max(tot.max_live_at_pause, live);
      uint64_t ops = 0;
      while (backlog() != 0 || !parents.empty()) {
        step(false, false);
        require(++ops <= cfg.recovery_limit,
                [&] { return fmt::format("recovery: not quiescent after {} operates: backlog {} live parents {}", ops, backlog(), parents.size()); });
      }
      // Idle operates after recovery: only the native tick, and no progress.
      for (int i = 0; i < 8; ++i)
        step(false, false);
      tot.recovery_ops += ops;
      tot.max_recovery_ops = std::max(tot.max_recovery_ops, ops);
      check_stats("recovered");
      check_recovered(s0, t0);
      ++tot.recovery_cycles;
      if (cycle % 2 == 1 || cycle + 1 == cfg.cycles)
        end_all_cpus();
    }
    check_teardown_invariants();
    finalize_and_destroy();
  }

  std::string summary() const
  {
    const auto joined = [](const std::vector<uint64_t>& values) {
      std::string text;
      for (auto value : values)
        text += (text.empty() ? "" : "/") + std::to_string(value);
      return text;
    };
    return fmt::format("seed={} yaml={} tx={} feeders={} ops={} parents={} measured={} writes={} suppressed={} attempts={} accepted={} rejected={} "
                       "first_rejects={} partial_rejects={} sync_callbacks={} responses={} warmup_responses={} warmup_dropped={} carry_over={} "
                       "late_warmup_retained_visits={} warmup_partial_head_visits={} boundary_partial_heads={} boundary_rejected_heads={} "
                       "boundary_live_parents={} teardown_live={} accepted_by_cpu={} max_backlog={} max_outstanding={} max_stall_ps_with_work={} "
                       "stats_checks={} oor_packets={} oor_responses={} oor_dropped={} oor_in_warmup={} oor_by_queue={}",
                       seed, cfg.yaml, tx, cfg.feeders, tot.ops, tot.parents, tot.measured_parents, tot.write_parents, tot.suppressed_reads, tot.attempts,
                       tot.accepted, tot.rejected, tot.first_rejects, tot.partial_rejects, tot.sync, tot.responses, tot.warmup_responses, tot.warmup_dropped,
                       tot.carry_over_completions, tot.late_warmup_retained, tot.warmup_partial_head_visits, tot.boundary_partial_heads,
                       tot.boundary_rejected_heads, tot.boundary_live_parents, tot.teardown_live_parents, joined(tot.accepted_by_cpu), tot.max_backlog,
                       tot.max_outstanding, tot.max_stall_ops_with_work * static_cast<uint64_t>(period), tot.stats_checks, tot.oor_packets, tot.oor_responses,
                       tot.oor_dropped, tot.oor_in_warmup, joined(tot.oor_by_queue))
           + fmt::format(" recovery_cycles={} overloaded_cycles={} recovery_ops={} max_recovery_ops={} max_backlog_at_pause={} max_live_at_pause={}"
                         " invalid_request_cases={}",
                         tot.recovery_cycles, tot.overloaded_cycles, tot.recovery_ops, tot.max_recovery_ops, tot.max_backlog_at_pause, tot.max_live_at_pause,
                         tot.invalid_request_cases);
  }
};

config from_env()
{
  config c;
  const char* yaml = std::getenv("DIFF_YAML");
  c.yaml = yaml != nullptr ? yaml : "";
  c.tx = env_u64("DIFF_TX", 0);
  c.period = static_cast<int64_t>(env_u64("DIFF_PERIOD", 0));
  c.feeders = env_u64("DIFF_FEEDERS", 1);
  c.parents = env_u64("DIFF_PARENTS", 3000);
  c.late_warmup = env_u64("DIFF_LATE_WARMUP", 1) != 0;
  c.stats_every = env_u64("DIFF_STATS_EVERY", 61);
  c.max_burst = env_u64("DIFF_MAX_BURST", 24);
  c.backlog = env_u64("DIFF_BACKLOG", 160);
  c.drain = env_u64("DIFF_NO_DRAIN", 0) == 0;
  c.oor_percent = env_u64("DIFF_OOR_PERCENT", 4);
  c.cycles = env_u64("DIFF_CYCLES", 20);
  c.burst_ops = env_u64("DIFF_BURST_OPS", 40);
  c.burst_size = env_u64("DIFF_BURST_SIZE", 48);
  c.recovery_limit = env_u64("DIFF_RECOVERY_LIMIT", 4000000);
  return c;
}

// Every invalid request that must stop the run, in warmup and in a measured
// phase, each on a fresh adapter over cfg's native YAML: out-of-range load,
// RFO, translation and write requests at the first byte past capacity, 256
// blocks past it and the top of the address space; and invalid cores sending
// in-range demand requests and in-range or out-of-range prefetches. Throws
// failure on the first discrepancy; returns the number of cases.
uint64_t check_invalid_requests(config cfg)
{
  cfg.feeders = 1;
  struct request_case {
    std::size_t queue;
    access_type type;
    int address_class; // 0 top in-range block; 1 first byte past capacity; 2 256 blocks past; 3 top of the address space
    uint32_t cpu;
  };
  std::vector<request_case> cases;
  const uint32_t valid_core = static_cast<uint32_t>(champsim::defs::num_cpus - 1);
  for (const auto& [queue, type] : std::vector<std::pair<std::size_t, access_type>>{
           {0, access_type::LOAD}, {0, access_type::RFO}, {0, access_type::TRANSLATION}, {1, access_type::LOAD}, {2, access_type::WRITE}}) {
    for (int address_class = 1; address_class <= 3; ++address_class)
      cases.push_back({queue, type, address_class, valid_core});
  }
  for (const uint32_t cpu : {static_cast<uint32_t>(champsim::defs::num_cpus), std::numeric_limits<uint32_t>::max()}) {
    cases.push_back({0, access_type::LOAD, 0, cpu});
    cases.push_back({1, access_type::PREFETCH, 0, cpu});
    cases.push_back({1, access_type::PREFETCH, 1, cpu});
    cases.push_back({2, access_type::PREFETCH, 3, cpu});
  }
  uint64_t checked = 0;
  for (const bool warmup : {true, false}) {
    for (const auto& c : cases) {
      harness h{cfg, ++checked};
      try {
        h.begin(warmup, warmup ? "warmup" : "measured");
        const uint64_t addresses[] = {h.capacity - BLOCK_SIZE, h.capacity + 17, h.capacity + 256 * BLOCK_SIZE + BLOCK_SIZE - 1,
                                      std::numeric_limits<uint64_t>::max()};
        h.expect_stopped(c.queue, c.type, addresses[c.address_class], c.cpu);
      } catch (const failure& error) {
        // The harness names the case number where it would name a seed.
        throw failure(fmt::format("invalid-request case {} of {}: {}", checked, 2 * cases.size(), error.what()));
      }
    }
  }
  return checked;
}

// A native fixture with its buffers shrunk, written to a unique temporary file.
class temporary_yaml
{
  std::filesystem::path path_;

public:
  temporary_yaml(const std::string& fixture, const std::vector<std::pair<std::string, std::string>>& replacements)
  {
    std::ifstream in(fixture);
    if (!in)
      throw std::runtime_error("missing native test fixture " + fixture);
    std::string text{std::istreambuf_iterator<char>{in}, {}};
    for (const auto& [from, to] : replacements) {
      const auto pos = text.find(from);
      if (pos == std::string::npos)
        throw std::runtime_error("fixture " + fixture + " has no '" + from + "'");
      text.replace(pos, from.size(), to);
    }
    static unsigned sequence = 0;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() / fmt::format("champsim-706-{}-{}-{}.yaml", std::random_device{}(), stamp, ++sequence);
    std::ofstream out(path_);
    out << text;
  }
  temporary_yaml(const temporary_yaml&) = delete;
  temporary_yaml& operator=(const temporary_yaml&) = delete;
  ~temporary_yaml()
  {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
  }
  std::string path() const { return path_.string(); }
};
} // namespace oracle706

// Runs in every build, so a disabled build's `make test TEST_NUM=706` has a
// case that is not skipped. The recovery campaign reads the adapter's
// diagnostics through stdout_capture, and the smoke case's tiny variants come
// from temporary_yaml.
TEST_CASE("The differential oracle's stdout capture and fixture rewriting work in any build")
{
  for (int round = 0; round < 2; ++round) {
    oracle706::stdout_capture capture;
    fmt::print("captured {}\n", round);
    std::fputs("through stdio\n", stdout);
    REQUIRE(capture.text() == fmt::format("captured {}\nthrough stdio\n", round));
  }
  REQUIRE_THROWS_WITH(oracle706::temporary_yaml("configs/ramulator2/ddr4.yaml", {{"no_such_key: 1", "x"}}), Catch::Matchers::ContainsSubstring("has no"));
  std::string path;
  {
    oracle706::temporary_yaml shrunk{"configs/ramulator2/ddr4.yaml", {{"read_buffer_size: 32", "read_buffer_size: 1"}}};
    path = shrunk.path();
    std::ifstream in(path);
    const std::string text{std::istreambuf_iterator<char>{in}, {}};
    REQUIRE(text.find("read_buffer_size: 1\n") != std::string::npos);
    REQUIRE(text.find("read_buffer_size: 32") == std::string::npos);
    REQUIRE(text.find("write_buffer_size: 32") != std::string::npos);
  }
  REQUIRE_FALSE(std::filesystem::exists(path));
}

TEST_CASE("The Ramulator2 adapter matches an independent model over the real native driver")
{
  if (!champsim::ramulator2_available())
    SKIP("native build disabled");
  // Tiny native buffers force rejections, partial heads and long backlogs.
  const std::vector<std::pair<std::string, std::string>> tiny_ddr4{{"read_buffer_size: 32", "read_buffer_size: 1"},
                                                                   {"write_buffer_size: 32", "write_buffer_size: 2"},
                                                                   {"priority_buffer_size: 1568", "priority_buffer_size: 1"}};
  const std::vector<std::pair<std::string, std::string>> tiny_lpddr5{{"read_buffer_size: 32", "read_buffer_size: 1"},
                                                                     {"write_buffer_size: 32", "write_buffer_size: 1"},
                                                                     {"priority_buffer_size: 1568", "priority_buffer_size: 1"}};
  oracle706::temporary_yaml ddr4_tiny{"configs/ramulator2/ddr4.yaml", tiny_ddr4};
  oracle706::temporary_yaml lpddr5_tiny{"configs/ramulator2/lpddr5.yaml", tiny_lpddr5};
  struct variant {
    std::string name, yaml;
    std::size_t tx;
    int64_t period;
    std::size_t feeders;
  };
  const std::vector<variant> variants{{"ddr4", "configs/ramulator2/ddr4.yaml", 64, 833, 1},
                                      {"lpddr5", "configs/ramulator2/lpddr5.yaml", 32, 1453, 1},
                                      {"ddr4-tiny", ddr4_tiny.path(), 64, 833, 1},
                                      {"lpddr5-tiny", lpddr5_tiny.path(), 32, 1453, 1},
                                      {"lpddr5-tiny-2feeders", lpddr5_tiny.path(), 32, 1453, 2}};
  oracle706::totals all{};
  for (const auto& v : variants) {
    for (uint64_t seed = 1; seed <= 2; ++seed) {
      CAPTURE(v.name, seed);
      oracle706::config cfg;
      cfg.yaml = v.yaml;
      cfg.tx = v.tx;
      cfg.period = v.period;
      cfg.feeders = v.feeders;
      cfg.parents = 400;
      cfg.stats_every = 7;
      oracle706::harness h{cfg, seed};
      try {
        h.run();
      } catch (const oracle706::failure& error) {
        FAIL(error.what());
      }
      all.attempts += h.tot.attempts;
      all.rejected += h.tot.rejected;
      all.partial_rejects += h.tot.partial_rejects;
      all.sync += h.tot.sync;
      all.responses += h.tot.responses;
      all.warmup_responses += h.tot.warmup_responses;
      all.boundary_live_parents += h.tot.boundary_live_parents;
      all.carry_over_completions += h.tot.carry_over_completions;
      all.oor_packets += h.tot.oor_packets;
      all.oor_responses += h.tot.oor_responses;
      all.oor_dropped += h.tot.oor_dropped;
      all.oor_in_warmup += h.tot.oor_in_warmup;
      for (std::size_t q = 0; q < 3; ++q)
        all.oor_by_queue[q] += h.tot.oor_by_queue[q];
    }
  }
  // The smoke case must keep exercising what it exists to check.
  CHECK(all.rejected > 0);
  CHECK(all.partial_rejects > 0);
  CHECK(all.sync > 0);
  CHECK(all.responses > 0);
  CHECK(all.warmup_responses > 0);
  CHECK(all.boundary_live_parents > 0);
  CHECK(all.carry_over_completions > 0);
  CHECK(all.oor_responses > 0);
  CHECK(all.oor_dropped > 0);
  CHECK(all.oor_in_warmup > 0);
  CHECK(all.oor_packets > all.oor_in_warmup);
  CHECK(all.oor_by_queue[0] > 0);
  CHECK(all.oor_by_queue[1] > 0);
  CHECK(all.oor_by_queue[2] > 0);

  // Producer-pause recovery over tiny buffers, one feeder and two.
  oracle706::totals recovery{};
  for (const auto& v : variants) {
    if (v.name.find("tiny") == std::string::npos)
      continue;
    CAPTURE(v.name);
    oracle706::config cfg;
    cfg.yaml = v.yaml;
    cfg.tx = v.tx;
    cfg.period = v.period;
    cfg.feeders = v.feeders;
    cfg.stats_every = 7;
    cfg.cycles = 4;
    cfg.burst_ops = 16;
    cfg.burst_size = 24;
    oracle706::harness h{cfg, 1};
    try {
      h.recover();
    } catch (const oracle706::failure& error) {
      FAIL(error.what());
    }
    recovery.recovery_cycles += h.tot.recovery_cycles;
    recovery.overloaded_cycles += h.tot.overloaded_cycles;
    recovery.oor_packets += h.tot.oor_packets;
    recovery.max_backlog_at_pause = std::max(recovery.max_backlog_at_pause, h.tot.max_backlog_at_pause);
  }
  CHECK(recovery.recovery_cycles == 12);
  CHECK(recovery.overloaded_cycles == 12);
  CHECK(recovery.max_backlog_at_pause > 100);
  CHECK(recovery.oor_packets > 0);

  // Requests that must stop the run, over both fixtures' capacities.
  for (const auto& v : variants) {
    if (v.feeders != 1 || v.name.find("tiny") != std::string::npos)
      continue;
    CAPTURE(v.name);
    oracle706::config cfg;
    cfg.yaml = v.yaml;
    cfg.tx = v.tx;
    cfg.period = v.period;
    try {
      CHECK(oracle706::check_invalid_requests(cfg) == 46);
    } catch (const oracle706::failure& error) {
      FAIL(error.what());
    }
  }
}

namespace oracle706
{
// Runs one scenario per seed and prints one summary line for each;
// test/ramulator2/run_differential.py parses them. Returns the failures.
unsigned campaign(const config& cfg, void (harness::*scenario)())
{
  const auto seeds = env_u64("DIFF_SEEDS", 10);
  const auto seed0 = env_u64("DIFF_SEED0", 1);
  unsigned failures = 0;
  for (uint64_t seed = seed0; seed < seed0 + seeds; ++seed) {
    const auto t0 = std::chrono::steady_clock::now();
    std::string verdict = "PASS";
    std::string line;
    try {
      harness h{cfg, seed};
      try {
        // Once per run: the requests that must stop it, over the same YAML.
        if (seed == seed0)
          h.tot.invalid_request_cases = check_invalid_requests(cfg);
        (h.*scenario)();
      } catch (const failure& error) {
        verdict = std::string{"FAIL "} + error.what();
      }
      line = h.summary();
    } catch (const failure& error) {
      verdict = std::string{"FAIL "} + error.what();
      line = fmt::format("seed={} yaml={}", seed, cfg.yaml);
    }
    failures += verdict != "PASS";
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    fmt::print("{} wall={:.2f}s {}\n", line, seconds, verdict);
    std::fflush(stdout);
  }
  return failures;
}
} // namespace oracle706

TEST_CASE("Differential campaign: production adapter over the real native driver", "[.differential]")
{
  if (!champsim::ramulator2_available())
    SKIP("native build disabled");
  const auto cfg = oracle706::from_env();
  if (cfg.yaml.empty())
    SKIP("set DIFF_YAML to run this campaign (test/ramulator2/run_differential.py does)");
  REQUIRE(oracle706::campaign(cfg, &oracle706::harness::run) == 0);
}

TEST_CASE("Recovery campaign: overload bursts and producer pauses over the real native driver", "[.differential-recovery]")
{
  if (!champsim::ramulator2_available())
    SKIP("native build disabled");
  const auto cfg = oracle706::from_env();
  if (cfg.yaml.empty())
    SKIP("set DIFF_YAML to run this campaign (test/ramulator2/run_differential.py does)");
  REQUIRE(oracle706::campaign(cfg, &oracle706::harness::recover) == 0);
}

TEST_CASE("Differential diagnostic: dump native statistic paths", "[.differential-dump]")
{
  if (!champsim::ramulator2_available())
    SKIP("native build disabled");
  const auto cfg = oracle706::from_env();
  if (cfg.yaml.empty())
    SKIP("set DIFF_YAML to a native YAML to run this diagnostic");
  champsim::runtime_config rc;
  rc.set("ramulator2.config=" + cfg.yaml);
  auto driver = champsim::make_ramulator2_driver(rc);
  for (const auto& v : driver->statistics().values) {
    std::string path;
    for (const auto& part : v.path)
      path += "/" + part;
    fmt::print("{}\n", path);
  }
}

namespace
{
int64_t native_int(const champsim::native_memory_statistics& n, const std::vector<std::string>& path)
{
  for (const auto& v : n.values)
    if (v.path == path)
      return std::get<int64_t>(v.value);
  throw std::runtime_error("missing native statistic");
}
} // namespace

// Prints the adapter's read latency beside native read_latency for an isolated
// read and a write-forwarded read. Characterization only: the adapter measures
// from its admission operate to its completion operate (one native tick less
// than native depart - arrive for an isolated read, 0 for a forwarded one).
TEST_CASE("Differential diagnostic: adapter read latency versus native read_latency", "[.differential-latency]")
{
  if (!champsim::ramulator2_available())
    SKIP("native build disabled");
  const auto cfg = oracle706::from_env();
  if (cfg.yaml.empty())
    SKIP("set DIFF_YAML to a native YAML to run this diagnostic");
  champsim::runtime_config rc;
  rc.set("ramulator2.config=" + cfg.yaml);
  auto real = champsim::make_ramulator2_driver(rc);
  const auto period = real->clock_period().count();
  const auto tx = real->transaction_bytes();
  champsim::channel feeder;
  auto backend = champsim::make_ramulator2_memory_backend(std::move(real), {&feeder});
  auto& memory = backend->clocked_component();
  memory.warmup = false;
  memory.begin_phase();
  auto run_until_response = [&](uint64_t limit) {
    uint64_t ops = 0;
    while (feeder.returned.empty() && ops < limit) {
      memory._operate();
      ++ops;
    }
    return ops;
  };
  const auto report = [&](const char* label) {
    const auto s = backend->statistics().sim_ramulator2.value();
    const auto served = native_int(s.native, {"memory_system", "controller", "channel0", "num_read_reqs_served"});
    const auto forwarded = native_int(s.native, {"memory_system", "controller", "channel0", "num_read_reqs_forwarded"});
    const auto native_cycles = native_int(s.native, {"memory_system", "controller", "channel0", "read_latency"});
    fmt::print("{}: tx={} period={} adapter total_read_latency_ps={} samples={} (= {} ticks) | native read_latency={} cycles over served={} forwarded={}\n",
               label, tx, period, s.total_read_latency_ps, s.read_latency_samples, s.total_read_latency_ps / static_cast<uint64_t>(period), native_cycles,
               served, forwarded);
  };
  champsim::channel::request_type r;
  r.address = champsim::address{0x2000000};
  r.cpu = 0;
  REQUIRE(feeder.add_rq(r));
  const auto ops = run_until_response(100000);
  REQUIRE_FALSE(feeder.returned.empty());
  fmt::print("isolated read returned after {} operates\n", ops);
  report("isolated read");
  feeder.returned.clear();
  for (int i = 0; i < 200; ++i)
    memory._operate();
  memory.begin_phase();
  // Hold a write in the native buffer with a burst, then read its address.
  for (uint64_t i = 0; i < 8; ++i) {
    auto w = r;
    w.address = champsim::address{0x4000000 + i * 0x100000};
    REQUIRE(feeder.add_wq(w));
  }
  memory._operate();
  auto f = r;
  f.address = champsim::address{0x4000000 + 7 * 0x100000};
  REQUIRE(feeder.add_rq(f));
  const auto fops = run_until_response(100000);
  REQUIRE_FALSE(feeder.returned.empty());
  fmt::print("forwarded read returned after {} operates\n", fops);
  report("forwarded read");
}
