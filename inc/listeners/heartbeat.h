#ifndef HEARTBEAT_H
#define HEARTBEAT_H

#include <chrono>
#include <deque>
#include <iostream>
#include <string>
#include <vector>
#include <fmt/chrono.h>

#include "events.h"
#include "instruction.h"

class Heartbeat
{
public:
  std::ostream* std_out;

  explicit Heartbeat(std::ostream* so) { std_out = so; }

  static constexpr auto cli_key = "Heartbeat";

  // Instructions retired between heartbeat lines -- NOT cycles, despite what
  // the old name said. Settable from the command line with
  // --heartbeat-frequency; 10M is the historical default.
  uint64_t instructions_between_printouts = 10000000;

  // One cache line or more per core. Every core updates its own state on every retire, and with
  // simulation threads, cores packed into shared lines would bounce those lines between them.
  struct alignas(64) core_state {
    uint64_t retired = 0;
    uint64_t retired_last_printout = 0;
    uint64_t cycles_last_printout = 0;
    uint64_t retired_start_phase = 0;
    uint64_t cycles_start_phase = 0;
    bool switched_phase = false; // a begin_phase event since this core's last RETIRE
    std::string deferred;        // lines held while simulation threads run
  };
  std::vector<core_state> cores;

  // While simulation threads run, each core's lines are held in its state and written by
  // flush_deferred(), in core order, instead of from inside O3_CPU::operate().
  bool defer = false;

  void flush_deferred()
  {
    bool wrote = false;
    for (auto& core : cores) {
      if (!core.deferred.empty()) {
        std_out->write(core.deferred.data(), static_cast<std::streamsize>(core.deferred.size()));
        core.deferred.clear();
        wrote = true;
      }
    }
    if (wrote) {
      std_out->flush();
    }
  }

  template <Event e, typename... Args>
  void handle_event(Args&&... args);

  // Grows the per-core state to cover cpu. Simulation threads require it sized beforehand.
  void add_cpu(uint32_t cpu)
  {
    if (cpu >= cores.size()) {
      cores.resize(cpu + 1);
    }
  }
};

std::chrono::seconds elapsed_time();

namespace heartbeat
{

template <Event e, typename... Args>
inline void handle_event(Heartbeat* hb, Args&... args)
{
  // std::cout << "WARNING: generic handle event\n";
}

template <>
inline void handle_event<Event::BEGIN_PHASE>(Heartbeat* hb, [[maybe_unused]] bool& is_warmup)
{
  for (auto& core : hb->cores) {
    core.switched_phase = true;
  }
}

template <>
inline void handle_event<Event::RETIRE>(Heartbeat* hb, uint32_t& cpu, std::deque<ooo_model_instr>::const_iterator& begin,
                                        std::deque<ooo_model_instr>::const_iterator& end, uint64_t& current_cycles)
{
  hb->add_cpu(cpu);
  auto& core = hb->cores[cpu];
  core.retired += static_cast<uint64_t>(std::distance(begin, end));

  if (core.switched_phase) {
    core.switched_phase = false;
    core.retired_start_phase = core.retired;
    core.cycles_start_phase = current_cycles;
  }

  if (core.retired >= core.retired_last_printout + hb->instructions_between_printouts) {

    auto heartbeat_instr = static_cast<double>(core.retired - core.retired_last_printout);
    auto heartbeat_cycle = static_cast<double>(current_cycles - core.cycles_last_printout);

    auto phase_instr = static_cast<double>(core.retired - core.retired_start_phase);
    auto phase_cycle = static_cast<double>(current_cycles - core.cycles_start_phase);

    auto line =
        fmt::format("Heartbeat CPU {} instructions: {} cycles: {} heartbeat IPC: {:.4} cumulative IPC: {:.4} (Simulation time: {:%H hr %M min %S sec})\n", cpu,
                    core.retired, current_cycles, heartbeat_instr / heartbeat_cycle, phase_instr / phase_cycle, elapsed_time());
    if (hb->defer) {
      core.deferred += line;
    } else {
      hb->std_out->write(line.data(), static_cast<std::streamsize>(line.size()));

      // Flush explicitly. A heartbeat exists to show the run is alive, and stdout
      // is block-buffered when redirected to a file -- which is how long runs are
      // actually launched -- so without this the lines appear only in 4 KiB
      // batches, or not at all until the run ends.
      hb->std_out->flush();
    }

    core.retired_last_printout = core.retired;
    core.cycles_last_printout = current_cycles;
  }
}

} // namespace heartbeat

template <Event e, typename... Args>
void Heartbeat::handle_event(Args&&... args)
{
  heartbeat::handle_event<e>(this, std::forward<Args>(args)...);
}

#endif