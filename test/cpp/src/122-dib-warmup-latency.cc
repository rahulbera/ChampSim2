#include <algorithm>
#include <catch.hpp>

#include "bandwidth.h"
#include "champsim.h"
#include "defaults.hpp"
#include "instr.h"
#include "mocks.hpp"
#include "ooo_cpu.h"
#include "operable.h"

// promote_to_decode() charges DIB_HIT_LATENCY to a DIB hit and DECODE_LATENCY to
// a miss. Warmup exists to populate structures rather than to be timed, so the
// decode path has always been free during warmup -- but the DIB path was not,
// which made the two front-end routes asymmetric during warmup in a way nothing
// intended. Every other DIB test sets warmup = false, which is why that survived.
//
// The two scenarios below are a pair: during warmup the hit latency must make no
// difference, and outside warmup it must. Without the second one this file would
// pass even if DIB hits were never charged at all.

namespace
{
// Drive the same instruction through the core twice and return how long the
// SECOND pass took -- the second pass is the one that hits in the DIB.
long second_pass_cycles(unsigned dib_hit_latency, bool warmup)
{
  constexpr unsigned fetch_latency = 3;
  constexpr unsigned decode_latency = 1;
  constexpr unsigned dispatch_latency = 2;
  constexpr unsigned schedule_latency = 2;
  constexpr unsigned execute_latency = 2;

  do_nothing_MRC mock_L1I{fetch_latency}, mock_L1D;
  O3_CPU uut{champsim::core_builder{champsim::defaults::default_core}
                 .fetch_queues(&mock_L1I.queues)
                 .data_queues(&mock_L1D.queues)
                 .decode_latency(decode_latency)
                 .dispatch_latency(dispatch_latency)
                 .schedule_latency(schedule_latency)
                 .execute_latency(execute_latency)
                 .dib_hit_latency(dib_hit_latency)
                 .execute_width(champsim::bandwidth::maximum_type{1})
                 .decode_width(champsim::bandwidth::maximum_type{1})
                 .dispatch_width(champsim::bandwidth::maximum_type{1})
                 .fetch_width(champsim::bandwidth::maximum_type{1})
                 .retire_width(champsim::bandwidth::maximum_type{1})};
  uut.warmup = warmup;

  std::vector test_instructions(1, champsim::test::instruction_with_ip(1));
  std::array<champsim::operable*, 3> elements{{&uut, &mock_L1I, &mock_L1D}};

  // First pass: misses the DIB, and fills it at decode.
  uut.IFETCH_BUFFER.insert(std::end(uut.IFETCH_BUFFER), std::begin(test_instructions), std::end(test_instructions));
  for (long i = 0; uut.num_retired < 1 && i < 500; ++i)
    for (auto op : elements)
      op->_operate();

  // Second pass: hits the DIB.
  const auto begin = uut.current_time;
  uut.IFETCH_BUFFER.insert(std::end(uut.IFETCH_BUFFER), std::begin(test_instructions), std::end(test_instructions));
  for (long i = 0; uut.num_retired < 2 && i < 500; ++i)
    for (auto op : elements)
      op->_operate();

  REQUIRE(uut.num_retired == 2);
  return (uut.current_time - begin) / uut.clock_period;
}
} // namespace

SCENARIO("A DIB hit is not charged its latency during warmup")
{
  GIVEN("Two cores differing only in their DIB hit latency, both warming up")
  {
    const auto cheap = second_pass_cycles(0, true);
    const auto expensive = second_pass_cycles(8, true);

    THEN("The DIB hit latency makes no difference during warmup")
    {
      INFO("dib_hit_latency=0 took " << cheap << ", dib_hit_latency=8 took " << expensive);
      CHECK(cheap == expensive);
    }
  }
}

SCENARIO("A DIB hit IS charged its latency outside warmup")
{
  // The control. Without this, the scenario above would pass if DIB hits were
  // never charged their latency at all.
  GIVEN("Two cores differing only in their DIB hit latency, not warming up")
  {
    const auto cheap = second_pass_cycles(0, false);
    const auto expensive = second_pass_cycles(8, false);

    THEN("The more expensive DIB hit takes longer")
    {
      INFO("dib_hit_latency=0 took " << cheap << ", dib_hit_latency=8 took " << expensive);
      CHECK(expensive > cheap);
    }
  }
}
