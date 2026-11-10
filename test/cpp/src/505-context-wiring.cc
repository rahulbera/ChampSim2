#include <catch.hpp>

#include <cstdint>
#include <vector>

#include "environment.h"
#include "operable.h"
#include "phase_info.h"
#include "tracereader.h"

namespace champsim
{
long do_cycle(std::vector<std::reference_wrapper<champsim::operable>>&, const std::vector<std::reference_wrapper<O3_CPU>>&, std::vector<tracereader>&,
              const std::vector<std::vector<std::size_t>>&, chrono::clock&);
phase_stats do_phase(const phase_info&, environment&, std::vector<tracereader>&, chrono::clock&, const simulation_knobs&);
}

// Stage 4.2: do_cycle feeds each hardware context its own trace, keyed by
// trace_index[core][context] (structural, not a flattened stride). The
// tracereader stamped a trace's instructions with asid {position, position}, so
// the asid a context sees is exactly the value named by its trace_index entry.
// Before 4.2 context 1 was never fed; these tests pin that it now is.
namespace
{
champsim::tracereader trace_with_asid(uint8_t asid) { return champsim::tracereader{[asid] { return ooo_model_instr{asid, input_instr{}}; }}; }

// A core with `contexts` hardware contexts. Each partitioned capacity is a
// multiple of the context count, as a core with several contexts requires.
auto context_core(uint32_t index, std::size_t contexts)
{
  return champsim::core_builder{}
      .index(index)
      .num_contexts(contexts)
      .ifetch_buffer_size(2)
      .decode_buffer_size(2)
      .dib_hit_buffer_size(2)
      .dispatch_buffer_size(2)
      .rob_size(2)
      .lq_size(2)
      .sq_size(2);
}

// A trace that returns one record and then reports EOF, so a phase driven by it
// completes on the first tick without retiring anything.
struct finite_reader {
  unsigned remaining = 1;
  ooo_model_instr operator()()
  {
    --remaining;
    return ooo_model_instr{0, input_instr{}};
  }
  bool eof() const { return remaining == 0; }
};

// A minimal environment for do_phase: the two cores, one operable that forwards
// the phase hooks to both, and a stub memory backend. do_phase only needs those
// to name every context's row and collect the contexts' statistics.
struct context_environment : champsim::environment {
  O3_CPU first, second;

  struct forwarder : champsim::operable {
    O3_CPU& first;
    O3_CPU& second;
    forwarder(O3_CPU& f, O3_CPU& s) : operable(champsim::chrono::picoseconds{1}), first(f), second(s) {}
    void begin_phase() override
    {
      first.begin_phase();
      second.begin_phase();
    }
    void end_phase(unsigned cpu) override
    {
      first.end_phase(cpu);
      second.end_phase(cpu);
    }
    long operate() override { return 1; }
  } op;

  struct memory : champsim::memory_backend {
    champsim::operable& op;
    explicit memory(champsim::operable& o) : op(o) {}
    champsim::operable& clocked_component() override { return op; }
    champsim::data::bytes size() const override { return champsim::data::bytes{64}; }
    champsim::memory_statistics statistics() const override { return {}; }
    std::string_view name() const override { return "legacy"; }
  } mem;

  context_environment() : first{context_core(0, 2)}, second{context_core(1, 1)}, op{first, second}, mem{op} {}

  std::vector<std::reference_wrapper<O3_CPU>> cpu_view() override { return {first, second}; }
  std::vector<std::reference_wrapper<CACHE>> cache_view() override { return {}; }
  std::vector<std::reference_wrapper<PageTableWalker>> ptw_view() override { return {}; }
  champsim::memory_backend& memory_view() override { return mem; }
  std::vector<std::reference_wrapper<champsim::operable>> operable_view() override { return {op}; }
};
} // namespace

