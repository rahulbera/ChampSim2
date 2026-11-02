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

#ifndef CORE_BUILDER_H
#define CORE_BUILDER_H

#include <cstdint>
#include <limits>

#include "chrono.h"

class CACHE;
class O3_CPU;
namespace champsim
{
class channel;
template <typename...>
class core_builder_module_type_holder
{
};

// How a core's single ROB budget (rob_size) is divided among its hardware
// contexts.
//
//   equal  -- a fixed equal quota per context (rob_size / num_contexts), the
//             total-capacity-conserving baseline: the aggregate in-flight ROB
//             entries across contexts can never exceed one rob_size budget.
//             This is the Stage 5.2b "rigid split" and remains the N=1 lock
//             and V2.4-control denominator.
//   active -- the quota follows the *actually active* context count
//             (rob_size / max(1, active)), where `active` is the per-cycle
//             count of contexts with a non-empty ROB. Idle-context quota is
//             reclaimed, so an under-committed run can push a surviving
//             context's ROB toward the full budget. At N=1 this is exactly
//             rob_size/1 = equal, so single-context output is unchanged.
enum class rob_partition { equal, active };

// B2.4 swap-cost mode (bmt-design.md section 6). `instant` models the swap as an
// atomic 1-cycle event (the default, so a BMT-off run is byte-identical);
// `firmware` models the paper's step-by-step switch latency (~60 cycles total:
// rsave 16 + rrestore 16 + detect/flush/select/gate overhead ~28), during which
// the slot stalls (no fetch).
enum class bmt_switch_mode { instant, firmware };

// B2.5 long-latency-load detection source (bmt-design.md §5.6). `latency` is
// the B2.1 fixed in-flight-cycle threshold (the default, so a BMT-off run is
// byte-identical); `miss` is the miss-edge trigger -- a load is flagged the
// cycle its LLC miss is confirmed (the request's own arrival at the LLC, not a
// fixed cutoff), reusing the LLC's per-context load-miss census as the edge
// signal. The two arms coexist so the acceptance experiment can reconcile them.
enum class bmt_trigger_mode { latency, miss };

namespace detail
{
struct core_builder_base {
  uint32_t m_cpu{};
  champsim::chrono::picoseconds m_clock_period{250};
  std::size_t m_dib_set{1};
  std::size_t m_dib_way{1};
  std::size_t m_dib_window{1};
  std::size_t m_ifetch_buffer_size{1};
  std::size_t m_decode_buffer_size{1};
  std::size_t m_dispatch_buffer_size{1};

  // Hardware contexts (SMT threads) per core. Stage 3 targets dual-context; the
  // default stays 1 so single-context results are bit-identical.
  std::size_t m_num_contexts{1};

  // BMT virtual threads per core (C_virt). 0 means "unset", and the O3_CPU
  // constructor then falls back to num_contexts -- the identity case where the
  // virtual layer is inert and results are byte-identical.
  std::size_t m_num_virtual_threads{0};

  // BMT master switch and long-latency-load detection threshold (B2.1). The
  // threshold is in core cycles; the O3_CPU constructor scales it by the clock
  // period into a duration. Both default to "off" (false / the conservative 56c)
  // so a default run is byte-identical to B1/V2.5.
  bool m_bmt_enabled{false};
  std::size_t m_bmt_miss_threshold{56};
  // B2.2 swap-out switch. Refines bmt_enabled: detection marks the loads, the
  // swap-out acts on those marks. Off == byte-identical to B2.1 (detect-only).
  bool m_bmt_swap_enabled{false};

  // B2.4 swap-cost mode (instant vs firmware). Default instant == atomic swap,
  // byte-identical to B2.3.
  bmt_switch_mode m_bmt_switch_mode{bmt_switch_mode::instant};

  // B3.0 per-event trace log switch (bmt.trace_events). Default off, so a
  // default run is byte-identical; when on it emits one stderr line per
  // swap-out/swap-in/defer plus an end-of-run aggregate summary.
  bool m_bmt_trace_events{false};

