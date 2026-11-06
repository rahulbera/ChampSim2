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

  // Lanes go to workers round-robin, in lane order.
  auto worker_of = [&](std::size_t lane) {
    return static_cast<std::size_t>(std::distance(std::begin(distinct), std::find(std::begin(distinct), std::end(distinct), lane))) % nthreads;
  };

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
    const auto worker = lanes[i] == environment::no_lane ? std::size_t{0} : worker_of(lanes[i]);
    canonical.push_back({op, i, lanes[i], worker, is_cpu, op->clock_period == quantum});
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
      cpu_worker.push_back(0);
    } else {
      cpu_worker.push_back(it->worker);
    }
  }
  requested.resize(std::size(cpus));
  pushed.resize(std::size(cpus));
  first_id.resize(std::size(cpus));

  for (const auto& e : canonical) {
    order.push_back(&e);
  }
  build(canonical_schedule, order);

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
  stopping.store(true, std::memory_order_release);
  publish(epoch.load(std::memory_order_relaxed) + 1);
  for (auto& t : workers) {
    t.join();
  }
  auto& heartbeat = std::get<0>(listeners);
  heartbeat.flush_deferred();
  heartbeat.defer = false;
}

void champsim::parallel_tick::build(std::vector<step>& schedule, const std::vector<const entry*>& sequence) const
{
  std::size_t after_cpus{0};
  for (std::size_t i = 0; i < std::size(sequence); ++i) {
    if (sequence[i]->is_cpu) {
      after_cpus = i + 1;
    }
  }

  schedule.clear();
  bool refill_placed = false;
  for (std::size_t i = 0; i < std::size(sequence);) {
    if (sequence[i]->lane == environment::no_lane || nthreads == 1) {
      schedule.push_back({sequence[i]->op, {}, false});
      ++i;
      continue;
    }
    // Every core has operated before this run, so each one's refill count is final.
    step run_step{nullptr, std::vector<std::vector<operable*>>(nthreads), parallel_refill && !refill_placed && i >= after_cpus};
    refill_placed = refill_placed || run_step.refill;
    for (; i < std::size(sequence) && sequence[i]->lane != environment::no_lane; ++i) {
      run_step.by_worker[sequence[i]->worker].push_back(sequence[i]->op);
    }
    schedule.push_back(std::move(run_step));
  }
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
  long progress{0};
  for (operable* op : run->by_worker[worker]) {
    progress += op->operate_on(*run_clock);
  }
  if (run->refill) {
    for (std::size_t c = 0; c < std::size(cpus); ++c) {
      if (cpu_worker[c] == worker) {
        refill(c);
      }
    }
  }
  slots[worker].progress = progress;
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
  auto& cpu = *cpus[cpu_index];
  auto& trace = run_traces->at(run_trace_index->at(cpu.cpu));
  long count{0};
  for (; count < requested[cpu_index] && !trace.eof(); ++count) {
    cpu.input_queue.push_back(trace(first_id[cpu_index] + static_cast<uint64_t>(count)));
  }
  pushed[cpu_index] = count;
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
  const auto& schedule = schedule_for(clock);
  run_clock = &clock;
  run_traces = &traces;
  run_trace_index = &trace_index;
  auto& heartbeat = std::get<0>(listeners);

  long progress{0};
  bool refilled = false;
  for (const auto& current : schedule) {
    if (current.serial != nullptr) {
      progress += current.serial->operate_on(clock);
      continue;
    }

    if (current.refill) {
      // Number every core's instructions now, in core order, as the serial loop would.
      uint64_t total{0};
      for (std::size_t c = 0; c < std::size(cpus); ++c) {
        requested[c] = std::max<long>(0, cpus[c]->IN_QUEUE_SIZE - static_cast<long>(std::size(cpus[c]->input_queue)));
        total += static_cast<uint64_t>(requested[c]);
      }
      tick_first_id = tracereader::reserve_ids(total);
      auto next = tick_first_id;
      for (std::size_t c = 0; c < std::size(cpus); ++c) {
        first_id[c] = next;
        next += static_cast<uint64_t>(requested[c]);
      }
    }

    run = &current;
    progress += dispatch();
    heartbeat.flush_deferred();

    if (current.refill) {
      refilled = true;
      // A trace that ended early read fewer than its block: renumber the tick's instructions
      // without the gaps, as the serial loop would have numbered them.
      bool short_read = false;
      for (std::size_t c = 0; c < std::size(cpus); ++c) {
        short_read = short_read || pushed[c] < requested[c];
      }
      if (short_read) {
        auto next = tick_first_id;
        for (std::size_t c = 0; c < std::size(cpus); ++c) {
          auto& queue = cpus[c]->input_queue;
          for (auto it = std::prev(std::end(queue), pushed[c]); it != std::end(queue); ++it) {
            it->instr_id = next++;
          }
        }
        tracereader::set_next_id(next);
      }
    }
  }

  if (!refilled) {
    serial_refill(traces, trace_index);
  }
  return progress;
}