TEST_CASE("do_cycle feeds two contexts of one core from two traces with distinct asids")
{
  O3_CPU cpu{context_core(0, 2)};
  std::vector<std::reference_wrapper<O3_CPU>> cpus{cpu};
  std::vector<std::reference_wrapper<champsim::operable>> operables{};
  std::vector<champsim::tracereader> traces;
  traces.push_back(trace_with_asid(0)); // asid {0,0}
  traces.push_back(trace_with_asid(1)); // asid {1,1}
  champsim::chrono::clock clock;

  std::vector<std::vector<std::size_t>> trace_index{{0, 1}};
  champsim::do_cycle(operables, cpus, traces, trace_index, clock);

  REQUIRE(!cpu.contexts[0].input_queue.empty());
  REQUIRE(!cpu.contexts[1].input_queue.empty());
  for (const auto& instr : cpu.contexts[0].input_queue) {
    CHECK(instr.asid == champsim::address_space_id{0, 0});
  }
  for (const auto& instr : cpu.contexts[1].input_queue) {
    CHECK(instr.asid == champsim::address_space_id{1, 1});
  }
}

TEST_CASE("do_cycle keys trace_index by (core, context), not by a shared stride")
{
  // Non-uniform context counts: a flat core_index * num_contexts + context
  // stride has no single num_contexts to divide by, so it would mis-index this
  // machine. The structural [core][context] key does not.
  O3_CPU core0{context_core(0, 2)};
  O3_CPU core1{champsim::core_builder{}.index(1).num_contexts(1)};
  std::vector<std::reference_wrapper<O3_CPU>> cpus{core0, core1};
  std::vector<std::reference_wrapper<champsim::operable>> operables{};
  std::vector<champsim::tracereader> traces;
  traces.push_back(trace_with_asid(0)); // asid {0,0}
  traces.push_back(trace_with_asid(1)); // asid {1,1}
  traces.push_back(trace_with_asid(2)); // asid {2,2}
  champsim::chrono::clock clock;

  std::vector<std::vector<std::size_t>> trace_index{{0, 1}, {2}};
  champsim::do_cycle(operables, cpus, traces, trace_index, clock);

  REQUIRE(!core0.contexts[0].input_queue.empty());
  REQUIRE(!core0.contexts[1].input_queue.empty());
  REQUIRE(!core1.contexts[0].input_queue.empty());
  for (const auto& instr : core0.contexts[0].input_queue) {
    CHECK(instr.asid == champsim::address_space_id{0, 0});
  }
  for (const auto& instr : core0.contexts[1].input_queue) {
    CHECK(instr.asid == champsim::address_space_id{1, 1});
  }
  for (const auto& instr : core1.contexts[0].input_queue) {
    CHECK(instr.asid == champsim::address_space_id{2, 2});
  }
}

TEST_CASE("do_phase names every context by its core and context")
{
  context_environment env;
  std::vector<champsim::tracereader> traces;
  traces.emplace_back(finite_reader{});
  traces.emplace_back(finite_reader{});
  traces.emplace_back(finite_reader{});
  champsim::chrono::clock clock;
  champsim::phase_info phase{"Simulation", false, 1, {{0, 1}, {2}}, {"a", "b", "c"}};

  const auto stats = champsim::do_phase(phase, env, traces, clock, {});

  // One row per (core, context), in core-major/context-minor order: the trace
  // each row runs, the naming slot's path, and that context's statistics name.
  REQUIRE(stats.trace_names == std::vector<std::string>{"a", "b", "c"});
  REQUIRE(std::size(stats.slots) == 3);
  REQUIRE(stats.slots.at(0).path() == "cpu0.ctx0");
  REQUIRE(stats.slots.at(1).path() == "cpu0.ctx1");
  REQUIRE(stats.slots.at(2).path() == "cpu1");
  REQUIRE(stats.roi_cpu_stats.at(0).name == "CPU 0 ctx 0");
  REQUIRE(stats.roi_cpu_stats.at(1).name == "CPU 0 ctx 1");
  REQUIRE(stats.roi_cpu_stats.at(2).name == "CPU 1");
}