  // B2.5 detection source (latency threshold vs miss edge) and the optional
  // miss-arm dwell (cycles between the LLC miss edge and swap-out). Default
  // latency / dwell 0 == the B2.4 behavior.
  bmt_trigger_mode m_bmt_trigger_mode{bmt_trigger_mode::latency};
  std::size_t m_bmt_miss_dwell{0};

  // B3.1.5 quota-finish seal switch. Default off, so a default run is inert and
  // byte-identical to the pre-seal simulator.
  bool m_quota_finish{false};

  // B3.4 per-virtual small predictor state (GHR/RAS) keying. Default off == the
  // shared single-instance GHR/RAS, byte-identical to prior stages. When on, the
  // branch predictor's GHR (ghist) and the BTB's RAS are keyed by asid[0] (the
  // virtual thread's trace slot) instead of shared across contexts; the big
  // tables (PHT / BTB body) stay shared.
  bool m_per_virtual_small_state{false};

  // B3.2 Scenario-B self-swap suppression (bmt.suppress_self_swap). Default off
  // == current behavior (a miss in the dual-live tail swaps the thread out and
  // back into itself, a pure flush+restore with no latency hiding). On, a swap-out
  // is skipped when no OTHER suspended virtual thread exists to swap in -- i.e.
  // exactly the Scenario-B self-swap. Measurement-only A/B knob (bmt-design.md
  // §5.6 tail), not a fix to adopt.
  bool m_bmt_suppress_self_swap{false};

  // B4.0 sparse slow load (ooo_cpu.cpu0.bmt.sparse_slow_load.*). Default off.
  // `enabled` samples `ratio` of demand loads by a deterministic (asid, ip)
  // hash; `extra_latency` is the return-path wait in core cycles, added at the
  // LLC. Off == no mark == byte-identical.
  bool m_bmt_sparse_slow_load_enabled{false};
  double m_bmt_sparse_slow_load_ratio{0.0};
  std::size_t m_bmt_sparse_slow_load_extra_latency{0};

  // B2.5 miss-edge signal source: a pointer to the shared LLC, whose per-context
  // load-miss counter (filled when the LLC is built with per_context_counters)
  // is the edge the `miss` arm diffs. Wired by static_environment, like l1i.
  CACHE* m_llc{};

  std::size_t m_dib_hit_buffer_size{1};

  std::size_t m_register_file_size{1};
  std::size_t m_rob_size{1};
  champsim::rob_partition m_rob_partition{champsim::rob_partition::equal};
  std::size_t m_lq_size{1};
  std::size_t m_sq_size{1};

  champsim::bandwidth::maximum_type m_fetch_width{1};
  champsim::bandwidth::maximum_type m_decode_width{1};
  champsim::bandwidth::maximum_type m_dispatch_width{1};
  champsim::bandwidth::maximum_type m_schedule_width{1};
  champsim::bandwidth::maximum_type m_execute_width{1};
  champsim::bandwidth::maximum_type m_lq_width{1};
  champsim::bandwidth::maximum_type m_sq_width{1};
  champsim::bandwidth::maximum_type m_retire_width{1};
  champsim::bandwidth::maximum_type m_dib_inorder_width{1};

  unsigned m_dib_hit_latency{};

  unsigned m_mispredict_penalty{};
  unsigned m_decode_latency{};
  unsigned m_dispatch_latency{};
  unsigned m_schedule_latency{};
  unsigned m_execute_latency{};

  CACHE* m_l1i{};
  champsim::bandwidth::maximum_type m_l1i_bw{1};
  champsim::bandwidth::maximum_type m_l1d_bw{1};
  champsim::channel* m_fetch_queues{};
  champsim::channel* m_data_queues{};
};
} // namespace detail

template <typename B = core_builder_module_type_holder<>, typename T = core_builder_module_type_holder<>>
class core_builder : public detail::core_builder_base
{
  using self_type = core_builder<B, T>;

  friend class ::O3_CPU;

