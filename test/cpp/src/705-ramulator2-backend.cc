#include <limits>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "channel.h"
#include "defs.h"
#include "operable.h"
#include "ramulator2_memory_backend.h"
#include "ramulator2_test_driver.hpp"
#include "util/to_underlying.h"

namespace
{
champsim::channel::request_type read_request(uint64_t address = 0x1057)
{
  champsim::channel::request_type request;
  request.address = champsim::address{address};
  request.v_address = champsim::address{0xabcdef};
  request.data = champsim::address{0x123456};
  request.pf_metadata = 17;
  request.instr_depend_on_me = {5, 9};
  request.cpu = 0;
  return request;
}
} // namespace

TEST_CASE("Ramulator retains a partially submitted 32-byte head without resending accepted fragments")
{
  champsim::channel feeder;
  const auto original = read_request();
  REQUIRE(feeder.add_rq(original));
  auto state = std::make_shared<ramulator2_test::driver_state>();
  state->decisions = {true, false, true};
  auto backend = champsim::make_ramulator2_memory_backend(std::make_unique<ramulator2_test::driver>(state, 32), {&feeder});
  auto& memory = backend->clocked_component();
  memory.warmup = false;
  memory.begin_phase();

  memory._operate();
  REQUIRE(feeder.returned.empty());
  REQUIRE(feeder.RQ.size() == 1);
  REQUIRE(state->callbacks.size() == 1);
  state->callbacks[0]();
  REQUIRE(feeder.returned.empty());

  state->complete_on_tick = {1};
  memory._operate();
  REQUIRE(feeder.RQ.empty());
  REQUIRE(feeder.returned.size() == 1);
  REQUIRE(feeder.returned.front().pf_metadata == original.pf_metadata);
  REQUIRE(feeder.returned.front().address == original.address);
  REQUIRE(feeder.returned.front().v_address == original.v_address);
  REQUIRE(feeder.returned.front().data == original.data);
  REQUIRE(feeder.returned.front().instr_depend_on_me == original.instr_depend_on_me);
  REQUIRE(state->accepted == std::vector<ramulator2_test::driver_state::submission>{{false, 0x1040, 0, 32}, {false, 0x1060, 0, 32}});
  REQUIRE(state->attempts.size() == 3);
  const auto stats = backend->statistics().sim_ramulator2.value();
  REQUIRE(stats.accepted_reads == 1);
  REQUIRE(stats.completed_reads == 1);
  REQUIRE(stats.accepted_fragments == 2);
  REQUIRE(stats.completed_fragments == 2);
  REQUIRE(stats.rejected_submissions == 1);
  REQUIRE(stats.outstanding_parents == 0);
  REQUIRE(stats.outstanding_fragments == 0);
  REQUIRE(stats.total_read_latency_ps == 625);
  REQUIRE(stats.read_latency_samples == 1);
}

namespace
{
struct fixture {
  champsim::channel feeder;
  std::shared_ptr<ramulator2_test::driver_state> state = std::make_shared<ramulator2_test::driver_state>();
  std::unique_ptr<champsim::memory_backend> backend;
  explicit fixture(std::size_t bytes = 64)
      : backend(champsim::make_ramulator2_memory_backend(std::make_unique<ramulator2_test::driver>(state, bytes), {&feeder}))
  {
    memory().warmup = false;
    memory().begin_phase();
  }
  champsim::operable& memory() { return backend->clocked_component(); }
  long step() { return memory()._operate(); }
  champsim::ramulator2_statistics stats() { return backend->statistics().sim_ramulator2.value(); }
};
} // namespace

