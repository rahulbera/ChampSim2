#include <array>
#include <catch.hpp>

#include "instr.h"
#include "mocks.hpp"
#include "ooo_cpu.h"

// The decoded-instruction buffer (DIB) lets an instruction skip both the L1I
// access and decode when its window was decoded recently. Its hit rate is a
// front-end property worth measuring on its own, so the core counts every
// lookup.
//
// A lookup is charged PER INSTRUCTION: `do_check_dib` runs once for each
// instruction that reaches the fetch stage, gated by `dib_checked`.
//
// What a hit means is subtler than "a neighbour shares my window". The DIB is
// filled at DECODE (`do_dib_update`), many cycles after `check_dib` has already
// classified the whole fetch group, so a cold fetch group misses in its
// entirety -- four instructions sharing a 16-byte window are four misses on
// their first pass. Hits come from re-executing code that has already decoded.
// The scenarios below pin both halves of that, because it is the part a reader
// of the statistic is most likely to get wrong.

SCENARIO("A DIB lookup that finds nothing is counted as a miss")
{
  GIVEN("A core with an empty DIB")
  {
    do_nothing_MRC mock_L1I;
    do_nothing_MRC mock_L1D;
    O3_CPU uut{champsim::core_builder{}.dib_window(16).fetch_queues(&mock_L1I.queues).data_queues(&mock_L1D.queues)};

    THEN("No lookup has been counted yet")
    {
      REQUIRE(uut.sim_stats.dib_lookups() == 0);
      REQUIRE(uut.sim_stats.dib_hits == 0);
      REQUIRE(uut.sim_stats.dib_misses == 0);
    }

    WHEN("An instruction is checked against the DIB")
    {
      auto instr = champsim::test::instruction_with_ip(0xdeadbeef);
      uut.do_check_dib(instr);

      THEN("The lookup is counted, and counted as a miss")
      {
        REQUIRE(uut.sim_stats.dib_lookups() == 1);
        REQUIRE(uut.sim_stats.dib_hits == 0);
        REQUIRE(uut.sim_stats.dib_misses == 1);
      }
    }
  }
}

SCENARIO("A lookup that finds the window already in the table is counted as a hit")
{
  // These scenarios fill the DIB by hand, which is what the decode stage does
  // via `do_dib_update`. They pin the classification of a table lookup, NOT the
  // timing -- see the pipeline scenarios below for when a fill actually lands.
  GIVEN("A core whose DIB holds one decoded instruction")
  {
    do_nothing_MRC mock_L1I;
    do_nothing_MRC mock_L1D;
    O3_CPU uut{champsim::core_builder{}.dib_window(16).fetch_queues(&mock_L1I.queues).data_queues(&mock_L1D.queues)};

    auto first = champsim::test::instruction_with_ip(0xdeadbeef);
    uut.do_check_dib(first);
    uut.do_dib_update(first);

    WHEN("The same instruction is checked again")
    {
      auto again = champsim::test::instruction_with_ip(0xdeadbeef);
      uut.do_check_dib(again);

      THEN("The second lookup is counted as a hit, and the first remains a miss")
      {
        REQUIRE(uut.sim_stats.dib_lookups() == 2);
        REQUIRE(uut.sim_stats.dib_hits == 1);
        REQUIRE(uut.sim_stats.dib_misses == 1);
      }
    }

    WHEN("A different instruction in the same DIB window is checked")
    {
      auto neighbor = champsim::test::instruction_with_ip(0xdeadbeee);
      uut.do_check_dib(neighbor);

      THEN("It is a second lookup, and the table matches on the window")
      {
        REQUIRE(uut.sim_stats.dib_lookups() == 2);
        REQUIRE(uut.sim_stats.dib_hits == 1);
        REQUIRE(uut.sim_stats.dib_misses == 1);
      }
    }
  }
}

