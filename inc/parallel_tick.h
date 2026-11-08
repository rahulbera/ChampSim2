#ifndef PARALLEL_TICK_H
#define PARALLEL_TICK_H

#include <atomic>
#include <chrono>
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
 * alone on the calling thread. When the environment says what interacts with what, a no-lane
 * operable that interacts with nothing in the run before it runs ahead of that run, which in the
 * standard machine moves the LLC ahead of the cores and leaves one run per tick: the cores and
 * their caches.
 *
 * Every core's refill joins the tick's last run. Instruction IDs are global and numbered in core
 * order, which needs every core's refill count, so a lane reads with provisional IDs and they are
 * set after the run: by the lane itself at the start of its next run when nothing in between can
 * see a core, otherwise by the calling thread straight away.
 *
 * The order rarely changes. An operable on the fastest clock is exactly one quantum behind the
 * clock at the start of every tick, so only slower operables can sort later than canonical
 * order, and only when they are behind by less. The schedule is rebuilt only when that moves
 * one of them, which in the standard machine is never: DRAM, the one slower clock, is last.
 *
 * Lanes cost different amounts: a busy core does several times the work of a stalled one, and a
 * run lasts as long as its slowest worker. Each lane has an owner. With fewer threads than lanes,
 * every few thousand ticks the lanes are reassigned from their measured cost, heaviest first to
 * the least loaded worker. That evens out the average, but a core's cost comes in bursts that
 * last tens to hundreds of ticks, so within a run a worker also steals: it runs its own lanes,
 * the one that cost most last time first, and then the costliest lane no worker has started. A
 * lane is claimed atomically, so each runs exactly once per run. Which thread runs a lane does
 * not change what it computes.
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

  // Parallel runs in a tick of the standard order.
  [[nodiscard]] std::size_t runs_per_tick() const;

  // Gives every refilled instruction its final ID now. A phase's end does this itself; anything
  // that reads input queues between ticks (deadlock diagnostics) calls it first.
  void settle();

private:
  struct entry {
    operable* op;
    std::size_t index;           // in canonical order
    std::size_t lane;            // as the environment names it
    std::size_t lane_index;      // dense, 0 to the number of lanes; no_lane for none
    std::size_t idle_lane_index; // the lane that may check idle() for it, or no_lane
    bool is_cpu;
    bool fast; // on the smallest clock period
  };

  // One lane's operables within a run, in order, and what its runner does around them.
  struct lane_run {
    std::size_t lane_index;
    std::vector<operable*> ops;
    std::vector<std::size_t> cpus; // the cores in this lane, whose IDs it sets and whose input it refills
    // In the tick's last run: the later no-lane operables whose idle() this lane checks, each with its bit.
    std::vector<std::pair<const operable*, uint64_t>> idle_checks;
  };

  // One step of a tick: a no-lane operable on the calling thread, or a run of laned operables,
  // each lane's in order, with each worker's own lanes listed.
  struct step {
    operable* serial = nullptr;
    std::vector<lane_run> lanes;
    std::vector<std::vector<std::size_t>> by_worker; // indices into lanes
    std::vector<std::size_t> outside;                // the cores in no lane of this run
    bool refill = false;
    // A no-lane operable: the lane and bit that say it is idle, if one checks.
    std::size_t idle_lane_index = static_cast<std::size_t>(-1);
    uint64_t idle_bit = 0;
  };

  // A cache line per worker: each writes only its own slot.
  struct alignas(64) slot {
    std::atomic<uint32_t> done{0};
    long progress = 0;
    std::vector<std::size_t> order; // its own lanes in this run, costliest first
    std::exception_ptr error;
  };

  // A cache line per lane, written by whichever worker runs it.
  struct alignas(64) lane_slot {
    std::atomic<uint32_t> claimed{0};   // the last run that claimed it
    std::atomic<uint64_t> predicted{0}; // what it cost the last time it ran, which orders the next run
    uint64_t cost = 0;                  // since the last reassignment
    uint64_t idle_bits = 0;             // from the tick's last run
  };

  std::vector<entry> canonical;
  std::vector<const entry*> slow;
  chrono::clock::duration quantum{};
  bool checked_alignment = false;
  bool aligned = false; // every fast operable one quantum behind the clock, so only slow ones move

  std::vector<const entry*> canonical_order;
  std::vector<step> canonical_schedule;
  std::vector<step> moved_schedule;
  std::vector<const entry*> late;
  std::vector<const entry*> moved_late; // the late operables moved_schedule was built for
  std::vector<const entry*> order;

  // Which operables, by canonical index, may interact; empty when unknown.
  std::vector<std::vector<char>> interacts;

  std::vector<O3_CPU*> cpus;
  std::vector<std::size_t> cpu_lane; // each core's lane index
  bool parallel_refill = false;
  bool deferred_ids = false; // no no-lane operable interacts with a core
  bool ids_pending = false;  // the last refill's IDs are still provisional
  bool run_fixup = false;    // the current run sets them first

  // Which worker owns each lane, and each lane's state.
  std::vector<std::size_t> lane_worker;
  std::unique_ptr<lane_slot[]> lane_slots;
  uint64_t tick_count = 0;

  // The last refill, by core: how many instructions it read, and their first ID.
  std::vector<long> pushed;
  std::vector<uint64_t> first_id;

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
  void rebalance();
  const std::vector<step>& schedule_for(const chrono::clock& clock);
  void work(std::size_t worker, uint32_t run_number);
  bool claim(std::size_t k, uint32_t run_number);
  long run_lane(std::size_t k);
  void worker_loop(std::size_t worker);
  void publish(uint32_t run_number);
  long dispatch();
  void refill(std::size_t cpu_index);
  void set_ids(std::size_t cpu_index);
  void number_refill();
  void serial_refill(std::vector<tracereader>& traces, const std::vector<std::size_t>& trace_index);
};
} // namespace champsim

#endif
