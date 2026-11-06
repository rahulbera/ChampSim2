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

#include "parallel_tick.h"

#include <algorithm>
#include <climits>
#include <numeric>
#include <stdexcept>
#include <fmt/core.h>
#if defined(__linux__)
#include <linux/futex.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include "environment.h"
#include "event_listeners.h"
#include "ooo_cpu.h"
#include "operable.h"
#include "tracereader.h"

namespace
{
void cpu_relax()
{
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  asm volatile("yield");
#endif
}

// A wait spins this many times before it gives up the processor: long enough to cover the
// serial operables between two runs (a few microseconds), short enough that a wait between
// phases, or behind a descheduled thread, does not hold a processor another thread needs.
constexpr unsigned spins_before_sleep = 1U << 11;

// Sleeps while *word holds expected (or a spurious wake-up ends it). Without futexes, yields.
void sleep_while(std::atomic<uint32_t>& word, uint32_t expected)
{
#if defined(__linux__)
  syscall(SYS_futex, reinterpret_cast<uint32_t*>(&word), FUTEX_WAIT_PRIVATE, expected, nullptr, nullptr, 0);
#else
  (void)word;
  (void)expected;
  std::this_thread::yield();
#endif
}

void wake_all(std::atomic<uint32_t>& word)
{
#if defined(__linux__)
  syscall(SYS_futex, reinterpret_cast<uint32_t*>(&word), FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0);
#else
  (void)word;
#endif
}

// Lane costs are sampled on one tick in this many, and lanes are reassigned after this many
// samples (about 4,000 ticks), if that lowers the most loaded worker's cost by 5% or more.
constexpr uint64_t ticks_per_sample = 64;
constexpr uint64_t samples_per_rebalance = 64;

// The processors this process may run on, or 0 if unknown.
std::size_t available_processors()
{
#if defined(__linux__)
  cpu_set_t set;
  CPU_ZERO(&set);
  if (sched_getaffinity(0, sizeof(set), &set) == 0) {
    return static_cast<std::size_t>(CPU_COUNT(&set));
  }
#endif
  return std::thread::hardware_concurrency();
}

template <typename Pred>
void spin_until(Pred done)
{
  for (unsigned spins = 0; !done(); ++spins) {
    if (spins < spins_before_sleep) {
      cpu_relax();
    } else {
      std::this_thread::yield();
    }
  }
}
} // namespace

