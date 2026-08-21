#ifndef BTB_PERFECT_INDIRECT_RETURN_STACK_H
#define BTB_PERFECT_INDIRECT_RETURN_STACK_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>

#include "address.h"
#include "champsim.h"

// Copied from btb/basic_btb/. ChampSim compiles EVERY module into EVERY binary
// (config.sh discovers and compiles every module it finds), so an unqualified copy of these
// classes collides at link time with basic_btb's. The namespace gives this copy
// distinct symbols; the code inside is unmodified.
namespace perfect_indirect_impl
{
struct return_stack {
  static constexpr std::size_t max_size = 64;
  static constexpr std::size_t num_call_size_trackers = 1024;

  std::deque<champsim::address> stack;

  /*
   * The following structure identifies the size of call instructions so we can
   * find the target for a call's return, since calls may have different sizes.
   */
  std::array<typename champsim::address::difference_type, num_call_size_trackers> call_size_trackers;

  return_stack() { std::fill(std::begin(call_size_trackers), std::end(call_size_trackers), 4); }

  std::pair<champsim::address, bool> prediction();
  void push(champsim::address ip);
  void calibrate_call_size(champsim::address branch_target);
};

} // namespace perfect_indirect_impl

#endif
