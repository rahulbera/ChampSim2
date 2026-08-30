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
// A ratio with no denominator is a quiet NaN, not a zero and not an omitted
// key: the statistics document keeps an identical schema for every run, and
// `nan` is a genuine TOML float literal. This mirrors toml_ratio().
double ratio(uint64_t num, uint64_t denom)
{
  return (denom > 0) ? static_cast<double>(num) / static_cast<double>(denom) : std::numeric_limits<double>::quiet_NaN();
}

// cache_stats::module_stat_value holds int64_t because a TOML integer IS
// signed 64-bit. Every counter below is bounded by the access count -- a few
// million -- so the conversion cannot lose anything; making it explicit is
// what keeps an unrepresentable value from ever reaching the document.
constexpr int64_t as_toml_integer(uint64_t value) { return static_cast<int64_t>(value); }
} // namespace

std::size_t generic_markov::sequence_hash::operator()(const sequence& seq) const noexcept
{
  // FNV-1a. Stable across runs, platforms and compilers, which std::hash is
  // not required to be -- and this hash decides the bucket a measurement lands
  // in, so a run must be reproducible.
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

  // positive_value rather than value: a zero history length would make every
  // key the empty sequence and a zero degree would predict nothing, and both
  // are sweep points that must fail loudly rather than report the numbers of a
  // degenerate model.
  history_length = cfg.positive_value<std::size_t>(base + ".history_length", 1);
  predict_degree = cfg.positive_value<std::size_t>(base + ".predict_degree", 4);
  issue_prefetch = cfg.value<bool>(base + ".issue_prefetch", false);

  // Consulted unconditionally, then validated -- a typo must be rejected by
  // name rather than silently selecting the default stream, which would
  // produce a plausible-looking result for the wrong experiment.
  const auto stream_name = cfg.value<std::string>(base + ".train_on", "all");
  if (stream_name == "all") {
    train_on = stream::all;
  } else if (stream_name == "miss") {
    train_on = stream::miss;
  } else {
    // std::runtime_error, NOT a logic_error: main.cc wraps environment
    // construction in `catch (const std::runtime_error&)`, and
    // std::invalid_argument derives from logic_error, so it would escape and
    // abort the process instead of printing a diagnostic and returning 1. On a
    // sweep that is the difference between a job that says what is wrong and a
    // job that dumps core. runtime_config::positive_value and the module
    // registry both throw runtime_error for the same reason.
    throw std::runtime_error{base + ".train_on must be \"all\" or \"miss\", not \"" + stream_name + "\""};
  }

  history.clear();
  // H + 1, not H: step 2 pushes before it erases, so the window transiently
  // holds one more than the history length.
  history.reserve(history_length + 1);
  // At least two: step 3 ranks two whenever two exist, to detect a rank-1 tie.
  ranked.reserve(std::max(predict_degree, std::size_t{2}));
}