  template <typename OTHER_B, typename OTHER_T>
  friend class core_builder;

  explicit core_builder(const detail::core_builder_base& other) : detail::core_builder_base(other) {}

public:
  core_builder() = default;

  self_type& index(uint32_t cpu_);

  /**
   * Specify the core's clock period.
   */
  self_type& clock_period(champsim::chrono::picoseconds clock_period_);

  /**
   * Specify the number of sets in the Decoded Instruction Buffer.
   */
  self_type& dib_set(std::size_t dib_set_);

  /**
   * Specify the number of ways in the Decoded Instruction Buffer.
   */
  self_type& dib_way(std::size_t dib_way_);

  /**
   * Specify the size of the window within which Decoded Instruction Buffer entries are equivalent.
   */
  self_type& dib_window(std::size_t dib_window_);

  /**
   * Specify the maximum size of the instruction fetch buffer.
   */
  self_type& ifetch_buffer_size(std::size_t ifetch_buffer_size_);

  /**
   * Specify the maximum size of the decode buffer.
   */
  self_type& decode_buffer_size(std::size_t decode_buffer_size_);

  /**
   * Specify the maximum size of the dispatch buffer.
   */
  self_type& dispatch_buffer_size(std::size_t dispatch_buffer_size_);

  /**
   * Specify the number of hardware contexts (SMT threads) on this core.
   */
  self_type& num_contexts(std::size_t num_contexts_);

  /**
   * Specify the number of BMT virtual threads (virtual contexts) on this core.
   * Must be >= num_contexts; 0 lets the core fall back to num_contexts.
   */
  self_type& num_virtual_threads(std::size_t num_virtual_threads_);

  /**
   * Master switch for every BMT mechanism. Default off == byte-identical to B1/V2.5.
   */
  self_type& bmt_enabled(bool bmt_enabled_);

  /**
   * Long-latency-load detection threshold, in core cycles.
   */
  self_type& bmt_miss_threshold(std::size_t bmt_miss_threshold_);

  /**
   * B2.2 swap-out switch (refines bmt_enabled). Default off.
   */
  self_type& bmt_swap_enabled(bool bmt_swap_enabled_);

  /**
   * B2.4 swap-cost mode (instant vs firmware). Default instant.
   */
  self_type& bmt_switch(bmt_switch_mode bmt_switch_mode_);

  /**
   * B3.0 per-event trace log switch. Default off.
   */
  self_type& bmt_trace_events(bool bmt_trace_events_);

  /**
   * B2.5 detection source: latency threshold (default) or miss edge.
   */
  self_type& bmt_trigger(bmt_trigger_mode bmt_trigger_mode_);

  /**
   * B2.5 miss-arm dwell, in cycles between the LLC miss edge and swap-out.
   */
  self_type& bmt_miss_dwell(std::size_t bmt_miss_dwell_);

  /**
   * B3.1.5 quota-finish seal switch. Default off.
   */
  self_type& quota_finish(bool quota_finish_);

  /**
   * B3.4 per-virtual small predictor state (GHR/RAS) keying. Default off.
   */
  self_type& per_virtual_small_state(bool per_virtual_small_state_);

  /**
   * B3.2 Scenario-B self-swap suppression. Default off == current behavior.
   */
  self_type& bmt_suppress_self_swap(bool bmt_suppress_self_swap_);

  /**
   * B4.0 sparse slow load: enable deterministic sampling of ultra-long-wait
   * loads. Default off.
   */
  self_type& bmt_sparse_slow_load_enabled(bool bmt_sparse_slow_load_enabled_);

  /**
   * B4.0 sparse slow load: sampling ratio in [0,1].
   */
  self_type& bmt_sparse_slow_load_ratio(double bmt_sparse_slow_load_ratio_);

  /**
   * B4.0 sparse slow load: return-path extra latency, in core cycles.
   */
  self_type& bmt_sparse_slow_load_extra_latency(std::size_t bmt_sparse_slow_load_extra_latency_);

