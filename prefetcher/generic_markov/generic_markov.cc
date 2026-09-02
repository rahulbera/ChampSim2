#include "generic_markov.h"

#include <algorithm>
#include <array>
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
  graded_top1_hit = false;
  if (has_pending) {
    ++scored_predictions;
    if (pending_tied) {
      ++top1_ties;
    }
    graded_top1_hit = (!std::empty(pending) && pending.front() == block);
    if (graded_top1_hit) {
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
      ++entry.topall_correct;
    }
    if (graded_top1_hit) {
      ++entry.top1_correct;
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
      std::partial_sort_copy(
          std::begin(entry.candidates), std::end(entry.candidates), std::begin(ranked), std::end(ranked),
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
  std::map<uint64_t, uint64_t> occurrence_histogram{};
  uint64_t sum_cardinality_per_key{0};
  uint64_t sum_cardinality_by_occurrence{0};
  uint64_t total_key_occurrences{0};
  uint64_t max_cardinality{0};
  uint64_t keys_seen_once{0};

  for (const auto& [seq, entry] : table) {
    const auto cardinality = static_cast<uint64_t>(std::size(entry.candidates));
    ++cardinality_histogram[cardinality];
    ++occurrence_histogram[entry.total_count];
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

  // Repeat mass: each key's occurrences past its first sighting. Singletons
  // contribute nothing, which is the point -- they can never predict.
  const auto repeat_occurrences = total_key_occurrences - distinct_keys;

  // Keys whose cardinality falls in [lo, hi]. The histogram is already built
  // for the percentiles above, so this is a walk, not a second pass.
  const auto cardinality_band = [&](uint64_t lo, uint64_t hi) -> uint64_t {
    uint64_t keys{0};
    for (auto it = cardinality_histogram.lower_bound(lo); it != std::end(cardinality_histogram) && it->first <= hi; ++it) {
      keys += it->second;
    }
    return keys;
  };

  // Fewest keys whose occurrences reach `fraction` of the mass, most frequent
  // first. Walks the histogram downward and may take part of a bucket, so the
  // answer is exact rather than bucket-rounded.
  //
  // `cut_count` / `from_cut` name the partial bucket, so a second walk can
  // rebuild the SAME key set: every key above cut_count, plus from_cut of the
  // keys at it. Which of the tied keys is arbitrary but deterministic (map
  // order), and the count is what the threshold defines either way.
  struct occupancy_cut {
    uint64_t keys{};
    uint64_t cut_count{};
    uint64_t from_cut{};
  };

  const auto occupancy = [&](double fraction, bool repeat_only) -> occupancy_cut {
    const auto mass = repeat_only ? repeat_occurrences : total_key_occurrences;
    if (mass == 0) {
      return {};
    }
    const auto target = static_cast<uint64_t>(std::ceil(fraction * static_cast<double>(mass)));
    uint64_t taken{0};
    uint64_t cumulative{0};
    for (auto it = std::rbegin(occurrence_histogram); it != std::rend(occurrence_histogram); ++it) {
      const auto occurrences = it->first;
      const auto keys = it->second;
      const auto per_key = repeat_only ? (occurrences - 1) : occurrences;
      if (per_key == 0) {
        continue;
      }
      const auto need = target - cumulative;
      if (per_key * keys >= need) {
        const auto from_cut = (need + per_key - 1) / per_key;
        return {taken + from_cut, occurrences, from_cut};
      }
      cumulative += per_key * keys;
      taken += keys;
    }
    return {taken, 0, 0};
  };

  // A hardware budget holds N keys, not a share of the mass. Same cut shape as
  // occupancy, so the second walk treats both identically. Singletons are
  // eligible here -- unlike an occupancy set, which cannot reach them -- because
  // a real table holds whatever it holds.
  const auto top_n = [&](uint64_t n) -> occupancy_cut {
    if (n == 0 || distinct_keys == 0) {
      return {};
    }
    uint64_t taken{0};
    for (auto it = std::rbegin(occurrence_histogram); it != std::rend(occurrence_histogram); ++it) {
      if (taken + it->second >= n) {
        return {n, it->first, n - taken};
      }
      taken += it->second;
    }
    return {taken, 0, 0}; // the whole table is smaller than the budget
  };

  const auto o50 = occupancy(0.50, true);
  const auto o80 = occupancy(0.80, true);
  const auto o90 = occupancy(0.90, true);
  const auto o80_all = occupancy(0.80, false);
  const auto top1k = top_n(1000);
  const auto top10k = top_n(10000);
  const auto top50k = top_n(50000);

  // Second walk: the credit earned by each occupancy set. A key counts if it is
  // above the cut, or is one of the first `from_cut` keys sitting exactly on it.
  // Same bands as the whole-table distribution, so the two are read side by
  // side: 1, 2, 3-4, 5-8, 9-16, 17-32, 33-64, 65+.
  constexpr std::size_t band_count{8};
  const auto band_index = [](uint64_t cardinality) -> std::size_t {
    if (cardinality <= 2) {
      return static_cast<std::size_t>(cardinality) - 1;
    }
    std::size_t index{2};
    for (uint64_t bound = 4; bound <= 64 && cardinality > bound; bound *= 2) {
      ++index;
    }
    return index;
  };

  // Bits a SIGNED delta needs: 4, 8, 16, 24, 32, then wider. Signed because a
  // successor is as often below its key as above it.
  constexpr std::size_t delta_bucket_count{6};
  const auto delta_bucket = [](int64_t delta) -> std::size_t {
    std::size_t bucket{0};
    for (unsigned bits : {4U, 8U, 16U, 24U, 32U}) {
      const int64_t limit = int64_t{1} << (bits - 1);
      if (delta >= -limit && delta < limit) {
        return bucket;
      }
      ++bucket;
    }
    return delta_bucket_count - 1;
  };

  struct coverage {
    uint64_t top1{};
    uint64_t topall{};
    uint64_t budget{};
    uint64_t sum_cardinality{};
    std::array<uint64_t, band_count> bands{};
    std::array<uint64_t, delta_bucket_count> delta_bits{};
  };
  // Slot 6 is the whole table: a cut at count 0 includes every key, so the
  // same loop produces the unfiltered baseline without a second pass.
  std::array<coverage, 7> covered{};
  const std::array<occupancy_cut, 7> cuts{o50, o80, o90, top1k, top10k, top50k, occupancy_cut{distinct_keys, 0, 0}};
  for (std::size_t i = 0; i < std::size(cuts); ++i) {
    covered.at(i).budget = cuts.at(i).from_cut;
  }
  for (const auto& [seq, entry] : table) {
    for (std::size_t i = 0; i < std::size(cuts); ++i) {
      const auto& cut = cuts.at(i);
      if (cut.keys == 0) {
        continue;
      }
      bool included = entry.total_count > cut.cut_count;
      if (!included && entry.total_count == cut.cut_count && covered.at(i).budget > 0) {
        --covered.at(i).budget;
        included = true;
      }
      if (included) {
        covered.at(i).top1 += entry.top1_correct;
        covered.at(i).topall += entry.topall_correct;
        const auto cardinality = static_cast<uint64_t>(std::size(entry.candidates));
        covered.at(i).sum_cardinality += cardinality;
        ++covered.at(i).bands.at(band_index(cardinality));

        // Per STORED candidate: each is one table slot that would hold a delta.
        // Measured from the key's last address, which is the whole key at H=1
        // and the most recent one above it.
        const auto anchor = static_cast<int64_t>(seq.back());
        for (const auto& cand : entry.candidates) {
          ++covered.at(i).delta_bits.at(delta_bucket(static_cast<int64_t>(cand.addr) - anchor));
        }
      }
    }
  }

  // Emitted in this order: module_stat_block keeps insertion order, so each
  // ratio sits with the operands it came from and the groups stay together.
  auto& out = intern_->roi_stats.module_block("generic_markov");

  // --- trained on (map: warmup + ROI; train_events: ROI alone) ------------
  out.set("train_events", as_toml_integer(train_events));
  out.set("distinct_keys", as_toml_integer(distinct_keys));
  out.set("total_key_occurrences", as_toml_integer(total_key_occurrences));
  out.set("keys_seen_once", as_toml_integer(keys_seen_once));

  // --- how concentrated is the reoccurrence -------------------------------
  // oNN_keys: fewest keys holding NN% of the repeat mass. The _by_total_occ
  // pair uses every occurrence instead, where singletons dilute the answer.
  out.set("repeat_occurrences", as_toml_integer(repeat_occurrences));
  out.set("o50_keys", as_toml_integer(o50.keys));
  out.set("o50_key_frac", ratio(o50.keys, distinct_keys));
  out.set("o80_keys", as_toml_integer(o80.keys));
  out.set("o80_key_frac", ratio(o80.keys, distinct_keys));
  out.set("o90_keys", as_toml_integer(o90.keys));
  out.set("o90_key_frac", ratio(o90.keys, distinct_keys));
  out.set("o95_keys", as_toml_integer(occupancy(0.95, true).keys));
  out.set("o95_key_frac", ratio(occupancy(0.95, true).keys, distinct_keys));

  // Credit earned by each occupancy set: what coverage survives if only these
  // keys are kept. Denominator is predict_attempts, so it is directly
  // comparable to the whole-table top1_correct/predict_attempts.
  out.set("o50_top1_correct", as_toml_integer(covered.at(0).top1));
  out.set("o50_top1_coverage", ratio(covered.at(0).top1, predict_attempts));
  out.set("o50_topall_correct", as_toml_integer(covered.at(0).topall));
  out.set("o50_topall_coverage", ratio(covered.at(0).topall, predict_attempts));
  out.set("o80_top1_correct", as_toml_integer(covered.at(1).top1));
  out.set("o80_top1_coverage", ratio(covered.at(1).top1, predict_attempts));
  out.set("o80_topall_correct", as_toml_integer(covered.at(1).topall));
  out.set("o80_topall_coverage", ratio(covered.at(1).topall, predict_attempts));
  out.set("o90_top1_correct", as_toml_integer(covered.at(2).top1));
  out.set("o90_top1_coverage", ratio(covered.at(2).top1, predict_attempts));
  out.set("o90_topall_correct", as_toml_integer(covered.at(2).topall));
  out.set("o90_topall_coverage", ratio(covered.at(2).topall, predict_attempts));

  // Fan-out of the keys that carry the reoccurrence. Same bands as the
  // whole-table distribution above; each set's bands sum to its own key count.
  out.set("o50_mean_cardinality", ratio(covered.at(0).sum_cardinality, o50.keys));
  out.set("o50_keys_w_cardinality_1_1", as_toml_integer(covered.at(0).bands.at(0)));
  out.set("o50_keys_w_cardinality_2_2", as_toml_integer(covered.at(0).bands.at(1)));
  out.set("o50_keys_w_cardinality_3_4", as_toml_integer(covered.at(0).bands.at(2)));
  out.set("o50_keys_w_cardinality_5_8", as_toml_integer(covered.at(0).bands.at(3)));
  out.set("o50_keys_w_cardinality_9_16", as_toml_integer(covered.at(0).bands.at(4)));
  out.set("o50_keys_w_cardinality_17_32", as_toml_integer(covered.at(0).bands.at(5)));
  out.set("o50_keys_w_cardinality_33_64", as_toml_integer(covered.at(0).bands.at(6)));
  out.set("o50_keys_w_cardinality_65_plus", as_toml_integer(covered.at(0).bands.at(7)));

  out.set("o80_mean_cardinality", ratio(covered.at(1).sum_cardinality, o80.keys));
  out.set("o80_keys_w_cardinality_1_1", as_toml_integer(covered.at(1).bands.at(0)));
  out.set("o80_keys_w_cardinality_2_2", as_toml_integer(covered.at(1).bands.at(1)));
  out.set("o80_keys_w_cardinality_3_4", as_toml_integer(covered.at(1).bands.at(2)));
  out.set("o80_keys_w_cardinality_5_8", as_toml_integer(covered.at(1).bands.at(3)));
  out.set("o80_keys_w_cardinality_9_16", as_toml_integer(covered.at(1).bands.at(4)));
  out.set("o80_keys_w_cardinality_17_32", as_toml_integer(covered.at(1).bands.at(5)));
  out.set("o80_keys_w_cardinality_33_64", as_toml_integer(covered.at(1).bands.at(6)));
  out.set("o80_keys_w_cardinality_65_plus", as_toml_integer(covered.at(1).bands.at(7)));

  out.set("o90_mean_cardinality", ratio(covered.at(2).sum_cardinality, o90.keys));
  out.set("o90_keys_w_cardinality_1_1", as_toml_integer(covered.at(2).bands.at(0)));
  out.set("o90_keys_w_cardinality_2_2", as_toml_integer(covered.at(2).bands.at(1)));
  out.set("o90_keys_w_cardinality_3_4", as_toml_integer(covered.at(2).bands.at(2)));
  out.set("o90_keys_w_cardinality_5_8", as_toml_integer(covered.at(2).bands.at(3)));
  out.set("o90_keys_w_cardinality_9_16", as_toml_integer(covered.at(2).bands.at(4)));
  out.set("o90_keys_w_cardinality_17_32", as_toml_integer(covered.at(2).bands.at(5)));
  out.set("o90_keys_w_cardinality_33_64", as_toml_integer(covered.at(2).bands.at(6)));
  out.set("o90_keys_w_cardinality_65_plus", as_toml_integer(covered.at(2).bands.at(7)));

  // --- what a FIXED BUDGET of keys would capture ---------------------------
  // The occupancy sets above are unbounded; these are the N most-recurred keys,
  // which is the shape real hardware has. key_frac is the share of the table
  // they occupy, so a small frac with high coverage is the interesting case.
  out.set("top_1000_key_frac", ratio(top1k.keys, distinct_keys));
  out.set("top_1000_top1_correct", as_toml_integer(covered.at(3).top1));
  out.set("top_1000_top1_coverage", ratio(covered.at(3).top1, predict_attempts));
  out.set("top_1000_topall_correct", as_toml_integer(covered.at(3).topall));
  out.set("top_1000_topall_coverage", ratio(covered.at(3).topall, predict_attempts));
  out.set("top_1000_mean_cardinality", ratio(covered.at(3).sum_cardinality, top1k.keys));
  out.set("top_1000_keys_w_cardinality_1_1", as_toml_integer(covered.at(3).bands.at(0)));
  out.set("top_1000_keys_w_cardinality_2_2", as_toml_integer(covered.at(3).bands.at(1)));
  out.set("top_1000_keys_w_cardinality_3_4", as_toml_integer(covered.at(3).bands.at(2)));
  out.set("top_1000_keys_w_cardinality_5_8", as_toml_integer(covered.at(3).bands.at(3)));
  out.set("top_1000_keys_w_cardinality_9_16", as_toml_integer(covered.at(3).bands.at(4)));
  out.set("top_1000_keys_w_cardinality_17_32", as_toml_integer(covered.at(3).bands.at(5)));
  out.set("top_1000_keys_w_cardinality_33_64", as_toml_integer(covered.at(3).bands.at(6)));
  out.set("top_1000_keys_w_cardinality_65_plus", as_toml_integer(covered.at(3).bands.at(7)));

  out.set("top_10000_key_frac", ratio(top10k.keys, distinct_keys));
  out.set("top_10000_top1_correct", as_toml_integer(covered.at(4).top1));
  out.set("top_10000_top1_coverage", ratio(covered.at(4).top1, predict_attempts));
  out.set("top_10000_topall_correct", as_toml_integer(covered.at(4).topall));
  out.set("top_10000_topall_coverage", ratio(covered.at(4).topall, predict_attempts));
  out.set("top_10000_mean_cardinality", ratio(covered.at(4).sum_cardinality, top10k.keys));
  out.set("top_10000_keys_w_cardinality_1_1", as_toml_integer(covered.at(4).bands.at(0)));
  out.set("top_10000_keys_w_cardinality_2_2", as_toml_integer(covered.at(4).bands.at(1)));
  out.set("top_10000_keys_w_cardinality_3_4", as_toml_integer(covered.at(4).bands.at(2)));
  out.set("top_10000_keys_w_cardinality_5_8", as_toml_integer(covered.at(4).bands.at(3)));
  out.set("top_10000_keys_w_cardinality_9_16", as_toml_integer(covered.at(4).bands.at(4)));
  out.set("top_10000_keys_w_cardinality_17_32", as_toml_integer(covered.at(4).bands.at(5)));
  out.set("top_10000_keys_w_cardinality_33_64", as_toml_integer(covered.at(4).bands.at(6)));
  out.set("top_10000_keys_w_cardinality_65_plus", as_toml_integer(covered.at(4).bands.at(7)));

  out.set("top_50000_key_frac", ratio(top50k.keys, distinct_keys));
  out.set("top_50000_top1_correct", as_toml_integer(covered.at(5).top1));
  out.set("top_50000_top1_coverage", ratio(covered.at(5).top1, predict_attempts));
  out.set("top_50000_topall_correct", as_toml_integer(covered.at(5).topall));
  out.set("top_50000_topall_coverage", ratio(covered.at(5).topall, predict_attempts));
  out.set("top_50000_mean_cardinality", ratio(covered.at(5).sum_cardinality, top50k.keys));
  out.set("top_50000_keys_w_cardinality_1_1", as_toml_integer(covered.at(5).bands.at(0)));
  out.set("top_50000_keys_w_cardinality_2_2", as_toml_integer(covered.at(5).bands.at(1)));
  out.set("top_50000_keys_w_cardinality_3_4", as_toml_integer(covered.at(5).bands.at(2)));
  out.set("top_50000_keys_w_cardinality_5_8", as_toml_integer(covered.at(5).bands.at(3)));
  out.set("top_50000_keys_w_cardinality_9_16", as_toml_integer(covered.at(5).bands.at(4)));
  out.set("top_50000_keys_w_cardinality_17_32", as_toml_integer(covered.at(5).bands.at(5)));
  out.set("top_50000_keys_w_cardinality_33_64", as_toml_integer(covered.at(5).bands.at(6)));
  out.set("top_50000_keys_w_cardinality_65_plus", as_toml_integer(covered.at(5).bands.at(7)));

  // --- successor deltas: how few bits would encode the stored addresses -----
  // delta = successor - the key's last address, signed, in cachelines, counted
  // per STORED candidate since each is one table slot.
  //
  // Each candidate lands in the SMALLEST width that holds it, so these
  // partition the set: 8b means 5..8 bits, not "fits in 8". They sum to the
  // set's candidate count.
  out.set("all_delta_4b_candidates", as_toml_integer(covered.at(6).delta_bits.at(0)));
  out.set("all_delta_8b_candidates", as_toml_integer(covered.at(6).delta_bits.at(1)));
  out.set("all_delta_16b_candidates", as_toml_integer(covered.at(6).delta_bits.at(2)));
  out.set("all_delta_24b_candidates", as_toml_integer(covered.at(6).delta_bits.at(3)));
  out.set("all_delta_32b_candidates", as_toml_integer(covered.at(6).delta_bits.at(4)));
  out.set("all_delta_wider_candidates", as_toml_integer(covered.at(6).delta_bits.at(5)));

  out.set("o50_delta_4b_candidates", as_toml_integer(covered.at(0).delta_bits.at(0)));
  out.set("o50_delta_8b_candidates", as_toml_integer(covered.at(0).delta_bits.at(1)));
  out.set("o50_delta_16b_candidates", as_toml_integer(covered.at(0).delta_bits.at(2)));
  out.set("o50_delta_24b_candidates", as_toml_integer(covered.at(0).delta_bits.at(3)));
  out.set("o50_delta_32b_candidates", as_toml_integer(covered.at(0).delta_bits.at(4)));
  out.set("o50_delta_wider_candidates", as_toml_integer(covered.at(0).delta_bits.at(5)));

  out.set("o80_delta_4b_candidates", as_toml_integer(covered.at(1).delta_bits.at(0)));
  out.set("o80_delta_8b_candidates", as_toml_integer(covered.at(1).delta_bits.at(1)));
  out.set("o80_delta_16b_candidates", as_toml_integer(covered.at(1).delta_bits.at(2)));
  out.set("o80_delta_24b_candidates", as_toml_integer(covered.at(1).delta_bits.at(3)));
  out.set("o80_delta_32b_candidates", as_toml_integer(covered.at(1).delta_bits.at(4)));
  out.set("o80_delta_wider_candidates", as_toml_integer(covered.at(1).delta_bits.at(5)));

  out.set("o90_delta_4b_candidates", as_toml_integer(covered.at(2).delta_bits.at(0)));
  out.set("o90_delta_8b_candidates", as_toml_integer(covered.at(2).delta_bits.at(1)));
  out.set("o90_delta_16b_candidates", as_toml_integer(covered.at(2).delta_bits.at(2)));
  out.set("o90_delta_24b_candidates", as_toml_integer(covered.at(2).delta_bits.at(3)));
  out.set("o90_delta_32b_candidates", as_toml_integer(covered.at(2).delta_bits.at(4)));
  out.set("o90_delta_wider_candidates", as_toml_integer(covered.at(2).delta_bits.at(5)));

  out.set("top_1000_delta_4b_candidates", as_toml_integer(covered.at(3).delta_bits.at(0)));
  out.set("top_1000_delta_8b_candidates", as_toml_integer(covered.at(3).delta_bits.at(1)));
  out.set("top_1000_delta_16b_candidates", as_toml_integer(covered.at(3).delta_bits.at(2)));
  out.set("top_1000_delta_24b_candidates", as_toml_integer(covered.at(3).delta_bits.at(3)));
  out.set("top_1000_delta_32b_candidates", as_toml_integer(covered.at(3).delta_bits.at(4)));
  out.set("top_1000_delta_wider_candidates", as_toml_integer(covered.at(3).delta_bits.at(5)));

  out.set("top_10000_delta_4b_candidates", as_toml_integer(covered.at(4).delta_bits.at(0)));
  out.set("top_10000_delta_8b_candidates", as_toml_integer(covered.at(4).delta_bits.at(1)));
  out.set("top_10000_delta_16b_candidates", as_toml_integer(covered.at(4).delta_bits.at(2)));
  out.set("top_10000_delta_24b_candidates", as_toml_integer(covered.at(4).delta_bits.at(3)));
  out.set("top_10000_delta_32b_candidates", as_toml_integer(covered.at(4).delta_bits.at(4)));
  out.set("top_10000_delta_wider_candidates", as_toml_integer(covered.at(4).delta_bits.at(5)));

  out.set("top_50000_delta_4b_candidates", as_toml_integer(covered.at(5).delta_bits.at(0)));
  out.set("top_50000_delta_8b_candidates", as_toml_integer(covered.at(5).delta_bits.at(1)));
  out.set("top_50000_delta_16b_candidates", as_toml_integer(covered.at(5).delta_bits.at(2)));
  out.set("top_50000_delta_24b_candidates", as_toml_integer(covered.at(5).delta_bits.at(3)));
  out.set("top_50000_delta_32b_candidates", as_toml_integer(covered.at(5).delta_bits.at(4)));
  out.set("top_50000_delta_wider_candidates", as_toml_integer(covered.at(5).delta_bits.at(5)));
  out.set("o80_keys_by_total_occurrence", as_toml_integer(o80_all.keys));
  out.set("o80_key_frac_by_total_occurrence", ratio(o80_all.keys, distinct_keys));

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

  // The successor-cardinality DISTRIBUTION, not just its percentiles: exact for
  // 1 and 2, then doubling. These bands partition the table -- they must sum to
  // distinct_keys.
  out.set("keys_w_cardinality_1_1", as_toml_integer(cardinality_band(1, 1)));
  out.set("keys_w_cardinality_2_2", as_toml_integer(cardinality_band(2, 2)));
  out.set("keys_w_cardinality_3_4", as_toml_integer(cardinality_band(3, 4)));
  out.set("keys_w_cardinality_5_8", as_toml_integer(cardinality_band(5, 8)));
  out.set("keys_w_cardinality_9_16", as_toml_integer(cardinality_band(9, 16)));
  out.set("keys_w_cardinality_17_32", as_toml_integer(cardinality_band(17, 32)));
  out.set("keys_w_cardinality_33_64", as_toml_integer(cardinality_band(33, 64)));
  out.set("keys_w_cardinality_65_plus", as_toml_integer(cardinality_band(65, std::numeric_limits<uint64_t>::max())));

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
