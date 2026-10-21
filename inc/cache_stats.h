#ifndef CACHE_STATS_H
#define CACHE_STATS_H

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>

#include "channel.h"
#include "event_counter.h"

struct cache_stats {
  std::string name;
  // prefetch stats
  uint64_t pf_requested = 0;
  uint64_t pf_issued = 0;
  uint64_t pf_useful = 0;
  uint64_t pf_useless = 0;
  uint64_t pf_fill = 0;

  champsim::stats::event_counter<std::pair<access_type, std::remove_cv_t<decltype(NUM_CPUS)>>> hits = {};
  champsim::stats::event_counter<std::pair<access_type, std::remove_cv_t<decltype(NUM_CPUS)>>> misses = {};
  champsim::stats::event_counter<std::pair<access_type, std::remove_cv_t<decltype(NUM_CPUS)>>> miss_merge = {};
  champsim::stats::event_counter<std::pair<access_type, std::remove_cv_t<decltype(NUM_CPUS)>>> fill = {};

  long total_miss_latency_cycles{};

  // B1.4: read-only per-context load-access census, bucketed by the request's
  // asid[0]. Under the B1 identity binding asid[0] == ctx_index, and in B2 it
  // becomes the virtual_id (bmt-design.md §1.2), so the bucket key is the
  // virtual-thread identity throughout. Collected only when the cache is built
  // with cache_builder::per_context_counters(true); the array is otherwise all
  // zeros and the document omits it, so a default-off run is byte-identical.
  std::array<uint64_t, std::numeric_limits<uint8_t>::max() + 1> per_context_load_hits{};
  std::array<uint64_t, std::numeric_limits<uint8_t>::max() + 1> per_context_load_misses{};
};

cache_stats operator-(cache_stats lhs, cache_stats rhs);

#endif