  /**
   * B2.5 miss-edge signal source: the shared LLC whose per-context load-miss
   * counter the `miss` arm reads. Wired by static_environment.
   */
  self_type& llc(CACHE* llc_);

  /**
   * Specify the maximum size of the DIB hit buffer.
   */
  self_type& dib_hit_buffer_size(std::size_t dib_hit_buffer_size_);

  /**
   * Specify the maximum size of the physical register file.
   */
  self_type& register_file_size(std::size_t register_file_size_);

  /**
   * Specify the maximum size of the reorder buffer.
   */
  self_type& rob_size(std::size_t rob_size_);

  /**
   * Specify how the ROB budget is divided among hardware contexts. `equal` and
   * `active` are implemented (see rob_partition).
   */
  self_type& rob_partition(champsim::rob_partition rob_partition_);

  /**
   * Specify the maximum size of the load queue.
   */
  self_type& lq_size(std::size_t lq_size_);

  /**
   * Specify the maximum size of the store queue.
   */
  self_type& sq_size(std::size_t sq_size_);

  /**
   * Specify the width of the instruction fetch.
   */
  self_type& fetch_width(champsim::bandwidth::maximum_type fetch_width_);

  /**
   * Specify the width of the decode.
   */
  self_type& decode_width(champsim::bandwidth::maximum_type decode_width_);

  /**
   * Specify the width of the dispatch.
   */
  self_type& dispatch_width(champsim::bandwidth::maximum_type dispatch_width_);

  /**
   * Specify the width of the scheduler.
   */
  self_type& schedule_width(champsim::bandwidth::maximum_type schedule_width_);

  /**
   * Specify the width of the execution.
   */
  self_type& execute_width(champsim::bandwidth::maximum_type execute_width_);

  /**
   * Specify the width of the load issue.
   */
  self_type& lq_width(champsim::bandwidth::maximum_type lq_width_);

  /**
   * Specify the width of the store issue.
   */
  self_type& sq_width(champsim::bandwidth::maximum_type sq_width_);

  /**
   * Specify the width of the retirement.
   */
  self_type& retire_width(champsim::bandwidth::maximum_type retire_width_);

  /**
   * Specify the maximum size of the DIB inorder width.
   */
  self_type& dib_inorder_width(champsim::bandwidth::maximum_type dib_inorder_width_);

  /**
   * Specify the reset penalty, in cycles, that follows a misprediction.
   * Note that this value is in addition to the cost of restarting the pipeline, which will depend on the number of instructions inflight at the time when the
   * misprediction is detected.
   */
  self_type& mispredict_penalty(unsigned mispredict_penalty_);

  /**
   * Specify the latency of the decode.
   */
  self_type& decode_latency(unsigned decode_latency_);

  /**
   * Specify the latency of dispatch.
   */
  self_type& dispatch_latency(unsigned dispatch_latency_);

  /**
   * Specify the latency of the scheduler.
   */
  self_type& schedule_latency(unsigned schedule_latency_);

  /**
   * Specify the latency of execution.
   */
  self_type& execute_latency(unsigned execute_latency_);

  /**
   * Specify the latency of execution.
   */
  self_type& dib_hit_latency(unsigned dib_hit_latency_);

  /**
   * Specify a pointer to the L1I cache. This is only used to transmit branch triggers for prefetcher branch hooks.
   */
  self_type& l1i(CACHE* l1i_);

  /**
   * Specify the instruction cache bandwidth.
   */
  self_type& l1i_bandwidth(champsim::bandwidth::maximum_type l1i_bw_);

  /**
   * Specify the data cache bandwidth.
   */
  self_type& l1d_bandwidth(champsim::bandwidth::maximum_type l1d_bw_);

  /**
   * Specify the downstream queues to the instruction cache.
   */
  self_type& fetch_queues(champsim::channel* fetch_queues_);

  /**
   * Specify the downstream queues to the data cache.
   */
  self_type& data_queues(champsim::channel* data_queues_);

  /**
   * Specify the branch direction predictor.
   */
  template <typename... Bs>
  core_builder<core_builder_module_type_holder<Bs...>, T> branch_predictor();

