// Lifecycle of the real native driver and of the production adapter over it:
// repeated construction and destruction, statistics and counter resets along
// the way, and teardown, with and without finalize(), while native requests,
// adapter parent contexts and weak-mailbox callbacks are still live.
//
// Under RAMULATOR2_SANITIZE=1 (test/ramulator2/README.md) the host and the
// native library are both instrumented, so AddressSanitizer and LeakSanitizer
// check that such teardown neither touches freed memory nor leaks. The
// assertions make every build check callback bookkeeping, and require that
// the runs really did end with that live work, so the teardown being checked
// cannot quietly become an idle one.
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <variant>
#include <catch2/catch_test_macros.hpp>

#include "channel.h"
#include "defs.h"
#include "memory_stats.h"
#include "operable.h"
#include "ramulator2_driver.h"
#include "ramulator2_memory_backend.h"
#include "runtime_config.h"

namespace
{
constexpr const char* fixtures[] = {"configs/ramulator2/ddr4.yaml", "configs/ramulator2/lpddr5.yaml"};

champsim::runtime_config config_for(const char* path)
{
  champsim::runtime_config cfg;
  cfg.set(std::string{"ramulator2.config="} + path);
  return cfg;
}

// A native controller counter summed over every channel.
uint64_t native_total(const champsim::native_memory_statistics& stats, const std::string& leaf)
{
  uint64_t total = 0;
  for (const auto& item : stats.values) {
    if (item.path.empty() || item.path.back() != leaf)
      continue;
    if (const auto* value = std::get_if<int64_t>(&item.value))
      total += static_cast<uint64_t>(*value);
    else if (const auto* large = std::get_if<uint64_t>(&item.value))
      total += *large;
  }
  return total;
}
} // namespace

TEST_CASE("The native driver releases every callback when destroyed with live requests, finalized or not")
{
  if (!champsim::ramulator2_available())
    SKIP("native build disabled");
  std::mt19937_64 rng{12345};
  unsigned unfinalized_with_pending = 0;
  uint64_t synchronous = 0;
  for (int iteration = 0; iteration < 48; ++iteration) {
    CAPTURE(iteration);
    // Callbacks count completions through a weak reference and hold `copies`
    // strongly, so its use count is the number of callback copies still alive.
    auto completed = std::make_shared<uint64_t>(0);
    auto copies = std::make_shared<char>(0);
    uint64_t accepted = 0;
    {
      auto driver = champsim::make_ramulator2_driver(config_for(fixtures[iteration % 2]));
      const uint64_t transaction = driver->transaction_bytes();
      const uint64_t window = uint64_t{1} << 22; // Repeated addresses make native writes coalesce and reads forward.
      const auto rounds = 50 + rng() % 400;
      for (uint64_t round = 0; round < rounds; ++round) {
        const auto burst = rng() % 6;
        for (uint64_t k = 0; k < burst; ++k) {
          const bool write = rng() % 4 == 0;
          const uint64_t address = 0x100000 + (rng() % window) / transaction * transaction;
          const auto cpu = static_cast<uint32_t>(rng() % champsim::defs::num_cpus);
          const auto before = *completed;
          if (driver->send(write, address, cpu, transaction, [weak = std::weak_ptr<uint64_t>{completed}, held = copies] {
                if (auto live = weak.lock())
                  ++*live;
              })) {
            ++accepted;
            synchronous += *completed - before;
          }
        }
        driver->tick();
        if (round % 97 == 13)
          REQUIRE_FALSE(driver->statistics().values.empty());
        if (round % 151 == 7)
          driver->reset_stats();
      }
      REQUIRE(*completed <= accepted);
      if (iteration % 3 == 0) {
        driver->finalize();
        REQUIRE_FALSE(driver->statistics().values.empty()); // A printer can still read them.
      } else {
        unfinalized_with_pending += *completed < accepted && copies.use_count() > 1;
      }
    }
    REQUIRE(copies.use_count() == 1); // No native callback copy outlives the driver.
  }
  REQUIRE(unfinalized_with_pending > 0);
  REQUIRE(synchronous > 0); // Callbacks from inside send() were exercised too.
}

TEST_CASE("The production adapter tears down over the real driver with live parents, finalized or not")
{
  if (!champsim::ramulator2_available())
    SKIP("native build disabled");
  std::mt19937_64 rng{777};
  unsigned with_live_parents = 0, with_feeder_backlog = 0;
  uint64_t coalesced_writes = 0;
  for (int iteration = 0; iteration < 24; ++iteration) {
    CAPTURE(iteration);
    champsim::channel feeder;
    auto backend = champsim::make_ramulator2_memory_backend(champsim::make_ramulator2_driver(config_for(fixtures[iteration % 2])), {&feeder});
    auto& memory = backend->clocked_component();
    memory.warmup = iteration % 4 == 3;
    memory.begin_phase();
    // A 64-block window coalesces writes inside send(), whose synchronous
    // callbacks reach the weak mailbox before the adapter pops the head.
    const uint64_t window = (iteration / 2) % 2 == 0 ? uint64_t{1} << 24 : uint64_t{1} << 12;
    uint64_t next_id = 1, returned = 0, requested = 0;
    const auto ticks = 2000 + rng() % 6000;
    for (uint64_t tick = 0; tick < ticks; ++tick) {
      const auto burst = rng() % 3;
      for (uint64_t k = 0; k < burst; ++k) {
        champsim::channel::request_type request;
        request.cpu = static_cast<uint32_t>(rng() % champsim::defs::num_cpus);
        request.address = champsim::address{0x100000 + rng() % window};
        request.instr_id = next_id++;
        request.response_requested = rng() % 5 != 0;
        request.instr_depend_on_me = {request.instr_id, request.instr_id + 1};
        switch (rng() % 3) {
        case 0:
          request.type = access_type::WRITE;
          feeder.add_wq(request);
          break;
        case 1:
          request.type = access_type::PREFETCH;
          if (feeder.add_pq(request) && request.response_requested)
            ++requested;
          break;
        default:
          if (feeder.add_rq(request) && request.response_requested)
            ++requested;
        }
      }
      memory._operate();
      returned += feeder.returned.size();
      feeder.returned.clear();
      if (tick == ticks / 3) {
        memory.end_phase(0);
        coalesced_writes += native_total(backend->statistics().sim_ramulator2.value().native, "num_write_reqs_coalesced");
        memory.warmup = false;
        memory.begin_phase();
      }
      if (tick % 1009 == 5)
        (void)backend->statistics();
    }
    REQUIRE(returned <= requested);
    memory.end_phase(0);
    const auto stats = backend->statistics();
    REQUIRE(stats.roi_ramulator2.has_value());
    coalesced_writes += native_total(stats.roi_ramulator2->native, "num_write_reqs_coalesced");
    with_live_parents += stats.roi_ramulator2->outstanding_parents > 0;
    with_feeder_backlog += !(feeder.RQ.empty() && feeder.PQ.empty() && feeder.WQ.empty());
    if (iteration % 2 == 0)
      backend->finalize();
    // Destroyed here with feeder backlog, partially admitted heads and live native requests.
  }
  REQUIRE(with_live_parents > 0);
  REQUIRE(with_feeder_backlog > 0);
  REQUIRE(coalesced_writes > 0);
}
