#include <algorithm>
#include <catch.hpp>
#include <deque>
#include <optional>
#include <stdexcept>

#include "environment.h"
#include "instr.h"
#include "mocks.hpp"
#include "parallel_tick.h"
#include "phase_info.h"
#include "tracereader.h"

namespace champsim
{
phase_stats do_phase(const phase_info&, environment&, std::vector<tracereader>&, chrono::clock&, const simulation_knobs&);
}

namespace
{
// Three cores, each with its own mock L1I and L1D in its lane, and a no-lane probe between the
// cores and the caches, where a real machine's LLC sits. The probe sees every core, so it fixes
// what each tick must have done before it, and after it, at any thread count. Everything runs on
// a 2 ps clock unless the probe is given another period. One that is not a multiple of 2 is due on
// some ticks while later than the rest, which moves it after the caches.
struct lane_environment : champsim::environment {
  static constexpr std::size_t num_cores = 3;
  std::deque<do_nothing_MRC> l1i, l1d;
  std::deque<O3_CPU> cores;

  struct probe_type : champsim::operable {
    lane_environment& env;
    std::vector<std::vector<long long>> retired; // per tick, each core's retired count, then each mock cache's accepted requests
    std::vector<std::vector<uint64_t>> queued;   // per tick, every queued instruction ID, core by core
    explicit probe_type(lane_environment& e) : env(e) {}
    long operate() override
    {
      auto& counts = retired.emplace_back();
      auto& ids = queued.emplace_back();
      // With interactions declared, it reads only what it declares: the mock caches.
      for (auto& core : env.cores) {
        if (!env.declare_interactions) {
          counts.push_back(core.num_retired);
          for (const auto& instr : core.input_queue) {
            ids.push_back(instr.instr_id);
          }
        }
      }
      for (std::size_t c = 0; c < num_cores; ++c) {
        counts.push_back(static_cast<long long>(std::size(env.l1i[c].addresses)));
        counts.push_back(static_cast<long long>(std::size(env.l1d[c].addresses)));
      }
      return 0;
    }
  } probe{*this};

  // Throws from inside a lane, on a worker thread when there is more than one.
  struct thrower_type : champsim::operable {
    long throw_at = 0;
    long calls = 0;
    long operate() override
    {
      if (++calls == throw_at) {
        throw std::runtime_error{"thrown from a lane"};
      }
      return 0;
    }
  } thrower;

  // In core c's lane, after its caches: records the core's queued instruction IDs every tick, so
  // a lane sees the same IDs as the serial loop's operables do.
  struct recorder_type : champsim::operable {
    lane_environment& env;
    std::size_t core;
    std::vector<std::vector<uint64_t>> seen;
    recorder_type(lane_environment& e, std::size_t c) : env(e), core(c) {}
    long operate() override
    {
      auto& ids = seen.emplace_back();
      for (const auto& instr : env.cores[core].input_queue) {
        ids.push_back(instr.instr_id);
      }
      return 0;
    }
  };
  std::deque<recorder_type> recorders;

  // No lane, after the caches: records every change in its core's L1D, and is idle exactly when
  // there is none, so skipping it when idle() says so must change nothing.
  struct walker_type : champsim::operable {
    lane_environment& env;
    std::size_t core;
    std::size_t last = 0;
    std::vector<std::pair<long long, std::size_t>> seen;
    walker_type(lane_environment& e, std::size_t c) : env(e), core(c) {}
    [[nodiscard]] bool idle() const override { return std::size(env.l1d[core].addresses) == last; }
    long operate() override
    {
      const auto now = std::size(env.l1d[core].addresses);
      if (now == last) {
        return 0;
      }
      seen.emplace_back(current_time.time_since_epoch().count(), now);
      last = now;
      return 1;
    }
  };
  std::deque<walker_type> walkers;
  bool declare_idle_lanes = false;

  // With interactions declared, the probe reads only the mock caches' own state, which the cores
  // never touch, so a multi-threaded tick may run it ahead of the cores.
  bool declare_interactions = false;