champsim::parallel_tick::parallel_tick(environment& env, std::size_t threads)
{
  const auto view = env.operable_view();
  const auto lanes = env.operable_lanes();
  if (std::size(lanes) != std::size(view)) {
    throw std::logic_error{"environment::operable_lanes() must give one lane per operable"};
  }

  std::vector<std::size_t> distinct;
  for (auto lane : lanes) {
    if (lane != environment::no_lane && std::find(std::begin(distinct), std::end(distinct), lane) == std::end(distinct)) {
      distinct.push_back(lane);
    }
  }
  std::sort(std::begin(distinct), std::end(distinct));
  nthreads = std::max<std::size_t>(1, std::min(threads, std::size(distinct)));

  // Spinning threads that outnumber the processors wait for each other to be scheduled at every
  // run: a run that takes microseconds takes milliseconds. Use no more threads than processors.
  if (const auto processors = available_processors(); processors > 0 && nthreads > processors) {
    static bool warned = false; // once per run, not once per phase
    if (!warned) {
      fmt::print(stderr, "[THREADS] WARNING: {} simulation threads requested but only {} processors are available; using {}\n", nthreads, processors,
                 processors);
      warned = true;
    }
    nthreads = processors;
  }

  // Lanes start round-robin, in lane order; rebalance() moves them.
  auto index_of = [&](std::size_t lane) {
    return lane == environment::no_lane ? environment::no_lane
                                        : static_cast<std::size_t>(std::distance(std::begin(distinct), std::find(std::begin(distinct), std::end(distinct), lane)));
  };
  for (std::size_t li = 0; li < std::size(distinct); ++li) {
    lane_worker.push_back(li % nthreads);
  }
  lane_cost.resize(std::size(distinct));

  for (O3_CPU& cpu : env.cpu_view()) {
    cpus.push_back(&cpu);
  }
  quantum = chrono::clock::duration::max();
  for (operable& op : view) {
    quantum = std::min(quantum, op.clock_period);
  }
  for (std::size_t i = 0; i < std::size(view); ++i) {
    operable* op = &view[i].get();
    const bool is_cpu = std::find(std::begin(cpus), std::end(cpus), op) != std::end(cpus);
    canonical.push_back({op, i, lanes[i], index_of(lanes[i]), is_cpu, op->clock_period == quantum});
  }
  for (const auto& e : canonical) {
    if (!e.fast) {
      slow.push_back(&e);
    }
  }

  parallel_refill = true;
  for (auto* cpu : cpus) {
    auto it = std::find_if(std::begin(canonical), std::end(canonical), [cpu](const entry& e) { return e.op == cpu; });
    if (it == std::end(canonical) || it->lane == environment::no_lane) {
      parallel_refill = false;
      cpu_lane.push_back(environment::no_lane);
    } else {
      cpu_lane.push_back(it->lane_index);
    }
  }
  pushed.resize(std::size(cpus));
  first_id.resize(std::size(cpus));

  if (const auto known = env.operable_interactions(); known.has_value()) {
    if (std::size(*known) != std::size(view)) {
      throw std::logic_error{"environment::operable_interactions() must list one entry per operable"};
    }
    interacts.assign(std::size(view), std::vector<char>(std::size(view), 0));
    for (std::size_t i = 0; i < std::size(view); ++i) {
      for (auto j : (*known)[i]) {
        interacts.at(i).at(j) = interacts.at(j).at(i) = 1;
      }
    }
    deferred_ids = parallel_refill;
    for (const auto& e : canonical) {
      for (const auto& f : canonical) {
        deferred_ids = deferred_ids && !(e.lane == environment::no_lane && f.is_cpu && interacts[e.index][f.index]);
      }
    }
  }

  for (const auto& e : canonical) {
    canonical_order.push_back(&e);
  }
  build(canonical_schedule, canonical_order);

  // The heartbeat listener is shared by every core: size it now, and hold its lines while the
  // workers run.
  auto& heartbeat = std::get<0>(listeners);
  for (auto* cpu : cpus) {
    heartbeat.add_cpu(cpu->cpu);
  }
  heartbeat.defer = nthreads > 1;

  slots = std::make_unique<slot[]>(nthreads);
  for (std::size_t w = 1; w < nthreads; ++w) {
    workers.emplace_back([this, w] { worker_loop(w); });
  }
}

champsim::parallel_tick::~parallel_tick()
{
  settle();
  stopping.store(true, std::memory_order_release);
  publish(epoch.load(std::memory_order_relaxed) + 1);
  for (auto& t : workers) {
    t.join();
  }
  auto& heartbeat = std::get<0>(listeners);
  heartbeat.flush_deferred();
  heartbeat.defer = false;
}