  /**
   * Specify the branch target predictor.
   */
  template <typename... Ts>
  core_builder<B, core_builder_module_type_holder<Ts...>> btb();
};
} // namespace champsim

template <typename B, typename T>
auto champsim::core_builder<B, T>::index(uint32_t cpu_) -> self_type&
{
  m_cpu = cpu_;
  return *this;
}

template <typename B, typename T>
auto champsim::core_builder<B, T>::clock_period(champsim::chrono::picoseconds clock_period_) -> self_type&
{
  m_clock_period = clock_period_;
  return *this;
}

template <typename B, typename T>
auto champsim::core_builder<B, T>::dib_set(std::size_t dib_set_) -> self_type&
{
  m_dib_set = dib_set_;
  return *this;
}

template <typename B, typename T>
auto champsim::core_builder<B, T>::dib_way(std::size_t dib_way_) -> self_type&
{
  m_dib_way = dib_way_;
  return *this;
}

template <typename B, typename T>
auto champsim::core_builder<B, T>::dib_window(std::size_t dib_window_) -> self_type&
{
  m_dib_window = dib_window_;
  return *this;
}

template <typename B, typename T>
auto champsim::core_builder<B, T>::ifetch_buffer_size(std::size_t ifetch_buffer_size_) -> self_type&
{
  m_ifetch_buffer_size = ifetch_buffer_size_;
  return *this;
}

template <typename B, typename T>
auto champsim::core_builder<B, T>::decode_buffer_size(std::size_t decode_buffer_size_) -> self_type&
{
  m_decode_buffer_size = decode_buffer_size_;
  return *this;
}

template <typename B, typename T>
auto champsim::core_builder<B, T>::dispatch_buffer_size(std::size_t dispatch_buffer_size_) -> self_type&
{
  m_dispatch_buffer_size = dispatch_buffer_size_;
  return *this;
}

template <typename B, typename T>
auto champsim::core_builder<B, T>::num_contexts(std::size_t num_contexts_) -> self_type&
{
  m_num_contexts = num_contexts_;
  return *this;
}

template <typename B, typename T>
auto champsim::core_builder<B, T>::num_virtual_threads(std::size_t num_virtual_threads_) -> self_type&
{
  m_num_virtual_threads = num_virtual_threads_;
  return *this;
}

template <typename B, typename T>
auto champsim::core_builder<B, T>::bmt_enabled(bool bmt_enabled_) -> self_type&
{
  m_bmt_enabled = bmt_enabled_;
  return *this;
}

template <typename B, typename T>
auto champsim::core_builder<B, T>::bmt_miss_threshold(std::size_t bmt_miss_threshold_) -> self_type&
{
  m_bmt_miss_threshold = bmt_miss_threshold_;
  return *this;
}

template <typename B, typename T>
auto champsim::core_builder<B, T>::bmt_swap_enabled(bool bmt_swap_enabled_) -> self_type&
{
  m_bmt_swap_enabled = bmt_swap_enabled_;
  return *this;
}

template <typename B, typename T>
auto champsim::core_builder<B, T>::bmt_switch(bmt_switch_mode bmt_switch_mode_) -> self_type&
{
  m_bmt_switch_mode = bmt_switch_mode_;
  return *this;
}

template <typename B, typename T>
auto champsim::core_builder<B, T>::bmt_trace_events(bool bmt_trace_events_) -> self_type&
{
  m_bmt_trace_events = bmt_trace_events_;
  return *this;
}

template <typename B, typename T>
auto champsim::core_builder<B, T>::bmt_trigger(bmt_trigger_mode bmt_trigger_mode_) -> self_type&
{
  m_bmt_trigger_mode = bmt_trigger_mode_;
  return *this;
}

template <typename B, typename T>
auto champsim::core_builder<B, T>::bmt_miss_dwell(std::size_t bmt_miss_dwell_) -> self_type&
{
  m_bmt_miss_dwell = bmt_miss_dwell_;
  return *this;
}

