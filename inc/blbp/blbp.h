/*
 * BLBP-style bit-level perceptron indirect target predictor.
 *
 * CLEAN-ROOM implementation of the mechanism in E. Garza, S. Mirbagher-Ajorpaz,
 * T. A. Khan, D. A. Jimenez, "Bit-Level Perceptron Prediction for Indirect
 * Branches," ISCA 2019. No reference implementation was ever published
 * (verified 2026-08-13), so unlike the vendored ITTAGE there is nothing to
 * diff against: the external ground truth for the core arithmetic is the
 * paper's own worked examples, encoded as golden tests in
 * test/cpp/src/168-blbp-core.cc (Figure 3's three-step weight trace, Figure 4's
 * similarity semantics).
 *
 * This header deliberately says "BLBP-STYLE": the mechanism follows the
 * paper's Algorithms 1-2, but constants (history intervals, transfer curve)
 * are re-tuned for our trace zoo by tools/blbp_tune. The paper's published
 * constants are the defaults, so default-config == nearest-reproduction proxy.
 *
 * TWO LAYERS, and the reason for them:
 *
 *   engine     y_out computation, candidate similarity, selection, training.
 *              Takes explicit row indices and candidate bit-vectors, knows
 *              nothing about histories or the IBTB. This is the layer the
 *              paper's tiny examples (K=4, N=1, M=1) run against directly.
 *
 *   predictor  the full machine: GHIST, local histories, folded-history row
 *              hashing, IBTB with region-compressed targets and SRRIP, region
 *              table, adaptive per-bit thresholds. Owns an engine.
 *
 * RUNTIME-CONFIGURED, unlike ITTAGE's template CFG: the tuner must evaluate
 * hundreds of configurations without recompiling, and the golden tests need
 * degenerate configs. The indirection cost is irrelevant at simulation speed.
 *
 * Interpretation choices where the paper is ambiguous or silent, each pinned
 * by a test:
 *   - Similarity uses 0/1 bit semantics -- the sum of y_out[k] over the
 *     candidate's 1-bits -- per section 3.7's "sum of the bitwise AND" and
 *     Figure 4's arithmetic (0+19+0+32 = 51). Not +/-1 encoding.
 *   - Ties select the LATER candidate: Figure 3's second prediction shows
 *     P1 = 8 <= P2 = 8 resolving to target2, contradicting Algorithm 1's
 *     strict '>' under first-to-last iteration. We follow the figure.
 *   - Figure 4's printed "43" for the second candidate is an arithmetic typo
 *     (-1 + 10 + 32 = 41); the semantics above give 41 and the figure's own
 *     conclusion (candidate 1 wins) is unaffected.
 *   - A zero weight contributes transfer[0] with POSITIVE sign (the bit
 *     prediction convention is y >= 0 means 1).
 *   - M (rows per weight table) is never stated in the paper; it is derived
 *     from the ~64 KB budget in the shell (M = 952; strictly counted the config is 64.48 KB -- see the shell's accounting).
 *   - The GHIST row hash is a folded-XOR over the interval (the construction
 *     the ITTAGE lineage uses); the paper writes only "hashGHIST".
 */

#ifndef INC_BLBP_BLBP_H
#define INC_BLBP_BLBP_H

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

namespace champsim::blbp
{

// ---------------------------------------------------------------------------
// Engine configuration: the arithmetic core's knobs only.
// ---------------------------------------------------------------------------
struct engine_config {
  int K = 12;         // target bits predicted
  int N = 8;          // sub-predictors (weight tables)
  int M = 947;        // rows per table; indexing is mod M, no pow2 constraint
  int weight_max = 7; // 4-bit sign/magnitude

  // transfer[|w|] applied to each weight magnitude before summation, sign
  // re-applied. Identity {0..7} disables the optimization; Figure 5's curve is
  // {2,4,6,8,11,14,18,24}.
  std::vector<int> transfer{2, 4, 6, 8, 11, 14, 18, 24};

  int theta_init = 14;        // per-bit training threshold seed
  bool adaptive_theta = true; // O-GEHL-style threshold adaptation
  bool selective_bits = true; // train only bits that differ across candidates
};

// ---------------------------------------------------------------------------
// The arithmetic core. Golden-tested against the paper's Figures 3 and 4.
// ---------------------------------------------------------------------------
class engine
{
public:
  explicit engine(engine_config cfg)
      : cfg_(std::move(cfg)), weights_(static_cast<std::size_t>(cfg_.N) * static_cast<std::size_t>(cfg_.M) * static_cast<std::size_t>(cfg_.K), 0),
        theta_(static_cast<std::size_t>(cfg_.K), cfg_.theta_init), theta_counter_(static_cast<std::size_t>(cfg_.K), 0)
  {
    assert(static_cast<int>(cfg_.transfer.size()) == cfg_.weight_max + 1);
  }