  explicit lane_environment(long probe_period)
  {
    probe.clock_period = champsim::chrono::picoseconds{probe_period};
    thrower.clock_period = champsim::chrono::picoseconds{2};
    for (std::size_t c = 0; c < num_cores; ++c) {
      l1i.emplace_back(static_cast<int>(c));     // a different latency per core, so cores
      l1d.emplace_back(static_cast<int>(2 * c)); // drain their input queues at different rates
      l1i.back().clock_period = l1d.back().clock_period = champsim::chrono::picoseconds{2};
    }
    for (std::size_t c = 0; c < num_cores; ++c) {
      recorders.emplace_back(*this, c);
      recorders.back().clock_period = champsim::chrono::picoseconds{2};
      walkers.emplace_back(*this, c);
      walkers.back().clock_period = champsim::chrono::picoseconds{2};
    }
    for (std::size_t c = 0; c < num_cores; ++c) {
      cores.emplace_back(champsim::core_builder{}
                             .index(static_cast<uint32_t>(c))
                             .ifetch_buffer_size(16)
                             .decode_buffer_size(16)
                             .dispatch_buffer_size(16)
                             .register_file_size(128)
                             .rob_size(16)
                             .lq_size(8)
                             .sq_size(8)
                             .clock_period(champsim::chrono::picoseconds{2})
                             .fetch_width(champsim::bandwidth::maximum_type{static_cast<long>(num_cores - c)})
                             .fetch_queues(&l1i[c].queues)
                             .data_queues(&l1d[c].queues));
    }
  }

  std::vector<std::reference_wrapper<O3_CPU>> cpu_view() override { return {std::begin(cores), std::end(cores)}; }
  std::vector<std::reference_wrapper<CACHE>> cache_view() override { return {}; }
  std::vector<std::reference_wrapper<PageTableWalker>> ptw_view() override { return {}; }

  struct memory : champsim::memory_backend {
    champsim::operable& op;
    explicit memory(champsim::operable& o) : op(o) {}
    champsim::operable& clocked_component() override { return op; }
    champsim::data::bytes size() const override { return champsim::data::bytes{64}; }
    champsim::memory_statistics statistics() const override { return {}; }
    std::string_view name() const override { return "legacy"; }
  } mem{probe};
  champsim::memory_backend& memory_view() override { return mem; }

