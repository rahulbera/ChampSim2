#include <array>
#include <chrono>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "instr.h"
#include "mocks.hpp"
#include "ooo_cpu.h"
#include "operable.h"

namespace
{
struct printing_operable : champsim::operable {
  bool printed = false;
  long operate() override { return 0; }
  void print_deadlock() override { printed = true; }
};

struct throwing_operable : champsim::operable {
  long operate() override { return 0; }
  void print_deadlock() override { throw std::out_of_range{"vector::_M_range_check: __n (which is 155) >= this->size() (which is 128)"}; }
};

// Captures what enclosed code prints to stdout, where the deadlock printers
// write with fmt::print, and restores stdout even if that code throws (the
// pattern of 706-ramulator2-differential.cc).
class stdout_capture
{
  std::FILE* file_ = std::tmpfile();
  int saved_ = -1;
  void restore()
  {
    if (saved_ >= 0) {
      std::fflush(stdout);
      dup2(saved_, fileno(stdout));
      close(saved_);
      saved_ = -1;
    }
  }

public:
  stdout_capture()
  {
    if (file_ == nullptr) {
      throw std::runtime_error("stdout_capture: cannot create a temporary file");
    }
    std::fflush(stdout);
    saved_ = dup(fileno(stdout));
    if (saved_ < 0 || dup2(fileno(file_), fileno(stdout)) < 0) {
      if (saved_ >= 0) {
        close(saved_);
      }
      std::fclose(file_);
      throw std::runtime_error("stdout_capture: cannot redirect stdout");
    }
  }
  stdout_capture(const stdout_capture&) = delete;
  stdout_capture& operator=(const stdout_capture&) = delete;
  ~stdout_capture()
  {
    restore();
    std::fclose(file_);
  }
  std::string text()
  {
    restore();
    std::rewind(file_);
    std::string result;
    char buffer[4096];
    for (std::size_t n; (n = std::fread(buffer, 1, sizeof buffer, file_)) > 0;) {
      result.append(buffer, n);
    }
    return result;
  }
};

template <typename F>
std::string stdout_of(F&& print)
{
  stdout_capture capture;
  print();
  return capture.text();
}
} // namespace

TEST_CASE("Deadlock diagnostics continue past an operable whose printer throws")
{
  // do_phase aborts right after these diagnostics. An exception escaping one
  // printer would reach std::terminate instead, before the later operables --
  // the memory backend prints last -- and before stdout is flushed.
  throwing_operable core;
  printing_operable cache;
  throwing_operable walker;
  printing_operable memory;
  const std::vector<std::reference_wrapper<champsim::operable>> operables{core, cache, walker, memory};
  REQUIRE_NOTHROW(champsim::print_deadlock_diagnostics(operables));
  REQUIRE(cache.printed);
  REQUIRE(memory.printed);
}

TEST_CASE("The core's store-queue diagnostics name the loads waiting on each store")
{
  do_nothing_MRC mock_L1I, mock_L1D;
  O3_CPU uut{champsim::core_builder{}.lq_size(4).sq_size(4).fetch_queues(&mock_L1I.queues).data_queues(&mock_L1D.queues)};
  const std::array<uint8_t, 2> asid{0, 0};
  uut.SQ.emplace_back(champsim::address{0x1000}, 5, champsim::address{0x400}, asid);
  for (auto [slot, load] : {std::pair{0, 7}, std::pair{1, 9}}) {
    uut.LQ.at(slot).emplace(champsim::address{0x1000}, load, champsim::address{0x404}, asid);
    uut.LQ.at(slot)->producer_id = 5;
    uut.SQ.back().lq_depend_on_me.emplace_back(uut.LQ.at(slot));
  }

  SECTION("While the store has not issued, both loads wait on it")
  {
    const auto printed = stdout_of([&] { uut.print_deadlock(); });
    REQUIRE_THAT(printed, Catch::Matchers::ContainsSubstring("instr_id: 5 address: 0x1000") && Catch::Matchers::ContainsSubstring("LQ waiting: [7, 9]"));
  }

  SECTION("Once the store has forwarded, its released or reused slots name no load")
  {
    // do_finish_store releases each waiting load but leaves lq_depend_on_me
    // referring to the slots, and a younger load can take one.
    uut.SQ.back().fetch_issued = true;
    uut.LQ.at(0).reset();
    uut.LQ.at(1).emplace(champsim::address{0x2000}, 11, champsim::address{0x408}, asid);
    const auto printed = stdout_of([&] { uut.print_deadlock(); });
    REQUIRE_THAT(printed, Catch::Matchers::ContainsSubstring("instr_id: 5 address: 0x1000") && Catch::Matchers::ContainsSubstring("LQ waiting: []"));
  }
}

TEST_CASE("The core's deadlock diagnostics count register dependencies only for renamed instructions")
{
  // Until do_scheduling renames an instruction its operands are architectural
  // IDs, which say nothing about the physical register file.
  do_nothing_MRC mock_L1I, mock_L1D;
  O3_CPU uut{champsim::core_builder{}
                 .schedule_width(champsim::bandwidth::maximum_type{128})
                 .register_file_size(8)
                 .fetch_queues(&mock_L1I.queues)
                 .data_queues(&mock_L1D.queues)};
  for (uint64_t id : {1, 2, 3}) {
    auto instr = champsim::test::instruction_with_ip(id);
    instr.instr_id = id;
    instr.ready_time = champsim::chrono::clock::time_point{};
    uut.ROB.push_back(instr);
  }
  uut.ROB.at(0).destination_registers = {5};                                                // renamed: writes a register not yet valid
  uut.ROB.at(1).source_registers = {5};                                                     // renamed: waits on that register
  uut.ROB.at(2).source_registers = {3};                                                     // architectural 3; physical 3 is not valid
  uut.ROB.at(2).ready_time = champsim::chrono::clock::time_point{} + std::chrono::hours{1}; // not ready, so not renamed
  uut.schedule_instruction();
  REQUIRE(uut.ROB.at(1).scheduled);
  REQUIRE_FALSE(uut.ROB.at(2).scheduled);

  const auto printed = stdout_of([&] { uut.print_deadlock(); });
  const auto line_of = [&](uint64_t id) {
    const auto at = printed.find("[cpu0_ROB] entry:   " + std::to_string(id - 1) + " instr_id: " + std::to_string(id) + " ");
    REQUIRE(at != std::string::npos);
    return printed.substr(at, printed.find('\n', at) - at);
  };
  REQUIRE_THAT(line_of(2), Catch::Matchers::ContainsSubstring("num_reg_dependent: 1"));
  REQUIRE_THAT(line_of(3), Catch::Matchers::ContainsSubstring("num_reg_dependent: -"));
}