namespace
{
auto make_pipelined_core(do_nothing_MRC& l1i, do_nothing_MRC& l1d)
{
  return O3_CPU{champsim::core_builder{}
                    .dib_set(32)
                    .dib_way(8)
                    .dib_window(16)
                    .ifetch_buffer_size(64)
                    .fetch_width(champsim::bandwidth::maximum_type{6})
                    .decode_width(champsim::bandwidth::maximum_type{6})
                    .l1i_bandwidth(champsim::bandwidth::maximum_type{10})
                    .l1d_bandwidth(champsim::bandwidth::maximum_type{10})
                    .fetch_queues(&l1i.queues)
                    .data_queues(&l1d.queues)};
}
} // namespace

SCENARIO("A cold fetch group misses on every one of its instructions")
{
  // The counter-intuitive case, and the reason this file exists. `check_dib`
  // classifies up to FETCH_WIDTH instructions in one cycle, but the DIB is not
  // filled until those instructions reach decode. So sharing a window with an
  // instruction fetched alongside you buys nothing: the whole group misses.
  GIVEN("Four instructions that all fall in one 16-byte DIB window")
  {
    do_nothing_MRC mock_L1I;
    do_nothing_MRC mock_L1D;
    auto uut = make_pipelined_core(mock_L1I, mock_L1D);
    std::array<champsim::operable*, 3> elements{{&uut, &mock_L1I, &mock_L1D}};

    for (uint64_t ip : {0x1000ULL, 0x1004ULL, 0x1008ULL, 0x100cULL}) {
      uut.IFETCH_BUFFER.push_back(champsim::test::instruction_with_ip(ip));
    }

    WHEN("They are fetched together")
    {
      for (int i = 0; i < 40; ++i) {
        for (auto op : elements) {
          op->_operate();
        }
      }

      THEN("All four lookups miss -- the fill has not happened yet for any of them")
      {
        REQUIRE(uut.sim_stats.dib_lookups() == 4);
        REQUIRE(uut.sim_stats.dib_hits == 0);
        REQUIRE(uut.sim_stats.dib_misses == 4);
      }
    }
  }
}

SCENARIO("A window that has already decoded hits when its code runs again")
{
  GIVEN("A core that has fetched and decoded one instruction")
  {
    do_nothing_MRC mock_L1I;
    do_nothing_MRC mock_L1D;
    auto uut = make_pipelined_core(mock_L1I, mock_L1D);
    std::array<champsim::operable*, 3> elements{{&uut, &mock_L1I, &mock_L1D}};

    uut.IFETCH_BUFFER.push_back(champsim::test::instruction_with_ip(0x1000ULL));
    for (int i = 0; i < 40; ++i) {
      for (auto op : elements) {
        op->_operate();
      }
    }

    REQUIRE(uut.sim_stats.dib_lookups() == 1);
    REQUIRE(uut.sim_stats.dib_misses == 1);

    WHEN("A later instruction in the same window is fetched")
    {
      uut.IFETCH_BUFFER.push_back(champsim::test::instruction_with_ip(0x1004ULL));
      for (int i = 0; i < 40; ++i) {
        for (auto op : elements) {
          op->_operate();
        }
      }

      THEN("It hits, because decode filled the window in the meantime")
      {
        REQUIRE(uut.sim_stats.dib_lookups() == 2);
        REQUIRE(uut.sim_stats.dib_hits == 1);
        REQUIRE(uut.sim_stats.dib_misses == 1);
      }
    }
  }
}

SCENARIO("Warmup DIB lookups do not reach the region of interest")
{
  GIVEN("A core that has already performed lookups")
  {
    do_nothing_MRC mock_L1I;
    do_nothing_MRC mock_L1D;
    O3_CPU uut{champsim::core_builder{}.dib_window(16).fetch_queues(&mock_L1I.queues).data_queues(&mock_L1D.queues)};

    auto instr = champsim::test::instruction_with_ip(0xdeadbeef);
    uut.do_check_dib(instr);
    uut.do_dib_update(instr);
    auto again = champsim::test::instruction_with_ip(0xdeadbeef);
    uut.do_check_dib(again);

    REQUIRE(uut.sim_stats.dib_lookups() == 2);

    WHEN("The next phase begins")
    {
      uut.begin_phase();

      THEN("The counters start from zero")
      {
        REQUIRE(uut.sim_stats.dib_lookups() == 0);
        REQUIRE(uut.sim_stats.dib_hits == 0);
        REQUIRE(uut.sim_stats.dib_misses == 0);
      }
    }
  }
}