TEST_CASE("Ramulator transports cache blocks at 32, 64 and 128 native bytes")
{
  using submission = ramulator2_test::driver_state::submission;
  const std::vector<std::pair<std::size_t, std::vector<submission>>> cases{
      {32, {{false, 0x1040, 0, 32}, {false, 0x1060, 0, 32}}}, {64, {{false, 0x1040, 0, 64}}}, {128, {{false, 0x1040, 0, 64}}}};
  for (const auto& [bytes, expected] : cases) {
    CAPTURE(bytes);
    fixture uut{bytes};
    REQUIRE(uut.feeder.add_rq(read_request()));
    uut.step();
    REQUIRE(uut.feeder.RQ.empty());
    REQUIRE(uut.feeder.returned.empty());
    REQUIRE(uut.state->accepted == expected);
    uut.state->complete_on_tick = bytes == 32 ? std::deque<std::size_t>{1, 0} : std::deque<std::size_t>{0};
    uut.step();
    REQUIRE(uut.feeder.returned.size() == 1);
    REQUIRE(uut.feeder.returned.front().address == champsim::address{0x1057});
    REQUIRE(uut.stats().outstanding_parents == 0);
  }
}

TEST_CASE("Ramulator rejects invalid transaction geometry before ingesting traffic")
{
  for (const auto bytes : {std::size_t{0}, std::size_t{3}}) {
    REQUIRE_THROWS_WITH(fixture{bytes}, Catch::Matchers::ContainsSubstring("transaction"));
  }
  REQUIRE_THROWS_WITH(champsim::make_ramulator2_memory_backend(nullptr, {}), Catch::Matchers::ContainsSubstring("driver"));
}

TEST_CASE("Independent same-address parents wait for their own out-of-order fragments")
{
  fixture uut{32};
  auto first = read_request();
  auto second = first;
  second.pf_metadata = 29;
  second.instr_depend_on_me = {42};
  REQUIRE(uut.feeder.add_rq(first));
  REQUIRE(uut.feeder.add_rq(second));
  uut.step();
  REQUIRE(uut.feeder.RQ.empty());
  REQUIRE(uut.state->accepted.size() == 4);
  uut.state->complete_on_tick = {3, 0};
  uut.step();
  REQUIRE(uut.feeder.returned.empty());
  REQUIRE(uut.stats().outstanding_parents == 2);
  REQUIRE(uut.stats().outstanding_fragments == 2);
  uut.state->complete_on_tick = {2};
  uut.step();
  REQUIRE(uut.feeder.returned.size() == 1);
  REQUIRE(uut.feeder.returned.front().pf_metadata == 29);
  REQUIRE(uut.feeder.returned.front().instr_depend_on_me == second.instr_depend_on_me);
  uut.state->complete_on_tick = {1};
  uut.step();
  REQUIRE(uut.feeder.returned.size() == 2);
  REQUIRE(uut.feeder.returned.back().pf_metadata == 17);
  REQUIRE(uut.stats().total_read_latency_ps == 3125);
  REQUIRE(uut.stats().read_latency_samples == 2);
  REQUIRE(uut.stats().outstanding_parents == 0);
}

TEST_CASE("The second block of a larger native burst retains a separate parent response")
{
  fixture uut{128};
  auto first = read_request(0x1007);
  auto second = read_request(0x1047);
  second.pf_metadata = 19;
  REQUIRE(uut.feeder.add_rq(first));
  REQUIRE(uut.feeder.add_rq(second));
  uut.step();
  REQUIRE(uut.state->accepted == std::vector<ramulator2_test::driver_state::submission>{{false, 0x1000, 0, 64}, {false, 0x1040, 0, 64}});
  uut.state->complete_on_tick = {1, 0};
  uut.step();
  REQUIRE(uut.feeder.returned.size() == 2);
  REQUIRE(uut.feeder.returned.front().address == second.address);
  REQUIRE(uut.feeder.returned.front().pf_metadata == 19);
  REQUIRE(uut.feeder.returned.back().address == first.address);
}

TEST_CASE("Synchronous last-fragment writes release bookkeeping without unsolicited responses")
{
  fixture uut{32};
  uut.state->immediate_writes = true;
  REQUIRE(uut.feeder.add_wq(read_request())); // response_requested deliberately remains true
  REQUIRE(uut.feeder.add_wq(read_request()));
  REQUIRE(uut.step() > 0);
  REQUIRE(uut.feeder.WQ.empty());
  REQUIRE(uut.feeder.returned.empty());
  const auto stats = uut.stats();
  REQUIRE(stats.accepted_writes == 2);
  REQUIRE(stats.completed_writes == 2);
  REQUIRE(stats.accepted_fragments == 4);
  REQUIRE(stats.completed_fragments == 4);
  REQUIRE(stats.outstanding_parents == 0);
  REQUIRE(stats.outstanding_fragments == 0);
  REQUIRE(stats.read_latency_samples == 0);
}

