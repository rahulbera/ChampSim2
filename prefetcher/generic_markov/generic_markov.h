#ifndef PREFETCHER_GENERIC_MARKOV_H
#define PREFETCHER_GENERIC_MARKOV_H

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "address.h"
#include "champsim.h"
#include "modules.h"
#include "runtime_config.h"

/*
 * A generic Markov correlation instrument, after Joseph and Grunwald,
 * "Prefetching Using Markov Predictors", IEEE Trans. Computers 48(2), 1999,
 * with that design's hardware constraints deliberately removed.
 *
 * Learns {A,B,C,D} -> {E,F,G,H}: the left side is the last `history_length`
 * cacheline addresses, the right side the SET of one-step successors.
 *
 * A measurement instrument, not a prefetcher. It issues nothing while
 * issue_prefetch is false, which keeps the measured address stream independent
 * of history_length. Space is unbounded and exact-keyed: ~1.2 GB at H=32 at LLC
 * scale, and NOT affordable at L1D or L2C volumes.
 *
 * Rationale and metric definitions: proj/specs/2026-08-30-generic-markov-design.md
 */
class generic_markov : public champsim::modules::prefetcher
{
public:
  // last_seen breaks count ties, and is unique per training event -- so
  // (count, last_seen) is a total order and needs no third sort key.
  struct candidate {
    uint64_t addr{};
    uint64_t count{};
    uint64_t last_seen{};
  };

  // total_count is kept, not recomputed: summing per lookup would make the
  // hot keys quadratic.
  struct successors {
    std::vector<candidate> candidates{};
    uint64_t total_count{};
    // Per-key credit, so coverage can be attributed to a SUBSET of the table.
    uint64_t top1_correct{};
    uint64_t topall_correct{};
  };

  using sequence = std::vector<uint64_t>;

  // std::hash has no specialization for vector. FNV-1a over the bytes.
  struct sequence_hash {
    std::size_t operator()(const sequence& seq) const noexcept;
  };

  // `all` trains on every lookup reaching this cache; `miss` only on those
  // that missed -- the paper's miss reference stream.
  enum class stream { all, miss };

  using prefetcher::prefetcher;

  void configure(const champsim::runtime_config& cfg, std::string_view prefix);

  uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                    uint32_t metadata_in);
  uint32_t prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in);

  void prefetcher_begin_phase();
  void prefetcher_end_phase();

  // Test seams. lookup() points INTO the map: a later insert can dangle it.
  [[nodiscard]] std::size_t table_size() const { return table.size(); }
  [[nodiscard]] const successors* lookup(const sequence& seq) const;

private:
  // Consulted unconditionally: an unread user key is fatal.
  std::size_t history_length{1};
  std::size_t predict_degree{4};
  stream train_on{stream::all};
  bool issue_prefetch{false};

  // The sliding window, oldest first. A vector because it IS the map key.
  sequence history{};

  std::unordered_map<sequence, successors, sequence_hash> table{};

  // The last prediction, graded against the NEXT access. Survives a phase
  // boundary with the table and window.
  std::vector<uint64_t> pending{};
  bool has_pending{false};
  bool pending_tied{false};

  // Step 0 grades top-1, step 1 owns the key it belongs to. Carries the verdict
  // between them.
  bool graded_top1_hit{false};

  // Scratch, so a hot key does not reallocate per lookup.
  std::vector<candidate> ranked{};

  // NEVER reset: restarting it would leave warmup candidates looking newer
  // than anything learned since.
  uint64_t train_clock{};

  // Per-phase counters. The TABLE is not reset -- training carries across the
  // warmup boundary on purpose.
  uint64_t train_events{};
  uint64_t predict_attempts{};
  uint64_t predict_hits{};
  uint64_t sum_cardinality{};
  uint64_t sum_key_occurrences{};
  uint64_t scored_predictions{};
  uint64_t top1_correct{};
  uint64_t top1_ties{};
  uint64_t topk_correct{};

  // Addresses emitted: the denominator for degree-k accuracy.
  uint64_t predicted_addresses{};

  // Successor present at any rank: the ceiling. top1 <= topk <= topall.
  uint64_t topall_correct{};
};

#endif
