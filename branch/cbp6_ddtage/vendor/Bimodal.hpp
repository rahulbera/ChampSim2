////////////////////////////////////////////////////////////////////////
//
//  Code submitted for the 6th Championship Branch Prediction (CBP2025)
//
//  Author: Alberto Ros (aros@ditec.um.es)
//
//  Paper #2: A Deep Dive Into TAGE-SC-L
//
//  Code: Bimodal predictor with hysteresis bits
//
//  Note: This code is derived from the TAGE-SC-L code by André Seznec
//    at the 5th Championship Branch Prediction and provided by the  
//    CBP2025 organizers as baseline.
//
////////////////////////////////////////////////////////////////////////

#ifndef BIMODAL_H
#define BIMODAL_H

#include "Utils.hpp"

class Bimodal {

public:

  struct Entry {
    uint8_t pred = 0; // prediction bit
    uint8_t hyst = 1; // hysteresis bit
  };
    
  Bimodal() {
    table = new Entry[ENTRIES];
  }

  int size() const {
    cout << "Bimodal [bimodal::table] & Prediction: 2$^{" << (int)LOG_ENTRIES << "}$ 1-bit entries, hysteresis: 2$^{" << LOG_ENTRIES - HYST_SHIFT << "}$ 1-bit entries & " << ((double)(ENTRIES + (1 << (LOG_ENTRIES - HYST_SHIFT)))) / (8 * 1024) << " KB \\\\" << endl;  
    return ENTRIES + (1 << (LOG_ENTRIES - HYST_SHIFT));
  }

  bool predict(uint64_t pc, bool& conf) {
    uint32_t index = getIndex(pc);
    conf = (table[index].pred == table[index >> HYST_SHIFT].hyst);
    bool pred = table[index].pred;
    uint8_t ctr = getCtr(index);
    ctrUpdate(ctr, pred, 2); // Updated with the prediction
    return pred;
  }

  void update(uint64_t pc, bool taken) {
    uint32_t index = getIndex(pc);
    uint8_t ctr = getCtr(index);
    ctrUpdate(ctr, taken, 2);
    table[index].pred = ctr >> 1;
    table[index >> HYST_SHIFT].hyst = ctr & 1;
  }
  
private:

  uint32_t getIndex(uint64_t pc) const {
    return (pc ^ (pc >> 2)) & ENTRIES_MASK;
  }

  uint8_t getCtr(uint32_t index) const {
    return (table[index].pred << 1) + (table[index >> HYST_SHIFT].hyst);
  }

  static const uint8_t LOG_ENTRIES = 16; // Log of number of entries in bimodal predictor
  static const uint8_t HYST_SHIFT = 2;   // Bimodal hysteresis shared by 4 entries

  // Computed
  static const uint32_t ENTRIES = 1 << LOG_ENTRIES;
  static const uint32_t ENTRIES_MASK = ENTRIES - 1;

  Entry* table; // Bimodal table
};
#endif // BIMODAL_H