  // Cores (0-2), the probe (3), each core's caches (4-9), the thrower in core 2's lane (10), each
  // core's recorder (11-13), then each core's walker (14-16).
  std::vector<std::reference_wrapper<champsim::operable>> operable_view() override
  {
    std::vector<std::reference_wrapper<champsim::operable>> view{std::begin(cores), std::end(cores)};
    view.emplace_back(probe);
    for (std::size_t c = 0; c < num_cores; ++c) {
      view.emplace_back(l1i[c]);
      view.emplace_back(l1d[c]);
    }
    view.emplace_back(thrower);
    view.insert(std::end(view), std::begin(recorders), std::end(recorders));
    view.insert(std::end(view), std::begin(walkers), std::end(walkers));
    return view;
  }
  std::vector<std::size_t> operable_lanes() override
  {
    std::vector<std::size_t> lanes{0, 1, 2, no_lane};
    for (std::size_t c = 0; c < num_cores; ++c) {
      lanes.insert(std::end(lanes), 2, c);
    }
    lanes.push_back(2);
    lanes.insert(std::end(lanes), {0, 1, 2});
    lanes.insert(std::end(lanes), 3, no_lane);
    return lanes;
  }
  std::vector<std::size_t> operable_idle_lanes() override
  {
    std::vector<std::size_t> lanes(14, no_lane);
    for (std::size_t c = 0; c < num_cores; ++c) {
      lanes.push_back(declare_idle_lanes ? c : no_lane); // a walker reads only its core's L1D
    }
    return lanes;
  }
  std::optional<std::vector<std::vector<std::size_t>>> operable_interactions() override
  {
    if (!declare_interactions) {
      return std::nullopt;
    }
    std::vector<std::vector<std::size_t>> peers(17);
    const auto link = [&](std::size_t a, std::size_t b) {
      peers[a].push_back(b);
      peers[b].push_back(a);
    };
    for (std::size_t c = 0; c < num_cores; ++c) {
      link(c, 4 + 2 * c); // the core and its L1I
      link(c, 5 + 2 * c); // and its L1D
      link(c, 11 + c);    // and its recorder
      link(3, 4 + 2 * c); // the probe reads the mocks
      link(3, 5 + 2 * c);
      link(14 + c, 5 + 2 * c); // the walker reads the L1D
    }
    return peers;
  }
};

struct outcome {
  std::vector<std::vector<std::pair<long long, std::size_t>>> walked;
  std::vector<std::vector<std::vector<uint64_t>>> recorded; // per core, per tick, relative to the run's first ID
  std::vector<std::vector<long long>> seen_retired;
  std::vector<std::vector<uint64_t>> seen_queued; // relative to the run's first ID
  std::vector<std::vector<uint64_t>> final_queues;
  std::vector<long long> retired;
};

// Instructions with a load every third one, so the L1D lanes have work too. A finite reader
// ends after `length` instructions.
struct reader {
  uint64_t ip;
  long remaining;
  long count = 0;
  ooo_model_instr operator()()
  {
    --remaining;
    ip += 4;
    if (++count % 3 == 0) {
      return champsim::test::instruction_with_ip_and_source_memory(champsim::address{ip}, champsim::address{0x100000 + 64 * ip});
    }
    return champsim::test::instruction_with_ip(ip);
  }
  bool eof() const { return remaining <= 0; }
};

// Reports the first tick that differs, not the whole history.
void check_same(const outcome& threaded, const outcome& serial)
{
  CHECK(threaded.retired == serial.retired);
  CHECK(threaded.final_queues == serial.final_queues);
  CHECK(threaded.recorded == serial.recorded);
  CHECK(threaded.walked == serial.walked);
  REQUIRE(std::size(threaded.seen_retired) == std::size(serial.seen_retired));
  for (std::size_t tick = 0; tick < std::size(serial.seen_retired); ++tick) {
    INFO("tick " << tick);
    REQUIRE(threaded.seen_retired[tick] == serial.seen_retired[tick]);
    REQUIRE(threaded.seen_queued[tick] == serial.seen_queued[tick]);
  }
}

outcome run(std::size_t threads, long trace_length, long throw_at = 0, long probe_period = 2, long phase_length = 120, bool interactions = false,
            bool idle_lanes = false)
{
  lane_environment env{probe_period};
  env.declare_interactions = interactions;
  env.declare_idle_lanes = idle_lanes;
  env.thrower.throw_at = throw_at;
  std::vector<champsim::tracereader> traces;
  for (std::size_t c = 0; c < lane_environment::num_cores; ++c) {
    traces.emplace_back(reader{0x1000 * (c + 1), trace_length + static_cast<long>(7 * c)});
  }
  champsim::chrono::clock clock;
  champsim::phase_info phase{"Simulation", false, phase_length, {0, 1, 2}, {"a", "b", "c"}};
  champsim::simulation_knobs knobs;
  knobs.threads = threads;

  // Instruction IDs are global, so compare them relative to the first one this run hands out.
  const auto first = champsim::tracereader::next_id();
  champsim::do_phase(phase, env, traces, clock, knobs);

  outcome result;
  result.seen_retired = env.probe.retired;
  for (auto ids : env.probe.queued) {
    for (auto& id : ids) {
      id -= first;
    }
    result.seen_queued.push_back(ids);
  }
  for (auto& walker : env.walkers) {
    result.walked.push_back(walker.seen);
  }
  for (auto& recorder : env.recorders) {
    auto& per_tick = result.recorded.emplace_back();
    for (auto ids : recorder.seen) {
      for (auto& id : ids) {
        id -= first;
      }
      per_tick.push_back(ids);
    }
  }
  for (auto& core : env.cores) {
    std::vector<uint64_t> queue;
    for (const auto& instr : core.input_queue) {
      queue.push_back(instr.instr_id - first);
    }
    result.final_queues.push_back(queue);
    result.retired.push_back(core.num_retired);
  }
  return result;
}
} // namespace