  const engine_config& config() const { return cfg_; }

  // y_out[k] = sum over sub-predictors of sign(w) * transfer(|w|), for the row
  // each sub-predictor selected. `rows` holds one row index per sub-predictor.
  std::vector<int> compute_yout(const std::vector<int>& rows) const
  {
    assert(static_cast<int>(rows.size()) == cfg_.N);
    std::vector<int> y(static_cast<std::size_t>(cfg_.K), 0);
    for (int i = 0; i < cfg_.N; ++i) {
      for (int k = 0; k < cfg_.K; ++k) {
        const int w = weight(i, rows[static_cast<std::size_t>(i)], k);
        const int amplified = cfg_.transfer[static_cast<std::size_t>(std::abs(w))];
        // A zero weight has no direction; the y >= 0 convention makes its
        // contribution positive. Pinned by the transfer-function test.
        y[static_cast<std::size_t>(k)] += (w < 0) ? -amplified : amplified;
      }
    }
    return y;
  }

  // Similarity: sum of y_out[k] over the candidate's 1-bits. 0/1 semantics per
  // section 3.7 and Figure 4 -- NOT a +/-1 dot product.
  static int similarity(const std::vector<int>& yout, const std::vector<uint8_t>& bits)
  {
    assert(bits.size() == yout.size());
    int s = 0;
    for (std::size_t k = 0; k < bits.size(); ++k) {
      if (bits[k]) {
        s += yout[k];
      }
    }
    return s;
  }

  // Selection: argmax similarity; ties go to the LATER candidate (Figure 3,
  // prediction 2). Returns the candidate index, or -1 if none.
  static int select(const std::vector<int>& yout, const std::vector<std::vector<uint8_t>>& candidates)
  {
    int best = -1;
    int best_sim = 0;
    for (std::size_t t = 0; t < candidates.size(); ++t) {
      const int s = similarity(yout, candidates[t]);
      if (best < 0 || s >= best_sim) { // >= : later candidate wins ties
        best_sim = s;
        best = static_cast<int>(t);
      }
    }
    return best;
  }

  // Training, per Algorithm 2. `rows` as in compute_yout; `actual_bits` is the
  // resolved target's low K bits; `candidates` feeds selective bit training.
  // Returns true if any weight moved (exposed for the adaptive-theta logic and
  // for tests).
  bool train(const std::vector<int>& rows, const std::vector<int>& yout, const std::vector<uint8_t>& actual_bits, bool mispredicted,
             const std::vector<std::vector<uint8_t>>& candidates)
  {
    assert(static_cast<int>(rows.size()) == cfg_.N);
    bool moved = false;
    for (int k = 0; k < cfg_.K; ++k) {
      if (cfg_.selective_bits && !bit_differs(candidates, k)) {
        continue; // suppress: this bit is identical across every candidate
      }
      const std::size_t ku = static_cast<std::size_t>(k);
      const bool bit_correct = (actual_bits[ku] != 0) == (yout[ku] >= 0);
      const int magnitude = std::abs(yout[ku]);

      if (cfg_.adaptive_theta) {
        adapt_theta(k, bit_correct, magnitude);
      }
      if (bit_correct && magnitude >= theta_[ku]) {
        continue; // confident and correct: no update
      }
      for (int i = 0; i < cfg_.N; ++i) {
        int& w = weight(i, rows[static_cast<std::size_t>(i)], k);
        if (actual_bits[ku] != 0) {
          if (w < cfg_.weight_max) {
            ++w;
            moved = true;
          }
        } else {
          if (w > -cfg_.weight_max) {
            --w;
            moved = true;
          }
        }
      }
    }
    (void)mispredicted; // per-bit correctness governs updates; the argument
                        // documents call sites and feeds no arithmetic
    return moved;
  }

