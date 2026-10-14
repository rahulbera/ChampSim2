#include <catch.hpp>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>

#include "champsim.h"
#include "tracereader.h"

namespace
{
// Shorter than one input_instr record by construction, however large that
// record is: 63 bytes cannot contain a whole 64-byte record.
const std::string partial_record(sizeof(input_instr) - 1, '\x00');
} // namespace

TEST_CASE("A bulk_tracereader over an empty trace returns a well-formed empty instruction and then reports eof")
{
  champsim::bulk_tracereader<input_instr, std::istringstream> uut{0, std::istringstream{}};

  const auto marker = uut();
  REQUIRE(marker.ip == champsim::address{});
  REQUIRE(marker.asid == champsim::address_space_id{std::numeric_limits<uint8_t>::max(), std::numeric_limits<uint8_t>::max()});
  REQUIRE_FALSE(marker.is_branch);
  REQUIRE(uut.eof());
}

TEST_CASE("A bulk_tracereader over a trace shorter than one record returns a well-formed empty instruction")
{
  champsim::bulk_tracereader<input_instr, std::istringstream> uut{0, std::istringstream{partial_record}};

  const auto marker = uut();
  REQUIRE(marker.ip == champsim::address{});
  REQUIRE(uut.eof());
}