TEST_CASE("Initial rejection neither admits a parent nor starts its native read latency")
{
  fixture uut;
  uut.state->decisions = {false, false, true};
  REQUIRE(uut.feeder.add_rq(read_request()));
  REQUIRE(uut.step() == 0);
  REQUIRE(uut.step() == 0);
  REQUIRE(uut.feeder.RQ.size() == 1);
  REQUIRE(uut.stats().accepted_reads == 0);
  REQUIRE(uut.stats().outstanding_parents == 0);
  REQUIRE(uut.stats().rejected_submissions == 2);
  uut.state->complete_on_tick = {0};
  REQUIRE(uut.step() > 0);
  REQUIRE(uut.feeder.returned.size() == 1);
  REQUIRE(uut.stats().accepted_reads == 1);
  REQUIRE(uut.stats().completed_reads == 1);
  REQUIRE(uut.stats().total_read_latency_ps == 0);
}

TEST_CASE("Response-suppressed reads still complete native bookkeeping and latency samples")
{
  fixture uut;
  auto request = read_request();
  request.response_requested = false;
  REQUIRE(uut.feeder.add_pq(request));
  uut.state->complete_on_tick = {0};
  uut.step();
  REQUIRE(uut.feeder.PQ.empty());
  REQUIRE(uut.feeder.returned.empty());
  REQUIRE(uut.stats().completed_reads == 1);
  REQUIRE(uut.stats().read_latency_samples == 1);
  REQUIRE(uut.stats().outstanding_parents == 0);
}

TEST_CASE("Each feeder queue retains its own FIFO prefix and native read-write ordering")
{
  fixture uut;
  auto request = read_request(0x1000);
  request.type = access_type::WRITE; // RFO arriving through RQ is still a native read
  REQUIRE(uut.feeder.add_rq(request));
  REQUIRE(uut.feeder.add_rq(read_request(0x2000)));
  REQUIRE(uut.feeder.add_pq(read_request(0x3000)));
  REQUIRE(uut.feeder.add_wq(read_request(0x1000)));
  uut.state->decisions = {false, true, true};
  uut.step();
  REQUIRE(uut.state->attempts == std::vector<ramulator2_test::driver_state::submission>{{false, 0x1000, 0, 64}, {false, 0x3000, 0, 64}, {true, 0x1000, 0, 64}});
  REQUIRE(uut.feeder.RQ.size() == 2);
  REQUIRE(uut.feeder.PQ.empty());
  REQUIRE(uut.feeder.WQ.empty());
  REQUIRE(uut.feeder.returned.empty()); // no adapter-side write forwarding
}

TEST_CASE("A partially submitted head cannot admit the next parent in that queue")
{
  fixture uut{32};
  REQUIRE(uut.feeder.add_rq(read_request(0x1057)));
  REQUIRE(uut.feeder.add_rq(read_request(0x2057)));
  uut.state->decisions = {true, false};
  uut.step();
  REQUIRE(uut.feeder.RQ.size() == 2);
  REQUIRE(uut.state->attempts.size() == 2);
  uut.step();
  REQUIRE(uut.feeder.RQ.empty());
  REQUIRE(uut.state->accepted
          == std::vector<ramulator2_test::driver_state::submission>{
              {false, 0x1040, 0, 32}, {false, 0x1060, 0, 32}, {false, 0x2040, 0, 32}, {false, 0x2060, 0, 32}});
  REQUIRE(uut.state->attempts.size() == 5);
}