  // --- test seams -----------------------------------------------------------
  void set_weights_for_test(int table, int row, const std::vector<int>& w)
  {
    for (int k = 0; k < cfg_.K; ++k) {
      weight(table, row, k) = w[static_cast<std::size_t>(k)];
    }
  }
  std::vector<int> weights_for_test(int table, int row) const
  {
    std::vector<int> out;
    for (int k = 0; k < cfg_.K; ++k) {
      out.push_back(weight(table, row, k));
    }
    return out;
  }
  int theta_for_test(int k) const { return theta_[static_cast<std::size_t>(k)]; }

private:
  static bool bit_differs(const std::vector<std::vector<uint8_t>>& candidates, int k)
  {
    if (candidates.size() < 2) {
      return true; // nothing to compare against: train normally
    }
    const uint8_t first = candidates.front()[static_cast<std::size_t>(k)];
    return std::any_of(candidates.begin() + 1, candidates.end(), [&](const std::vector<uint8_t>& c) { return c[static_cast<std::size_t>(k)] != first; });
  }

  // Seznec's adaptive threshold (O-GEHL): a per-bit counter nudges theta so
  // that trainings caused by low confidence roughly balance trainings caused
  // by mispredictions.
  void adapt_theta(int k, bool bit_correct, int magnitude)
  {
    const std::size_t ku = static_cast<std::size_t>(k);
    if (!bit_correct) {
      if (++theta_counter_[ku] >= theta_adapt_period) {
        theta_counter_[ku] = 0;
        ++theta_[ku];
      }
    } else if (magnitude < theta_[ku]) {
      if (--theta_counter_[ku] <= -theta_adapt_period) {
        theta_counter_[ku] = 0;
        if (theta_[ku] > 1) {
          --theta_[ku];
        }
      }
    }
  }

  int& weight(int table, int row, int k)
  {
    return weights_[(static_cast<std::size_t>(table) * static_cast<std::size_t>(cfg_.M) + static_cast<std::size_t>(row)) * static_cast<std::size_t>(cfg_.K)
                    + static_cast<std::size_t>(k)];
  }
  int weight(int table, int row, int k) const
  {
    return weights_[(static_cast<std::size_t>(table) * static_cast<std::size_t>(cfg_.M) + static_cast<std::size_t>(row)) * static_cast<std::size_t>(cfg_.K)
                    + static_cast<std::size_t>(k)];
  }

  static constexpr int theta_adapt_period = 32;

  engine_config cfg_;
  std::vector<int> weights_; // [N][M][K], values in [-weight_max, +weight_max]
  std::vector<int> theta_;
  std::vector<int> theta_counter_;
};

// ---------------------------------------------------------------------------
// Full-machine configuration.
// ---------------------------------------------------------------------------
struct predictor_config {
  engine_config engine{};

  // Global-history intervals, one per sub-predictor AFTER the local one
  // (sub-predictor 0 is local history). Paper defaults, tuned in their
  // section 3.6; ours are re-tuned by tools/blbp_tune.
  std::vector<std::pair<int, int>> intervals{{0, 13}, {1, 33}, {23, 49}, {44, 85}, {77, 149}, {159, 270}, {252, 630}};

  int ghist_bits = 630;
  int local_entries = 256;
  int local_bits = 10;
  int local_target_bit = 3; // local history records this bit of each target

  int ibtb_sets = 64;
  int ibtb_ways = 64;
  int tag_bits = 8;
  int region_entries = 128;
  int region_offset_bits = 20; // low bits stored per entry; the rest is the region
};

// ---------------------------------------------------------------------------
// The full machine. Owns an engine; adds histories, hashing, IBTB and the
// region table. See the header comment for the layering rationale.
// ---------------------------------------------------------------------------
class predictor
{
public:
  explicit predictor(predictor_config cfg) : cfg_(std::move(cfg)), engine_(cfg_.engine)
  {
    assert(static_cast<int>(cfg_.intervals.size()) == cfg_.engine.N - 1);
    ghist_.assign(static_cast<std::size_t>(cfg_.ghist_bits), 0);
    local_.assign(static_cast<std::size_t>(cfg_.local_entries), 0);
    ibtb_.assign(static_cast<std::size_t>(cfg_.ibtb_sets) * static_cast<std::size_t>(cfg_.ibtb_ways), {});
    regions_.assign(static_cast<std::size_t>(cfg_.region_entries), {});
  }

  // Record a conditional branch outcome into the global history. The adapter
  // calls this for every conditional branch, mirroring where basic_btb feeds
  // its own indirect predictor's direction history.
  void note_conditional(bool taken)
  {
    ghist_.pop_back();
    ghist_.insert(ghist_.begin(), taken ? 1 : 0);
  }

