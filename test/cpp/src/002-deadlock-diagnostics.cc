#include <functional>
#include <stdexcept>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "operable.h"

namespace
{
struct printing_operable : champsim::operable {
  bool printed = false;
  long operate() override { return 0; }
  void print_deadlock() override { printed = true; }
};

struct throwing_operable : champsim::operable {
  long operate() override { return 0; }
  void print_deadlock() override { throw std::out_of_range{"vector::_M_range_check: __n (which is 155) >= this->size() (which is 128)"}; }
};
} // namespace

TEST_CASE("Deadlock diagnostics continue past an operable whose printer throws")
{
  // do_phase aborts right after these diagnostics. An exception escaping one
  // printer would reach std::terminate instead, before the later operables --
  // the memory backend prints last -- and before stdout is flushed.
  throwing_operable core;
  printing_operable cache;
  throwing_operable walker;
  printing_operable memory;
  const std::vector<std::reference_wrapper<champsim::operable>> operables{core, cache, walker, memory};
  REQUIRE_NOTHROW(champsim::print_deadlock_diagnostics(operables));
  REQUIRE(cache.printed);
  REQUIRE(memory.printed);
}
