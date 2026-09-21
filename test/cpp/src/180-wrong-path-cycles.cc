#include <catch.hpp>

#include "core_stats.h"
#include "defaults.hpp"
#include "mocks.hpp"
#include "ooo_cpu.h"

// Cycles lost to wrong-path fetch, the ChampSim analogue of CBP2025's CycWP.
//
// ChampSim does not fetch down a wrong path -- on a misprediction it freezes
// fetch (src/ooo_cpu.cc:157) and restarts it either at decode or at execute
// completion, plus BRANCH_MISPREDICT_PENALTY. The cycles between those two
// points produced no useful work, and are what CBP2025 reports as CycWP.
//
// Reported per 1K instructions as CycWPKI alongside MPKI, because the two say
// different things: a predictor that mispredicts rarely but expensively and one
// that mispredicts often but cheaply can share an MPKI and differ in cost.

namespace
{
auto make_core(do_nothing_MRC& l1i, do_nothing_MRC& l1d)
{
  return O3_CPU{champsim::core_builder{champsim::defaults::default_core}.fetch_queues(&l1i.queues).data_queues(&l1d.queues)};
}
} // namespace

TEST_CASE("Resuming fetch with no stall in progress charges nothing")
{
  // do_complete_execution resumes fetch for any instruction carrying the
  // mispredicted bit; if no stall was recorded there is nothing to charge, and
  // charging from a default-constructed time point would bill the epoch.
  do_nothing_MRC l1i, l1d;
  auto uut = make_core(l1i, l1d);

  REQUIRE_FALSE(uut.fetch_stalled_on_mispredict);
  uut.resume_fetch_after_mispredict();

  REQUIRE(uut.sim_stats.cycles_on_wrong_path == 0);
}

TEST_CASE("Resuming fetch charges the frozen interval plus the penalty")
{
  do_nothing_MRC l1i, l1d;
  auto uut = make_core(l1i, l1d);

  // Freeze fetch 40 cycles ago.
  constexpr long frozen_for = 40;
  uut.fetch_stalled_on_mispredict = true;
  uut.fetch_stall_begin = uut.current_time - (frozen_for * uut.clock_period);

  uut.resume_fetch_after_mispredict();

  // The charge runs from the freeze until fetch actually restarts, which is
  // BRANCH_MISPREDICT_PENALTY after now.
  const auto penalty = static_cast<uint64_t>((uut.fetch_resume_time - uut.current_time) / uut.clock_period);
  REQUIRE(uut.sim_stats.cycles_on_wrong_path == static_cast<uint64_t>(frozen_for) + penalty);
}

TEST_CASE("A resumed stall is not charged twice")
{
  // Both the decode and the execute path can call this for the same branch.
  do_nothing_MRC l1i, l1d;
  auto uut = make_core(l1i, l1d);

  uut.fetch_stalled_on_mispredict = true;
  uut.fetch_stall_begin = uut.current_time - (40 * uut.clock_period);

  uut.resume_fetch_after_mispredict();
  const auto after_first = uut.sim_stats.cycles_on_wrong_path;
  REQUIRE(after_first > 0);
  REQUIRE_FALSE(uut.fetch_stalled_on_mispredict);

  uut.resume_fetch_after_mispredict();
  REQUIRE(uut.sim_stats.cycles_on_wrong_path == after_first);
}

TEST_CASE("Wrong-path cycles are carried through the stats subtraction")
{
  // cpu_stats are differenced to produce a phase's numbers, so the new field
  // has to participate like the others.
  cpu_stats later{};
  cpu_stats earlier{};
  later.cycles_on_wrong_path = 900;
  earlier.cycles_on_wrong_path = 250;

  const auto diff = later - earlier;
  REQUIRE(diff.cycles_on_wrong_path == 650);
}