TEST_CASE("Native source identity is preserved and invalid IDs fail even during warmup")
{
  for (const bool warmup : {false, true}) {
    for (const auto cpu : {static_cast<uint32_t>(champsim::defs::num_cpus), std::numeric_limits<uint32_t>::max()}) {
      fixture uut;
      uut.memory().warmup = warmup;
      auto request = read_request();
      request.cpu = cpu;
      REQUIRE(uut.feeder.add_rq(request));
      REQUIRE_THROWS_WITH(uut.step(), Catch::Matchers::ContainsSubstring("core id"));
      REQUIRE(uut.feeder.RQ.size() == 1);
      REQUIRE(uut.state->attempts.empty());
    }
  }
  fixture uut;
  auto request = read_request();
  request.cpu = static_cast<uint32_t>(champsim::defs::num_cpus - 1);
  REQUIRE(uut.feeder.add_rq(request));
  uut.step();
  REQUIRE(std::get<2>(uut.state->accepted.at(0)) == request.cpu);
}

namespace
{
// ramulator2_test::driver reports 16 MiB. The first address past it is the
// block a physical next_line prefetcher asks for after the top frame's last
// cache block.
constexpr uint64_t fixture_capacity = uint64_t{1} << 24;

champsim::channel::request_type prefetch_request(uint64_t address)
{
  auto request = read_request(address);
  request.type = access_type::PREFETCH;
  return request;
}

void require_original_response(const champsim::channel& feeder, const champsim::channel::request_type& original)
{
  REQUIRE(feeder.returned.size() == 1);
  REQUIRE(feeder.returned.front().address == original.address);
  REQUIRE(feeder.returned.front().v_address == original.v_address);
  REQUIRE(feeder.returned.front().data == original.data);
  REQUIRE(feeder.returned.front().pf_metadata == original.pf_metadata);
  REQUIRE(feeder.returned.front().instr_depend_on_me == original.instr_depend_on_me);
}
} // namespace

TEST_CASE("An out-of-range prefetch read is answered without a native submission and counted in either phase")
{
  const bool warmup = GENERATE(false, true);
  CAPTURE(warmup);
  fixture uut;
  REQUIRE(static_cast<uint64_t>(uut.backend->size().count()) == fixture_capacity);
  uut.memory().warmup = warmup;
  uut.memory().begin_phase();
  const auto request = prefetch_request(fixture_capacity);
  REQUIRE(uut.feeder.add_pq(request));
  long progress = 0;
  REQUIRE_NOTHROW(progress = uut.step());
  REQUIRE(progress > 0);
  REQUIRE(uut.feeder.PQ.empty());
  require_original_response(uut.feeder, request);
  REQUIRE(uut.state->attempts.empty());
  const auto stats = uut.stats();
  REQUIRE(stats.out_of_range_prefetches == 1);
  REQUIRE(stats.accepted_reads == 0);
  REQUIRE(stats.completed_reads == 0);
  REQUIRE(stats.rejected_submissions == 0);
  REQUIRE(stats.read_latency_samples == 0);
  REQUIRE(stats.outstanding_parents == 0);

  // A prefetch-as-load cache forwards its PREFETCH packets through RQ.
  uut.feeder.returned.clear();
  const auto through_rq = prefetch_request(fixture_capacity + 0x1234);
  REQUIRE(uut.feeder.add_rq(through_rq));
  REQUIRE_NOTHROW(uut.step());
  REQUIRE(uut.feeder.RQ.empty());
  require_original_response(uut.feeder, through_rq);
  REQUIRE(uut.state->attempts.empty());
  REQUIRE(uut.stats().out_of_range_prefetches == 2);

  // The counter is a per-phase event count.
  uut.memory().begin_phase();
  REQUIRE(uut.stats().out_of_range_prefetches == 0);
}

TEST_CASE("Response-suppressed and write-queue out-of-range prefetches are removed and counted without a response")
{
  const bool warmup = GENERATE(false, true);
  CAPTURE(warmup);
  fixture uut;
  uut.memory().warmup = warmup;
  uut.memory().begin_phase();
  auto suppressed = prefetch_request(fixture_capacity);
  suppressed.response_requested = false;
  REQUIRE(uut.feeder.add_pq(suppressed));
  auto write = prefetch_request(std::numeric_limits<uint64_t>::max());
  REQUIRE(write.response_requested); // deliberately left true: a write never answers upstream
  REQUIRE(uut.feeder.add_wq(write));
  REQUIRE_NOTHROW(uut.step());
  REQUIRE(uut.feeder.PQ.empty());
  REQUIRE(uut.feeder.WQ.empty());
  REQUIRE(uut.feeder.returned.empty());
  REQUIRE(uut.state->attempts.empty());
  REQUIRE(uut.stats().out_of_range_prefetches == 2);
  REQUIRE(uut.stats().accepted_writes == 0);
}

