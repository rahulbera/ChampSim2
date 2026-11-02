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

#include "champsim.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <numeric>
#include <vector>
#include <fmt/chrono.h>
#include <fmt/core.h>

#include "environment.h"
#include "event_listeners.h"
#include "ooo_cpu.h"
#include "operable.h"
#include "phase_info.h"
#include "tracereader.h"

const auto start_time = std::chrono::steady_clock::now();

std::chrono::seconds elapsed_time() { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start_time); }

namespace champsim
{
long do_cycle(std::vector<std::reference_wrapper<operable>>& operables, const std::vector<std::reference_wrapper<O3_CPU>>& cpus,
              std::vector<tracereader>& traces, const std::vector<std::vector<std::size_t>>& trace_index, champsim::chrono::clock& global_clock)
{
  std::stable_sort(std::begin(operables), std::end(operables),
                   [](const champsim::operable& lhs, const champsim::operable& rhs) { return lhs.current_time < rhs.current_time; });

  // Operate
  long progress{0};
  for (champsim::operable& op : operables) {
    progress += op.operate_on(global_clock);
  }

  // Read from trace: one trace per (core, virtual thread) pair. trace_index is
  // keyed [core][virtual_id] and names the positional (command-line) index of the
  // trace feeding that virtual thread; slot_map names which virtual_id is bound to
  // each physical slot. The tracereader already stamped that trace's instructions
  // with asid {position, position} (D-A: the global trace slot), so this slot's
  // asid is {trace_index[cpu.cpu][v], trace_index[cpu.cpu][v]} by construction.
  // do_cycle assigns nothing -- the asid lives in the trace. C_virt == C_phys
  // collapses this to the old single-context read (slot_map[c] == c).
  for (O3_CPU& cpu : cpus) {
    for (std::size_t c = 0; c < std::size(cpu.contexts); ++c) {
      // B2.2: an empty slot (slot_map == -1) holds a suspended virtual thread and
      // reads no trace until swap-in rebinds it (B2.3). Under the identity binding
      // (default) slot_map[c] == c, so this collapses to the old read.
      const long bound = cpu.slot_map.at(c);
      if (bound < 0)
        continue;
      auto& ctx = cpu.contexts[c];
      auto& trace = traces.at(trace_index.at(cpu.cpu).at(static_cast<std::size_t>(bound)));
      // V2.5: arm register reclamation once this context's trace is exhausted.
      // Under the default repeat=true the tracereader never reports EOF, so this
      // stays false and reclamation is dead code; with sim.trace_repeat=false it
      // lets the core reclaim a drained thread's physical registers.
      if (trace.eof()) {
        ctx.trace_eof = true;
      }
      // B3.1.5: a quota-finished virtual thread reads no more trace -- its
      // pipeline drains and its slot is reclaimed (see reclaim_finished_threads).
      // The marker is only ever set while quota_finish is on, so this is inert
      // (and byte-identical) by default.
      const bool quota_finished = cpu.virtual_threads.at(static_cast<std::size_t>(bound)).st == champsim::virtual_thread::state::finished;
      for (auto pkt_count = cpu.IN_QUEUE_SIZE - static_cast<long>(std::size(ctx.input_queue)); !trace.eof() && !quota_finished && pkt_count > 0; --pkt_count) {
        ctx.input_queue.push_back(trace());
      }
    }
  }

  return progress;
}

phase_stats do_phase(const phase_info& phase, environment& env, std::vector<tracereader>& traces, champsim::chrono::clock& global_clock,
                     const simulation_knobs& knobs)
{
  auto operables = env.operable_view();
  auto [phase_name, is_warmup, length, trace_index, trace_names] = phase;

  // Initialize phase
  for (champsim::operable& op : operables) {
    op.warmup = is_warmup;
    op.begin_phase();
  }

  // The environment owns a fixed component set throughout a phase.
  const auto cpus = env.cpu_view();
  auto cycle_operables = operables;

  const auto time_quantum = std::accumulate(std::cbegin(operables), std::cend(operables), champsim::chrono::clock::duration::max(),
                                            [](const auto acc, const operable& y) { return std::min(acc, y.clock_period); });

  bool livelock_trigger{false};
  const uint64_t livelock_period{knobs.livelock_period};
  uint64_t livelock_timer{0};
  //                                   die | critical | warning
  std::vector<double> livelock_threshold{0.01, 0.02, 0.05};
  std::vector<uint64_t> livelock_instr(std::size(cpus), 0);

  // Core-wide retirement progress for the livelock check. This is the SUM over
  // the core's hardware contexts: it advances whenever ANY context retires, so
  // two threads legitimately alternating "one stalled, one running" still counts
  // as progress and is not misflagged as livelock. With one context this is
  // exactly the old single-context cpu.sim_instr().
  const auto core_total_instr = [](const O3_CPU& cpu) {
    long long total = 0;
    for (std::size_t c = 0; c < std::size(cpu.contexts); ++c)
      total += cpu.sim_instr(c);
    return total;
  };

  // Perform phase
  int stalled_cycle{0};
  std::vector<bool> phase_complete(std::size(cpus), false);
  auto next_phase_complete = phase_complete;
  while (!std::accumulate(std::begin(phase_complete), std::end(phase_complete), true, std::logical_and{})) {
    next_phase_complete = phase_complete;
    global_clock.tick(time_quantum);

    // Restore canonical order before sorting: retaining last cycle's sorted
    // order changes how equal-time operables are visited.
    std::copy(std::begin(operables), std::end(operables), std::begin(cycle_operables));
    auto progress = do_cycle(cycle_operables, cpus, traces, trace_index, global_clock);

    if (progress == 0) {
      ++stalled_cycle;
    } else {
      stalled_cycle = 0;
    }

    // Livelock detect, every livelock_period cycles, check progress and alert the user
    livelock_timer++;
    if (livelock_timer >= livelock_period) {
      // for each cpu
      for (O3_CPU& cpu : cpus) {
        // for each threshold
        for (auto thres = std::begin(livelock_threshold); thres != std::end(livelock_threshold); thres++) {
          double livelock_ipc = std::ceil(core_total_instr(cpu) - livelock_instr[cpu.cpu]) / std::ceil(livelock_period);
          if (livelock_ipc <= *thres) {
            if (std::distance(std::begin(livelock_threshold), thres) == 0) {
              livelock_trigger = true;
              fmt::print("{} CPU {} panic: IPC {:.5g} < {:.5g}\n", phase_name, cpu.cpu, livelock_ipc, *thres);
            } else if (std::distance(std::begin(livelock_threshold), thres) == 1)
              fmt::print("{} CPU {} critical: IPC {:.5g} < {:.5g}\n", phase_name, cpu.cpu, livelock_ipc, *thres);
            else
              fmt::print("{} CPU {} warning: IPC {:.5g} < {:.5g}\n", phase_name, cpu.cpu, livelock_ipc, *thres);

            break;
          }
        }
        livelock_instr[cpu.cpu] = core_total_instr(cpu);
      }
      livelock_timer = 0;
    }

    if (stalled_cycle >= knobs.deadlock_cycle || livelock_trigger) {
      print_deadlock_diagnostics(operables);
      // abort() flushes nothing. With stdout a pipe or a file, as in a batch
      // job, the buffered tail of these diagnostics -- the memory backend's,
      // printed last -- would otherwise be lost.
      std::cout.flush();
      std::fflush(stdout);
      abort();
    }

    // Check for phase finish. A core is complete when every hardware context has
    // either retired its share of the phase or exhausted its trace (EOF). An
    // EOF'd context stops receiving input (do_cycle gates on !trace.eof()) and no
    // longer participates in the retirement check, but its in-flight instructions
    // still drain while its sibling keeps running. This merges the old two rules
    // -- "retire length" and "any trace EOF ends everything" -- into one
    // per-context predicate, so a short trace no longer cuts its sibling off
    // mid-phase. contexts.size() == 1 collapses it to the old single-context
    // union (retire length || trace EOF), byte-identical.
    for (O3_CPU& cpu : cpus) {
      // B3.1.5: stamp the seal threshold so mark_quota_finished() (operate) can
      // compare against this phase's length. Constant across the phase, so the
      // first stamp of the phase is enough; a stale value from the previous phase
      // is harmless because begin_phase resets num_retired to zero.
      cpu.quota_length = length;

      // record_roi_completion is meaningful per-context under both conventions:
      // under the default (off) it latches each context's own ROI-completion
      // cycle; under quota_finish a BMT slot's value is the first time that slot
      // retired `length` across whatever virtual threads ran on it (approximate),
      // while a slot arm's value is exactly the thread's. Latched idempotently.
      for (std::size_t c = 0; c < std::size(cpu.contexts); ++c)
        cpu.record_roi_completion(c, length);

      bool core_complete = true;
      if (cpu.quota_finish && !is_warmup) {
        // B3.1.5 per-virtual-thread budget: a core is complete when every virtual
        // thread has retired its own share of the phase (or its trace EOF'd). For
        // a slot arm (C_virt == C_phys) virtual_threads[v].num_retired equals
        // sim_instr(v), so this is the old per-context union exactly; for a BMT
        // arm it is the paper's "fixed total instruction budget" -- the fast
        // thread's tail idle no longer closes the window.
        for (std::size_t v = 0; v < std::size(cpu.virtual_threads); ++v) {
          const bool done = cpu.virtual_threads[v].num_retired >= length || traces.at(trace_index.at(cpu.cpu).at(v)).eof();
          core_complete = core_complete && done;
        }
      } else {
        for (std::size_t c = 0; c < std::size(cpu.contexts); ++c) {
          // B2.2/B2.3: a suspended (empty) slot is not finished -- its virtual
          // thread is recoverable and will be re-bound. Gate both the retirement
          // and the EOF terms on the slot being bound, so an empty slot never
          // completes the phase from a stale retirement count (the V3.3
          // "suspended == finished" conflation, which BMT must not inherit). Under
          // the default binding (slot_map[c] == c) bound >= 0 always holds, so this
          // is byte-identical to the old union (retire length || trace EOF).
          const long bound = cpu.slot_map.at(c);
          const bool retired = bound >= 0 && cpu.sim_instr(c) >= length;
          const bool eof = bound >= 0 && traces.at(trace_index.at(cpu.cpu).at(static_cast<std::size_t>(bound))).eof();
          core_complete = core_complete && (retired || eof);
        }
      }
      next_phase_complete[cpu.cpu] = next_phase_complete[cpu.cpu] || core_complete;
    }

    for (O3_CPU& cpu : cpus) {
      if (next_phase_complete[cpu.cpu] != phase_complete[cpu.cpu]) {
        for (champsim::operable& op : operables) {
          op.end_phase(cpu.cpu);
        }

        for (std::size_t c = 0; c < std::size(cpu.contexts); ++c) {
          fmt::print("{} finished CPU {} instructions: {} cycles: {} cumulative IPC: {:.4g} (Simulation time: {:%H hr %M min %S sec})\n", phase_name, cpu.cpu,
                     cpu.sim_instr(c), cpu.sim_cycle(c), std::ceil(cpu.sim_instr(c)) / std::ceil(cpu.sim_cycle(c)), elapsed_time());
        }
      }
    }

    phase_complete = next_phase_complete;
  }

  for (O3_CPU& cpu : cpus) {
    for (std::size_t c = 0; c < std::size(cpu.contexts); ++c) {
      fmt::print("{} complete CPU {} instructions: {} cycles: {} cumulative IPC: {:.4g} (Simulation time: {:%H hr %M min %S sec})\n", phase_name, cpu.cpu,
                 cpu.sim_instr(c), cpu.sim_cycle(c), std::ceil(cpu.sim_instr(c)) / std::ceil(cpu.sim_cycle(c)), elapsed_time());
    }
  }

  phase_stats stats;
  stats.name = phase.name;

  for (const auto& core_row : trace_index) {
    for (std::size_t trace_idx : core_row) {
      stats.trace_names.push_back(trace_names.at(trace_idx));
    }
  }

  // Per-context core statistics, flattened in [core][context] order. contexts
  // with a single slot collapse this to one row per core, byte-identical to the
  // previous ctx()-only transform.
  for (const O3_CPU& cpu : cpus) {
    for (const auto& c : cpu.contexts) {
      stats.sim_cpu_stats.push_back(c.sim_stats);
      stats.roi_cpu_stats.push_back(c.roi_stats);
    }
  }

  auto caches = env.cache_view();
  std::transform(std::begin(caches), std::end(caches), std::back_inserter(stats.sim_cache_stats), [](const CACHE& cache) { return cache.sim_stats; });
  std::transform(std::begin(caches), std::end(caches), std::back_inserter(stats.roi_cache_stats), [](const CACHE& cache) { return cache.roi_stats; });

  auto memory_stats = env.memory_view().statistics();
  stats.sim_dram_stats = std::move(memory_stats.sim_dram);
  stats.roi_dram_stats = std::move(memory_stats.roi_dram);
  stats.sim_ramulator2 = std::move(memory_stats.sim_ramulator2);
  stats.roi_ramulator2 = std::move(memory_stats.roi_ramulator2);

  return stats;
}

// simulation entry point
std::vector<phase_stats> main(environment& env, std::vector<phase_info>& phases, std::vector<tracereader>& traces, const simulation_knobs& knobs)
{
  for (champsim::operable& op : env.operable_view()) {
    op.initialize();
  }

  champsim::chrono::clock global_clock;
  std::vector<phase_stats> results;
  for (auto phase : phases) {
    // call event listeners
    handle_event<Event::BEGIN_PHASE>(phase.is_warmup);
    // handle_begin_phase(0, phase.is_warmup);

    auto stats = do_phase(phase, env, traces, global_clock, knobs);
    if (!phase.is_warmup) {
      results.push_back(stats);
    }
  }

  env.memory_view().finalize();
  return results;
}
} // namespace champsim
