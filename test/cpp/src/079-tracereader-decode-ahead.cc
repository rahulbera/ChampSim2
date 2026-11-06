#include <catch.hpp>
#include <stdexcept>
#include <vector>

#include "instr.h"
#include "tracereader.h"

namespace
{
// Instructions at IP 1, 2, 3, ..., ending after `length`, or throwing when the `throw_at`th is read.
struct counting_reader {
  uint64_t next = 1;
  uint64_t length;
  uint64_t throw_at = 0;
  ooo_model_instr operator()()
  {
    if (next == throw_at) {
      throw std::runtime_error{"corrupt trace"};
    }
    return champsim::test::instruction_with_ip(next++);
  }
  bool eof() const { return next > length; }
};

// Reads as the simulator's refill does: check eof(), then read.
std::vector<uint64_t> drain(champsim::tracereader& reader)
{
  std::vector<uint64_t> ips;
  while (!reader.eof()) {
    ips.push_back(reader().ip.to<uint64_t>());
  }
  return ips;
}
} // namespace

TEST_CASE("A trace decoded ahead reads the same instructions and ends in the same place")
{
  champsim::tracereader plain{counting_reader{1, 1000}};
  champsim::tracereader ahead{counting_reader{1, 1000}};
  ahead.decode_ahead(8); // smaller than the trace, so the ring wraps and the producer sleeps
  const auto expected = drain(plain);
  REQUIRE(std::size(expected) == 1000);
  CHECK(drain(ahead) == expected);
  CHECK(ahead.eof());
}

TEST_CASE("A trace decoded ahead fails where it would have failed")
{
  champsim::tracereader ahead{counting_reader{1, 1000, 5}};
  ahead.decode_ahead(8);
  for (uint64_t ip = 1; ip < 5; ++ip) {
    REQUIRE_FALSE(ahead.eof());
    CHECK(ahead().ip.to<uint64_t>() == ip);
  }
  REQUIRE_FALSE(ahead.eof());
  CHECK_THROWS_WITH(ahead(), "corrupt trace");
}

TEST_CASE("A trace decoded ahead can be destroyed while its decoder waits")
{
  champsim::tracereader ahead{counting_reader{1, 1000000}};
  ahead.decode_ahead(8);
  CHECK(ahead().ip.to<uint64_t>() == 1);
  // The decoder fills the ring and sleeps; destroying the reader must wake and join it.
}