TEST_CASE("Out-of-range demand requests and invalid prefetch sources still fail in either phase")
{
  const bool warmup = GENERATE(false, true);
  CAPTURE(warmup);
  for (const auto type : {access_type::LOAD, access_type::RFO, access_type::WRITE, access_type::TRANSLATION}) {
    CAPTURE(access_type_names.at(champsim::to_underlying(type)));
    for (const bool write_queue : {false, true}) {
      CAPTURE(write_queue);
      fixture uut;
      uut.memory().warmup = warmup;
      auto request = read_request(fixture_capacity);
      request.type = type;
      REQUIRE((write_queue ? uut.feeder.add_wq(request) : uut.feeder.add_rq(request)));
      REQUIRE_THROWS_WITH(uut.step(), Catch::Matchers::ContainsSubstring("out of range"));
      REQUIRE(uut.feeder.RQ.size() + uut.feeder.WQ.size() == 1);
      REQUIRE(uut.feeder.returned.empty());
      REQUIRE(uut.state->attempts.empty());
      REQUIRE(uut.stats().out_of_range_prefetches == 0);
    }
  }
  fixture uut;
  uut.memory().warmup = warmup;
  auto request = prefetch_request(fixture_capacity);
  request.cpu = static_cast<uint32_t>(champsim::defs::num_cpus);
  REQUIRE(uut.feeder.add_pq(request));
  REQUIRE_THROWS_WITH(uut.step(), Catch::Matchers::ContainsSubstring("core id"));
  REQUIRE(uut.feeder.PQ.size() == 1);
  REQUIRE(uut.feeder.returned.empty());
  REQUIRE(uut.stats().out_of_range_prefetches == 0);
}

TEST_CASE("An out-of-range prefetch waits behind a rejected head in its own feeder queue")
{
  fixture uut;
  const auto head = prefetch_request(0x2000);
  const auto beyond = prefetch_request(fixture_capacity);
  REQUIRE(uut.feeder.add_pq(head));
  REQUIRE(uut.feeder.add_pq(beyond));
  uut.state->decisions = {false};
  REQUIRE(uut.step() == 0);
  REQUIRE(uut.feeder.PQ.size() == 2);
  REQUIRE(uut.feeder.returned.empty());
  REQUIRE(uut.stats().out_of_range_prefetches == 0);

  // Once the head is admitted, the packet behind it is answered in the same step.
  REQUIRE(uut.step() > 0);
  REQUIRE(uut.feeder.PQ.empty());
  require_original_response(uut.feeder, beyond);
  REQUIRE(uut.state->accepted == std::vector<ramulator2_test::driver_state::submission>{{false, 0x2000, 0, BLOCK_SIZE}});
  const auto stats = uut.stats();
  REQUIRE(stats.out_of_range_prefetches == 1);
  REQUIRE(stats.rejected_submissions == 1);
  REQUIRE(stats.accepted_reads == 1);
  REQUIRE(stats.outstanding_parents == 1);
}

