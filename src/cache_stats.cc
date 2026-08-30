#include "cache_stats.h"

#include <algorithm>
#include <iterator>

void cache_stats::module_stat_block::set(std::string_view key, module_stat_value value)
{
  const auto found = std::find_if(std::begin(entries), std::end(entries), [key](const auto& e) { return e.first == key; });
  if (found == std::end(entries)) {
    entries.emplace_back(std::string{key}, value);
  } else {
    found->second = value;
  }
}

const cache_stats::module_stat_value* cache_stats::module_stat_block::find(std::string_view key) const
{
  const auto found = std::find_if(std::begin(entries), std::end(entries), [key](const auto& e) { return e.first == key; });
  return (found == std::end(entries)) ? nullptr : &found->second;
}

cache_stats::module_stat_block& cache_stats::module_block(std::string_view module)
{
  const auto found = std::find_if(std::begin(module_stats), std::end(module_stats), [module](const auto& b) { return b.module == module; });
  if (found != std::end(module_stats)) {
    return *found;
  }
  module_stats.push_back(module_stat_block{std::string{module}, {}});
  return module_stats.back();
}

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

  result.total_miss_latency_cycles = lhs.total_miss_latency_cycles - rhs.total_miss_latency_cycles;

  // module_stats is deliberately NOT differenced. A module publishes whatever
  // it wants under names only it understands -- a percentile or a hit rate has
  // no meaningful difference, and a module that wanted one would have to
  // compute it itself anyway. The result keeps the empty map.
  //
  // Nothing calls this operator: it has no use anywhere in src, inc or test.
  return result;
}
