////////////////////////////////////////////////////////////////////////
//
//  Code submitted for the 6th Championship Branch Prediction (CBP2025)
//
//  Author: Alberto Ros (aros@ditec.um.es)
//
//  Paper #2: A Deep Dive Into TAGE-SC-L
//
//  Code: Useful classes and functions for branch prediction
//
//  Note: This code is derived from the TAGE-SC-L code by André Seznec
//    at the 5th Championship Branch Prediction and provided by the  
//    CBP2025 organizers as baseline.
//
////////////////////////////////////////////////////////////////////////

#ifndef UTILS_H
#define UTILS_H

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <unordered_map>
#include <vector>
#include <array>
#include <iostream>

using namespace std;

#define OPT_HIST_SQSE

#define FIX_LOOP_USEFUL
#define OPT_LOOP_CONF_FINE
#define OPT_CHOOSER
#define OPT_CHOOSE_LOOP
#define OPT_LOOP_PRED_INDEP
#define OPT_ALT2

#define OPT_TAGE_ALLOC
#define OPT_REPL_TAGE

#define FIX_LOOP_FAST_CHANGE
#define FIX_LOOP_AGE
#define OPT_HASHING
#define OPT_REDUC_SC

uint64_t getUniqueInstrId(uint64_t seq_no, uint8_t piece) {
  assert(piece < 16);
  return (seq_no << 4) | (piece & 0x000F);
}

uint8_t log2Int(uint16_t value) {
  uint8_t log;
  for (log = 15; log != 0; log--) {
    if (value & (1 << log)) break;
  }
  return log;
}

uint64_t hashShift(uint64_t pc, unsigned shift) { return pc ^ (pc >> shift); }

// up-down saturating counter
void ctrUpdate(int8_t& ctr, bool taken, int nbits) {
  if (taken && ctr < ((1 << (nbits - 1)) - 1)) ctr++;
  if (!taken && ctr > -(1 << (nbits - 1))) ctr--;
}

// up-down saturating counter
void ctrUpdate(int16_t& ctr, bool taken, int nbits) {
  if (taken && ctr < ((1 << (nbits - 1)) - 1)) ctr++;
  if (!taken && ctr > -(1 << (nbits - 1))) ctr--;
}

// up-down saturating counter for unsigned
void ctrUpdate(uint8_t& ctr, bool taken, int nbits) {
  if (taken && ctr < ((1 << nbits) - 1)) ctr++;
  if (!taken && ctr > 0) ctr--;
}

// Array of saturating counters
template <uint16_t LOG_SIZE = 13, uint8_t CTR_WIDTH = 2, typename T = int8_t>
class SatCtrArray {
public:
  static const int ENTRIES = 1 << LOG_SIZE;
  static const int MASK = ENTRIES - 1;
  SatCtrArray() { }
  SatCtrArray(int init) { for (int i = 0; i < ENTRIES; i++) ctrs[i] = init; }
  uint32_t size() {
    cout << "Counter: " << (int)CTR_WIDTH << " bits, " << ENTRIES << " entries & " << ((double)(CTR_WIDTH * ENTRIES)) / (8 * 1024) << " KB \\\\" << endl;
    return CTR_WIDTH * ENTRIES;
  }
  T getValue(uint64_t hash) { return ctrs[getIndex(hash)]; }
  void update(uint64_t hash, bool up) { ctrUpdate(ctrs[getIndex(hash)], up, CTR_WIDTH); }
private:
  uint32_t getIndex(uint64_t hash) { return hash & MASK; }
  array<T, ENTRIES> ctrs = {};
};

#define HISTBUFFERLENGTH 4096 // we use a 4K entries history buffer to store the branch history (this allows us to explore using history length up to 4K)

class folded_history {
public:
  unsigned comp;
  int CLENGTH;
  int OLENGTH;
  int OUTPOINT;

  folded_history() {}

  void init(int original_length, int compressed_length) {
    comp = 0;
    OLENGTH = original_length;
    CLENGTH = compressed_length;
    OUTPOINT = OLENGTH % CLENGTH;
  }

  void update(array<uint8_t, HISTBUFFERLENGTH>& h, int PT) {
    comp = (comp << 1) ^ h[PT & (HISTBUFFERLENGTH - 1)];
    comp ^= h[(PT + OLENGTH) & (HISTBUFFERLENGTH - 1)] << OUTPOINT;
    comp ^= (comp >> CLENGTH);
    comp = (comp) & ((1 << CLENGTH) - 1);
  }
};

#define NHIST 42 // Twice the number of different histories
#define BORN 9   // < BORN in the table for low hist lengths; >= BORN in the table for high lengths

using tage_index_t = array<folded_history, NHIST + 1>;
using tage_tag_t = array<folded_history, NHIST + 1>;

// Pseudo random number generator: use available information to allocate entries
uint64_t random_phist;
int random_ptghist;
uint64_t seed = 0;

int myRandom() {
  seed++;
  seed ^= random_phist;
  seed = (seed >> 21) + (seed << 11);
  seed ^= (int64_t)random_ptghist;
  seed = (seed >> 10) + (seed << 22);
  return (seed & 0xFFFFFFFF);
}

#endif // UTILS_H
