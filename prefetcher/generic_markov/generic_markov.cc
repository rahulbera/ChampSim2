#include "generic_markov.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>

#include "cache.h"

namespace
{
// No denominator gives a quiet NaN, not a zero: the document keeps one schema
// for every run, and `nan` is a real TOML float. Mirrors toml_ratio().
double ratio(uint64_t num, uint64_t denom)
{
  return (denom > 0) ? static_cast<double>(num) / static_cast<double>(denom) : std::numeric_limits<double>::quiet_NaN();
}

// module_stat_value is int64_t because a TOML integer is signed 64-bit. Every
// counter here is bounded by the access count, so the cast cannot lose.
constexpr int64_t as_toml_integer(uint64_t value) { return static_cast<int64_t>(value); }
} // namespace

std::size_t generic_markov::sequence_hash::operator()(const sequence& seq) const noexcept
{
  // FNV-1a: stable across platforms and compilers, which std::hash is not
  // required to be.
  constexpr uint64_t offset_basis{1469598103934665603ULL};
  constexpr uint64_t prime{1099511628211ULL};

  uint64_t hash{offset_basis};
  for (const auto value : seq) {
    for (unsigned shift = 0; shift < 64U; shift += 8U) {
      hash ^= (value >> shift) & 0xffULL;
      hash *= prime;
    }
  }
  return static_cast<std::size_t>(hash);
}

const generic_markov::successors* generic_markov::lookup(const sequence& seq) const
{
  const auto found = table.find(seq);
  return (found == std::end(table)) ? nullptr : &found->second;
}

void generic_markov::configure(const champsim::runtime_config& cfg, std::string_view prefix)
{
  const std::string base{prefix};

  // positive_value: a zero length or degree is a degenerate model whose
  // numbers look plausible, so it must fail loudly.
  history_length = cfg.positive_value<std::size_t>(base + ".history_length", 1);
  predict_degree = cfg.positive_value<std::size_t>(base + ".predict_degree", 4);
  issue_prefetch = cfg.value<bool>(base + ".issue_prefetch", false);

  // Validated by name: silently defaulting on a typo would produce a
  // plausible result for the wrong experiment.
  const auto stream_name = cfg.value<std::string>(base + ".train_on", "all");
  if (stream_name == "all") {
    train_on = stream::all;
  } else if (stream_name == "miss") {
    train_on = stream::miss;
  } else {
    // runtime_error, NOT logic_error: main.cc catches only runtime_error, so
    // an invalid_argument would abort the process instead of reporting.
    throw std::runtime_error{base + ".train_on must be \"all\" or \"miss\", not \"" + stream_name + "\""};
  }

  history.clear();
  history.reserve(history_length + 1); // step 2 pushes before it erases
  ranked.reserve(std::max(predict_degree, std::size_t{2}));
}

