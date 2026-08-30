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
 * "Prefetching Using Markov Predictors", IEEE Trans. Computers 48(2), Feb.
 * 1999 -- with every hardware constraint of that design deliberately removed.
 *
 * It learns correlations of the form
 *
 *     {A, B, C, D}  ->  {E, F, G, H}
 *
 * where the left side is the last `history_length` cacheline addresses and the
 * right side is the SET of addresses observed to follow that sequence. The
 * right side is breadth-first -- every node reachable in one step from the
 * current node -- not a depth-first chain of successive prefetches.
 *
 * WHAT THIS IS FOR. It is a measurement instrument, not a prefetcher. It
 * answers two questions about a workload:
 *
 *   1. How does right-side cardinality change as the left side lengthens?
 *      Cardinality bounds ACCURACY: a sequence with eight observed successors
 *      cannot be predicted better than one-in-eight without more information.
 *   2. How much does a length-H sequence actually recur? Recurrence bounds
 *      COVERAGE: a correlation that pinpoints the successor perfectly is worth
 *      nothing if its key never comes round again.
 *
 * The expected tension is that (1) improves and (2) degrades as H rises. This
 * module measures where that trade sits.
 *
 * WHY IT DOES NOT PREFETCH BY DEFAULT. If predictions became real prefetches,
 * the address stream being measured would itself become a function of H, and
 * the cardinality-versus-H curve would be partly an artifact of the instrument
 * perturbing its own input. `issue_prefetch` is reserved for when this becomes
 * a prefetcher; while it is false the module is provably inert (no
 * prefetch_line call exists on that path).
 *
 * SPACE. Deliberately unbounded: no table size, no fan-out cap, no LRU
 * approximation of the transition probabilities. The keys are full address
 * sequences rather than hashed fingerprints, so the distinct-key count this
 * reports is exact rather than collision-inflated. At LLC scale that is about
 * 1.2 GB at H = 32 on the widest of our traces. It is NOT affordable at L1D or
 * L2C volumes; see proj/specs/2026-08-30-generic-markov-design.md.
 */
class generic_markov : public champsim::modules::prefetcher
{
public:
  // One observed successor of a left-side sequence, with how often it followed.
  struct candidate {
    uint64_t addr{};
    uint64_t count{};
  };

  // The right side of one correlation. `total_count` is the sum of the
  // candidate counts, kept alongside rather than recomputed: it is how many
  // times this key has been trained, and summing a wide right side on every
  // lookup would make the hot keys quadratic.
  struct successors {
    std::vector<candidate> candidates{};
    uint64_t total_count{};
  };

  using sequence = std::vector<uint64_t>;

  // std::hash has no specialization for vector. FNV-1a over the bytes, the
  // same construction champsim::toml_printer::config_id uses.
  struct sequence_hash {
    std::size_t operator()(const sequence& seq) const noexcept;
  };

  // Which addresses train the model. `all` is every lookup that reached this
  // cache; `miss` is only those that missed -- the paper's miss reference
  // stream.
  enum class stream { all, miss };

  using prefetcher::prefetcher;

  void configure(const champsim::runtime_config& cfg, std::string_view prefix);

  uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                    uint32_t metadata_in);
  uint32_t prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in);

  void prefetcher_begin_phase();
  void prefetcher_end_phase();

  // Test seams. The behaviour tests drive the module through a real CACHE, so
  // they need to read what it learned without befriending the class.
  //
  // lookup() returns a pointer INTO the map: any later access that inserts can
  // rehash and leave it dangling. Read it before driving the module further.
  [[nodiscard]] std::size_t table_size() const { return table.size(); }
  [[nodiscard]] const successors* lookup(const sequence& seq) const;

private:
  // Knobs. Every one is consulted unconditionally in configure(): the runtime
  // configuration makes a user key that nothing reads fatal, so a knob read
  // only on some paths would turn a valid configuration into a crash.
  std::size_t history_length{1};
  std::size_t predict_degree{4};
  stream train_on{stream::all};
  bool issue_prefetch{false};

  // The sliding window, oldest first. A vector rather than a deque because it
  // IS the map key -- lookups need no conversion, and erase-from-front costs
  // at most history_length moves.
  sequence history{};

  std::unordered_map<sequence, successors, sequence_hash> table{};

  // The top-k of the most recent prediction, in rank order. Graded against the
  // NEXT access, which is the only moment it can be checked.
  //
  // These survive a phase boundary along with the table and the window: the
  // address stream does not restart, so a prediction made on the last warmup
  // access is genuinely a statement about the first ROI access and is graded
  // there. It moves scored_predictions by at most one.
  std::vector<uint64_t> pending{};
  bool has_pending{false};
  bool pending_tied{false};

  // Scratch for ranking, kept as a member so a hot key does not reallocate on
  // every lookup.
  std::vector<candidate> ranked{};

  // Counters for one phase. Reset at every begin_phase, so what is published
  // at the end of the region of interest describes the region of interest.
  // The TABLE is never reset -- training carries across the warmup boundary on
  // purpose, so the ROI is measured against a model in the position a real
  // prefetcher would be in.
  uint64_t train_events{};
  uint64_t predict_attempts{};
  uint64_t predict_hits{};
  uint64_t sum_cardinality{};
  uint64_t sum_key_occurrences{};
  uint64_t scored_predictions{};
  uint64_t top1_correct{};
  uint64_t top1_ties{};
  uint64_t topk_correct{};
};

#endif