void champsim::parallel_tick::build(std::vector<step>& schedule, const std::vector<const entry*>& given) const
{
  // A no-lane operable that interacts with nothing in the whole run of laned operables before it
  // commutes with that run, so it can run first, which joins that run to the one after it.
  auto sequence = given;
  for (bool moved = !std::empty(interacts); moved;) {
    moved = false;
    for (std::size_t i = 0; i < std::size(sequence); ++i) {
      if (sequence[i]->lane != environment::no_lane) {
        continue;
      }
      auto j = i;
      while (j > 0 && sequence[j - 1]->lane != environment::no_lane) {
        --j;
      }
      const auto free = std::none_of(std::next(std::begin(sequence), static_cast<std::ptrdiff_t>(j)), std::next(std::begin(sequence), static_cast<std::ptrdiff_t>(i)),
                                     [&](const entry* e) { return interacts[e->index][sequence[i]->index] != 0; });
      if (j < i && free) {
        std::rotate(std::next(std::begin(sequence), static_cast<std::ptrdiff_t>(j)), std::next(std::begin(sequence), static_cast<std::ptrdiff_t>(i)),
                    std::next(std::begin(sequence), static_cast<std::ptrdiff_t>(i + 1)));
        moved = true;
      }
    }
  }

  schedule.clear();
  for (std::size_t i = 0; i < std::size(sequence);) {
    if (sequence[i]->lane == environment::no_lane || nthreads == 1) {
      schedule.push_back({sequence[i]->op, {}, {}, false});
      ++i;
      continue;
    }
    step run_step{nullptr, {}, std::vector<std::vector<std::size_t>>(nthreads), false};
    std::vector<std::size_t> position(std::size(lane_worker), environment::no_lane);
    for (; i < std::size(sequence) && sequence[i]->lane != environment::no_lane; ++i) {
      auto& at = position[sequence[i]->lane_index];
      if (at == environment::no_lane) {
        at = std::size(run_step.lanes);
        run_step.lanes.push_back({sequence[i]->lane_index, {}});
      }
      run_step.lanes[at].ops.push_back(sequence[i]->op);
    }
    for (std::size_t k = 0; k < std::size(run_step.lanes); ++k) {
      run_step.by_worker[lane_worker[run_step.lanes[k].lane_index]].push_back(k);
    }
    schedule.push_back(std::move(run_step));
  }

  // The serial loop refills every core after the whole tick. The tick's last run is after every
  // core has operated, and the refill can join it when nothing after it can see a core.
  const auto last = std::find_if(std::rbegin(schedule), std::rend(schedule), [](const step& x) { return x.serial == nullptr; });
  const auto sees_a_core = [this](const operable* op) {
    if (std::empty(interacts)) {
      return true;
    }
    const auto& e = *std::find_if(std::begin(canonical), std::end(canonical), [op](const entry& x) { return x.op == op; });
    return std::any_of(std::begin(canonical), std::end(canonical), [&](const entry& f) { return f.is_cpu && interacts[e.index][f.index] != 0; });
  };
  if (parallel_refill && last != std::rend(schedule) && std::none_of(std::rbegin(schedule), last, [&](const step& x) { return sees_a_core(x.serial); })) {
    last->refill = true;
  }
}

std::size_t champsim::parallel_tick::runs_per_tick() const
{
  return static_cast<std::size_t>(std::count_if(std::begin(canonical_schedule), std::end(canonical_schedule), [](const step& x) { return x.serial == nullptr; }));
}

const std::vector<champsim::parallel_tick::step>& champsim::parallel_tick::schedule_for(const chrono::clock& clock)
{
  const auto behind = clock.now() - quantum;
  if (!checked_alignment) {
    checked_alignment = true;
    aligned = std::all_of(std::begin(canonical), std::end(canonical), [behind](const entry& e) { return !e.fast || e.op->current_time == behind; });
  }

  auto stable_by_time = [](std::vector<const entry*>& sequence) {
    for (auto it = std::begin(sequence); it != std::end(sequence); ++it) {
      const auto value = *it;
      auto hole = it;
      for (; hole != std::begin(sequence) && value->op->current_time < (*std::prev(hole))->op->current_time; --hole) {
        *hole = *std::prev(hole);
      }
      *hole = value;
    }
  };

  // Every operable has caught up to the previous tick, so none is more than a quantum behind.
  // The fast ones are exactly that far behind; a slow one is too (a tie, which keeps canonical
  // order) or is later.
  bool early = false;
  late.clear();
  for (const auto* e : slow) {
    early = early || e->op->current_time < behind;
    if (e->op->current_time > behind) {
      late.push_back(e);
    }
  }

  if (!aligned || early) {
    // The serial loop's own rule, applied to every operable.
    order.clear();
    for (const auto& e : canonical) {
      order.push_back(&e);
    }
    stable_by_time(order);
    build(moved_schedule, order);
    moved_late.clear();
    return moved_schedule;
  }

  stable_by_time(late);
  bool unmoved = true;
  for (std::size_t k = 0; k < std::size(late); ++k) {
    unmoved = unmoved && late[k]->index == std::size(canonical) - std::size(late) + k;
  }
  if (unmoved) {
    return canonical_schedule; // the late operables are already the last ones, in time order
  }
  if (late != moved_late || std::empty(moved_schedule)) {
    order.clear();
    for (const auto& e : canonical) {
      if (std::find(std::begin(late), std::end(late), &e) == std::end(late)) {
        order.push_back(&e);
      }
    }
    order.insert(std::end(order), std::begin(late), std::end(late));
    build(moved_schedule, order);
    moved_late = late;
  }
  return moved_schedule;
}