SCENARIO("The core counts a DIB lookup for every instruction it fetches")
{
  GIVEN("A core with three instructions in three different DIB windows")
  {
    do_nothing_MRC mock_L1I;
    do_nothing_MRC mock_L1D;
    auto uut = make_pipelined_core(mock_L1I, mock_L1D);
    std::array<champsim::operable*, 3> elements{{&uut, &mock_L1I, &mock_L1D}};

    for (uint64_t ip : {0x1000ULL, 0x2000ULL, 0x3000ULL}) {
      uut.IFETCH_BUFFER.push_back(champsim::test::instruction_with_ip(ip));
    }

    WHEN("The core operates")
    {
      for (int i = 0; i < 40; ++i) {
        for (auto op : elements) {
          op->_operate();
        }
      }

      THEN("Each instruction contributed exactly one lookup, and each missed")
      {
        REQUIRE(uut.sim_stats.dib_lookups() == 3);
        REQUIRE(uut.sim_stats.dib_hits == 0);
        REQUIRE(uut.sim_stats.dib_misses == 3);
      }
    }
  }
}

SCENARIO("Subtracting core statistics subtracts the DIB counters")
{
  GIVEN("Two snapshots of the same core")
  {
    cpu_stats earlier{};
    earlier.dib_hits = 250;
    earlier.dib_misses = 50;

    cpu_stats later{};
    later.dib_hits = 900;
    later.dib_misses = 100;

    WHEN("The earlier snapshot is subtracted from the later one")
    {
      auto diff = later - earlier;

      THEN("Both counters, and the lookup total, describe the interval")
      {
        REQUIRE(diff.dib_hits == 650);
        REQUIRE(diff.dib_misses == 50);
        REQUIRE(diff.dib_lookups() == 700);
      }
    }
  }
}

SCENARIO("The region of interest freezes when this core finishes, not when another does")
{
  // `end_phase` snapshots sim_stats into roi_stats only for the CPU that
  // finished. With more than one core the others keep fetching afterwards, so
  // this is what makes the ROI a per-core window rather than a whole-run total.
  GIVEN("CPU 1 in a multi-core run, part way through a phase")
  {
    do_nothing_MRC mock_L1I;
    do_nothing_MRC mock_L1D;
    O3_CPU uut{champsim::core_builder{}.index(1).dib_window(16).fetch_queues(&mock_L1I.queues).data_queues(&mock_L1D.queues)};

    uut.begin_phase();
    for (uint64_t ip : {0x1000ULL, 0x2000ULL, 0x3000ULL}) {
      auto instr = champsim::test::instruction_with_ip(ip);
      uut.do_check_dib(instr);
    }

    WHEN("A different CPU finishes the phase")
    {
      uut.end_phase(0);

      THEN("This core's region of interest has not been captured yet")
      {
        REQUIRE(uut.sim_stats.dib_lookups() == 3);
        REQUIRE(uut.roi_stats.dib_lookups() == 0);
      }
    }

    WHEN("This CPU finishes the phase and then keeps fetching")
    {
      auto fourth = champsim::test::instruction_with_ip(0x4000ULL);
      uut.do_check_dib(fourth);
      uut.end_phase(1);

      auto fifth = champsim::test::instruction_with_ip(0x5000ULL);
      uut.do_check_dib(fifth);
      uut.end_phase(0); // a slower core finishes later

      THEN("The region of interest holds the four lookups it had at the time")
      {
        REQUIRE(uut.roi_stats.dib_lookups() == 4);
        REQUIRE(uut.roi_stats.dib_misses == 4);
      }

      THEN("While the whole-phase counters keep running")
      {
        REQUIRE(uut.sim_stats.dib_lookups() == 5);
        REQUIRE(uut.sim_stats.dib_misses == 5);
      }
    }
  }
}
