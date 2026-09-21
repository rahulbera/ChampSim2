/*
 * Replays a dumped CBP6 call log into a bare tenant predictor, outside ChampSim.
 *
 * What it checks: driving a fresh tenant with exactly the dumped call sequence
 * reproduces every prediction the in-simulator tenant made, bit for bit. It does
 * NOT re-run the protocol checker -- that runs live in the simulator, behind
 * CBP6_PROTOCOL_CHECK.
 *
 * That establishes that the tenant's output is a pure function
 * of the adapter's call sequence -- that nothing leaks in from ChampSim state,
 * and that a dumped log is a complete description of a run. That makes the log a
 * regression oracle: refactor the adapter, replay an old log, and any behavioural
 * change shows up as a mismatch.
 *
 * Deliberately outside the ChampSim build. Anything under branch/ is compiled
 * and linked into every simulator binary, so a file with main() there would
 * collide at link time.
 *
 * Usage: cbp6_replay <call-log>
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

// Hoisted so the vendored header's own includes become no-ops inside the
// anonymous namespace below -- same technique the module uses.
#include <array>
#include <assert.h>
#include <inttypes.h>
#include <iostream>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unordered_map>
#include <vector>

#include "cbp6/cbp6_call_record.h"

namespace
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-function"
#include "cbp2016_tage_sc_l.hpp"
#pragma GCC diagnostic pop
} // namespace

using champsim::cbp6::call_kind;
using champsim::cbp6::call_record;

int main(int argc, char** argv)
{
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <call-log>\n", argv[0]);
    return 2;
  }

  std::ifstream in{argv[1], std::ios::binary};
  if (!in) {
    std::fprintf(stderr, "cannot open %s\n", argv[1]);
    return 2;
  }

  // `static` is load-bearing, not style. cbp_hist_t has a user-provided default
  // constructor that initializes only ltable and WITHLOOP, leaving IMLIcount,
  // GHIST, phist and others indeterminate -- and because that constructor is
  // user-provided, `T{}` does not zero them either. Only static storage
  // guarantees zero-initialization before the constructor runs.
  //
  // As an automatic local, the first predict() indexes IMHIST[256] with a
  // garbage IMLIcount and segfaults. The ChampSim module is safe for the same
  // reason: champsim::cbp6::host lives in a function-local static.
  static CBP2016_TAGE_SC_L tenant;
  tenant.setup();

  std::size_t records = 0;
  std::size_t predictions = 0;
  std::size_t mismatches = 0;
  std::size_t first_mismatch_at = 0;

  call_record rec{};
  while (in.read(reinterpret_cast<char*>(&rec), sizeof(rec))) { // NOLINT
    ++records;

    switch (rec.kind) {
    case call_kind::predict: {
      const bool replayed = tenant.predict(rec.seq_no, 0, rec.pc);
      ++predictions;
      if (replayed != rec.pred_dir) {
        if (mismatches == 0) {
          first_mismatch_at = records;
        }
        ++mismatches;
      }
      break;
    }
    case call_kind::history_update:
      tenant.history_update(rec.seq_no, 0, rec.pc, rec.brtype, rec.pred_dir, rec.resolve_dir, rec.next_pc);
      break;
    case call_kind::update:
      tenant.update(rec.seq_no, 0, rec.pc, rec.resolve_dir, rec.pred_dir, rec.next_pc);
      break;
    case call_kind::track_other:
      tenant.TrackOtherInst(rec.pc, rec.brtype, rec.pred_dir, rec.resolve_dir, rec.next_pc);
      break;
    }
  }

  std::printf("replayed %zu records, %zu predictions\n", records, predictions);

  // An empty or truncated log must not report success: a green exit on a log
  // containing nothing is worse than no oracle at all.
  if (predictions == 0) {
    std::printf("RESULT: FAIL -- the log contained no predictions (empty, truncated, or capped dump?)\n");
    return 1;
  }
  if (!in.eof()) {
    std::printf("RESULT: FAIL -- the log ends with a partial record; it was truncated mid-write\n");
    return 1;
  }

  if (mismatches == 0) {
    std::printf("RESULT: OK -- every replayed prediction matched the simulator\n");
    return 0;
  }

  std::printf("RESULT: MISMATCH -- %zu of %zu predictions differed (first at record %zu)\n", mismatches, predictions, first_mismatch_at);
  return 1;
}
