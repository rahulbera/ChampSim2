/*

This code implements a hashed perceptron branch predictor using geometric
history lengths and dynamic threshold setting.  It was written by Daniel
A. Jiménez in March 2019.


The original perceptron branch predictor is from Jiménez and Lin, "Dynamic
Branch Prediction with Perceptrons," HPCA 2001.

The idea of using multiple independently indexed tables of perceptron weights
is from Jiménez, "Fast Path-Based Neural Branch Prediction," MICRO 2003 and
later expanded in "Piecewise Linear Branch Prediction" from ISCA 2005.

The idea of using hashes of branch history to reduce the number of independent
tables is documented in three contemporaneous papers:

1. Seznec, "Revisiting the Perceptron Predictor," IRISA technical report, 2004.

2. Tarjan and Skadron, "Revisiting the Perceptron Predictor Again," UVA
technical report, 2004, expanded and published in ACM TACO 2005 as "Merging
path and gshare indexing in perceptron branch prediction"; introduces the term
"hashed perceptron."

3. Loh and Jiménez, "Reducing the Power and Complexity of Path-Based Neural
Branch Prediction," WCED 2005.

The ideas of using "geometric history lengths" i.e. hashing into tables with
histories of exponentially increasing length, as well as dynamically adjusting
the theta parameter, are from Seznec, "The O-GEHL Branch Predictor," from CBP
2004, expanded later as "Analysis of the O-GEometric History Length Branch
Predictor" in ISCA 2005.

This code uses these ideas, but prefers simplicity over absolute accuracy (I
wrote it in about an hour and later spent more time on this comment block than
I did on the code). These papers and subsequent papers by Jiménez and other
authors significantly improve the accuracy of perceptron-based predictors but
involve tricks and analysis beyond the needs of a tool like ChampSim that
targets cache optimizations. If you want accuracy at any cost, see the winners
of the latest branch prediction contest, CBP 2016 as of this writing, but
prepare to have your face melted off by the complexity of the code you find
there. If you are a student being asked to code a good branch predictor for
your computer architecture class, don't copy this code; there are much better
sources for you to plagiarize.

*/

#include "hashed_perceptron.h"

#include <numeric>

bool hashed_perceptron::predict_branch(champsim::address pc, champsim::address /*predicted_target*/, bool /*always_taken*/, uint8_t /*branch_type*/,
                                       champsim::address_space_id asid)
{
  auto get_index = [pc_slice = pc.slice_lower<TABLE_INDEX_BITS>().to<uint64_t>() ^ static_cast<uint64_t>(champsim::asid_word(asid))](const auto& hist) {
    return hist.value() ^ pc_slice; // seed in the PC to spread accesses around (like gshare) XOR in the last word
  };
  const auto& ghist = ghist_words[ghist_key(asid)];
  perceptron_result result;
  std::transform(std::cbegin(ghist), std::cend(ghist), std::begin(result.indices), get_index);

  // add the selected weights to the perceptron sum
  result.yout = std::inner_product(std::begin(tables), std::end(tables), std::begin(result.indices), 0, std::plus<>{},
                                   [](const auto& table, const auto& index) { return table.at(index).value(); });
  last_result = result;
  return result.yout >= THRESHOLD;
}

void hashed_perceptron::last_branch_result(champsim::address pc, champsim::address /*branch_target*/, bool taken, uint8_t /*branch_type*/,
                                           champsim::address_space_id asid)
{
  auto& ghist = ghist_words[ghist_key(asid)];
  for (auto& hist : ghist) {
    hist.push_back(taken);
  }

  // perceptron learning rule: train if misprediction or weak correct prediction
  bool prediction_correct = (taken == (last_result.yout >= THRESHOLD));
  bool prediction_weak = (std::abs(last_result.yout) < theta);
  if (!prediction_correct || prediction_weak) {
    for (std::size_t i = 0; i < std::size(tables); i++)
      tables[i][last_result.indices[i]] += taken ? 1 : -1; // update weights
    adjust_threshold(prediction_correct);
  }
  (void)asid; // the ASID folds into the ghist table index, never the history shift
}

// dynamic threshold setting from Seznec's O-GEHL paper
void hashed_perceptron::adjust_threshold(bool correct)
{
  constexpr int SPEED = 18; // speed for dynamic threshold setting
  if (!correct) {
    // increase theta after enough mispredictions
    tc++;
    if (tc >= SPEED) {
      theta++;
      tc = 0;
    }
  } else {
    // decrease theta after enough weak but correct predictions
    tc--;
    if (tc <= -SPEED) {
      theta--;
      tc = 0;
    }
  }
}

std::size_t hashed_perceptron::ghist_key(champsim::address_space_id asid) const
{
  // B3.4: key the GHR by the virtual thread only when the per-virtual small state
  // switch (read from the core's bmt table in configure()) is on. Off stays at
  // index 0 == the single shared GHR, byte-identical to the prior layout.
  return per_virtual_ ? asid[0] : 0;
}

void hashed_perceptron::configure(const champsim::runtime_config& cfg, std::string_view prefix)
{
  // B3.4: the knob lives under the core's [ooo_cpu.cpu0.bmt] table, which is the
  // module prefix minus its trailing ".hashed_perceptron" component. The module
  // consults it (default off) so a default run keeps the shared GHR.
  const auto dot = prefix.rfind('.');
  const auto core_prefix = (dot == std::string_view::npos) ? std::string_view{} : prefix.substr(0, dot);
  per_virtual_ = cfg.value<bool>(std::string{core_prefix} + ".bmt.per_virtual_small_state", false);
}

uint64_t hashed_perceptron::small_state_fingerprint(champsim::address_space_id asid) const
{
  // FNV-1a over the folded-history values. The absolute value is irrelevant; the
  // swap assertion only compares snapshot-then vs snapshot-now for equality.
  const auto& ghist = ghist_words[ghist_key(asid)];
  uint64_t h = 1469598103934665603ull;
  for (const auto& hist : ghist) {
    h ^= static_cast<uint64_t>(hist.value());
    h *= 1099511628211ull;
  }
  return h;
}
