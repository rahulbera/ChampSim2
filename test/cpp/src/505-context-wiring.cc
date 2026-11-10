#include <array>
#include <catch.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
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
std::vector<std::vector<std::size_t>> context_trace_index(const std::vector<std::reference_wrapper<O3_CPU>>& cpus);
}

// do_cycle feeds each hardware context its own trace, keyed by
// trace_index[core][context] (structural, not a flattened stride). The
// tracereader stamped a trace's instructions with asid {position, position}, so
// the asid a context sees is exactly the value named by its trace_index entry.
// Before Stage 4 context 1 was never fed; these tests pin that it now is.
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

TEST_CASE("do_cycle keys trace_index by each core's index and context, whatever the order of cores and traces")
{
  // The cores are listed core1 before core0 and the trace table's rows are not
  // in positional order either. The key is cpu.cpu, not the vector's position,
  // so a stride over the cpus vector (or a flattened core * 2 + c) mis-feeds
  // both cores here.
  O3_CPU core0{context_core(0, 2)};
  O3_CPU core1{context_core(1, 2)};
  std::vector<std::reference_wrapper<O3_CPU>> cpus{core1, core0};
  std::vector<std::reference_wrapper<champsim::operable>> operables{};
  std::vector<champsim::tracereader> traces;
  traces.push_back(trace_with_asid(40));
  traces.push_back(trace_with_asid(41));
  traces.push_back(trace_with_asid(42));
  traces.push_back(trace_with_asid(43));
  champsim::chrono::clock clock;

  // trace_index[cpu.cpu][context]: core0 reads traces 3 and 1, core1 reads 0
  // and 2.
  std::vector<std::vector<std::size_t>> trace_index{{3, 1}, {0, 2}};
  champsim::do_cycle(operables, cpus, traces, trace_index, clock);

  REQUIRE(!core0.contexts[0].input_queue.empty());
  REQUIRE(!core0.contexts[1].input_queue.empty());
  REQUIRE(!core1.contexts[0].input_queue.empty());
  REQUIRE(!core1.contexts[1].input_queue.empty());
  for (const auto& instr : core0.contexts[0].input_queue) {
    CHECK(instr.asid == champsim::address_space_id{43, 43});
  }
  for (const auto& instr : core0.contexts[1].input_queue) {
    CHECK(instr.asid == champsim::address_space_id{41, 41});
  }
  for (const auto& instr : core1.contexts[0].input_queue) {
    CHECK(instr.asid == champsim::address_space_id{40, 40});
  }
  for (const auto& instr : core1.contexts[1].input_queue) {
    CHECK(instr.asid == champsim::address_space_id{42, 42});
  }
}

TEST_CASE("main()'s trace table numbers the traces core-major and context-minor")
{
  O3_CPU core0_1{context_core(0, 1)};
  O3_CPU core0_2{context_core(0, 2)};
  O3_CPU core1_1{context_core(1, 1)};
  O3_CPU core1_2{context_core(1, 2)};

  // {1} -> {{0}}; {2,1} -> {{0,1},{2}}; {1,2} -> {{0},{1,2}}; {2,2} ->
  // {{0,1},{2,3}}. The counter walks cores then contexts, so a flat
  // cpu * max_contexts + context stride would mis-number the {2,1} and {1,2}
  // machines.
  REQUIRE(champsim::context_trace_index({core0_1}) == std::vector<std::vector<std::size_t>>{{0}});
  REQUIRE(champsim::context_trace_index({core0_2, core1_1}) == std::vector<std::vector<std::size_t>>{{0, 1}, {2}});
  REQUIRE(champsim::context_trace_index({core0_1, core1_2}) == std::vector<std::vector<std::size_t>>{{0}, {1, 2}});
  REQUIRE(champsim::context_trace_index({core0_2, core1_2}) == std::vector<std::vector<std::size_t>>{{0, 1}, {2, 3}});
}

TEST_CASE("get_tracereaders opens trace k in address space {k, k}")
{
  // Two-record v1 files, named by pid so parallel runs do not collide, removed
  // afterwards. get_tracereaders numbers each reader by its command-line
  // position, and get_tracereader stamps every record with {position, position}.
  const std::string stem = (std::filesystem::temp_directory_path() / ("champsim-505-" + std::to_string(::getpid()) + "-")).string();
  std::array<std::string, 3> names{stem + "0.trace", stem + "1.trace", stem + "2.trace"};
  for (const auto& name : names) {
    std::ofstream out{name, std::ios::binary};
    input_instr recs[2]{};
    out.write(reinterpret_cast<const char*>(recs), static_cast<std::streamsize>(sizeof(recs)));
    out.close();
  }

  auto traces = get_tracereaders({names[0], names[1], names[2]}, false, false, 1);
  for (std::size_t k = 0; k < std::size(traces); ++k) {
    CHECK(traces.at(k)().asid == champsim::address_space_id{static_cast<uint8_t>(k), static_cast<uint8_t>(k)});
  }

  for (const auto& name : names) {
    std::filesystem::remove(name);
  }
}
