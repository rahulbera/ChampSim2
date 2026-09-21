////////////////////////////////////////////////////////////////////////
//
//  Code submitted for the 6th Championship Branch Prediction (CBP2025)
//
//  Author: Alberto Ros (aros@ditec.um.es)
//
//  Paper #2: A Deep Dive Into TAGE-SC-L
//
//  Code: A loop predictor using a skewed cache
//
//  Note: This code is derived from the TAGE-SC-L code by André Seznec
//    at the 5th Championship Branch Prediction and provided by the  
//    CBP2025 organizers as baseline.
//
////////////////////////////////////////////////////////////////////////

#ifndef LOOPPREDICTOR_H
#define LOOPPREDICTOR_H

#include "Utils.hpp"

class LoopPredictor {

public:
    
  int size() {
    cout << "Loop " << (double)(ENTRIES * (TAG_WIDTH + 2 * NUMITER_WIDTH + CTR_WIDTH + AGE_WIDTH + 1)) / (8 * 1024) << endl;
    return ENTRIES * (TAG_WIDTH + 2 * NUMITER_WIDTH + CTR_WIDTH + AGE_WIDTH + 1);
  }

  bool predict(uint64_t pc, uint8_t& conf) {
    ui.hit = false;
    for (int i = 0; i < ASSOC; i++) {
      ui.index = getIndex(pc, i); // Skewed index
      if (table[ui.index].tag == getTag(pc, i)) {
        ui.hit = true;
#ifdef OPT_LOOP_CONF_FINE
        uint16_t good = table[ui.index].numIter * table[ui.index].ctr;
        if (good > 0) good--; // To be equivalent to the previous code
        ui.conf = conf = (table[ui.index].ctr == CTR_MAX) ? CONF_MAX : min(log2Int(good), CONF_MAX);
#else
        ui.conf = conf = ((table[ui.index].ctr == CTR_MAX) || (table[ui.index].ctr * table[ui.index].numIter > 128)) ? CONF_MAX : (table[ui.index].numIter ? 1 : 0);
#endif
        ui.pred = (table[ui.index].currentIter + 1 == table[ui.index].numIter) ? !table[ui.index].dir : table[ui.index].dir;
        break;
      }
    }
    return ui.pred;
  }

  void update(uint64_t pc, bool taken, bool pred, bool other_pred, int prob_alloc, bool used) {
    if (ui.hit) { // It was a hit in the loop table
      if (ui.conf == CONF_MAX && ui.pred != taken) { // Prediction was incorrect: free the entry
        table[ui.index].numIter = 0;
        table[ui.index].currentIter = 0;
        table[ui.index].ctr = 0;
        table[ui.index].age = 0;
      }
      
      if (ui.conf == CONF_MAX && ui.pred == taken
          && (ui.pred != other_pred || (myRandom() & 7) == 0)) { // Loop good, other not
        ctrUpdate(table[ui.index].age, true, AGE_WIDTH);
      }
      
      table[ui.index].currentIter++;
      table[ui.index].currentIter &= NUMITER_MASK; // Why not just to dealocate the entry?
      if (table[ui.index].currentIter > table[ui.index].numIter) { // Treat like the 1st encounter of the loop
        table[ui.index].numIter = 0;
        table[ui.index].ctr = 0;
      }
      
      if (taken != table[ui.index].dir) { // Change in the common direction
#ifdef FIX_LOOP_FAST_CHANGE
        if (table[ui.index].numIter == 0 && table[ui.index].currentIter == 1) { // Init, and was wrong, change direction
          table[ui.index].currentIter = 2;
          table[ui.index].dir = taken;
        } else
#endif
        if (table[ui.index].currentIter == table[ui.index].numIter) { // End of loop
          ctrUpdate(table[ui.index].ctr, true, CTR_WIDTH);
          if (table[ui.index].numIter < 3) { // Do not predict when the loop count is 1 or 2: free the entry
	    table[ui.index].numIter = 0;
	    table[ui.index].ctr = 0;
	    table[ui.index].age = 0;
	    table[ui.index].dir = taken;
            //table[ui.index].age = 0;
	  }
	  table[ui.index].currentIter = 0;
        } else {
          if (table[ui.index].numIter == 0) { // First complete nest
            table[ui.index].numIter = table[ui.index].currentIter;
            table[ui.index].currentIter = 0;
            table[ui.index].ctr = 0;
          } else { // Not the same number of iterations as last time: free the entry or variable loop
            table[ui.index].numIter = 0;
            table[ui.index].currentIter = 0;
            table[ui.index].ctr = 0;
#ifdef FIX_LOOP_AGE
            table[ui.index].age = 0;
#endif
          }
        }
      }

      } else if (pred != taken) { // Not present and misprediction
      int alloc_way = myRandom() & 3;
      int index = getIndex(pc, alloc_way);
      int X = (myRandom() & prob_alloc);
      if (X == 0) { // Allocate
        if (table[index].age == 0) {
          table[index].tag = getTag(pc, alloc_way);
          table[index].numIter = 0;
          table[index].currentIter = 0;
          table[index].ctr = 0;
          table[index].age = AGE_HALF;
          table[index].dir = !taken; // Most of mispredictions are on last iterations
        } else {
          table[index].age--;
        }
      }
    }
  }

private:

