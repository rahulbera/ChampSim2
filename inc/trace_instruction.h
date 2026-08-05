/*
 *    Copyright 2023 The ChampSim Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef TRACE_INSTRUCTION_H
#define TRACE_INSTRUCTION_H

#include <cstddef>
#include <limits>

// special registers that help us identify branches
namespace champsim
{
constexpr char REG_STACK_POINTER = 6;
constexpr char REG_FLAGS = 25;
constexpr char REG_INSTRUCTION_POINTER = 26;
} // namespace champsim

// instruction format
constexpr std::size_t NUM_INSTR_DESTINATIONS_SPARC = 4;
constexpr std::size_t NUM_INSTR_DESTINATIONS = 2;
constexpr std::size_t NUM_INSTR_SOURCES = 4;

// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays): These classes are deliberately trivial
struct input_instr {
  // instruction pointer or PC (Program Counter)
  unsigned long long ip;

  // branch info
  unsigned char is_branch;
  unsigned char branch_taken;

  unsigned char destination_registers[NUM_INSTR_DESTINATIONS]; // output registers
  unsigned char source_registers[NUM_INSTR_SOURCES];           // input registers

  unsigned long long destination_memory[NUM_INSTR_DESTINATIONS]; // output memory
  unsigned long long source_memory[NUM_INSTR_SOURCES];           // input memory
};

// Widest memory value the v2 trace format can carry, in bytes (AVX-512).
constexpr std::size_t MAX_MEM_VALUE_SIZE = 64;

// The v2 trace record: a 512-byte extension of input_instr.
//
// The first 64 bytes are layout-identical to input_instr, so a v2 record can be
// consumed anywhere a v1 record is expected. The remainder adds physical
// addresses, memory access widths, privilege level, instruction type, and --
// the reason the format exists -- the data values moved by each memory operand.
//
// The layout is fixed by the external tracer that produces these traces, so it
// is asserted rather than assumed. Natural alignment already yields exactly 512
// bytes with no padding, so no packing attribute is needed (and none is used:
// packing would risk unaligned access on the 64-bit members).
struct input_instr_v2 {
  // --- Block 1: layout-identical to input_instr (bytes 0-63) ---
  unsigned long long ip;

  unsigned char is_branch;
  unsigned char branch_taken;

  unsigned char destination_registers[NUM_INSTR_DESTINATIONS]; // output registers
  unsigned char source_registers[NUM_INSTR_SOURCES];           // input registers

  unsigned long long destination_memory[NUM_INSTR_DESTINATIONS]; // output memory (virtual)
  unsigned long long source_memory[NUM_INSTR_SOURCES];           // input memory (virtual)

  // --- Block 2: physical addresses and metadata (bytes 64-127) ---
  unsigned long long destination_memory_pa[NUM_INSTR_DESTINATIONS]; // output memory (physical)
  unsigned long long source_memory_pa[NUM_INSTR_SOURCES];           // input memory (physical)

  unsigned char source_memory_size[NUM_INSTR_SOURCES];           // load widths, bytes
  unsigned char destination_memory_size[NUM_INSTR_DESTINATIONS]; // store widths, bytes

  unsigned char privilege;  // 0 = user, 1 = kernel
  unsigned char instr_type; // 0 = int, 1 = fp, 2 = simd
  unsigned char reserved[8];

  // --- Block 3: memory values (bytes 128-511) ---
  unsigned char source_memory_value[NUM_INSTR_SOURCES][MAX_MEM_VALUE_SIZE];
  unsigned char destination_memory_value[NUM_INSTR_DESTINATIONS][MAX_MEM_VALUE_SIZE];
};

struct cloudsuite_instr {
  // instruction pointer or PC (Program Counter)
  unsigned long long ip;

  // branch info
  unsigned char is_branch;
  unsigned char branch_taken;

  unsigned char destination_registers[NUM_INSTR_DESTINATIONS_SPARC]; // output registers
  unsigned char source_registers[NUM_INSTR_SOURCES];                 // input registers

  unsigned long long destination_memory[NUM_INSTR_DESTINATIONS_SPARC]; // output memory
  unsigned long long source_memory[NUM_INSTR_SOURCES];                 // input memory

  unsigned char asid[2];
};
// NOLINTEND(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)

#endif