template <typename B, typename T>
auto champsim::core_builder<B, T>::quota_finish(bool quota_finish_) -> self_type&
{
  m_quota_finish = quota_finish_;
  return *this;
}

template <typename B, typename T>
auto champsim::core_builder<B, T>::per_virtual_small_state(bool per_virtual_small_state_) -> self_type&
{
  m_per_virtual_small_state = per_virtual_small_state_;
  return *this;
}

template <typename B, typename T>
auto champsim::core_builder<B, T>::bmt_suppress_self_swap(bool bmt_suppress_self_swap_) -> self_type&
{
  m_bmt_suppress_self_swap = bmt_suppress_self_swap_;
  return *this;
}

template <typename B, typename T>
auto champsim::core_builder<B, T>::bmt_sparse_slow_load_enabled(bool bmt_sparse_slow_load_enabled_) -> self_type&
{
  m_bmt_sparse_slow_load_enabled = bmt_sparse_slow_load_enabled_;
  return *this;
}

template <typename B, typename T>
auto champsim::core_builder<B, T>::bmt_sparse_slow_load_ratio(double bmt_sparse_slow_load_ratio_) -> self_type&
{
  m_bmt_sparse_slow_load_ratio = bmt_sparse_slow_load_ratio_;
  return *this;
}

template <typename B, typename T>
auto champsim::core_builder<B, T>::bmt_sparse_slow_load_extra_latency(std::size_t bmt_sparse_slow_load_extra_latency_) -> self_type&
{
  m_bmt_sparse_slow_load_extra_latency = bmt_sparse_slow_load_extra_latency_;
  return *this;
}

template <typename B, typename T>
auto champsim::core_builder<B, T>::llc(CACHE* llc_) -> self_type&
{
  m_llc = llc_;
  return *this;
}

template <typename B, typename T>
auto champsim::core_builder<B, T>::register_file_size(std::size_t register_file_size_) -> self_type&
{
  m_register_file_size = register_file_size_;
  return *this;
}

template <typename B, typename T>
auto champsim::core_builder<B, T>::rob_size(std::size_t rob_size_) -> self_type&
{
  m_rob_size = rob_size_;
  return *this;
}

template <typename B, typename T>
auto champsim::core_builder<B, T>::rob_partition(champsim::rob_partition rob_partition_) -> self_type&
{
  m_rob_partition = rob_partition_;
  return *this;
}

template <typename B, typename T>
auto champsim::core_builder<B, T>::dib_hit_buffer_size(std::size_t dib_hit_buffer_size_) -> self_type&
{
  m_dib_hit_buffer_size = dib_hit_buffer_size_;
  return *this;
}

template <typename B, typename T>
auto champsim::core_builder<B, T>::lq_size(std::size_t lq_size_) -> self_type&
{
  m_lq_size = lq_size_;
  return *this;
}

template <typename B, typename T>
auto champsim::core_builder<B, T>::sq_size(std::size_t sq_size_) -> self_type&
{
  m_sq_size = sq_size_;
  return *this;
}

template <typename B, typename T>
auto champsim::core_builder<B, T>::fetch_width(champsim::bandwidth::maximum_type fetch_width_) -> self_type&
{
  m_fetch_width = fetch_width_;
  return *this;
}

template <typename B, typename T>
auto champsim::core_builder<B, T>::decode_width(champsim::bandwidth::maximum_type decode_width_) -> self_type&
{
  m_decode_width = decode_width_;
  return *this;
}

template <typename B, typename T>
auto champsim::core_builder<B, T>::dispatch_width(champsim::bandwidth::maximum_type dispatch_width_) -> self_type&
{
  m_dispatch_width = dispatch_width_;
  return *this;
}

template <typename B, typename T>
auto champsim::core_builder<B, T>::schedule_width(champsim::bandwidth::maximum_type schedule_width_) -> self_type&
{
  m_schedule_width = schedule_width_;
  return *this;
}

