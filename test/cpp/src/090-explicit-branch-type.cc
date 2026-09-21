#include <catch.hpp>

#include <array>
#include <fstream>
#include <string>
#include <vector>

#include "instruction.h"
#include "trace_instruction.h"
#include "tracereader.h"

// A v2 record may state its branch type outright in reserved[0], announced by a
// feature bit in reserved[1]. These tests pin the contract:
//
//   * the explicit type is used when the bit is set,
//   * the register-pattern inference is used unchanged when it is not,
//   * and neither path can be reached by accident, because reserved[0] == 0 is
//     a VALID branch type (BRANCH_DIRECT_JUMP) -- so an all-zero record must
//     take the inference path, not be read as a direct jump.

namespace
{
input_instr_v2 with_explicit(branch_type bt, unsigned char taken)
{
  input_instr_v2 r{};
  r.ip = 0x1000;
  r.reserved[champsim::TRACE_RESERVED_BRANCH_TYPE] = static_cast<unsigned char>(bt);
  r.reserved[champsim::TRACE_RESERVED_FEATURES] =
      champsim::TRACE_FEATURE_EXPLICIT_BRANCH_TYPE | champsim::TRACE_FEATURE_FLAGS_REGISTER;
  r.reserved[champsim::TRACE_RESERVED_TRACER_ID] = 3;
  r.is_branch = (bt == NOT_BRANCH) ? 0 : 1;
  r.branch_taken = taken;
  return r;
}
} // namespace

TEST_CASE("An explicit branch type is used verbatim")
{
  auto [bt, taken] = GENERATE(table<branch_type, unsigned char>({
      {BRANCH_DIRECT_JUMP, 1},
      {BRANCH_INDIRECT, 1},
      {BRANCH_CONDITIONAL, 1},
      {BRANCH_CONDITIONAL, 0},
      {BRANCH_DIRECT_CALL, 1},
      {BRANCH_INDIRECT_CALL, 1},
      {BRANCH_RETURN, 1},
      {BRANCH_OTHER, 0},
  }));

  ooo_model_instr instr{0, with_explicit(bt, taken)};

  REQUIRE(instr.branch == bt);
  REQUIRE(instr.is_branch);
  REQUIRE(instr.branch_taken == (taken != 0));
}

TEST_CASE("An explicit NOT_BRANCH is not a branch")
{
  ooo_model_instr instr{0, with_explicit(NOT_BRANCH, 0)};

  REQUIRE(instr.branch == NOT_BRANCH);
  REQUIRE_FALSE(instr.is_branch);
  REQUIRE_FALSE(instr.branch_taken);
}

TEST_CASE("A not-taken conditional keeps its direction")
{
  // The bug this whole contract exists to prevent: under inference a
  // not-taken conditional whose flags register was dropped by the tracer
  // matched the direct-jump arm and had branch_taken rewritten to true.
  ooo_model_instr instr{0, with_explicit(BRANCH_CONDITIONAL, 0)};

  REQUIRE(instr.branch == BRANCH_CONDITIONAL);
  REQUIRE(instr.is_branch);
  REQUIRE_FALSE(instr.branch_taken);
}

TEST_CASE("Without the feature bit, classification falls back to inference")
{
  // reserved[0] says BRANCH_RETURN, but the feature bit is clear, so it must be
  // ignored entirely -- these bytes are indistinguishable from an old trace's
  // zero padding and must not be trusted.
  input_instr_v2 r{};
  r.ip = 0x1000;
  r.reserved[champsim::TRACE_RESERVED_BRANCH_TYPE] = static_cast<unsigned char>(BRANCH_RETURN);
  r.reserved[champsim::TRACE_RESERVED_FEATURES] = 0;
  // Register pattern of a conditional branch: reads IP + FLAGS, writes IP.
  r.is_branch = 1;
  r.branch_taken = 0;
  r.destination_registers[0] = champsim::REG_INSTRUCTION_POINTER;
  r.source_registers[0] = champsim::REG_INSTRUCTION_POINTER;
  r.source_registers[1] = champsim::REG_FLAGS;

  ooo_model_instr instr{0, r};

  REQUIRE(instr.branch == BRANCH_CONDITIONAL);
  REQUIRE_FALSE(instr.branch_taken);
}

TEST_CASE("An all-zero v2 record is inferred, not read as a direct jump")
{
  // BRANCH_DIRECT_JUMP is 0, so a zeroed reserved[] would be a valid explicit
  // "direct jump" if presence were keyed on the value instead of the feature
  // bit. A record with no registers at all infers to NOT_BRANCH.
  input_instr_v2 r{};
  r.ip = 0x1000;

  ooo_model_instr instr{0, r};

  REQUIRE(instr.branch == NOT_BRANCH);
  REQUIRE_FALSE(instr.branch_taken);
}

