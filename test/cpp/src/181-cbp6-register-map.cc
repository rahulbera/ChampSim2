#include <catch.hpp>

#include <array>
#include <set>

#include "cbp6/cbp6_register_map.h"
#include "trace_instruction.h"

// CBP2025 predictors index a 65-entry register file laid out AArch64-style:
// 0-30 integer, 31 stack pointer, 32-63 SIMD/FP, 64 flags. RUNLTS's
// make_reg_digest() branches on that number -- <32 hashes as an integer, 32-63
// as a float, ==64 as flag bits -- so a register mapped into the wrong band is
// hashed with the wrong function and quietly produces a meaningless feature.
//
// ChampSim's numbers are Pin's x86 REG enum truncated to a byte. Measured over
// 3M records of 723.llvm_r.sp1, exactly 38 distinct values occur:
//   3-18     general purpose registers
//   25       flags
//   26       instruction pointer
//   155-189  SIMD
// The mapping is asserted here rather than assumed, because nothing downstream
// can detect it being wrong.

using champsim::cbp6::cbp6_register;

TEST_CASE("x86 general purpose registers map into the integer band")
{
  for (unsigned char reg = 3; reg <= 18; ++reg) {
    const auto mapped = cbp6_register(reg);
    REQUIRE(mapped.has_value());
    REQUIRE(*mapped < 32); // integer band: hashed by run-length + low bits
  }
}

TEST_CASE("The flags register maps to the flags slot")
{
  const auto mapped = cbp6_register(champsim::REG_FLAGS);
  REQUIRE(mapped.has_value());
  REQUIRE(*mapped == 64); // RUNLTS hashes slot 64 as replicated flag bits
}

TEST_CASE("SIMD registers map into the floating-point band")
{
  for (unsigned char reg : {static_cast<unsigned char>(155), static_cast<unsigned char>(162), static_cast<unsigned char>(171),
                            static_cast<unsigned char>(179), static_cast<unsigned char>(187), static_cast<unsigned char>(189)}) {
    const auto mapped = cbp6_register(reg);
    REQUIRE(mapped.has_value());
    REQUIRE(*mapped >= 32);
    REQUIRE(*mapped < 64); // FP band: hashed by exponent classification
  }
}

TEST_CASE("The instruction pointer is not a data register and is dropped")
{
  // Its value is the PC, which the predictor already has; feeding it would
  // consume a register slot to tell the predictor something it knows.
  REQUIRE_FALSE(cbp6_register(champsim::REG_INSTRUCTION_POINTER).has_value());
}

TEST_CASE("Register 0 is the no-register sentinel and is dropped")
{
  REQUIRE_FALSE(cbp6_register(0).has_value());
}

TEST_CASE("Every mapped register lands in range and no two collide")
{
  // A collision would make two architectural registers share one slot, so one
  // would repeatedly invalidate the other's value.
  std::set<unsigned> seen;
  constexpr std::array<unsigned char, 37> observed{{3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  16,  17,  18,  25,  155, 156,
                                                    157, 158, 159, 160, 161, 162, 171, 172, 173, 174, 175, 176, 177, 178, 179, 187, 188, 189}};
  for (unsigned char reg : observed) {
    const auto mapped = cbp6_register(reg);
    REQUIRE(mapped.has_value());
    REQUIRE(*mapped <= 64);
    REQUIRE(seen.insert(*mapped).second); // no duplicates
  }
  REQUIRE(std::size(seen) == 37); // 16 GPR + flags + 20 SIMD
}

TEST_CASE("An unknown register number is dropped rather than guessed")
{
  // Mapping an unrecognised number into a band would silently hash it wrongly.
  REQUIRE_FALSE(cbp6_register(static_cast<unsigned char>(200)).has_value());
  REQUIRE_FALSE(cbp6_register(static_cast<unsigned char>(100)).has_value());
}