template <typename B, typename T>
auto champsim::core_builder<B, T>::execute_width(champsim::bandwidth::maximum_type execute_width_) -> self_type&
{
  m_execute_width = execute_width_;
  return *this;
}

template <typename B, typename T>
auto champsim::core_builder<B, T>::lq_width(champsim::bandwidth::maximum_type lq_width_) -> self_type&
{
  m_lq_width = lq_width_;
  return *this;
}

template <typename B, typename T>
auto champsim::core_builder<B, T>::sq_width(champsim::bandwidth::maximum_type sq_width_) -> self_type&
{
  m_sq_width = sq_width_;
  return *this;
}

template <typename B, typename T>
auto champsim::core_builder<B, T>::retire_width(champsim::bandwidth::maximum_type retire_width_) -> self_type&
{
  m_retire_width = retire_width_;
  return *this;
}

template <typename B, typename T>
auto champsim::core_builder<B, T>::dib_inorder_width(champsim::bandwidth::maximum_type dib_inorder_width_) -> self_type&
{
  m_dib_inorder_width = dib_inorder_width_;
  return *this;
}

template <typename B, typename T>
auto champsim::core_builder<B, T>::mispredict_penalty(unsigned mispredict_penalty_) -> self_type&
{
  m_mispredict_penalty = mispredict_penalty_;
  return *this;
}

template <typename B, typename T>
auto champsim::core_builder<B, T>::decode_latency(unsigned decode_latency_) -> self_type&
{
  m_decode_latency = decode_latency_;
  return *this;
}

template <typename B, typename T>
auto champsim::core_builder<B, T>::dib_hit_latency(unsigned dib_hit_latency_) -> self_type&
{
  m_dib_hit_latency = dib_hit_latency_;
  return *this;
}

template <typename B, typename T>
auto champsim::core_builder<B, T>::dispatch_latency(unsigned dispatch_latency_) -> self_type&
{
  m_dispatch_latency = dispatch_latency_;
  return *this;
}

template <typename B, typename T>
auto champsim::core_builder<B, T>::schedule_latency(unsigned schedule_latency_) -> self_type&
{
  m_schedule_latency = schedule_latency_;
  return *this;
}

template <typename B, typename T>
auto champsim::core_builder<B, T>::execute_latency(unsigned execute_latency_) -> self_type&
{
  m_execute_latency = execute_latency_;
  return *this;
}

template <typename B, typename T>
auto champsim::core_builder<B, T>::l1i(CACHE* l1i_) -> self_type&
{
  m_l1i = l1i_;
  return *this;
}

template <typename B, typename T>
auto champsim::core_builder<B, T>::l1i_bandwidth(champsim::bandwidth::maximum_type l1i_bw_) -> self_type&
{
  m_l1i_bw = l1i_bw_;
  return *this;
}

template <typename B, typename T>
auto champsim::core_builder<B, T>::l1d_bandwidth(champsim::bandwidth::maximum_type l1d_bw_) -> self_type&
{
  m_l1d_bw = l1d_bw_;
  return *this;
}

template <typename B, typename T>
auto champsim::core_builder<B, T>::fetch_queues(champsim::channel* fetch_queues_) -> self_type&
{
  m_fetch_queues = fetch_queues_;
  return *this;
}

template <typename B, typename T>
auto champsim::core_builder<B, T>::data_queues(champsim::channel* data_queues_) -> self_type&
{
  m_data_queues = data_queues_;
  return *this;
}

template <typename B, typename T>
template <typename... Bs>
auto champsim::core_builder<B, T>::branch_predictor() -> champsim::core_builder<core_builder_module_type_holder<Bs...>, T>
{
  return champsim::core_builder<core_builder_module_type_holder<Bs...>, T>{*this};
}

template <typename B, typename T>
template <typename... Ts>
auto champsim::core_builder<B, T>::btb() -> champsim::core_builder<B, core_builder_module_type_holder<Ts...>>
{
  return champsim::core_builder<B, core_builder_module_type_holder<Ts...>>{*this};
}

#endif
