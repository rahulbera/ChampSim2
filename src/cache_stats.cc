#include "cache_stats.h"

cache_stats operator-(cache_stats lhs, cache_stats rhs)
{
  cache_stats result;
  result.pf_requested = lhs.pf_requested - rhs.pf_requested;
  result.pf_issued = lhs.pf_issued - rhs.pf_issued;
  result.pf_useful = lhs.pf_useful - rhs.pf_useful;
  result.pf_useless = lhs.pf_useless - rhs.pf_useless;
  result.pf_fill = lhs.pf_fill - rhs.pf_fill;

  result.hits = lhs.hits - rhs.hits;
  result.misses = lhs.misses - rhs.misses;

  for (std::size_t i = 0; i < result.per_context_load_hits.size(); ++i) {
    result.per_context_load_hits[i] = lhs.per_context_load_hits[i] - rhs.per_context_load_hits[i];
    result.per_context_load_misses[i] = lhs.per_context_load_misses[i] - rhs.per_context_load_misses[i];
  }

  result.total_miss_latency_cycles = lhs.total_miss_latency_cycles - rhs.total_miss_latency_cycles;
  return result;
}