  // Predict the target for an indirect branch, or nothing if the IBTB holds no
  // candidates for this PC. "Nothing" must stay nothing -- the adapter reports
  // no-prediction rather than inventing a target.
  std::optional<uint64_t> predict(uint64_t pc) const
  {
    const auto cands = collect_candidates(pc);
    if (cands.empty()) {
      return std::nullopt;
    }
    const auto rows = compute_rows(pc);
    const auto yout = engine_.compute_yout(rows);
    std::vector<std::vector<uint8_t>> bitvecs;
    bitvecs.reserve(cands.size());
    for (const auto* e : cands) {
      bitvecs.push_back(low_bits(full_target(*e)));
    }
    const int sel = engine::select(yout, bitvecs);
    return full_target(*cands[static_cast<std::size_t>(sel)]);
  }

  // Train on a resolved indirect branch. SELF-CONTAINED: recomputes rows,
  // y_out and the candidate set itself (the ITTAGE defect-B lesson - the
  // direct-BTB-miss path calls update() without a paired predict()).
  void update(uint64_t pc, uint64_t target)
  {
    const auto rows = compute_rows(pc);
    const auto yout = engine_.compute_yout(rows);
    auto cands = collect_candidates(pc);
    std::vector<std::vector<uint8_t>> bitvecs;
    for (const auto* e : cands) {
      bitvecs.push_back(low_bits(full_target(*e)));
    }
    const int sel = bitvecs.empty() ? -1 : engine::select(yout, bitvecs);
    const bool mispredicted = (sel < 0) || (full_target(*cands[static_cast<std::size_t>(sel)]) != target);

    engine_.train(rows, yout, low_bits(target), mispredicted, bitvecs);

    ibtb_insert(pc, target);

    // Local history records a chosen bit of the ACTUAL target for this PC.
    auto& lh = local_[local_index(pc)];
    lh = static_cast<uint16_t>(((lh << 1) | ((target >> cfg_.local_target_bit) & 1)) & ((1U << cfg_.local_bits) - 1));
  }

  // --- test seams -----------------------------------------------------------
  int candidate_count_for_test(uint64_t pc) const { return static_cast<int>(collect_candidates(pc).size()); }
  uint16_t local_history_for_test(uint64_t pc) const { return local_[local_index(pc)]; }
  const engine& engine_for_test() const { return engine_; }

private:
  struct ibtb_entry {
    bool valid = false;
    uint16_t tag = 0;
    int region = -1;     // index into regions_, possibly dangling after reuse
    uint64_t offset = 0; // low region_offset_bits of the target
    uint8_t rrip = 3;    // SRRIP re-reference counter
  };
  struct region_entry {
    bool valid = false;
    uint64_t base = 0; // target >> region_offset_bits
    uint64_t lru = 0;
  };

  // Folded-XOR hash of a GHIST interval plus the PC, mod M. The paper writes
  // only "hashGHIST"; this construction is the folded-history lineage the
  // in-tree ITTAGE uses. A documented assumption, and a tunable in spirit --
  // but the fold is fixed, only intervals move during tuning.
  int hash_interval(uint64_t pc, int lo, int hi) const
  {
    uint64_t h = pc ^ (pc >> 13);
    uint64_t acc = 0;
    int shift = 0;
    for (int b = lo; b < hi && b < cfg_.ghist_bits; ++b) {
      acc ^= static_cast<uint64_t>(ghist_[static_cast<std::size_t>(b)]) << shift;
      if (++shift >= 24) {
        shift = 0;
      }
    }
    h ^= acc * 0x9E3779B97F4A7C15ULL;
    h ^= h >> 29;
    return static_cast<int>(h % static_cast<uint64_t>(cfg_.engine.M));
  }

  std::vector<int> compute_rows(uint64_t pc) const
  {
    std::vector<int> rows;
    rows.reserve(static_cast<std::size_t>(cfg_.engine.N));
    // Sub-predictor 0: local history.
    const uint64_t lh = local_[local_index(pc)];
    uint64_t h0 = (pc ^ (pc >> 7) ^ (lh * 0x9E3779B97F4A7C15ULL));
    rows.push_back(static_cast<int>(h0 % static_cast<uint64_t>(cfg_.engine.M)));
    for (const auto& [lo, hi] : cfg_.intervals) {
      rows.push_back(hash_interval(pc, lo, hi));
    }
    return rows;
  }