TEST_CASE("An out-of-range prefetch waits behind a partially submitted head, across a warmup phase")
{
  using submission = ramulator2_test::driver_state::submission;
  fixture uut{32};
  const auto head = prefetch_request(0x2000);
  const auto beyond = prefetch_request(fixture_capacity);
  REQUIRE(uut.feeder.add_pq(head));
  REQUIRE(uut.feeder.add_pq(beyond));
  uut.state->decisions = {true, false};
  uut.step();
  REQUIRE(uut.state->accepted == std::vector<submission>{{false, 0x2000, 0, 32}});
  REQUIRE(uut.feeder.PQ.size() == 2);
  REQUIRE(uut.feeder.returned.empty());
  REQUIRE(uut.stats().out_of_range_prefetches == 0);

  // Warmup neither bypasses nor continues the partial head, and so does not
  // reach the packet behind it either, however many steps it takes.
  uut.memory().warmup = true;
  uut.memory().begin_phase();
  for (int i = 0; i < 4; ++i) {
    uut.step();
  }
  REQUIRE(uut.state->accepted.size() == 1);
  REQUIRE(uut.feeder.PQ.size() == 2);
  REQUIRE(uut.feeder.returned.empty());
  REQUIRE(uut.stats().out_of_range_prefetches == 0);

  // Back in a measured phase, the head's second fragment is accepted and the
  // packet behind it is answered in the same step, once.
  uut.memory().warmup = false;
  uut.memory().begin_phase();
  uut.step();
  REQUIRE(uut.state->accepted == std::vector<submission>{{false, 0x2000, 0, 32}, {false, 0x2020, 0, 32}});
  REQUIRE(uut.feeder.PQ.empty());
  require_original_response(uut.feeder, beyond);
  auto stats = uut.stats();
  REQUIRE(stats.out_of_range_prefetches == 1);
  REQUIRE(stats.accepted_reads == 0); // the head was accepted in the first phase
  REQUIRE(stats.outstanding_parents == 1);
  uut.step();
  REQUIRE(uut.feeder.returned.size() == 1);
  uut.memory().end_phase(0);
  REQUIRE(uut.backend->statistics().roi_ramulator2.value().out_of_range_prefetches == 1);
}

TEST_CASE("A prefetch in the last native cache block is still submitted and answered natively")
{
  fixture uut;
  const auto request = prefetch_request(fixture_capacity - 1);
  REQUIRE(uut.feeder.add_pq(request));
  uut.step();
  REQUIRE(uut.feeder.PQ.empty());
  REQUIRE(uut.feeder.returned.empty());
  REQUIRE(uut.state->accepted == std::vector<ramulator2_test::driver_state::submission>{{false, fixture_capacity - BLOCK_SIZE, 0, BLOCK_SIZE}});
  uut.state->complete_on_tick = {0};
  uut.step();
  require_original_response(uut.feeder, request);
  const auto stats = uut.stats();
  REQUIRE(stats.accepted_reads == 1);
  REQUIRE(stats.completed_reads == 1);
  REQUIRE(stats.out_of_range_prefetches == 0);
}

TEST_CASE("Fast warmup returns reads while native maintenance clocks advance without submissions")
{
  fixture uut;
  uut.memory().warmup = true;
  uut.memory().begin_phase();
  const auto request = read_request();
  REQUIRE(uut.feeder.add_rq(request));
  auto suppressed = request;
  suppressed.response_requested = false;
  REQUIRE(uut.feeder.add_pq(suppressed));
  REQUIRE(uut.feeder.add_wq(request));
  REQUIRE(uut.step() > 0);
  REQUIRE(uut.feeder.RQ.empty());
  REQUIRE(uut.feeder.PQ.empty());
  REQUIRE(uut.feeder.WQ.empty());
  REQUIRE(uut.feeder.returned.size() == 1);
  REQUIRE(uut.feeder.returned.front().v_address == request.v_address);
  REQUIRE(uut.feeder.returned.front().data == request.data);
  REQUIRE(uut.state->attempts.empty());
  REQUIRE(uut.state->ticks == 1);
  REQUIRE(uut.stats().accepted_reads == 0);
  uut.memory().warmup = false;
  uut.memory().begin_phase();
  REQUIRE(uut.step() == 0);
  REQUIRE(uut.state->ticks == 2);
  REQUIRE(uut.memory().current_time.time_since_epoch() == champsim::chrono::picoseconds{1250});
  REQUIRE(uut.feeder.sim_stats.RQ_ACCESS == 0);
  REQUIRE(uut.feeder.roi_stats.RQ_ACCESS == 0);
}

