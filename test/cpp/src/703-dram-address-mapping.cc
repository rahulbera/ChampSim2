#include <array>
#include <catch.hpp>
#include <random>

#include "dram_controller.h"

namespace
{
unsigned log2_power_of_two(unsigned long value)
{
  unsigned result = 0;
  while (value > 1) {
    value >>= 1;
    ++result;
  }
  return result;
}
} // namespace

TEST_CASE("Legacy DRAM hashing matches coordinate-level XOR across geometries")
{
  const auto channels = GENERATE(1ul, 2ul, 4ul);
  const auto groups = GENERATE(1ul, 2ul, 4ul);
  const auto banks = GENERATE(1ul, 2ul, 4ul, 8ul);
  // The legacy zero-step swizzle for a single bank AND group does not terminate.
  // This performance test covers the terminating mapping contract.
  if (groups == 1 && banks == 1) {
    return;
  }
  const auto ranks = GENERATE(1ul, 2ul);
  const auto rows = GENERATE(256ul, 1024ul, 65536ul);
  CAPTURE(channels, groups, banks, ranks, rows);
  DRAM_ADDRESS_MAPPING mapping{champsim::data::bytes{8}, 8, channels, groups, banks, 1024, ranks, rows};
  const auto group_bits = log2_power_of_two(groups);
  const auto bank_bits = log2_power_of_two(banks);
  const auto segment_bits = group_bits + bank_bits;
  std::mt19937_64 random{42};

  for (unsigned sample = 0; sample < 64; ++sample) {
    const auto channel = random() % channels;
    const auto rank = random() % ranks;
    const auto group = random() % groups;
    const auto bank = random() % banks;
    const auto row = random() % rows;
    const auto column = random() % 128;
    uint64_t raw = random() % 64;
    unsigned position = 6;
    auto append = [&](uint64_t coordinate, unsigned width) {
      raw |= coordinate << position;
      position += width;
    };
    append(channel, log2_power_of_two(channels));
    append(group, group_bits);
    append(bank, bank_bits);
    append(column, 7);
    append(rank, log2_power_of_two(ranks));
    append(row, log2_power_of_two(rows));
    // Bits above the configured address space must not enter the hash.
    raw |= uint64_t{sample % 2} << 63;
    CAPTURE(raw);

    uint64_t folded = 0, parity = 0;
    for (auto remaining = row; remaining != 0; remaining >>= segment_bits) {
      folded ^= remaining & (groups * banks - 1);
    }
    for (auto remaining = row; remaining != 0; remaining >>= 1) {
      parity ^= remaining & 1;
    }
    const std::array<uint64_t, 6> expected{channel ^ (channels > 1 ? parity : 0),         rank, group ^ (folded & (groups - 1)),
                                           bank ^ ((folded >> group_bits) & (banks - 1)), row,  column};
    const champsim::address address{raw};
    const std::array<uint64_t, 6> actual{mapping.get_channel(address), mapping.get_rank(address), mapping.get_bankgroup(address),
                                         mapping.get_bank(address),    mapping.get_row(address),  mapping.get_column(address)};
    CHECK(actual == expected);
  }
}

TEST_CASE("Zero-width DRAM swizzling leaves an arbitrary input field unchanged")
{
  DRAM_ADDRESS_MAPPING mapping{champsim::data::bytes{8}, 8, 1, 1, 8, 1024, 1, 65536};
  for (uint64_t raw : {0ULL, 1ULL, 0xfedcba9876543210ULL}) {
    CHECK(mapping.swizzle_bits(champsim::address{raw}, 1, champsim::data::bits{0}, 0x55, 0) == 0x55);
    CHECK(mapping.swizzle_bits(champsim::address{raw}, 3, champsim::data::bits{2}, 0x55, 0) == 0x55);
  }
}