void champsim::parallel_tick::publish(uint32_t run_number)
{
  // Sequentially consistent, with the sleeper count in worker_loop: either this sees the
  // sleeper, or the sleeper's futex sees the new run number and does not sleep.
  epoch.store(run_number, std::memory_order_seq_cst);
  if (sleepers.load(std::memory_order_seq_cst) > 0) {
    wake_all(epoch);
  }
}

void champsim::parallel_tick::worker_loop(std::size_t worker)
{
  uint32_t seen = 0;
  while (true) {
    uint32_t now = seen;
    for (unsigned spins = 0; (now = epoch.load(std::memory_order_acquire)) == seen;) {
      if (++spins < spins_before_sleep) {
        cpu_relax();
      } else {
        sleepers.fetch_add(1, std::memory_order_seq_cst);
        sleep_while(epoch, seen);
        sleepers.fetch_sub(1, std::memory_order_seq_cst);
        spins = 0;
      }
    }
    if (stopping.load(std::memory_order_acquire)) {
      return;
    }
    seen = now;
    try {
      work(worker);
    } catch (...) {
      slots[worker].error = std::current_exception();
    }
    slots[worker].done.store(now, std::memory_order_release);
  }
}

void champsim::parallel_tick::work(std::size_t worker)
{
  using sampling_clock = std::chrono::steady_clock;
  if (run_fixup) {
    for (std::size_t c = 0; c < std::size(cpus); ++c) {
      if (lane_worker[cpu_lane[c]] == worker) {
        set_ids(c);
      }
    }
  }
  long progress{0};
  for (auto k : run->by_worker[worker]) {
    const auto& lane = run->lanes[k];
    const auto start = timed_tick ? sampling_clock::now() : sampling_clock::time_point{};
    for (operable* op : lane.ops) {
      progress += op->operate_on(*run_clock);
    }
    if (timed_tick) {
      lane_cost[lane.lane_index] += sampling_clock::now() - start;
    }
  }
  if (run->refill) {
    for (std::size_t c = 0; c < std::size(cpus); ++c) {
      if (lane_worker[cpu_lane[c]] == worker) {
        const auto start = timed_tick ? sampling_clock::now() : sampling_clock::time_point{};
        refill(c);
        if (timed_tick) {
          lane_cost[cpu_lane[c]] += sampling_clock::now() - start;
        }
      }
    }
  }
  slots[worker].progress = progress;
}

void champsim::parallel_tick::rebalance()
{
  // Longest processing time first: each lane, heaviest first, to the least loaded worker.
  std::vector<std::size_t> heaviest(std::size(lane_cost));
  std::iota(std::begin(heaviest), std::end(heaviest), std::size_t{0});
  std::stable_sort(std::begin(heaviest), std::end(heaviest), [this](auto a, auto b) { return lane_cost[a] > lane_cost[b]; });
  std::vector<std::chrono::steady_clock::duration> load(nthreads), current(nthreads);
  auto assigned = lane_worker;
  for (auto li : heaviest) {
    const auto w = static_cast<std::size_t>(std::distance(std::begin(load), std::min_element(std::begin(load), std::end(load))));
    assigned[li] = w;
    load[w] += lane_cost[li];
    current[lane_worker[li]] += lane_cost[li];
  }
  const auto worst_now = *std::max_element(std::begin(current), std::end(current));
  const auto worst_then = *std::max_element(std::begin(load), std::end(load));
  if (worst_then * 20 < worst_now * 19) {
    lane_worker = assigned;
    build(canonical_schedule, canonical_order);
    moved_schedule.clear();
    moved_late.clear();
  }
  std::fill(std::begin(lane_cost), std::end(lane_cost), std::chrono::steady_clock::duration::zero());
}

