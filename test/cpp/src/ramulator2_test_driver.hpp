#ifndef RAMULATOR2_TEST_DRIVER_HPP
#define RAMULATOR2_TEST_DRIVER_HPP

#include <deque>
#include <functional>
#include <memory>
#include <tuple>
#include <vector>

#include "ramulator2_driver.h"

namespace ramulator2_test
{
struct driver_state {
  using submission = std::tuple<bool, uint64_t, uint32_t, std::size_t>;
  std::deque<bool> decisions;
  std::vector<submission> attempts, accepted;
  std::vector<std::function<void()>> callbacks;
  std::deque<std::size_t> complete_on_tick;
  bool immediate_writes = false;
  uint64_t ticks = 0, resets = 0, finalizations = 0;
};

class driver final : public champsim::ramulator2_driver
{
  std::shared_ptr<driver_state> state;
  std::size_t bytes;

public:
  driver(std::shared_ptr<driver_state> state_, std::size_t transaction_bytes) : state(std::move(state_)), bytes(transaction_bytes) {}
  champsim::chrono::picoseconds clock_period() const override { return champsim::chrono::picoseconds{625}; }
  champsim::data::bytes size() const override { return champsim::data::bytes{1 << 24}; }
  std::size_t transaction_bytes() const override { return bytes; }
  bool send(bool write, uint64_t address, uint32_t cpu, std::size_t count, std::function<void()> done) override
  {
    const driver_state::submission request{write, address, cpu, count};
    state->attempts.push_back(request);
    bool accept = true;
    if (!state->decisions.empty()) {
      accept = state->decisions.front();
      state->decisions.pop_front();
    }
    if (accept) {
      state->accepted.push_back(request);
      state->callbacks.push_back(std::move(done));
      if (write && state->immediate_writes) {
        state->callbacks.back()();
      }
    }
    return accept;
  }
  void tick() override
  {
    ++state->ticks;
    auto ready = std::move(state->complete_on_tick);
    state->complete_on_tick.clear();
    for (const auto index : ready) {
      state->callbacks.at(index)();
    }
  }
  void reset_stats() override { ++state->resets; }
  champsim::native_memory_statistics statistics() override
  {
    return {{{{"memory_system", "native.with.dots", "ticks"}, state->ticks}}, "native_ticks: " + std::to_string(state->ticks)};
  }
  champsim::ramulator2_config_record config_record() const override { return {"fixture.yaml", "1234", "fixture-revision", "frontend: External\n"}; }
  void finalize() override { ++state->finalizations; }
};
} // namespace ramulator2_test

#endif