  std::vector<uint8_t> low_bits(uint64_t target) const
  {
    std::vector<uint8_t> bits(static_cast<std::size_t>(cfg_.engine.K));
    for (int k = 0; k < cfg_.engine.K; ++k) {
      bits[static_cast<std::size_t>(k)] = (target >> k) & 1;
    }
    return bits;
  }

  std::size_t local_index(uint64_t pc) const { return (pc >> 2) % static_cast<uint64_t>(cfg_.local_entries); }
  std::size_t set_index(uint64_t pc) const { return (pc >> 2) % static_cast<uint64_t>(cfg_.ibtb_sets); }
  uint16_t tag_of(uint64_t pc) const { return static_cast<uint16_t>((pc >> 2) ^ (pc >> 16)) & static_cast<uint16_t>((1U << cfg_.tag_bits) - 1); }

  uint64_t full_target(const ibtb_entry& e) const
  {
    // A dangling region pointer (region recycled since this entry was written)
    // reconstructs a wrong-but-well-formed address, exactly as cheap hardware
    // would. It loses the similarity race or mispredicts and is then retrained.
    const uint64_t base = (e.region >= 0 && regions_[static_cast<std::size_t>(e.region)].valid) ? regions_[static_cast<std::size_t>(e.region)].base : 0;
    return (base << cfg_.region_offset_bits) | e.offset;
  }

  std::vector<const ibtb_entry*> collect_candidates(uint64_t pc) const
  {
    std::vector<const ibtb_entry*> out;
    const std::size_t base = set_index(pc) * static_cast<std::size_t>(cfg_.ibtb_ways);
    const uint16_t tag = tag_of(pc);
    for (int w = 0; w < cfg_.ibtb_ways; ++w) {
      const auto& e = ibtb_[base + static_cast<std::size_t>(w)];
      if (e.valid && e.tag == tag) {
        out.push_back(&e);
      }
    }
    return out;
  }

  int region_of(uint64_t target)
  {
    const uint64_t base = target >> cfg_.region_offset_bits;
    int free = -1;
    int lru = 0;
    for (int i = 0; i < cfg_.region_entries; ++i) {
      auto& r = regions_[static_cast<std::size_t>(i)];
      if (r.valid && r.base == base) {
        r.lru = ++lru_clock_;
        return i;
      }
      if (!r.valid && free < 0) {
        free = i;
      }
      if (regions_[static_cast<std::size_t>(i)].lru < regions_[static_cast<std::size_t>(lru)].lru) {
        lru = i;
      }
    }
    const int victim = (free >= 0) ? free : lru;
    regions_[static_cast<std::size_t>(victim)] = {true, base, ++lru_clock_};
    return victim;
  }

  void ibtb_insert(uint64_t pc, uint64_t target)
  {
    const std::size_t base = set_index(pc) * static_cast<std::size_t>(cfg_.ibtb_ways);
    const uint16_t tag = tag_of(pc);
    const int region = region_of(target);
    const uint64_t offset = target & ((1ULL << cfg_.region_offset_bits) - 1);

    // Hit: same tag AND same target -> promote (SRRIP: rrip = 0).
    int invalid = -1;
    for (int w = 0; w < cfg_.ibtb_ways; ++w) {
      auto& e = ibtb_[base + static_cast<std::size_t>(w)];
      if (e.valid && e.tag == tag && e.region == region && e.offset == offset) {
        e.rrip = 0;
        return;
      }
      if (!e.valid && invalid < 0) {
        invalid = w;
      }
    }
    // Miss: fill an invalid way, else SRRIP victim (first rrip==3, aging).
    int victim = invalid;
    while (victim < 0) {
      for (int w = 0; w < cfg_.ibtb_ways; ++w) {
        if (ibtb_[base + static_cast<std::size_t>(w)].rrip >= 3) {
          victim = w;
          break;
        }
      }
      if (victim < 0) {
        for (int w = 0; w < cfg_.ibtb_ways; ++w) {
          ++ibtb_[base + static_cast<std::size_t>(w)].rrip;
        }
      }
    }
    ibtb_[base + static_cast<std::size_t>(victim)] = {true, tag, region, offset, 2}; // SRRIP long-rereference insert
  }

  predictor_config cfg_;
  engine engine_;
  std::vector<uint8_t> ghist_; // [0] = most recent conditional outcome
  std::vector<uint16_t> local_;
  std::vector<ibtb_entry> ibtb_; // sets x ways
  std::vector<region_entry> regions_;
  uint64_t lru_clock_ = 0;
};

} // namespace champsim::blbp

#endif
