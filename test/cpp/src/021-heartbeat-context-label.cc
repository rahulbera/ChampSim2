#include <catch.hpp>
#include <deque>
#include <sstream>
#include <string>

#include "instruction.h"
#include "listeners/heartbeat.h"
#include "trace_instruction.h"

namespace
{
TEST_CASE("The heartbeat labels a multi-context core and a single-context core")
{
  std::ostringstream stdout{};
  Heartbeat uut{&stdout};
  uut.contexts_per_cpu = {2, 1};
  uut.instructions_between_printouts = 4;

  bool in_warmup = false;
  uut.handle_event<Event::BEGIN_PHASE>(in_warmup);

  // Two 2-instruction RETIREs per core: four instructions retired, printed on
  // the second RETIRE (at cycle 1), once per core. Core 0 runs two hardware
  // contexts, so its label names that; core 1 runs one and keeps the bare name.
  std::deque<ooo_model_instr> two{{ooo_model_instr(0, input_instr()), ooo_model_instr(0, input_instr())}};
  for (uint32_t cpu = 0; cpu <= 1; ++cpu) {
    auto cb = std::cbegin(two);
    auto ce = std::cend(two);
    uint64_t cycles = 0;
    uut.handle_event<Event::RETIRE>(cpu, cb, ce, cycles);
    cycles = 1;
    uut.handle_event<Event::RETIRE>(cpu, cb, ce, cycles);
  }

  const std::string res = stdout.str();
  const std::string l1 = res.substr(0, res.find('\n') + 1);
  const std::string rest = res.substr(res.find('\n') + 1);
  const std::string l2 = rest.substr(0, rest.find('\n') + 1);
  const std::string tail = rest.substr(rest.find('\n') + 1);

  REQUIRE_THAT(l1, Catch::Matchers::StartsWith("Heartbeat CPU 0 (2 contexts) instructions: 4 cycles: 1 "));
  REQUIRE_THAT(l2, Catch::Matchers::StartsWith("Heartbeat CPU 1 instructions: 4 cycles: 1 "));
  REQUIRE(tail.length() < 2);
}
} // namespace