uint32_t generic_markov::prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                                  uint32_t metadata_in)
{
  // Filtered first, so the access does not enter the window either: the
  // sequence is over misses, not over lookups-with-gaps.
  if (train_on == stream::miss && cache_hit != 0) {
    return metadata_in;
  }

  const auto block = champsim::block_number{addr}.to<uint64_t>();

  // Step 0: grade the PREVIOUS access's prediction, which was a statement
  // about this address. Grading it against the access that produced it would
  // report a near-perfect predictor.
  if (has_pending) {
    ++scored_predictions;
    if (pending_tied) {
      ++top1_ties;
    }
    if (!std::empty(pending) && pending.front() == block) {
      ++top1_correct;
    }
    if (std::find(std::begin(pending), std::end(pending), block) != std::end(pending)) {
      ++topk_correct;
    }
    has_pending = false;
  }

  // Step 1: train the pre-shift window -> this address.
  if (std::size(history) == history_length) {
    auto& entry = table[history];
    const auto found = std::find_if(std::begin(entry.candidates), std::end(entry.candidates), [block](const candidate& cand) { return cand.addr == block; });

    // The ceiling, for free: this key is the one just predicted from, and the
    // search above runs BEFORE the insert. Keep it there or it counts every
    // access. A populated key implies a prediction was pending.
    if (found != std::end(entry.candidates)) {
      ++topall_correct;
    }

    ++train_clock;
    if (found == std::end(entry.candidates)) {
      entry.candidates.push_back({block, 1, train_clock});
    } else {
      ++found->count;
      found->last_seen = train_clock; // else recency means first sighting
    }
    ++entry.total_count;
    ++train_events;
  }

  // Step 2: shift this address into the window.
  history.push_back(block);
  if (std::size(history) > history_length) {
    history.erase(std::begin(history));
  }

  // Step 3: predict from the post-shift window, which is exactly the key the
  // NEXT access will train -- what makes step 0 exact.
  if (std::size(history) == history_length) {
    ++predict_attempts;

    const auto found = table.find(history);
    if (found != std::end(table)) {
      const auto& entry = found->second;
      ++predict_hits;
      sum_cardinality += static_cast<uint64_t>(std::size(entry.candidates));
      sum_key_occurrences += entry.total_count;

      // Count descending, ties on recency -- the paper's LRU ordering, which
      // carries signal where counts are silent. Still deterministic: last_seen
      // is a pure function of the stream.
      //
      // Two are always ranked when two exist, so the tie below is detectable
      // without a second pass.
      const auto keep = std::min(predict_degree, std::size(entry.candidates));
      const auto rank_count = std::min(std::max(predict_degree, std::size_t{2}), std::size(entry.candidates));
      ranked.resize(rank_count);
      std::partial_sort_copy(std::begin(entry.candidates), std::end(entry.candidates), std::begin(ranked), std::end(ranked),
                             [](const candidate& lhs, const candidate& rhs) { return (lhs.count != rhs.count) ? (lhs.count > rhs.count) : (lhs.last_seen > rhs.last_seen); });

      // Did recency rather than count decide rank 1? At large H nearly every
      // key is an all-way tie, so this separates "frequency knew" from
      // "recency knew" and must be read next to top1_rate.
      pending_tied = (rank_count >= 2) && (ranked.at(0).count == ranked.at(1).count);

      predicted_addresses += static_cast<uint64_t>(keep);

      pending.clear();
      std::transform(std::begin(ranked), std::begin(ranked) + static_cast<std::ptrdiff_t>(keep), std::back_inserter(pending),
                     [](const candidate& cand) { return cand.addr; });
      has_pending = true;

      if (issue_prefetch) {
        for (const auto pf_block : pending) {
          prefetch_line(champsim::address{champsim::block_number{pf_block}}, true, metadata_in);
        }
      }
    }
  }

  return metadata_in;
}

uint32_t generic_markov::prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr,
                                               uint32_t metadata_in)
{
  return metadata_in;
}

void generic_markov::prefetcher_begin_phase()
{
  // Only the counters reset. The table, window and pending prediction do not:
  // the address stream does not restart at a phase boundary.
  train_events = 0;
  predict_attempts = 0;
  predict_hits = 0;
  sum_cardinality = 0;
  sum_key_occurrences = 0;
  scored_predictions = 0;
  top1_correct = 0;
  top1_ties = 0;
  topk_correct = 0;
  topall_correct = 0;
  predicted_addresses = 0;
  // train_clock is deliberately absent: see its declaration.
}

