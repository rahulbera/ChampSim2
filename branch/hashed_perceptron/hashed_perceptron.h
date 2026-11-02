#ifndef BRANCH_HASHED_PERCEPTRON_H
#define BRANCH_HASHED_PERCEPTRON_H

#include <array>
#include <cstdint>
#include <string_view>
#include <tuple>
#include <vector>

#include "folded_shift_register.h"
#include "modules.h"
#include "msl/bits.h"
#include "msl/fwcounter.h"
#include "runtime_config.h"

class hashed_perceptron : champsim::modules::branch_predictor
{
  using bits = champsim::data::bits;                 // saves some typing
  constexpr static std::size_t NTABLES = 16;         // this many tables
  constexpr static bits MAXHIST{232};                // maximum history length
  constexpr static bits MINHIST{3};                  // minimum history length (for table 1; table 0 is biases)
  constexpr static std::size_t TABLE_SIZE = 1 << 12; // 12-bit indices for the tables
  constexpr static bits TABLE_INDEX_BITS{champsim::msl::lg2(TABLE_SIZE)};
  constexpr static int THRESHOLD = 1;

  constexpr static std::array<bits, NTABLES> history_lengths = {
      bits{},   MINHIST,  bits{4},  bits{6},  bits{8},  bits{10},  bits{14},  bits{19},
      bits{26}, bits{36}, bits{49}, bits{67}, bits{91}, bits{125}, bits{170}, MAXHIST}; // geometric global history lengths

  // tables of 8-bit weights
  std::array<std::array<champsim::msl::sfwcounter<8>, TABLE_SIZE>, NTABLES> tables{};

  // words that store the global history
  using history_type = folded_shift_register<TABLE_INDEX_BITS>;
  // B3.4 per-virtual small state: the GHR is keyed by asid[0] (the virtual
  // thread's trace slot) when the core's bmt.per_virtual_small_state is on, so
  // each virtual context keeps its own history across a swap-out/swap-in. The
  // default (off) index is 0 -- a single shared GHR, byte-identical to the prior
  // single-instance layout. The PHT `tables` stay shared (the paper replicates
  // only small state). Sized to the full uint8_t asid[0] range.
  static constexpr std::size_t NUM_ASID_KEYS = 1u << 8;
  std::array<std::array<history_type, NTABLES>, NUM_ASID_KEYS> ghist_words = []() {
    decltype(ghist_words) retval{};
    for (auto& row : retval) {
      std::transform(std::cbegin(history_lengths), std::cend(history_lengths), std::begin(row), [](const auto len) { return history_type{len}; });
    }
    return retval;
  }();

  int theta = 10;
  int tc = 0; // counter for threshold setting algorithm

  struct perceptron_result {
    std::array<uint64_t, std::tuple_size_v<decltype(history_lengths)>> indices = {}; // remember the indices into the tables from prediction to update
    int yout = 0;                                                                    // perceptron sum
  };

  perceptron_result last_result{};

  // B3.4 per-virtual small state switch, read from the core's
  // [ooo_cpu.cpu0.bmt] per_virtual_small_state knob in configure(). Off keeps the
  // single shared GHR; on keys it by asid[0]. The module reads the knob itself
  // (rather than the core) because a branch module cannot see O3_CPU's members.
  bool per_virtual_ = false;

  // B3.4 key: asid[0] when per_virtual_ is on, else 0 (the single shared GHR).
  std::size_t ghist_key(champsim::address_space_id asid) const;

public:
  using branch_predictor::branch_predictor;

  // SMT: single per-core instance, keyed by ASID (ghist table index folds
  // asid_word), so two contexts on one core share it safely.
  static constexpr champsim::modules::smt_capability capability = champsim::modules::smt_capability::shared;

  // B3.4: consult the core's bmt.per_virtual_small_state knob (sibling table to
  // this module's own). No knob is otherwise owned here, so the default is a
  // no-op read that leaves the shared GHR.
  void configure(const champsim::runtime_config& cfg, std::string_view prefix);

  bool predict_branch(champsim::address pc, champsim::address predicted_target, bool always_taken, uint8_t branch_type, champsim::address_space_id asid);
  void last_branch_result(champsim::address pc, champsim::address branch_target, bool taken, uint8_t branch_type, champsim::address_space_id asid);
  void adjust_threshold(bool correct);

  // B3.4 fingerprint of the per-asid GHR, for the swap-out/swap-in assertion.
  uint64_t small_state_fingerprint(champsim::address_space_id asid) const;
};

#endif
