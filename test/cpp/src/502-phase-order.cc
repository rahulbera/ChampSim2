#include <catch.hpp>
#include <deque>
#include <optional>
#include <tuple>

#include "environment.h"
#include "phase_info.h"
#include "tracereader.h"

namespace champsim
{
phase_stats do_phase(const phase_info&, environment&, std::vector<tracereader>&, chrono::clock&, const simulation_knobs&);
}

namespace
{
struct phase_environment : champsim::environment {
  O3_CPU first{champsim::core_builder{}.index(0)}, second{champsim::core_builder{}.index(1)};
  std::vector<std::pair<char, long long>> operations;
  std::vector<std::tuple<char, unsigned, long long>> endings;

  struct component : champsim::operable {
    phase_environment& env;
    char id;
    component(phase_environment& e, char i, long long period) : operable(champsim::chrono::picoseconds{period}), env(e), id(i) {}
    void begin_phase() override
    {
      if (id == 'F') {
        env.first.begin_phase();
        env.second.begin_phase();
      }
    }
    long operate() override
    {
      const auto tick = current_time.time_since_epoch().count();
      env.operations.emplace_back(id, tick);
      if (id == 'F') {
        ++env.first.num_retired;
        env.second.num_retired += (tick % 2 == 0);
        env.first.current_time = env.second.current_time = current_time;
      }
      return 1;
    }
    void end_phase(unsigned cpu) override { env.endings.emplace_back(id, cpu, env.fast.current_time.time_since_epoch().count()); }
  } slow{*this, 'S', 3}, fast{*this, 'F', 1}, medium{*this, 'M', 2};

  struct memory : champsim::memory_backend {
    component& op;
    explicit memory(component& c) : op(c) {}
    champsim::operable& clocked_component() override { return op; }
    champsim::data::bytes size() const override { return champsim::data::bytes{64}; }
    champsim::memory_statistics statistics() const override { return {}; }
    std::string_view name() const override { return "legacy"; }
  } mem{slow};

  std::vector<std::reference_wrapper<O3_CPU>> cpu_view() override { return {first, second}; }
  std::vector<std::reference_wrapper<CACHE>> cache_view() override { return {}; }
  std::vector<std::reference_wrapper<PageTableWalker>> ptw_view() override { return {}; }
  champsim::memory_backend& memory_view() override { return mem; }
  std::vector<std::reference_wrapper<champsim::operable>> operable_view() override { return {slow, fast, medium}; }
};
} // namespace

TEST_CASE("Phase scheduling preserves canonical ties and completes each CPU once")
{
  phase_environment env;
  std::vector<champsim::tracereader> traces;
  traces.emplace_back([] { return ooo_model_instr{0, input_instr{}}; });
  champsim::chrono::clock clock;
  champsim::phase_info phase{"Simulation", false, 3, {0, 0}, {"synthetic"}};
  const auto stats = champsim::do_phase(phase, env, traces, clock, {});

  const std::vector<std::pair<char, long long>> expected_operations{{'S', 3}, {'F', 1}, {'M', 2}, {'F', 2}, {'F', 3}, {'M', 4},
                                                                    {'S', 6}, {'F', 4}, {'F', 5}, {'M', 6}, {'F', 6}};
  const std::vector<std::tuple<char, unsigned, long long>> expected_endings{{'S', 0, 3}, {'F', 0, 3}, {'M', 0, 3}, {'S', 1, 6}, {'F', 1, 6}, {'M', 1, 6}};
  CHECK(env.operations == expected_operations);
  CHECK(env.endings == expected_endings);
  CHECK(env.first.sim_instr() == 6);
  CHECK(env.second.sim_instr() == 3);
  CHECK(stats.trace_names == std::vector<std::string>{"synthetic", "synthetic"});
}

namespace
{
// Twenty operables on one clock and one slower one, canonically first: more than the 16 elements
// libstdc++'s std::sort insertion-sorts, which is where a multi-core machine's tick order used to
// be scrambled.
struct wide_environment : champsim::environment {
  O3_CPU cpu{champsim::core_builder{}.index(0)};
  std::vector<std::pair<int, long long>> operations;

