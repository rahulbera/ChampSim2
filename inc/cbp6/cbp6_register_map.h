/*
 * Maps ChampSim's register numbers onto the 65-entry register file CBP2025
 * predictors index.
 *
 * CBP6 traces are AArch64: 0-30 integer, 31 stack pointer, 32-63 SIMD/FP, 64
 * flags. RUNLTS's make_reg_digest() branches on the number -- below 32 it hashes
 * as an integer (leading/trailing run lengths plus low bits), 32-63 as a float
 * (exponent classification), and 64 as replicated flag bits. A register mapped
 * into the wrong band is therefore hashed with the wrong function and yields a
 * feature that looks valid and means nothing.
 *
 * ChampSim's numbers are Pin's x86 REG enum truncated to a byte, which is dense
 * in neither value nor meaning. Measured over 3M records of 723.llvm_r.sp1,
 * exactly 38 distinct numbers occur: 3-18 (GPRs), 25 (flags), 26 (instruction
 * pointer) and 155-189 (SIMD).
 *
 * Anything unrecognised is dropped rather than mapped somewhere plausible: a
 * dropped register costs one feature, a mis-banded register corrupts one.
 */

#ifndef CBP6_REGISTER_MAP_H
#define CBP6_REGISTER_MAP_H

#include <cstdint>
#include <optional>

#include "trace_instruction.h"

namespace champsim::cbp6
{
// The slot a CBP2025 tenant should use for this ChampSim register, or nullopt if
// the register carries nothing useful (the no-register sentinel, the
// instruction pointer, or a number this mapping does not recognise).
[[nodiscard]] inline constexpr std::optional<unsigned> cbp6_register(unsigned char champsim_reg)
{
  // Flags: the one register whose slot is fixed by the tenant's hashing.
  if (champsim_reg == static_cast<unsigned char>(champsim::REG_FLAGS)) {
    return 64U;
  }

  // The instruction pointer is not a data register -- its value is the PC, which
  // the predictor is already given.
  if (champsim_reg == static_cast<unsigned char>(champsim::REG_INSTRUCTION_POINTER)) {
    return std::nullopt;
  }

  // General purpose registers, into the integer band.
  if (champsim_reg >= 3 && champsim_reg <= 18) {
    return static_cast<unsigned>(champsim_reg - 3);
  }

  // SIMD registers, into the floating-point band. The observed numbers fall in
  // three runs; they are packed in order rather than by value so the band stays
  // within 32-63.
  if (champsim_reg >= 155 && champsim_reg <= 162) {
    return 32U + static_cast<unsigned>(champsim_reg - 155);
  }
  if (champsim_reg >= 171 && champsim_reg <= 179) {
    return 40U + static_cast<unsigned>(champsim_reg - 171);
  }
  if (champsim_reg >= 187 && champsim_reg <= 189) {
    return 49U + static_cast<unsigned>(champsim_reg - 187);
  }

  return std::nullopt;
}
} // namespace champsim::cbp6

#endif