long champsim::parallel_tick::dispatch()
{
  const auto this_run = static_cast<uint32_t>(epoch.load(std::memory_order_relaxed) + 1);
  publish(this_run);

  std::exception_ptr error;
  try {
    work(0);
  } catch (...) {
    error = std::current_exception();
  }
  // One pass over every worker's slot per check, so the cache misses overlap.
  spin_until([&] {
    bool all = true;
    for (std::size_t w = 1; w < nthreads; ++w) {
      all = (slots[w].done.load(std::memory_order_acquire) == this_run) && all;
    }
    return all;
  });

  long progress{0};
  for (std::size_t w = 0; w < nthreads; ++w) {
    progress += slots[w].progress;
    if (!error && slots[w].error) {
      error = slots[w].error;
    }
    slots[w].error = nullptr;
  }
  if (error) {
    std::rethrow_exception(error);
  }
  return progress;
}

void champsim::parallel_tick::refill(std::size_t cpu_index)
{
  // As the serial loop, with provisional IDs: number_refill() decides the real ones.
  auto& cpu = *cpus[cpu_index];
  auto& trace = run_traces->at(run_trace_index->at(cpu.cpu));
  long count{0};
  for (auto wanted = cpu.IN_QUEUE_SIZE - static_cast<long>(std::size(cpu.input_queue)); count < wanted && !trace.eof(); ++count) {
    cpu.input_queue.push_back(trace(0));
  }
  pushed[cpu_index] = count;
}

void champsim::parallel_tick::set_ids(std::size_t cpu_index)
{
  auto& queue = cpus[cpu_index]->input_queue;
  auto id = first_id[cpu_index];
  for (auto it = std::prev(std::end(queue), pushed[cpu_index]); it != std::end(queue); ++it) {
    it->instr_id = id++;
  }
}

void champsim::parallel_tick::number_refill()
{
  // In core order, as the serial loop numbers them.
  uint64_t total{0};
  for (auto count : pushed) {
    total += static_cast<uint64_t>(count);
  }
  auto next = tracereader::reserve_ids(total);
  for (std::size_t c = 0; c < std::size(cpus); ++c) {
    first_id[c] = next;
    next += static_cast<uint64_t>(pushed[c]);
  }
  ids_pending = true;
  if (!deferred_ids) {
    settle();
  }
}

void champsim::parallel_tick::settle()
{
  if (ids_pending) {
    for (std::size_t c = 0; c < std::size(cpus); ++c) {
      set_ids(c);
    }
    ids_pending = false;
  }
}

void champsim::parallel_tick::serial_refill(std::vector<tracereader>& traces, const std::vector<std::size_t>& trace_index)
{
  // As do_cycle does it.
  for (O3_CPU* cpu : cpus) {
    auto& trace = traces.at(trace_index.at(cpu->cpu));
    for (auto pkt_count = cpu->IN_QUEUE_SIZE - static_cast<long>(std::size(cpu->input_queue)); !trace.eof() && pkt_count > 0; --pkt_count) {
      cpu->input_queue.push_back(trace());
    }
  }
}

long champsim::parallel_tick::operator()(std::vector<tracereader>& traces, const std::vector<std::size_t>& trace_index, const chrono::clock& clock)
{
  if (nthreads > 1 && nthreads < std::size(lane_worker)) {
    ++tick_count;
    timed_tick = tick_count % ticks_per_sample == 0;
    if (tick_count % (ticks_per_sample * samples_per_rebalance) == 0) {
      rebalance();
    }
  }
  const auto& schedule = schedule_for(clock);
  run_clock = &clock;
  run_traces = &traces;
  run_trace_index = &trace_index;
  auto& heartbeat = std::get<0>(listeners);

  long progress{0};
  bool refilled = false;
  bool first_run = true;
  for (const auto& current : schedule) {
    if (current.serial != nullptr) {
      progress += current.serial->operate_on(clock);
      continue;
    }

    run = &current;
    run_fixup = first_run && ids_pending; // the previous tick's IDs, before any core reads them
    progress += dispatch();
    heartbeat.flush_deferred();
    if (run_fixup) {
      ids_pending = false;
    }
    first_run = false;

    if (current.refill) {
      refilled = true;
      number_refill();
    }
  }

  if (!refilled) {
    settle();
    serial_refill(traces, trace_index);
  }
  return progress;
}
