#ifndef PARALLEL_TICK_H
#define PARALLEL_TICK_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "chrono.h"

class O3_CPU;

namespace champsim
{
class operable;
class tracereader;
struct environment;

/**
 * One simulation tick on several threads, with the same result as the serial loop.
 *
 * The serial loop runs every operable in a stable order -- earliest time first, then canonical
 * order -- and then refills each core's input queue from its trace. The environment puts each
 * operable in a lane, the core whose private state it touches, or in no lane. Neighbouring
 * operables of different lanes never touch the same state, so a run of laned operables can be
 * split by lane across threads, each lane keeping its own order, and a no-lane operable runs
 * alone on the calling thread. Every core's refill joins the first run of laned operables that
 * starts after every core has operated, with instruction IDs reserved in core order, so
 * instructions are numbered as the serial loop numbers them.
 *
 * The order rarely changes. An operable on the fastest clock is exactly one quantum behind the
 * clock at the start of every tick, so only slower operables can sort later than canonical
 * order, and only when they are behind by less. The schedule is rebuilt only when that moves
 * one of them, which in the standard machine is never: DRAM, the one slower clock, is last.
 *
 * The workers spin between runs, which in a tick are a few microseconds apart, and sleep when a
 * wait runs long (between phases, or on a host with fewer free processors than threads).
 */
class parallel_tick
{
public:
  // threads counts the calling thread, and is capped at the number of lanes.
  parallel_tick(environment& env, std::size_t threads);
  ~parallel_tick();
  parallel_tick(const parallel_tick&) = delete;
  parallel_tick& operator=(const parallel_tick&) = delete;

  // What do_cycle does for one tick. Returns the progress the operables made.
  long operator()(std::vector<tracereader>& traces, const std::vector<std::size_t>& trace_index, const chrono::clock& clock);

  [[nodiscard]] std::size_t threads() const { return nthreads; }

private:
  struct entry {
    operable* op;
    std::size_t index; // in canonical order
    std::size_t lane;
    std::size_t worker; // the thread that runs this lane
    bool is_cpu;
    bool fast; // on the smallest clock period
  };

  // One step of a tick: a no-lane operable on the calling thread, or a run of laned operables,
  // each worker's share in order.
  struct step {
    operable* serial = nullptr;
    std::vector<std::vector<operable*>> by_worker;
    bool refill = false;
  };

  // A cache line per worker: each writes only its own slot.
  struct alignas(64) slot {
    std::atomic<uint32_t> done{0};
    long progress = 0;
    std::exception_ptr error;
  };

  std::vector<entry> canonical;
  std::vector<const entry*> slow;
  chrono::clock::duration quantum{};
  bool checked_alignment = false;
  bool aligned = false; // every fast operable one quantum behind the clock, so only slow ones move

  std::vector<step> canonical_schedule;
  std::vector<step> moved_schedule;
  std::vector<const entry*> late;
  std::vector<const entry*> moved_late; // the late operables moved_schedule was built for
  std::vector<const entry*> order;

  std::vector<O3_CPU*> cpus;
  std::vector<std::size_t> cpu_worker;
  bool parallel_refill = false;

  // The refill of the current tick, by core.
  std::vector<long> requested;
  std::vector<long> pushed;
  std::vector<uint64_t> first_id;
  uint64_t tick_first_id = 0;

  // The current run, read by the workers after they see a new epoch.
  const step* run = nullptr;
  const chrono::clock* run_clock = nullptr;
  std::vector<tracereader>* run_traces = nullptr;
  const std::vector<std::size_t>* run_trace_index = nullptr;

  std::size_t nthreads;
  std::unique_ptr<slot[]> slots;
  // The run number the workers wait on. 32 bits, so a worker can sleep on it as a futex.
  alignas(64) std::atomic<uint32_t> epoch{0};
  std::atomic<uint32_t> sleepers{0};
  std::atomic<bool> stopping{false};
  std::vector<std::thread> workers;

  void build(std::vector<step>& schedule, const std::vector<const entry*>& sequence) const;
  const std::vector<step>& schedule_for(const chrono::clock& clock);
  void work(std::size_t worker);
  void worker_loop(std::size_t worker);
  void publish(uint32_t run_number);
  long dispatch();
  void refill(std::size_t cpu_index);
  void serial_refill(std::vector<tracereader>& traces, const std::vector<std::size_t>& trace_index);
};
} // namespace champsim

#endif