TEST_CASE("An out-of-range branch type falls back to inference")
{
  // A corrupt byte, or a future tracer using a value this build does not know,
  // must not be cast blindly into the enum.
  input_instr_v2 r{};
  r.ip = 0x1000;
  r.reserved[champsim::TRACE_RESERVED_BRANCH_TYPE] = 42;
  r.reserved[champsim::TRACE_RESERVED_FEATURES] = champsim::TRACE_FEATURE_EXPLICIT_BRANCH_TYPE;
  r.is_branch = 1;
  r.branch_taken = 1;
  r.destination_registers[0] = champsim::REG_INSTRUCTION_POINTER;
  r.source_registers[0] = champsim::REG_INSTRUCTION_POINTER;
  r.source_registers[1] = champsim::REG_FLAGS;

  ooo_model_instr instr{0, r};

  REQUIRE(instr.branch == BRANCH_CONDITIONAL); // inferred, not 42
}

TEST_CASE("A v1 record is unaffected by the reserved contract")
{
  // v1 has no reserved[] at all; the detector must compile it out rather than
  // fail, and behaviour must be exactly as before.
  input_instr r{};
  r.ip = 0x1000;
  r.is_branch = 1;
  r.branch_taken = 1;
  r.destination_registers[0] = champsim::REG_INSTRUCTION_POINTER;

  ooo_model_instr instr{0, r};

  REQUIRE(instr.branch == BRANCH_DIRECT_JUMP);
  REQUIRE(instr.is_branch);
  REQUIRE(instr.branch_taken);
}

TEST_CASE("The explicit path does not depend on the register fields")
{
  // The whole point: a conditional branch whose flags register never made it
  // into the record still classifies correctly. Under inference this exact
  // record becomes an always-taken BRANCH_DIRECT_JUMP.
  input_instr_v2 r = with_explicit(BRANCH_CONDITIONAL, 0);
  r.destination_registers[0] = champsim::REG_INSTRUCTION_POINTER;
  // no source registers at all -- flags were dropped by the tracer

  ooo_model_instr instr{0, r};

  REQUIRE(instr.branch == BRANCH_CONDITIONAL);
  REQUIRE_FALSE(instr.branch_taken);
}

// --- the feature probe used by get_tracereader() -----------------------------
//
// These exist because the first version of this guard lived inside
// bulk_tracereader, where champsim::repeatable reconstructs the object on every
// wraparound and re-armed it -- so the "once per trace" banner fired once per
// LAP, 26 times on a 25-wrap run. Nothing tested it, so nothing caught it.

TEST_CASE("v2_trace_declares_branch_type reads the feature bit off the first record")
{
  auto write_trace = [](const std::string& path, const std::vector<input_instr_v2>& recs) {
    std::ofstream out{path, std::ios::binary};
    out.write(reinterpret_cast<const char*>(std::data(recs)),
              static_cast<std::streamsize>(std::size(recs) * sizeof(input_instr_v2)));
  };

  SECTION("set when the producing tracer declared it")
  {
    std::vector<input_instr_v2> recs(4);
    for (auto& r : recs) {
      r.reserved[champsim::TRACE_RESERVED_FEATURES] = champsim::TRACE_FEATURE_EXPLICIT_BRANCH_TYPE;
    }
    write_trace("/tmp/cs_probe_yes.champsim2", recs);
    REQUIRE(champsim::v2_trace_declares_branch_type("/tmp/cs_probe_yes.champsim2"));
  }

  SECTION("clear on a pre-fix trace")
  {
    std::vector<input_instr_v2> recs(4); // all-zero reserved[]
    write_trace("/tmp/cs_probe_no.champsim2", recs);
    REQUIRE_FALSE(champsim::v2_trace_declares_branch_type("/tmp/cs_probe_no.champsim2"));
  }

  SECTION("a non-zero branch type alone does NOT count as declared")
  {
    // reserved[0] says BRANCH_RETURN but the feature bit is clear. Keying
    // presence off the value would make every zeroed record a "direct jump".
    std::vector<input_instr_v2> recs(4);
    for (auto& r : recs) {
      r.reserved[champsim::TRACE_RESERVED_BRANCH_TYPE] = static_cast<unsigned char>(BRANCH_RETURN);
    }
    write_trace("/tmp/cs_probe_val.champsim2", recs);
    REQUIRE_FALSE(champsim::v2_trace_declares_branch_type("/tmp/cs_probe_val.champsim2"));
  }

  SECTION("stays quiet on a file too short to tell")
  {
    std::ofstream out{"/tmp/cs_probe_short.champsim2", std::ios::binary};
    const std::array<char, 16> stub{};
    out.write(std::data(stub), std::size(stub));
    out.close();
    // Not enough bytes for a record: report "declared" so we do not add a
    // confusing second banner on top of whatever really went wrong.
    REQUIRE(champsim::v2_trace_declares_branch_type("/tmp/cs_probe_short.champsim2"));
  }

  SECTION("stays quiet on a missing file")
  {
    REQUIRE(champsim::v2_trace_declares_branch_type("/tmp/cs_probe_does_not_exist.champsim2"));
  }
}