  uint16_t getIndex(uint64_t pc, int way) {
    int li = (((pc ^ (pc >> 2)) & ((1 << LOG_SETS) - 1)) << 2);
    int lib = ((pc >> LOG_SETS) & ((1 << LOG_SETS) - 1));
    return (li ^ ((lib >> way) << LOG_ASSOC)) + way;
  }

  uint16_t getTag(uint64_t pc, int way) {
    return ((pc ^ (pc >> TAG_WIDTH)) >> LOG_SETS) & TAG_MASK;
  }
  
  static const int LOG_ENTRIES = 6;
  static const int LOG_ASSOC = 2;
  static const int NUMITER_WIDTH = 10; // Predict only loops with less than 4K iterations
  static const int TAG_WIDTH = 10;
  static const int CTR_WIDTH = 4;
  static const int AGE_WIDTH = 4;
  
  // Computed
  static const int ENTRIES = 1 << LOG_ENTRIES;
  static const int ASSOC = 1 << LOG_ASSOC;
  static const int LOG_SETS = LOG_ENTRIES - LOG_ASSOC;
  static const int SET_MASK = (1 << LOG_SETS) - 1;
  static const int NUMITER_MASK = (1 << NUMITER_WIDTH) - 1;
  static const int TAG_MASK = (1 << TAG_WIDTH) - 1;
  static const int CTR_MAX = (1 << CTR_WIDTH) - 1;
  static const int AGE_HALF = (1 << (AGE_WIDTH - 1)) - 1;
  static const int AGE_MAX = (1 << AGE_WIDTH) - 1;
  // ChampSim deviation 4 of 4: `const` -> `constexpr`.
  //
  // CONF_MAX is passed to std::min, which takes its arguments BY REFERENCE and
  // therefore odr-uses it, so a `static const` member declared in-class needs an
  // out-of-class definition that the submission never provides. It links anyway
  // at -O3, where std::min is inlined and the reference never materialises, and
  // fails to link at lower optimisation levels -- which is how ChampSim's test
  // binary found it while the simulator binaries built cleanly.
  //
  // `static constexpr` is implicitly inline in C++17, so this supplies the
  // definition without adding one. Same type, same value, no behaviour change:
  // verified by rebuilding and reproducing the sweep numbers bit-identically.
  static constexpr uint8_t CONF_MAX = 7;
  
  struct Entry {
    uint16_t tag = 0;         // TAG_WIDTH bits
    uint16_t numIter = 0;     // NUMITER_WIDTH bits
    uint16_t currentIter = 0; // NUMITER_WIDTH bits
    uint8_t ctr = 0;          // CTR_WIDTH bits
    uint8_t age = 0;          // AGE_WIDTH bits
    bool dir = false;         // 1 bit
  };
  
  struct UpdateInfo {
    bool hit;       // hit in table
    uint64_t index; // hitting entry
    uint8_t conf;   // confidence of prediction
    bool pred;      // prediction
  };

  UpdateInfo ui; // status per branch

  Entry table[ENTRIES];     // skewed associative  
};
#endif // LOOPPREDICTOR_H
