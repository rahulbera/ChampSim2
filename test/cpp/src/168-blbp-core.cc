#include <catch.hpp>
#include <cstdint>
#include <vector>

#include "blbp/blbp.h"

// The BLBP perceptron engine, tested against the PAPER'S OWN WORKED EXAMPLES.
//
// There is no reference implementation of BLBP to vendor or diff against, so
// these golden vectors from Garza et al. (ISCA 2019) are the only external
// ground truth the core math will ever meet. Figure 3 traces one weight vector
// through three predict/train steps with exact intermediate values; Figure 4
// pins the similarity semantics. If these pass, the arithmetic at the heart of
// the predictor is the paper's; everything above it is wiring, which the
// adapter tests and the invariance gate cover.
//
// The examples run against the ENGINE layer (explicit candidate injection, M=1
// so row selection is degenerate, identity transfer function) -- the layering
// exists precisely so the paper's tiny examples are directly encodable.

namespace
{
// Figure 3's setup: one sub-predictor, 4 target bits, one weight row,
// identity transfer, high fixed threshold (the example trains on every step,
// including the correct third prediction -- "reinforcement").
champsim::blbp::engine_config fig3_config()
{
  champsim::blbp::engine_config c;
  c.K = 4;
  c.N = 1;
  c.M = 1;
  c.weight_max = 7;
  c.transfer = {0, 1, 2, 3, 4, 5, 6, 7}; // identity: Figure 3 uses raw weights
  c.theta_init = 10;                     // > |y_out| throughout the example
  c.adaptive_theta = false;              // fixed threshold in the example
  c.selective_bits = false;              // the example trains every bit
  return c;
}

const std::vector<uint8_t> target1{0, 1, 0, 1}; // Fig 3's target_1 = 0101
const std::vector<uint8_t> target2{1, 0, 1, 1}; // Fig 3's target_2 = 1011
} // namespace

TEST_CASE("Figure 3, prediction 1: weights 3333 pick target2 and train to 2424")
{
  champsim::blbp::engine e{fig3_config()};
  e.set_weights_for_test(0, 0, {3, 3, 3, 3});

  const auto y = e.compute_yout(/*indices*/ {0});
  REQUIRE(y == std::vector<int>{3, 3, 3, 3});

  // Dot products with 0/1 bit semantics: P1 = 3+3 = 6, P2 = 3+3+3 = 9.
  REQUIRE(e.similarity(y, target1) == 6);
  REQUIRE(e.similarity(y, target2) == 9);
  REQUIRE(e.select(y, {target1, target2}) == 1); // target2 wins

  // Actual was target1: train. Bits 0101: w -= 1 on 0-bits, += 1 on 1-bits.
  e.train({0}, y, target1, /*mispredicted*/ true, {target1, target2});
  REQUIRE(e.weights_for_test(0, 0) == std::vector<int>{2, 4, 2, 4});
}

TEST_CASE("Figure 3, prediction 2: the 8-vs-8 tie goes to the LATER candidate")
{
  // Algorithm 1 as printed uses strict '>', under which the first candidate
  // would keep a tie -- but Figure 3 explicitly shows P1 = 8 <= P2 = 8
  // resolving to target2. We follow the figure. This test pins that choice so
  // nobody "fixes" it back to the pseudocode reading silently.
  champsim::blbp::engine e{fig3_config()};
  e.set_weights_for_test(0, 0, {2, 4, 2, 4});

  const auto y = e.compute_yout({0});
  REQUIRE(e.similarity(y, target1) == 8);
  REQUIRE(e.similarity(y, target2) == 8);
  REQUIRE(e.select(y, {target1, target2}) == 1); // tie -> later candidate

  e.train({0}, y, target1, true, {target1, target2});
  REQUIRE(e.weights_for_test(0, 0) == std::vector<int>{1, 5, 1, 5});
}

TEST_CASE("Figure 3, prediction 3: correct prediction still reinforces, converging to 0606")
{
  champsim::blbp::engine e{fig3_config()};
  e.set_weights_for_test(0, 0, {1, 5, 1, 5});

  const auto y = e.compute_yout({0});
  REQUIRE(e.similarity(y, target1) == 10);
  REQUIRE(e.similarity(y, target2) == 7);
  REQUIRE(e.select(y, {target1, target2}) == 0); // target1, correctly

  // Correct, but |y_k| = 1,5,1,5 all below theta = 10: reinforcement training.
  e.train({0}, y, target1, /*mispredicted*/ false, {target1, target2});
  REQUIRE(e.weights_for_test(0, 0) == std::vector<int>{0, 6, 0, 6});
}

TEST_CASE("Figure 4: similarity is the sum of y_out over the candidate's 1-bits")
{
  // y_out = (-1, 19, 10, 32). Paper: target (0,1,0,1) scores 0+19+0+32 = 51.
  // For (1,0,1,1) the figure prints 43 but -1+10+32 = 41 -- an arithmetic typo
  // in the paper; the semantics ("sum of the bitwise AND", section 3.7) give 41.
  // Either way target1 wins, which is the figure's conclusion.
  champsim::blbp::engine e{fig3_config()};
  const std::vector<int> y{-1, 19, 10, 32};

  REQUIRE(e.similarity(y, {0, 1, 0, 1}) == 51);
  REQUIRE(e.similarity(y, {1, 0, 1, 1}) == 41);
  REQUIRE(e.select(y, {{0, 1, 0, 1}, {1, 0, 1, 1}}) == 0);
}

TEST_CASE("Weights saturate at +/-7 and never wrap")
{
  auto cfg = fig3_config();
  champsim::blbp::engine e{cfg};
  e.set_weights_for_test(0, 0, {7, 7, -7, -7});

  // Push all four further in their saturated direction: 1-bits increment the
  // +7s, 0-bits decrement the -7s.
  const auto y = e.compute_yout({0});
  e.train({0}, y, {1, 1, 0, 0}, true, {});
  REQUIRE(e.weights_for_test(0, 0) == std::vector<int>{7, 7, -7, -7});
}

TEST_CASE("Selective bit training only touches bits that differ across candidates")
{
  // Candidates 0101 and 0111 agree on bits 0, 1 and 3; only bit 2 differs.
  // With selective training on, training must leave the agreeing bits' weights
  // alone -- the paper's defence against destructive aliasing (section 3.6).
  auto cfg = fig3_config();
  cfg.selective_bits = true;
  champsim::blbp::engine e{cfg};
  e.set_weights_for_test(0, 0, {3, 3, 3, 3});

  const auto y = e.compute_yout({0});
  e.train({0}, y, {0, 1, 0, 1}, true, {{0, 1, 0, 1}, {0, 1, 1, 1}});

  REQUIRE(e.weights_for_test(0, 0) == std::vector<int>{3, 3, 2, 3}); // only bit 2 trained
}

TEST_CASE("The transfer function amplifies weights before summation, preserving sign")
{
  auto cfg = fig3_config();
  cfg.transfer = {2, 4, 6, 8, 11, 14, 18, 24}; // Figure 5's curve
  champsim::blbp::engine e{cfg};
  e.set_weights_for_test(0, 0, {0, 3, -3, 7});

  // transfer(|w|) with w's sign: 0 -> +2? No: transfer applies to the weight's
  // magnitude and the sign is re-applied; a zero weight has no direction, so it
  // contributes transfer[0] with POSITIVE sign by the >= 0 convention.
  REQUIRE(e.compute_yout({0}) == std::vector<int>{2, 8, -8, 24});
}