uint32_t generic_markov::prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                                  uint32_t metadata_in)
{
  // Stream filter first, so a filtered access perturbs nothing at all: it does
  // not enter the window, so the sequence is over misses rather than over
  // lookups-with-gaps.
  if (train_on == stream::miss && cache_hit != 0) {
    return metadata_in;
  }

  const auto block = champsim::block_number{addr}.to<uint64_t>();

  // Step 0: grade the prediction made on the PREVIOUS access.
  //
  // That prediction was a statement about THIS address, so now is the first
  // moment it can be checked -- and the last, because step 3 overwrites it.
  // Grading it against the current access instead (the obvious mistake) would
  // compare a prediction with the address that produced it and report a
  // near-perfect predictor.
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
    if (found == std::end(entry.candidates)) {
      entry.candidates.push_back({block, 1});
    } else {
      ++found->count;
    }
    ++entry.total_count;
    ++train_events;
  }

  // Step 2: shift this address into the window.
  history.push_back(block);
  if (std::size(history) > history_length) {
    history.erase(std::begin(history));
  }

  // Step 3: predict from the post-shift window. Note the window is now exactly
  // the key that the NEXT access will train, which is what makes step 0 exact.
  if (std::size(history) == history_length) {
    ++predict_attempts;

    const auto found = table.find(history);
    if (found != std::end(table)) {
      const auto& entry = found->second;
      ++predict_hits;
      sum_cardinality += static_cast<uint64_t>(std::size(entry.candidates));
      sum_key_occurrences += entry.total_count;

      // Rank by count, descending. Ties break on the address so that a rerun
      // of the same trace produces the same top-k: unordered_map iteration
      // order and push_back order are both incidental, and a tie broken by
      // either would make top1_correct depend on them.
      //
      // At least two are ranked whenever two exist, even when predict_degree
      // is 1, so that the rank-1 tie below can always be detected without a
      // second pass over the candidates.
      const auto keep = std::min(predict_degree, std::size(entry.candidates));
      const auto rank_count = std::min(std::max(predict_degree, std::size_t{2}), std::size(entry.candidates));
      ranked.resize(rank_count);
      std::partial_sort_copy(std::begin(entry.candidates), std::end(entry.candidates), std::begin(ranked), std::end(ranked),
                             [](const candidate& lhs, const candidate& rhs) { return (lhs.count != rhs.count) ? (lhs.count > rhs.count) : (lhs.addr < rhs.addr); });

      // Was rank 1 decided by the address tie-break rather than by the counts?
      //
      // This matters for reading top1_rate. At large H nearly every
      // multi-successor key is an all-way tie -- every successor seen exactly
      // once -- so rank 1 is chosen by which block address is numerically
      // smallest, a property of the address layout and not of the correlation.
      // On those predictions top1_rate measures the tie-break, not the model,
      // so the fraction that were tied has to be published alongside it.
      pending_tied = (rank_count >= 2) && (ranked.at(0).count == ranked.at(1).count);

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
  // Counters describe one phase. The table, the window and the pending
  // prediction deliberately do NOT reset: the address stream does not restart
  // at a phase boundary, and clearing them would inject a cold start into the
  // first accesses of the region of interest that no real prefetcher would
  // suffer.
  train_events = 0;
  predict_attempts = 0;
  predict_hits = 0;
  sum_cardinality = 0;
  sum_key_occurrences = 0;
  scored_predictions = 0;
  top1_correct = 0;
  top1_ties = 0;
  topk_correct = 0;
}

void generic_markov::prefetcher_end_phase()
{
  // One pass over the map, O(distinct_keys). This is the only place the
  // per-key distributions can be computed, and the last moment anything can
  // reach the statistics document: champsim::main copies roi_stats into
  // phase_stats as soon as this returns.
  //
  // NOTE the asymmetry, and do not join these to the per-lookup counters as
  // though they shared a denominator: the map was trained over warmup AND the
  // region of interest, while every counter above covers the region of
  // interest alone.
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

    // The same cardinality, weighted by how often the key was trained. Paired
    // with sum_cardinality_per_key this isolates the WEIGHTING: both are read
    // off the same fully-trained map, so the only difference between their
    // means is whether a key counts once or once per occurrence.
    //
    // mean_cardinality_per_lookup does not have that property. It samples each
    // key at the moment it was met, so a key met early and grown later
    // contributes its young, smaller cardinality -- mixing a maturity effect
    // into what should be a pure weighting comparison, and making the result
    // move with ROI length as well as with H.
    sum_cardinality_by_occurrence += cardinality * entry.total_count;
    total_key_occurrences += entry.total_count;
  }

  const auto distinct_keys = static_cast<uint64_t>(table.size());

  // Nearest rank: the smallest cardinality whose cumulative key count reaches
  // ceil(fraction * distinct_keys). Exact, because the histogram is over every
  // key rather than bucketed.
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

  // Published into this module's OWN named block, so two modules at one cache
  // get two named tables rather than one merged bag.
  //
  // The order below is the order it is emitted: module_stat_block keeps
  // insertion order rather than sorting. That is deliberate -- the grouping IS
  // the explanation. Each ratio sits immediately after the operands it was
  // computed from, and statistics that answer the same question stay together,
  // so the table can be read top to bottom as an argument rather than
  // reassembled from an alphabetical list.
  auto& out = intern_->roi_stats.module_block("generic_markov");

  // --- what the model was trained on -------------------------------------
  // distinct_keys and the two totals describe the map after warmup AND the
  // region of interest; train_events counts the region of interest alone. Do
  // not divide one by the other.
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

  // --- how wide is the right side? the ACCURACY ceiling -------------------
  // per_key and weighted are read off the same fully-trained map and differ by
  // WEIGHTING ALONE, which is the comparison that means something. per_lookup
  // samples each key when it was met, so it also carries a maturity effect.
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
  // top1_rate is only interpretable next to top1_tie_rate, so they are
  // adjacent: where ties are common, rank 1 was chosen by the address
  // tie-break and the rate above it measures address layout, not the model.
  out.set("scored_predictions", as_toml_integer(scored_predictions));
  out.set("top1_correct", as_toml_integer(top1_correct));
  out.set("top1_rate", ratio(top1_correct, scored_predictions));
  out.set("top1_ties", as_toml_integer(top1_ties));
  out.set("top1_tie_rate", ratio(top1_ties, scored_predictions));
  out.set("topk_correct", as_toml_integer(topk_correct));
  out.set("topk_rate", ratio(topk_correct, scored_predictions));
}
