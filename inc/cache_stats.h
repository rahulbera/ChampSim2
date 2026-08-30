#ifndef CACHE_STATS_H
#define CACHE_STATS_H

#include <algorithm>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include "channel.h"
#include "event_counter.h"

struct cache_stats {
  std::string name;

  // A statistic published by a module.
  //
  // int64_t, NOT uint64_t: a TOML integer IS a signed 64-bit value, and toml++
  // rejects a decimal literal above INT64_MAX with "not representable in 64
  // bits" -- which kills the load of the WHOLE file, not just that key. Since
  // a run's own statistics document is a supported configuration source, an
  // unsigned counter above INT64_MAX would emit a document that cannot
  // reproduce its own run. Using the format's real type makes that
  // unrepresentable rather than merely unlikely.
  using module_stat_value = std::variant<int64_t, double>;

  // One module's statistics, IN THE ORDER THE MODULE PUBLISHED THEM.
  //
  // A vector, not a map. An ordered associative container sorts by key, which
  // silently discards the grouping the module wrote -- a ratio ends up pages
  // away from the operands it was computed from, and the reader has to
  // reassemble the reasoning. Insertion order is the module's editorial
  // decision about how its numbers should be read, so it is preserved.
  struct module_stat_block {
    std::string module{}; // the module's own name, e.g. "generic_markov"
    std::vector<std::pair<std::string, module_stat_value>> entries{};

    // Overwrite in place when the key is already present, else append.
    // Idempotent because end_phase runs once per FINISHING CPU, and
    // order-preserving because a re-publish must not move a key.
    void set(std::string_view key, module_stat_value value);

    [[nodiscard]] const module_stat_value* find(std::string_view key) const;
  };

  // One block per publishing module, so two modules at one cache get two named
  // tables rather than one merged bag. The module owns the name, which is why
  // it is stored rather than derived: the printer must not have to guess which
  // of several modules a statistic came from.
  //
  // A module fills its block from prefetcher_begin_phase/prefetcher_end_phase,
  // NOT from prefetcher_final_stats(). champsim::main copies roi_stats into
  // phase_stats when the phase ends, and final_stats() runs after that copy --
  // anything written there would never reach the statistics document.
  std::vector<module_stat_block> module_stats{};

  // This module's block, created on first use.
  module_stat_block& module_block(std::string_view module);

  // prefetch stats
  uint64_t pf_requested = 0;
  uint64_t pf_issued = 0;
  uint64_t pf_useful = 0;
  // A demand that merged into an in-flight prefetch: the prefetch saved part of
  // the miss, never all of it, so it is not counted useful.
  uint64_t pf_late = 0;
  uint64_t pf_useless = 0;
  uint64_t pf_fill = 0;

  champsim::stats::event_counter<std::pair<access_type, std::remove_cv_t<decltype(NUM_CPUS)>>> hits = {};
  champsim::stats::event_counter<std::pair<access_type, std::remove_cv_t<decltype(NUM_CPUS)>>> misses = {};
  champsim::stats::event_counter<std::pair<access_type, std::remove_cv_t<decltype(NUM_CPUS)>>> miss_merge = {};
  champsim::stats::event_counter<std::pair<access_type, std::remove_cv_t<decltype(NUM_CPUS)>>> fill = {};

  long total_miss_latency_cycles{};
};

cache_stats operator-(cache_stats lhs, cache_stats rhs);

#endif