TEST_CASE("A partially admitted read survives counter reset with its original latency start")
{
  fixture uut{32};
  REQUIRE(uut.feeder.add_rq(read_request()));
  uut.state->decisions = {true, false, false, true};
  uut.state->complete_on_tick = {0};
  uut.step();
  REQUIRE(uut.stats().outstanding_parents == 1);
  REQUIRE(uut.stats().outstanding_fragments == 0);
  uut.memory().begin_phase();
  REQUIRE(uut.stats().accepted_reads == 0);
  REQUIRE(uut.stats().completed_fragments == 0);
  REQUIRE(uut.stats().outstanding_parents == 1);
  REQUIRE(uut.step() == 0);
  REQUIRE(uut.feeder.RQ.size() == 1);
  uut.state->complete_on_tick = {1};
  uut.step();
  REQUIRE(uut.feeder.returned.size() == 1);
  const auto stats = uut.stats();
  REQUIRE(stats.accepted_reads == 0);
  REQUIRE(stats.completed_reads == 1);
  REQUIRE(stats.accepted_fragments == 1);
  REQUIRE(stats.completed_fragments == 1);
  REQUIRE(stats.rejected_submissions == 1);
  REQUIRE(stats.outstanding_parents == 0);
  REQUIRE(stats.total_read_latency_ps == 1250);
  REQUIRE(stats.read_latency_samples == 1);
}

TEST_CASE("Fully accepted fragments remain outstanding across a phase reset")
{
  fixture uut;
  REQUIRE(uut.feeder.add_rq(read_request()));
  uut.step();
  REQUIRE(uut.feeder.RQ.empty());
  uut.memory().begin_phase();
  REQUIRE(uut.stats().accepted_fragments == 0);
  REQUIRE(uut.stats().outstanding_fragments == 1);
  uut.state->complete_on_tick = {0};
  uut.step();
  REQUIRE(uut.stats().completed_reads == 1);
  REQUIRE(uut.stats().completed_fragments == 1);
  REQUIRE(uut.stats().total_read_latency_ps == 625);
  REQUIRE(uut.stats().outstanding_fragments == 0);
}

TEST_CASE("Each CPU completion freezes an owned memory ROI snapshot without finalizing")
{
  fixture uut;
  REQUIRE(uut.feeder.add_rq(read_request()));
  uut.state->complete_on_tick = {0};
  uut.step();
  uut.memory().end_phase(0);
  const auto first = uut.backend->statistics().roi_ramulator2.value();
  REQUIRE(first.completed_reads == 1);
  REQUIRE(uut.feeder.add_rq(read_request(0x2057)));
  uut.state->complete_on_tick = {1};
  uut.step();
  REQUIRE(uut.stats().completed_reads == 2);
  REQUIRE(uut.backend->statistics().roi_ramulator2->completed_reads == 1);
  REQUIRE(first.native.yaml == "native_ticks: 1");
  uut.memory().end_phase(1);
  REQUIRE(uut.backend->statistics().roi_ramulator2->completed_reads == 2);
  REQUIRE(uut.backend->statistics().roi_ramulator2->native.yaml == "native_ticks: 2");
  REQUIRE(uut.state->resets == 1);
  REQUIRE(uut.state->finalizations == 0);
}

TEST_CASE("Finalization does not drain outstanding traffic or emit late responses")
{
  fixture uut;
  REQUIRE(uut.feeder.add_rq(read_request()));
  uut.step();
  const auto record = uut.backend->config_record().value();
  REQUIRE(record.path == "fixture.yaml");
  REQUIRE(record.yaml == "frontend: External\n");
  uut.backend->finalize();
  uut.backend->finalize();
  REQUIRE(uut.state->finalizations == 1);
  REQUIRE(uut.state->ticks == 1);
  uut.state->callbacks[0]();
  REQUIRE(uut.feeder.returned.empty());
  REQUIRE_THROWS_WITH(uut.step(), Catch::Matchers::ContainsSubstring("finalized"));
  REQUIRE(uut.state->ticks == 1);
  uut.backend.reset();
  uut.state->callbacks[0]();
  REQUIRE(uut.feeder.returned.empty());
}
