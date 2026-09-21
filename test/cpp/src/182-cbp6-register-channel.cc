#include <catch.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "cbp6/cbp6_host.h"
#include "instruction.h"

// RUNLTS correlates on the values recently written to architectural registers.
// ChampSim delivers them in two steps: a decode notification marks a register's
// value unknown, and an execute notification supplies it when it is knowable
// (only loads, whose destination register takes the data read). These pin the
// wiring, because a dead channel is invisible -- the predictor keeps working and
// simply stops using the feature.

namespace
{
struct value_tenant {
  struct call {
    std::string name;
    uint64_t seq_no{};
    unsigned reg{};
    uint64_t value{};
  };
  std::vector<call> calls;

  void setup() {}
  void terminate() {}
  bool predict(uint64_t, uint8_t, uint64_t) { return false; }
  void history_update(uint64_t, uint8_t, uint64_t, int, bool, bool, uint64_t) {}
  void TrackOtherInst(uint64_t, int, bool, bool, uint64_t) {}
  void update(uint64_t, uint8_t, uint64_t, bool, bool, uint64_t) {}

  void decode_notify(uint64_t seq_no, uint8_t, uint64_t reg) { calls.push_back({"decode", seq_no, static_cast<unsigned>(reg), 0}); }
  void execute_notify(uint64_t seq_no, uint8_t, uint64_t reg, uint64_t value)
  {
    calls.push_back({"execute", seq_no, static_cast<unsigned>(reg), value});
  }
};

// A tenant with no register-value channel at all, like CBP2016 TAGE-SC-L.
struct plain_tenant {
  void setup() {}
  void terminate() {}
  bool predict(uint64_t, uint8_t, uint64_t) { return false; }
  void history_update(uint64_t, uint8_t, uint64_t, int, bool, bool, uint64_t) {}
  void TrackOtherInst(uint64_t, int, bool, bool, uint64_t) {}
  void update(uint64_t, uint8_t, uint64_t, bool, bool, uint64_t) {}
};

constexpr unsigned char SOME_GPR = 10;
} // namespace

TEST_CASE("A decoded destination register is reported to the tenant, remapped")
{
  champsim::cbp6::host<value_tenant> uut;
  uut.decode_notify(7, SOME_GPR);

  const auto& c = uut.tenant().calls;
  REQUIRE(std::size(c) == 1);
  REQUIRE(c.at(0).name == "decode");
  REQUIRE(c.at(0).seq_no == 7);
  REQUIRE(c.at(0).reg == *champsim::cbp6::cbp6_register(SOME_GPR));
}

TEST_CASE("A load's value reaches the tenant against the register decode reported")
{
  champsim::cbp6::host<value_tenant> uut;
  uut.decode_notify(7, SOME_GPR);
  uut.execute_notify(7, true, 0xdeadbeefULL);

  const auto& c = uut.tenant().calls;
  REQUIRE(std::size(c) == 2);
  REQUIRE(c.at(1).name == "execute");
  REQUIRE(c.at(1).seq_no == 7);
  REQUIRE(c.at(1).value == 0xdeadbeefULL);
  // The architectural register is captured at decode: by execute, ChampSim has
  // overwritten destination_registers with physical IDs.
  REQUIRE(c.at(1).reg == c.at(0).reg);
}

TEST_CASE("An instruction with no knowable value leaves the register unknown")
{
  // An ALU result is not in the trace. The decode notification already marked
  // the register invalid; execute must not invent a value.
  champsim::cbp6::host<value_tenant> uut;
  uut.decode_notify(7, SOME_GPR);
  uut.execute_notify(7, false, 0);

  const auto& c = uut.tenant().calls;
  REQUIRE(std::size(c) == 1);
  REQUIRE(c.at(0).name == "decode");
}

TEST_CASE("An execute notification without a matching decode is ignored")
{
  champsim::cbp6::host<value_tenant> uut;
  uut.execute_notify(99, true, 0x1234);

  REQUIRE(std::empty(uut.tenant().calls));
}

TEST_CASE("A register the mapping does not recognise is dropped, not guessed")
{
  champsim::cbp6::host<value_tenant> uut;
  uut.decode_notify(7, champsim::REG_INSTRUCTION_POINTER); // not a data register
  uut.execute_notify(7, true, 0x1234);

  REQUIRE(std::empty(uut.tenant().calls));
}

TEST_CASE("The in-flight register map drains, so it cannot grow without bound")
{
  champsim::cbp6::host<value_tenant> uut;
  for (uint64_t id = 1; id <= 1000; ++id) {
    uut.decode_notify(id, SOME_GPR);
    uut.execute_notify(id, id % 2 == 0, 0x42);
  }

  REQUIRE(uut.pending_register_writes() == 0);
}

TEST_CASE("A tenant with no register channel is unaffected")
{
  // The hooks must compile away for predictors that do not want them.
  champsim::cbp6::host<plain_tenant> uut;
  REQUIRE_NOTHROW(uut.decode_notify(1, SOME_GPR));
  REQUIRE_NOTHROW(uut.execute_notify(1, true, 0x99));
  REQUIRE(uut.pending_register_writes() == 0);
}
