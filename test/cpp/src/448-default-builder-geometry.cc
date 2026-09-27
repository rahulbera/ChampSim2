#include <catch.hpp>

#include "cache.h"
#include "champsim.h"
#include "defaults.hpp"
#include "defs.h"
#include "util/bits.h"
#include "util/to_underlying.h"

// defaults.hpp's builders are namespace-scope objects that every translation unit including it
// initializes dynamically, taking their offset bits from LOG2_BLOCK_SIZE and LOG2_PAGE_SIZE.
// Those were once dynamically initialized themselves in test builds, so a builder whose
// initializer ran first read 0 -- as under -flto -- and every cache built from it indexed its
// sets with the lowest address bits. Only 447-perfect-tlb noticed. This file's builders were
// initialized before main(), so these checks see what static initialization produced.
TEST_CASE("The default cache builders carry nonzero offset bits from static initialization")
{
  champsim::channel upper{};
  auto offset_bits = [&upper](auto builder) {
    return champsim::to_underlying(CACHE{builder.upper_levels({&upper})}.OFFSET_BITS);
  };

  const auto page_bits = champsim::lg2(champsim::defs::page_size);
  const auto block_bits = champsim::lg2(champsim::defs::block_size);
  REQUIRE(LOG2_PAGE_SIZE == page_bits);
  REQUIRE(LOG2_BLOCK_SIZE == block_bits);

  CHECK(offset_bits(champsim::defaults::default_itlb) == page_bits);
  CHECK(offset_bits(champsim::defaults::default_dtlb) == page_bits);
  CHECK(offset_bits(champsim::defaults::default_stlb) == page_bits);
  CHECK(offset_bits(champsim::defaults::default_l1i) == block_bits);
  CHECK(offset_bits(champsim::defaults::default_l1d) == block_bits);
  CHECK(offset_bits(champsim::defaults::default_l2c) == block_bits);
  CHECK(offset_bits(champsim::defaults::default_llc) == block_bits);
}