TEST_CASE("A multi-threaded tick gives the serial result at every thread count")
{
  const auto serial = run(1, 1000000);
  REQUIRE(std::size(serial.seen_retired) > 50);
  // Every core reaches the phase length; the faster ones keep retiring until the slowest does.
  REQUIRE(std::all_of(std::begin(serial.retired), std::end(serial.retired), [](auto retired) { return retired >= 120; }));
  for (std::size_t threads : {std::size_t{2}, std::size_t{3}, std::size_t{8}}) {
    INFO(threads << " threads");
    check_same(run(threads, 1000000), serial);
  }
}

TEST_CASE("A multi-threaded tick follows the serial order when a slower operable falls behind")
{
  // A 3 ps probe on a 2 ps clock is due on two ticks of three, and on one of them it is later than
  // the rest, so the serial loop runs it after the caches and the schedule has to move it there.
  const auto serial = run(1, 1000000, 0, 3);
  REQUIRE(std::size(serial.seen_retired) > 20);
  for (std::size_t threads : {std::size_t{2}, std::size_t{3}}) {
    INFO(threads << " threads");
    check_same(run(threads, 1000000, 0, 3), serial);
  }
}

TEST_CASE("A no-lane operable that interacts with no core runs ahead of the cores")
{
  // Declared interactions let the probe run first, which leaves one run per tick, and the IDs a
  // lane refills with are set at its next run. The serial loop, which ignores interactions, is
  // the reference.
  {
    lane_environment env{2};
    env.declare_interactions = true;
    CHECK(champsim::parallel_tick{env, 3}.runs_per_tick() == 1);
    env.declare_interactions = false;
    CHECK(champsim::parallel_tick{env, 3}.runs_per_tick() == 2);
  }
  const auto serial = run(1, 1000000, 0, 2, 120, true);
  for (std::size_t threads : {std::size_t{2}, std::size_t{3}}) {
    INFO(threads << " threads");
    check_same(run(threads, 1000000, 0, 2, 120, true), serial);
  }
  const auto ended = run(1, 3, 0, 2, 120, true);
  for (std::size_t threads : {std::size_t{2}, std::size_t{3}}) {
    INFO(threads << " threads, a trace ending mid-refill");
    check_same(run(threads, 3, 0, 2, 120, true), ended);
  }
}

TEST_CASE("A multi-threaded tick skips a no-lane operable only when it is idle")
{
  // Each walker is checked by its core's lane at the end of the run and skipped when idle; the
  // serial loop calls every one. Their records must agree.
  const auto serial = run(1, 1000000, 0, 2, 120, true, true);
  REQUIRE(std::all_of(std::begin(serial.walked), std::end(serial.walked), [](const auto& w) { return std::size(w) > 5; }));
  for (std::size_t threads : {std::size_t{2}, std::size_t{3}}) {
    INFO(threads << " threads");
    check_same(run(threads, 1000000, 0, 2, 120, true, true), serial);
  }
}

TEST_CASE("A multi-threaded tick gives the serial result while lanes move between threads")
{
  // Two threads for three lanes: lanes are reassigned from their measured cost every 4,096
  // ticks, and which thread runs a lane must not matter. The assignment depends on timing, so
  // the run is long enough to pass several reassignment points whichever way they go.
  const auto serial = run(1, 1000000, 0, 2, 15000);
  REQUIRE(std::size(serial.seen_retired) > 3 * 4096);
  INFO("2 threads");
  check_same(run(2, 1000000, 0, 2, 15000), serial);
}

TEST_CASE("A multi-threaded tick numbers instructions as the serial loop does when a trace ends")
{
  // Core 0's three-instruction trace ends inside the first refill, which asks it for six: the
  // instructions read after it, cores 1 and 2's, must not be numbered across the gap. (In steady
  // state a core asks for about one instruction a tick, so a longer trace ends without a gap.)
  const auto serial = run(1, 3);
  REQUIRE(serial.final_queues.at(0).size() == 3);
  for (std::size_t threads : {std::size_t{2}, std::size_t{3}}) {
    INFO(threads << " threads");
    check_same(run(threads, 3), serial);
  }
}

TEST_CASE("An exception thrown inside a lane reaches the caller")
{
  CHECK_THROWS_WITH(run(3, 1000000, 5), "thrown from a lane");
  CHECK_THROWS_WITH(run(1, 1000000, 5), "thrown from a lane");
}