void generic_markov::prefetcher_end_phase()
{
  // One pass, O(distinct_keys), and the last moment anything can reach the
  // document -- main copies roi_stats into phase_stats as this returns.
  //
  // These cover a map trained over warmup AND the ROI; the per-lookup counters
  // cover the ROI alone. Do not divide one by the other.
  std::map<uint64_t, uint64_t> cardinality_histogram{};
  uint64_t sum_cardinality_per_key{0};
  uint64_t sum_cardinality_by_occurrence{0};
  uint64_t total_key_occurrences{0};
  uint64_t max_cardinality{0};
  uint64_t keys_seen_once{0};

  for (const auto& [seq, entry] : table) {
    const auto cardinality = static_cast<uint64_t>(std::size(entry.candidates));
    ++cardinality_histogram[cardinality];
    sum_cardinality_per_key += cardinality;
    max_cardinality = std::max(max_cardinality, cardinality);
    if (entry.total_count == 1) {
      ++keys_seen_once;
    }

    // Weighted by training count. Paired with sum_cardinality_per_key this
    // isolates the WEIGHTING -- same map, same moment. (per_lookup samples
    // each key when it was met, so it also carries a maturity effect.)
    sum_cardinality_by_occurrence += cardinality * entry.total_count;
    total_key_occurrences += entry.total_count;
  }

  const auto distinct_keys = static_cast<uint64_t>(table.size());

  // Nearest rank, exact: the histogram covers every key rather than bucketing.
  const auto percentile = [&](double fraction) -> uint64_t {
    if (distinct_keys == 0) {
      return 0;
    }
    const auto target = static_cast<uint64_t>(std::ceil(fraction * static_cast<double>(distinct_keys)));
    uint64_t cumulative{0};
    for (const auto& [cardinality, keys] : cardinality_histogram) {
      cumulative += keys;
      if (cumulative >= target) {
        return cardinality;
      }
    }
    return max_cardinality;
  };

  // Emitted in this order: module_stat_block keeps insertion order, so each
  // ratio sits with the operands it came from and the groups stay together.
  auto& out = intern_->roi_stats.module_block("generic_markov");

  // --- trained on (map: warmup + ROI; train_events: ROI alone) ------------
  out.set("train_events", as_toml_integer(train_events));
  out.set("distinct_keys", as_toml_integer(distinct_keys));
  out.set("total_key_occurrences", as_toml_integer(total_key_occurrences));
  out.set("keys_seen_once", as_toml_integer(keys_seen_once));

  // --- does a sequence recur? the COVERAGE ceiling ------------------------
  out.set("predict_attempts", as_toml_integer(predict_attempts));
  out.set("predict_hits", as_toml_integer(predict_hits));
  out.set("lookup_hit_rate", ratio(predict_hits, predict_attempts));
  out.set("sum_key_occurrences", as_toml_integer(sum_key_occurrences));
  out.set("mean_key_occurrences", ratio(sum_key_occurrences, predict_hits));

  // --- right-side width: the ACCURACY ceiling -----------------------------
  // per_key vs weighted differ by WEIGHTING ALONE; per_lookup also carries a
  // maturity effect.
  out.set("sum_cardinality_per_key", as_toml_integer(sum_cardinality_per_key));
  out.set("mean_cardinality_per_key", ratio(sum_cardinality_per_key, distinct_keys));
  out.set("sum_cardinality_by_occurrence", as_toml_integer(sum_cardinality_by_occurrence));
  out.set("mean_cardinality_weighted", ratio(sum_cardinality_by_occurrence, total_key_occurrences));
  out.set("sum_cardinality", as_toml_integer(sum_cardinality));
  out.set("mean_cardinality_per_lookup", ratio(sum_cardinality, predict_hits));
  out.set("p50_cardinality", as_toml_integer(percentile(0.50)));
  out.set("p75_cardinality", as_toml_integer(percentile(0.75)));
  out.set("p90_cardinality", as_toml_integer(percentile(0.90)));
  out.set("max_cardinality", as_toml_integer(max_cardinality));

  // --- accuracy, measured rather than inferred ----------------------------
  // top1_rate is only interpretable next to top1_tie_rate, hence adjacent.
  out.set("scored_predictions", as_toml_integer(scored_predictions));
  out.set("top1_correct", as_toml_integer(top1_correct));
  out.set("top1_rate", ratio(top1_correct, scored_predictions));
  out.set("top1_ties", as_toml_integer(top1_ties));
  out.set("top1_tie_rate", ratio(top1_ties, scored_predictions));
  // The denominator for accuracy AT DEGREE k: one prediction emits up to k
  // addresses and at most one can be right.
  out.set("predicted_addresses", as_toml_integer(predicted_addresses));
  out.set("topk_correct", as_toml_integer(topk_correct));
  out.set("topk_rate", ratio(topk_correct, scored_predictions));
  out.set("topk_accuracy_per_address", ratio(topk_correct, predicted_addresses));

  // Successor present at ANY rank: independent of ordering and degree.
  // Accuracy at full degree is topall_correct / sum_cardinality.
  out.set("topall_correct", as_toml_integer(topall_correct));
  out.set("topall_rate", ratio(topall_correct, scored_predictions));
}
