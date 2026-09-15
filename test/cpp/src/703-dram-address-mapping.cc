#include <array>
#include <catch.hpp>
#include <limits>
#include <random>
#include <stdexcept>
#include <vector>

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

namespace
{
struct mapping_geometry {
  unsigned long channel_bytes, prefetch, channels, groups, banks, columns, ranks;
};

void check_boundary_coordinates(mapping_geometry geometry, unsigned row_bits)
{
  const auto [channel_bytes, prefetch, channels, groups, banks, columns, ranks] = geometry;
  const auto offset_bits = log2_power_of_two(channel_bytes * prefetch);
  const auto channel_bits = log2_power_of_two(channels);
  const auto group_bits = log2_power_of_two(groups);
  const auto bank_bits = log2_power_of_two(banks);
  const auto column_bits = log2_power_of_two(columns / prefetch);
  const auto rank_bits = log2_power_of_two(ranks);
  const auto row_start = offset_bits + channel_bits + group_bits + bank_bits + column_bits + rank_bits;
  const auto rows = 1ul << row_bits;
  const DRAM_ADDRESS_MAPPING mapping{champsim::data::bytes{static_cast<long long>(channel_bytes)}, prefetch, channels, groups, banks, columns, ranks, rows};
  const auto copied = mapping;
  CAPTURE(channel_bytes, prefetch, channels, groups, banks, columns, ranks, row_bits, row_start);

  // Decode coordinates independently of address_slicer and the descriptor schedule.
  // Single row bits catch missing partial tails and incorrect group/bank offsets;
  // full addresses and random coordinates catch accidental channel-wide hashing.
  std::vector<uint64_t> addresses{0, ~uint64_t{0}, (uint64_t{rows - 1} << row_start)};
  for (unsigned bit = 0; bit < row_start + row_bits; ++bit) {
    addresses.push_back(uint64_t{1} << bit);
  }
  std::mt19937_64 random{703};
  for (unsigned sample = 0; sample < 16; ++sample) {
    addresses.push_back(random());
  }
  for (auto raw : addresses) {
    CAPTURE(raw);
    unsigned position = offset_bits;
    auto take = [&](unsigned width) {
      const auto value = (raw >> position) & ((uint64_t{1} << width) - 1);
      position += width;
      return value;
    };
    auto channel = take(channel_bits);
    auto group = take(group_bits);
    auto bank = take(bank_bits);
    const auto column = take(column_bits);
    const auto rank = take(rank_bits);
    const auto row = take(row_bits);
    for (unsigned bit = 0; bit < row_bits; ++bit) {
      if (((row >> bit) & 1) != 0) {
        if (channels > 1) {
          channel ^= 1;
        }
        const auto coordinate_bit = bit % (group_bits + bank_bits);
        if (coordinate_bit < group_bits) {
          group ^= uint64_t{1} << coordinate_bit;
        } else {
          bank ^= uint64_t{1} << (coordinate_bit - group_bits);
        }
      }
    }
    const std::array<uint64_t, 6> expected{channel, rank, group, bank, row, column};
    for (const auto* mapper : {&mapping, &copied}) {
      const champsim::address address{raw};
      const std::array<uint64_t, 6> actual{mapper->get_channel(address), mapper->get_rank(address), mapper->get_bankgroup(address),
                                           mapper->get_bank(address),    mapper->get_row(address),  mapper->get_column(address)};
      CHECK(actual == expected);
    }
  }
}
} // namespace

TEST_CASE("DRAM hashing preserves short rows, partial segments, alternate layouts and copies")
{
  const auto layout = GENERATE((std::array<unsigned long, 3>{8, 8, 1024}), (std::array<unsigned long, 3>{4, 16, 512}),
                               (std::array<unsigned long, 3>{16, 8, 2048}), (std::array<unsigned long, 3>{16, 4, 256}));
  const auto channels = GENERATE(1ul, 2ul, 8ul);
  const auto group_bank = GENERATE((std::array<unsigned long, 2>{1, 8}), (std::array<unsigned long, 2>{8, 1}), (std::array<unsigned long, 2>{2, 8}),
                                   (std::array<unsigned long, 2>{8, 2}), (std::array<unsigned long, 2>{4, 4}));
  const auto ranks = GENERATE(1ul, 2ul);
  const auto stride = log2_power_of_two(group_bank[0] * group_bank[1]);
  for (auto row_bits : {0u, 1u, stride - 1, stride, stride + 1}) {
    check_boundary_coordinates({layout[0], layout[1], channels, group_bank[0], group_bank[1], layout[2], ranks}, row_bits);
  }
}

TEST_CASE("DRAM hashing preserves the last fast-domain bit and defined boundary-64 geometry")
{
  if constexpr (std::numeric_limits<unsigned long>::digits == 64) {
    const auto channels = GENERATE(1ul, 8ul);
    const auto groups = GENERATE(1ul, 2ul);
    const auto row_upper = GENERATE(63u, 64u);
    // All extraction lower bounds remain below 64, including the nonempty row.
    const auto row_start = 6 + log2_power_of_two(channels) + log2_power_of_two(groups) + 3 + 7 + 1;
    check_boundary_coordinates({8, 8, channels, groups, 8, 1024, 2}, row_upper - row_start);
  }
}

TEST_CASE("DRAM getters and public swizzle preserve checked row upper-bound errors")
{
  if constexpr (std::numeric_limits<unsigned long>::digits == 64) {
    const DRAM_ADDRESS_MAPPING mapping{champsim::data::bytes{8}, 8, 2, 2, 2, 1024, 1, 1ul << 50};
    const auto copied = mapping;
    for (const auto* mapper : {&mapping, &copied}) {
      for (uint64_t raw : {0ULL, ~0ULL}) {
        const champsim::address address{raw};
        auto check_error = [](auto operation) {
          CHECK_THROWS_AS(operation(), std::invalid_argument);
          CHECK_THROWS_WITH(operation(), "Upper bound is not representable in the underlying type");
        };
        check_error([&] { return mapper->get_channel(address); });
        check_error([&] { return mapper->get_bankgroup(address); });
        check_error([&] { return mapper->get_bank(address); });
        check_error([&] { return mapper->get_rank(address); });
        check_error([&] { return mapper->get_column(address); });
        check_error([&] { return mapper->get_row(address); });
        check_error([&] { return mapper->swizzle_bits(address, 1, champsim::data::bits{0}, 0x55, 1); });
        check_error([&] { return mapper->swizzle_bits(address, 1, champsim::data::bits{0}, 0x55, 0); });
      }
    }
  }
}