  struct component : champsim::operable {
    wide_environment& env;
    int id;
    component(wide_environment& e, int i, long long period) : operable(champsim::chrono::picoseconds{period}), env(e), id(i) {}
    long operate() override
    {
      env.operations.emplace_back(id, current_time.time_since_epoch().count());
      if (id == 1) {
        ++env.cpu.num_retired;
        env.cpu.current_time = current_time;
      }
      return 1;
    }
  };
  std::deque<component> components;

  struct memory : champsim::memory_backend {
    component& op;
    explicit memory(component& c) : op(c) {}
    champsim::operable& clocked_component() override { return op; }
    champsim::data::bytes size() const override { return champsim::data::bytes{64}; }
    champsim::memory_statistics statistics() const override { return {}; }
    std::string_view name() const override { return "legacy"; }
  };
  std::optional<memory> mem;

  wide_environment()
  {
    components.emplace_back(*this, 0, 3);
    for (int i = 1; i <= 20; ++i) {
      components.emplace_back(*this, i, 2);
    }
    mem.emplace(components.front());
  }

  std::vector<std::reference_wrapper<O3_CPU>> cpu_view() override { return {cpu}; }
  std::vector<std::reference_wrapper<CACHE>> cache_view() override { return {}; }
  std::vector<std::reference_wrapper<PageTableWalker>> ptw_view() override { return {}; }
  champsim::memory_backend& memory_view() override { return *mem; }
  std::vector<std::reference_wrapper<champsim::operable>> operable_view() override { return {std::begin(components), std::end(components)}; }
};
} // namespace

TEST_CASE("Operables due at the same time keep canonical order beyond sixteen operables")
{
  wide_environment env;
  env.cpu.begin_phase();
  std::vector<champsim::tracereader> traces;
  traces.emplace_back([] { return ooo_model_instr{0, input_instr{}}; });
  champsim::chrono::clock clock;
  champsim::phase_info phase{"Simulation", false, 3, {0}, {"synthetic"}};
  champsim::do_phase(phase, env, traces, clock, {});

  // The clock ticks by 2. Tick 1 (time 2): every operable is at 0, so all run in canonical order.
  // Tick 2 (time 4): the slow operable, now at 3, is later than the twenty at 2 and runs last.
  // Tick 3 (time 6): it is not due.
  std::vector<std::pair<int, long long>> expected;
  expected.emplace_back(0, 3);
  for (int i = 1; i <= 20; ++i) {
    expected.emplace_back(i, 2);
  }
  for (int i = 1; i <= 20; ++i) {
    expected.emplace_back(i, 4);
  }
  expected.emplace_back(0, 6);
  for (int i = 1; i <= 20; ++i) {
    expected.emplace_back(i, 6);
  }
  CHECK(env.operations == expected);
}

TEST_CASE("A trace reaching EOF completes every remaining CPU on that tick")
{
  struct finite_reader {
    unsigned remaining = 1;
    ooo_model_instr operator()()
    {
      --remaining;
      return ooo_model_instr{0, input_instr{}};
    }
    bool eof() const { return remaining == 0; }
  };
  phase_environment env;
  std::vector<champsim::tracereader> traces;
  traces.emplace_back(finite_reader{});
  champsim::chrono::clock clock;
  champsim::phase_info phase{"Warmup", true, 100, {0, 0}, {"finite"}};
  champsim::do_phase(phase, env, traces, clock, {});

  const std::vector<std::tuple<char, unsigned, long long>> expected_endings{{'S', 0, 1}, {'F', 0, 1}, {'M', 0, 1}, {'S', 1, 1}, {'F', 1, 1}, {'M', 1, 1}};
  CHECK(env.endings == expected_endings);
  CHECK(env.first.sim_instr() == 1);
  CHECK(env.second.sim_instr() == 0);
  CHECK(env.first.input_queue.size() == 1);
  CHECK(env.second.input_queue.empty());
}
