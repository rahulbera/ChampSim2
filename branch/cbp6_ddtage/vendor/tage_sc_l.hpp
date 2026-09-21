////////////////////////////////////////////////////////////////////////
//
//  Code submitted for the 6th Championship Branch Prediction (CBP2025)
//
//  Author: Alberto Ros (aros@ditec.um.es)
//
//  Paper #2: A Deep Dive Into TAGE-SC-L
//
//  Code: The TAGE-SC-L predictor
//
//  Note: This code is derived from the TAGE-SC-L code by André Seznec
//    at the 5th Championship Branch Prediction and provided by the  
//    CBP2025 organizers as baseline.
//
////////////////////////////////////////////////////////////////////////

#ifndef _TAGE_PREDICTOR_H_
#define _TAGE_PREDICTOR_H_

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

#include "Bimodal.hpp"
#include "LoopPredictor.hpp"

// ChampSim deviation 3 of 3: PRINTSIZE disabled.
// The constructor calls predictorsize(), which writes a LaTeX storage table to
// stdout and a summary to stderr. config.sh links every module into every
// binary, so with this enabled the table is emitted at static-init time by any
// ChampSim executable, whether or not DD-TAGE is the selected predictor -- and
// on stdout, where it corrupts --json output. The budget it reports was checked
// once out-of-tree and is 191.900269 KB, matching the 191.90 KB in the
// submission.
// #define PRINTSIZE     // To get the predictor storage budget

#define LOOPPREDICTOR // Enable the loop predictor 
#define SC            // Enable statistical corrector
#define IMLI          // Enable inner-most loop iteration (IMLI-SIC)
#define LOCALH        // Enable the 1st local history
#ifdef LOCALH         
#define LOCALS        // Enable the 2nd local history
#define LOCALT        // Enable the 3rd local history
#endif

Bimodal bimodal;

#ifdef LOOPPREDICTOR
LoopPredictor loop_predictor;
#endif

#define LOGFLOCAL 9
#define NFLOCAL (1 << LOGFLOCAL) // Number of 1st local histories
#define LOGSLOCAL 6
#define NSLOCAL (1 << LOGSLOCAL) // Number of 2nd local histories
#define LOGTLOCAL 5
#define NTLOCAL (1 << LOGTLOCAL) // Number of 3rd local histories

// The statistical corrector components

#define PERCWIDTH 6 // Statistical corrector counter width
// The three BIAS tables in the SC component
// We play with the TAGE confidence here, with the number of the hitting bank
#define LOGBIAS 9
int8_t Bias[(1 << LOGBIAS)];
int8_t BiasSK[(1 << LOGBIAS)];
int8_t BiasBank[(1 << LOGBIAS)];

// In all the GEHL components, the two tables with the shortest history lengths have only half of the entries.

#ifdef IMLI
#define LOGINB 9 
#define INB 1
int Im[INB] = {8};
int8_t IGEHLA[INB][(1 << LOGINB)] = {{0}};
int8_t* IGEHL[INB];

#define LOGIMNB 10 
#define IMNB 2
int IMm[IMNB] = {10, 4};
int8_t IMGEHLA[IMNB][(1 << LOGIMNB)] = {{0}};
int8_t* IMGEHL[IMNB];
#endif

// Global branch GEHL
#define LOGGNB 11 
#define GNB 3
int Gm[GNB] = {40, 24, 10};
int8_t GGEHLA[GNB][(1 << LOGGNB)] = {{0}};
int8_t* GGEHL[GNB];

// Variation on global branch history
#define LOGPNB 11 
#define PNB 3
int Pm[PNB] = {25, 16, 9};
int8_t PGEHLA[PNB][(1 << LOGPNB)] = {{0}};
int8_t* PGEHL[PNB];

// 1st local history
#define LOGLNB 11 
#define LNB 3
int Lm[LNB] = {11, 6, 3};
int8_t LGEHLA[LNB][(1 << LOGLNB)] = {{0}};
int8_t* LGEHL[LNB];

// 2nd local history
#define LOGSNB 10 
#define SNB 3
int Sm[SNB] = {16, 11, 6};
int8_t SGEHLA[SNB][(1 << LOGSNB)] = {{0}};
int8_t* SGEHL[SNB];

// 3rd local history
#define LOGTNB 11 
#define TNB 2
int Tm[TNB] = {9, 4};
int8_t TGEHLA[TNB][(1 << LOGTNB)] = {{0}};
int8_t* TGEHL[TNB];

// playing with putting more weights (x2) on some of the SC components
// playing on using different update thresholds on SC
// update threshold for the statistical corrector
#define VARTHRES
#define WIDTHRES 12
#define WIDTHRESP 8
#ifdef VARTHRES
#define LOGSIZEUP 6 // not worth increasing
#else
#define LOGSIZEUP 0
#endif
#define INDUPD (pc ^ (pc >> 2)) & ((1 << LOGSIZEUP) - 1)
int16_t updatethreshold;
int16_t Pupdatethreshold[(1 << LOGSIZEUP)]; // size is fixed by LOGSIZEUP

#define LOGSIZEUPS (LOGSIZEUP / 2)
#define EWIDTH 6
#define INDUPDS ((pc ^ (pc >> 2)) & ((1 << (LOGSIZEUPS)) - 1))
int8_t WB[(1 << LOGSIZEUPS)];
int8_t WG[(1 << LOGSIZEUPS)];
int8_t WP[(1 << LOGSIZEUPS)];
int8_t WL[(1 << LOGSIZEUPS)];
int8_t WS[(1 << LOGSIZEUPS)];
int8_t WT[(1 << LOGSIZEUPS)];
int8_t WI[(1 << LOGSIZEUPS)];
int8_t WIM[(1 << LOGSIZEUPS)];

// The two counters used to choose between TAGE and SC on Low Conf SC
#define CONFWIDTH 7 // for the counters in the chooser
int8_t FirstH, SecondH;

class gentry { // TAGE global table entry
public:
  uint tag = 0;
  int8_t ctr = 0;
  uint8_t u = 0;
};

#define NBANKLOW 14  // number of banks in the shared bank-interleaved for low history lengths
#define NBANKHIGH 30 // number of banks in the shared bank-interleaved for high history lengths
int SizeTable[NHIST + 1];
bool NOSKIP[NHIST + 1]; // to manage the associativity for different history lengths

#define BORNTICK (1024 + 512)
int tick = 0; // for the reset of the u counter

#define NNN 2 // number of extra entries allocated on a TAGE misprediction (1+NNN)

#define PHISTWIDTH 27     // Width of the path history used in TAGE and SC
#define LOGG 11           // Logsize of the  banks in the tagged TAGE tables
#define TAG_WIDTH_LOW 9   // Width of the tags for low history banks
#define TAG_WIDTH_HIGH 12 // Width of the tags for high history banks
#define CWIDTH 3          // Predictor counter width on the TAGE tagged tables
#define C_MAX ((1 << (CWIDTH - 1)) - 1)
#define UWIDTH 1          // u counter width on TAGE (2 bits not worth the effort)

// For the TAGE predictor
gentry* gtable[NHIST + 1]; // tagged TAGE tables
int m[NHIST + 1];
int tag_width[NHIST + 1];
int logg[NHIST + 1];

// Choosers among predictors
// Counters to choose between longest match and alternate prediction on TAGE when weak confidence
#ifdef OPT_CHOOSER
#define USE_ALT_HASH (((min((ui.hit_bank - 1) >> 3, 7)) << 5) | (ui.alt_conf << 3) | ((ui.alt_bank > 0) << 2) | (ui.hit_conf & 1)) // hit_conf can only be 0 or 1
SatCtrArray<8, 6> use_alt_vs_hit;
#else
#define USE_ALT_HASH ((((ui.hit_bank - 1) >> 3) << 1) | (ui.alt_conf > 0))
SatCtrArray<4, 5> use_alt_vs_hit;
#endif

#define USE_SC_HASH ((((ui.hit_conf >= 2) ? ui.hit_conf : ((use_alt_vs_hit.getValue(USE_ALT_HASH) >= 0) ? 0 : 1)) << 2) | ((abs(ui.lsum) < (ui.thres >> 2)) ? 0 : ((abs(ui.lsum) < (ui.thres >> 1)) ? 1 : 2))) // best
SatCtrArray<4, 7> use_sc_vs_tage;

#ifdef OPT_LOOP_CONF_FINE
#define USE_LOOP_HASH ((((ui.loop_conf << 1) | (ui.hit_conf == C_MAX)) << 1) | (use_sc_vs_tage.getValue(USE_SC_HASH) >= 0))
SatCtrArray<5, 7> use_loop_vs_tagesc; // Counters to monitor whether loop prediction is beneficial
#else
#define USE_LOOP_HASH hashShift(pc, 2)
SatCtrArray<0, 7> use_loop_vs_tagesc; // counters to monitor whether loop prediction is beneficial
#endif

enum Component { BIM_COMP, LOOP_COMP, HIT_COMP, ALT_COMP, ALT2_COMP, SC_COMP, NUM_COMP };

int predictorsize() {
  int size = 0;
  int inter = 0;
  
  size += bimodal.size();
  
  cout << "TAGE [gtable] & Low-history tables $\\rightarrow$ Tag: " << tag_width[1] << " bits, conf: " << (int)CWIDTH << " bits, use: " << UWIDTH << " bit, 2$^{" << logg[1] << "} \\times$ " << NBANKLOW << " entries & " << (double)(NBANKLOW * (1 << (logg[1])) * (CWIDTH + UWIDTH + tag_width[1])) / (8 * 1024) << " KB \\\\" << endl;
  cout << "TAGE [gtable] & High-history tables $\\rightarrow$ Tag: " << tag_width[BORN] << " bits, conf: " << (int)CWIDTH << " bits, use: " << UWIDTH << " bit, 2$^{" << logg[BORN] << "} \\times$ " << NBANKHIGH << " entries & " << (double)(NBANKHIGH * (1 << (logg[BORN])) * (CWIDTH + UWIDTH + tag_width[BORN])) / (8 * 1024) << " KB \\\\" << endl;
  size += NBANKHIGH * (1 << (logg[BORN])) * (CWIDTH + UWIDTH + tag_width[BORN]);
  size += NBANKLOW * (1 << (logg[1])) * (CWIDTH + UWIDTH + tag_width[1]);

  cout << "TAGE [gtable] & Global history register: " << m[NHIST] << ", path history register: " << PHISTWIDTH << ", decrease replacement counter: " << log2Int(BORNTICK) + 1 << " & " << (double)(m[NHIST] + PHISTWIDTH + log2Int(BORNTICK) + 1) / (8 * 1024) << " KB \\\\" << endl;  
  size += m[NHIST];
  size += PHISTWIDTH;
  size += log2Int(BORNTICK) + 1;
  
  size += use_alt_vs_hit.size();
  fprintf(stderr, "  (TAGE %d bits, %f Kbits, %f KB)\n", size, size/1024.0, size/(1024.0*8));

#ifdef LOOPPREDICTOR
  inter = loop_predictor.size() + use_loop_vs_tagesc.size();
  fprintf(stderr, "  (LOOP %d bits, %f Kbits, %f KB)\n", inter, inter/1024.0, inter/(1024.0*8));
  size += inter;
#endif

#ifdef SC
  cout << "Thresholds and sum weights & " << ((double)WIDTHRES + (WIDTHRESP * ((1 << LOGSIZEUP))) + (3 * EWIDTH * (1 << LOGSIZEUPS))) / (8 * 1024) << " KB \\\\" << endl;
  inter += WIDTHRES;
  inter += WIDTHRESP * ((1 << LOGSIZEUP));  // the update threshold counters
  inter += 3 * EWIDTH * (1 << LOGSIZEUPS); // the extra weight of the partial sums

  cout << "Three bias tables & " << ((double)(PERCWIDTH) * 3 * (1 << (LOGBIAS))) / (8 * 1024) << " KB \\\\" << endl;  
  inter += (PERCWIDTH) * 3 * (1 << (LOGBIAS));

  cout << "Global history table & " << ((double)((GNB - 2) * (1 << (LOGGNB)) * (PERCWIDTH) + (1 << (LOGGNB - 1)) * (2 * PERCWIDTH)) + Gm[0]) / (8 * 1024) << " KB \\\\" << endl;  
  inter += (GNB - 2) * (1 << (LOGGNB)) * (PERCWIDTH) + (1 << (LOGGNB - 1)) * (2 * PERCWIDTH);
  inter += Gm[0]; // global histories for SC
  
  cout << "Path history table & " << ((double)((PNB - 2) * (1 << (LOGPNB)) * (PERCWIDTH) + (1 << (LOGPNB - 1)) * (2 * PERCWIDTH))) / (8 * 1024) << " KB \\\\" << endl;  
  inter += (PNB - 2) * (1 << (LOGPNB)) * (PERCWIDTH) + (1 << (LOGPNB - 1)) * (2 * PERCWIDTH);
  // we use phist already counted for these tables

#ifdef LOCALH
  cout << "Fist local history table & " << ((double)((LNB - 2) * (1 << (LOGLNB)) * (PERCWIDTH) + (1 << (LOGLNB - 1)) * (2 * PERCWIDTH)) + NFLOCAL * Lm[0] + EWIDTH * (1 << LOGSIZEUPS)) / (8 * 1024) << " KB \\\\" << endl;  
  inter += (LNB - 2) * (1 << (LOGLNB)) * (PERCWIDTH) + (1 << (LOGLNB - 1)) * (2 * PERCWIDTH);
  inter += NFLOCAL * Lm[0];
  inter += EWIDTH * (1 << LOGSIZEUPS);
#ifdef LOCALS
  cout << "Second local history table & " << ((double)((SNB - 2) * (1 << (LOGSNB)) * (PERCWIDTH) + (1 << (LOGSNB - 1)) * (2 * PERCWIDTH)) + NSLOCAL * Sm[0] + EWIDTH * (1 << LOGSIZEUPS)) / (8 * 1024) << " KB \\\\" << endl;  
  inter += (SNB - 2) * (1 << (LOGSNB)) * (PERCWIDTH) + (1 << (LOGSNB - 1)) * (2 * PERCWIDTH);
  inter += NSLOCAL * Sm[0];
  inter += EWIDTH * (1 << LOGSIZEUPS);
#ifdef LOCALT
  cout << "Third local history table & " << ((double)((TNB - 2) * (1 << (LOGTNB)) * (PERCWIDTH) + (1 << (LOGTNB - 1)) * (2 * PERCWIDTH)) + NTLOCAL * Tm[0] + EWIDTH * (1 << LOGSIZEUPS)) / (8 * 1024) << " KB \\\\" << endl;  
  inter += (TNB - 2) * (1 << (LOGTNB)) * (PERCWIDTH) + (1 << (LOGTNB - 1)) * (2 * PERCWIDTH);
  inter += NTLOCAL * Tm[0];
  inter += EWIDTH * (1 << LOGSIZEUPS);
#endif
#endif
#endif

#ifdef IMLI
#ifndef OPT_REDUC_SC
  cout << "IMLI table & " << ((double)((1 << (LOGINB - 1)) * PERCWIDTH) + Im[0] + EWIDTH * (1 << LOGSIZEUPS)) / (8 * 1024) << " KB \\\\" << endl;  
  inter += (1 << (LOGINB - 1)) * PERCWIDTH;
  inter += Im[0];
  inter += EWIDTH * (1 << LOGSIZEUPS); // the extra weight of the partial sums
#endif
  cout << "IMLI table & " << ((double)((IMNB * (1 << (LOGIMNB - 1)) * PERCWIDTH) + 256 * IMm[0] + EWIDTH * (1 << LOGSIZEUPS))) / (8 * 1024) << " KB \\\\" << endl;  
  inter += IMNB * (1 << (LOGIMNB - 1)) * PERCWIDTH;
  inter += 256 * IMm[0];
  inter += EWIDTH * (1 << LOGSIZEUPS); // the extra weight of the partial sums
#endif

#ifdef OPT_CHOOSER
//  inter += (2 + 4 + 16) * 3 * CONFWIDTH; // the counters in the chooser
  inter += use_sc_vs_tage.size(); // the counters in the chooser
#else
  inter += 2 * CONFWIDTH; // the 2 counters in the chooser
#endif
  fprintf(stderr, "  (SC   %d bits, %f Kbits, %f KB)\n", inter, inter/1024.0, inter/(1024.0*8));
  size += inter;  
#endif
  
#ifdef PRINTSIZE
  fprintf(stderr, " (TOTAL %d bits %f Kbits %f KB) ", size, size/1024.0, size/(1024.0*8));
  fprintf(stdout, " (TOTAL %d bits %f Kbits %f KB) ", size, size/1024.0, size/(1024.0*8));
#endif

  return size;
}

// The interface to the simulator is defined in cond_branch_predictor_interface.cc
// This predictor is a modified version of CBP2016 Tage.
// The CBP Tage predicted and updated the predictor right away.
// The simulator here provides 3 major hooks for the predictor:
// * get_cond_dir_prediction -> lookup the predictor and return the prediction.  This is invoked only for conditional branches.
// * spec_update -> This is used for updating the history. It provides the actual direction of the branch. This is invoked for all branches.
// * notify_instr_execute_resolve -> This hook is used to update the predictor. This is invoked for all the instructions and provides all information available
// at execute.
//    * Note: The history at update is different than history at predict. To ensure that the predictor is getting trained correctly,
//    at predict, we checkpoint the history in an unordered_map(update_info_inflight) using unique identifying id of the instruction.
//    When updating the predicor, we recover the prediction time history.
class CBP2016_TAGE_SC_L {
public:

  struct UpdateInfo {
    bool pred; // global pred
    bool tage_pred; // tage pred
    bool hit_pred;  // longest history bank tage pred
    bool alt_pred;  // second longest history (alternate) tage pred
    bool alt2_pred;  // maximun confidence bank tage pred
    bool loop_pred; // loop pred
    bool tage_loop_pred; // tage + loop pred
    bool tage_sc_pred; // tage + sc pred

    bool use_alt;
    bool use_sc;
    bool use_loop;
    Component comp;

    // State set in predict and used in update
    int GI[NHIST + 1];    // Indexes to the different tables are computed only once
    uint GTAG[NHIST + 1]; // Tags for the different tables are computed only once
    int hit_bank; // Longest matching bank
    int alt_bank; // Alternate matching bank
    int alt2_bank;  // Longest max confidence matching bank

    // Confidence
    int thres;
    bool bim_conf;
    uint8_t hit_conf;
    uint8_t alt_conf; // Confidence on the alternate prediction
    uint8_t alt2_conf;
    uint8_t loop_conf;
    int lsum; // lsum >= 0 is sc pred
    
    // Begin Conventional Histories
    uint64_t GHIST;
    std::array<uint8_t, HISTBUFFERLENGTH> ghist;
    uint64_t phist; // Path history
    int ptghist;
    tage_index_t ch_i;
    std::array<tage_tag_t, 2> ch_t;

    std::array<uint64_t, NFLOCAL> L_shist;
    std::array<uint64_t, NSLOCAL> S_slhist;
    std::array<uint64_t, NTLOCAL> T_slhist;

    std::array<uint64_t, 256> IMHIST;
    uint64_t IMLIcount; // Used to monitor the iteration number
    
#ifdef LOOPPREDICTOR
    LoopPredictor loop_predictor;
#ifdef OPT_LOOP_CONF_FINE
    SatCtrArray<5, 7> use_loop_vs_tagesc; // Counters to monitor if loop prediction is beneficial
#else
    SatCtrArray<0, 7> use_loop_vs_tagesc; // Counters to monitor if loop prediction is beneficial
#endif
    
#endif
    
  };

  UpdateInfo ui; // Running history always updated accurately

  // Checkpointed history. Can be accesed using the inst-id(seq_no/piece)
  std::unordered_map<uint64_t /*key*/, UpdateInfo /*val*/> update_info_inflight;
  
  CBP2016_TAGE_SC_L(void) {
    init(ui);
#ifdef PRINTSIZE
    predictorsize();
#endif
  }

  void setup() {}

  void init(UpdateInfo& current_hist) {
#ifndef OPT_HIST_SQSE
#define MINHIST 6 // not optimized so far
#define MAXHIST 3000
    m[1] = MINHIST;
    m[NHIST / 2] = MAXHIST;
    for (int i = 2; i <= NHIST / 2; i++) {
      m[i] = (int)(((double)MINHIST * pow((double)(MAXHIST) / (double)MINHIST, (double)(i - 1) / (double)(((NHIST / 2) - 1)))) + 0.5);
    }
#else
#define CHGHIST 15 // Point where the history changes from squares to super-exponential 
    for (int i = 1; i < CHGHIST; i++) {
      m[i] = (i * i) + 1; // Very close without the +1, perhaps try also higher histories at the high end
      //m[i] = (i-1)*(i-1);
    }
    double init = 1.2, inc = 0.1;
    for (int i = CHGHIST; i <= NHIST / 2; i++) {
      m[i] = (int)round(m[i-1]*init);
      init += inc;
    } 
#endif    
    for (int i = NHIST; i > 1; i--) {
      m[i] = m[(i + 1) / 2];
    }

#ifndef OPT_HIST_SQSE
#define BORNINFASSOC 9 // 2-way associativity for the medium history lengths
#define BORNSUPASSOC 23
    for (int i = 1; i <= NHIST; i++) {
      NOSKIP[i] = !(i & 1) || ((i >= BORNINFASSOC) & (i < BORNSUPASSOC));
    }
    // Just eliminate some extra tables (very very marginal)
    NOSKIP[4] = 0;
    NOSKIP[NHIST - 2] = 0;
    NOSKIP[8] = 0;
    NOSKIP[NHIST - 6] = 0;
#else
    for (int i = 1; i <= NHIST; i++) {
      NOSKIP[i] = !(i & 1);
    }
#endif
        
    for (int i = 1; i <= NHIST; i++) {
      tag_width[i] = (i < BORN) ? TAG_WIDTH_LOW : TAG_WIDTH_HIGH;
      logg[i] = LOGG;
    }
    gtable[1] = new gentry[NBANKLOW * (1 << LOGG)];
    SizeTable[1] = NBANKLOW * (1 << LOGG);
    for (int i = 2; i < BORN; i++) {
      gtable[i] = gtable[1];
    }
    gtable[BORN] = new gentry[NBANKHIGH * (1 << LOGG)];
    SizeTable[BORN] = NBANKHIGH * (1 << LOGG);
    for (int i = BORN + 1; i <= NHIST; i++) {
      gtable[i] = gtable[BORN];
    }
      
    for (int i = 1; i <= NHIST; i++) {
      current_hist.ch_i[i].init(m[i], (logg[i]));
      current_hist.ch_t[0][i].init(current_hist.ch_i[i].OLENGTH, tag_width[i]);
      current_hist.ch_t[1][i].init(current_hist.ch_i[i].OLENGTH, tag_width[i] - 1);
    }

    current_hist.phist = 0;

    for (int i = 0; i < HISTBUFFERLENGTH; i++)
      current_hist.ghist[i] = 0;
    current_hist.ptghist = 0;
    
    updatethreshold = 35 << 3;
    for (int i = 0; i < (1 << LOGSIZEUP); i++)
      Pupdatethreshold[i] = 0;

    for (int j = 0; j < (1 << LOGBIAS); j++) {
      switch (j & 3) {
      case 0:
        Bias[j] = -32;
        break;
      case 1:
        Bias[j] = 31;
        break;
      case 2:
        Bias[j] = -1;
        break;
      case 3:
        Bias[j] = 0;
        break;
      }
    }
    
    for (int j = 0; j < (1 << LOGBIAS); j++) {
      switch (j & 3) {
      case 0:
        BiasSK[j] = -8;
        break;
      case 1:
        BiasSK[j] = 7;
        break;
      case 2:
        BiasSK[j] = -32;
        break;
      case 3:
        BiasSK[j] = 31;
        break;
      }
    }
        
    for (int j = 0; j < (1 << LOGBIAS); j++) {
      switch (j & 3) {
      case 0:
        BiasBank[j] = -32;
        break;
      case 1:
        BiasBank[j] = 31;
        break;
      case 2:
        BiasBank[j] = -1;
        break;
      case 3:
        BiasBank[j] = 0;
        break;
      }
    }
    
    for (int i = 0; i < GNB; i++)
      GGEHL[i] = &GGEHLA[i][0];
    for (int i = 0; i < GNB; i++)
      for (int j = 0; j < ((1 << LOGGNB) - 1); j++) {
        if (!(j & 1)) {
          GGEHL[i][j] = -1;
        }
      }

    for (int i = 0; i < PNB; i++)
      PGEHL[i] = &PGEHLA[i][0];
    for (int i = 0; i < PNB; i++)
      for (int j = 0; j < ((1 << LOGPNB) - 1); j++) {
        if (!(j & 1)) {
          PGEHL[i][j] = -1;
        }
      }

    for (int i = 0; i < LNB; i++)
      LGEHL[i] = &LGEHLA[i][0];
    for (int i = 0; i < LNB; i++)
      for (int j = 0; j < ((1 << LOGLNB) - 1); j++) {
        if (!(j & 1)) {
          LGEHL[i][j] = -1;
        }
      }

    for (int i = 0; i < SNB; i++)
      SGEHL[i] = &SGEHLA[i][0];    
    for (int i = 0; i < SNB; i++)
      for (int j = 0; j < ((1 << LOGSNB) - 1); j++) {
        if (!(j & 1)) {
          SGEHL[i][j] = -1;
        }
      }
    
    for (int i = 0; i < TNB; i++)
      TGEHL[i] = &TGEHLA[i][0];
    for (int i = 0; i < TNB; i++)
      for (int j = 0; j < ((1 << LOGTNB) - 1); j++) {
        if (!(j & 1)) {
          TGEHL[i][j] = -1;
        }
      }

#ifdef IMLI
    for (int i = 0; i < INB; i++)
      IGEHL[i] = &IGEHLA[i][0];
    for (int i = 0; i < INB; i++)
      for (int j = 0; j < ((1 << LOGINB) - 1); j++) {
        if (!(j & 1)) {
          IGEHL[i][j] = -1;
        }
      }
    for (int i = 0; i < IMNB; i++)
      IMGEHL[i] = &IMGEHLA[i][0];
    for (int i = 0; i < IMNB; i++)
      for (int j = 0; j < ((1 << LOGIMNB) - 1); j++) {
        if (!(j & 1)) {
          IMGEHL[i][j] = -1;
        }
      }
#endif
        
    for (int i = 0; i < (1 << LOGSIZEUPS); i++) {
      WB[i] = 4;
      WG[i] = 7;
      WP[i] = 7;
      WL[i] = 7;
      WS[i] = 7;
      WT[i] = 7;
      WI[i] = 7;
      WIM[i] = -7;
    }
    
    for (int i = 0; i < NFLOCAL; i++) {
      current_hist.L_shist[i] = 0;
    }
    for (int i = 0; i < NSLOCAL; i++) {
      current_hist.S_slhist[i] = 3;
    }
    current_hist.GHIST = 0;
    current_hist.ptghist = 0;
    current_hist.phist = 0;
  }
  
  // the index functions for the tagged tables uses path history as in the OGEHL predictor
  // F serves to mix path history: not very important impact
  int F(uint64_t A, int size, int bank) const {
    A = A & ((1 << size) - 1);
    int A1 = (A & ((1 << logg[bank]) - 1));
    int A2 = (A >> logg[bank]);
    if (bank < logg[bank])
      A2 = ((A2 << bank) & ((1 << logg[bank]) - 1)) + (A2 >> (logg[bank] - bank));
    A = A1 ^ A2;
    if (bank < logg[bank])
      A = ((A << bank) & ((1 << logg[bank]) - 1)) + (A >> (logg[bank] - bank));
    return A;
  }

  // gindex computes a full hash of PC, ghist and phist
  int gindex(unsigned int pc, int bank, uint64_t hist, const tage_index_t& ch_i) const {
    int M = (m[bank] > PHISTWIDTH) ? PHISTWIDTH : m[bank];
    int index = pc ^ (pc >> (abs(logg[bank] - bank) + 1)) ^ ch_i[bank].comp ^ F(hist, M, bank);
    return index & ((1 << (logg[bank])) - 1);
  }

  //  tag computation
  uint16_t gtag(unsigned int pc, int bank, const tage_tag_t& tag_0_array, const tage_tag_t& tag_1_array) const {
    int tag = (pc) ^ tag_0_array[bank].comp ^ (tag_1_array[bank].comp << 1);
    return (tag & ((1 << (tag_width[bank])) - 1));
  }

  // TAGE PREDICTION: same code at fetch or retire time but the index and tags must recomputed
  bool Tagepred(uint64_t pc) {
    bool pred;
    ui.hit_bank = 0;
    ui.alt_bank = 0;
    ui.alt2_bank = 0;

    for (int i = 1; i <= NHIST; i += 2) {
      ui.GI[i] = gindex(pc, i, ui.phist, ui.ch_i);
      ui.GTAG[i] = gtag(pc, i, ui.ch_t[0], ui.ch_t[1]);
      ui.GTAG[i + 1] = ui.GTAG[i];
      ui.GI[i + 1] = ui.GI[i] ^ (ui.GTAG[i] & ((1 << LOGG) - 1));
    }

    int T = (pc ^ (ui.phist & ((1ULL << m[BORN]) - 1))) % NBANKHIGH;
    for (int i = BORN; i <= NHIST; i++) {
      if (NOSKIP[i]) {
        ui.GI[i] += (T << LOGG);
        T = (T + 1) % NBANKHIGH;
      }
    }

    T = (pc ^ (ui.phist & ((1ULL << m[1]) - 1))) % NBANKLOW;
    for (int i = 1; i <= BORN - 1; i++) {
      if (NOSKIP[i]) {
        ui.GI[i] += (T << LOGG);
        T = (T + 1) % NBANKLOW;
      }
    }

    // Bimodal component
    pred = bimodal.predict(pc, ui.bim_conf); 
    ui.hit_pred = ui.alt_pred = pred;
    ui.hit_conf = ui.alt_conf = 3 * ui.bim_conf;
    ui.comp = BIM_COMP;

    // TAGE search
    for (int i = NHIST; i > 0; i--) { 
      if (NOSKIP[i] && gtable[i][ui.GI[i]].tag == ui.GTAG[i]) {
        uint8_t conf = abs(2 * gtable[i][ui.GI[i]].ctr + 1) >> 1;
        if (!ui.hit_bank) { // TAGE bank with longest matching history
          ui.hit_bank = i;
          ui.hit_pred = (gtable[i][ui.GI[i]].ctr >= 0);
          ui.hit_conf = conf;
        } else if (!ui.alt_bank) { // Alternate TAGE bank
          ui.alt_bank = i;
          pred = ui.alt_pred = (gtable[i][ui.GI[i]].ctr >= 0);
          ui.alt_conf = conf;
          ui.comp = ALT_COMP;
        } else if (!ui.alt2_bank) { // Maximum confidence TAGE bank
          ui.alt2_bank = i;
          ui.alt2_pred = (gtable[i][ui.GI[i]].ctr >= 0);
          ui.alt2_conf = conf;
        }
        if (ui.alt2_bank) break;
      }
    }

    // TAGE chooser
    
    // If entry has low confidence and use_alt_vs_hit is positive use alt_pred
    ui.use_alt = (use_alt_vs_hit.getValue(USE_ALT_HASH) >= 0);
#ifdef OPT_CHOOSER
    if (ui.hit_bank > 0 && (ui.hit_conf > 1 || !ui.use_alt)) {
#else
    if (ui.hit_bank > 0 && (ui.hit_conf > 0 || !ui.use_alt)) {
#endif
      pred = ui.hit_pred;
      ui.comp = HIT_COMP;
    }
    
    return pred;
  }

  uint64_t get_1st_local_index(uint64_t pc) const {
    return ((pc ^ (pc >> 2)) & (NFLOCAL - 1));
  }

  uint64_t get_2nd_local_index(uint64_t pc) const {
    return (((pc ^ (pc >> 5))) & (NSLOCAL - 1));
  }

  uint64_t get_3rd_local_index(uint64_t pc) const {
    return (((pc ^ (pc >> (LOGTNB)))) & (NTLOCAL - 1)); // different hash for the history
  }

  uint64_t get_bias_index(uint64_t pc, const UpdateInfo& ui) const {
#ifdef OPT_LOOP_PRED_INDEP
    return (((pc ^ (pc >> 2)) << 2) | (((ui.hit_conf == 0) & (ui.hit_pred != ui.alt_pred)) << 1) | ui.tage_pred) & ((1 << LOGBIAS) - 1);
#else
    return (((((pc ^ (pc >> 2)) << 1) | ((ui.hit_conf == 0) & (ui.hit_pred != ui.alt_pred))) << 1) | ui.tage_loop_pred) & ((1 << LOGBIAS) - 1);
#endif
  }

  uint64_t get_biassk_index(uint64_t pc, const UpdateInfo& ui) const {
#ifdef OPT_LOOP_PRED_INDEP
    return (((pc ^ (pc >> (LOGBIAS - 2))) << 2) | ((ui.hit_conf == C_MAX) << 1) | ui.tage_pred) & ((1 << LOGBIAS) - 1);
#else
    return (((((pc ^ (pc >> (LOGBIAS - 2))) << 1) | ((ui.hit_conf == C_MAX))) << 1) | ui.tage_loop_pred) & ((1 << LOGBIAS) - 1);
#endif    
  }

  uint64_t get_biasbank_index(uint64_t pc, const UpdateInfo& ui) const {
#ifdef OPT_LOOP_PRED_INDEP
#ifdef OPT_HASHING
    return (((pc ^ (pc >> 2)) << 7) | (((ui.hit_bank + 1) >> 2) << 4) | ((ui.alt_bank > 0) << 3) |  (ui.hit_conf << 1) | ui.tage_pred) & ((1 << LOGBIAS) - 1);
#else
    return (((pc ^ (pc >> 2)) << 7) + (((ui.hit_bank + 1) >> 2) << 4) + ((ui.alt_bank > 0) << 3) + ((ui.hit_conf == 0) << 2) + ((ui.hit_conf == C_MAX) << 1) + ui.tage_pred) & ((1 << LOGBIAS) - 1);
#endif
#else
    return (ui.tage_loop_pred + (((ui.hit_bank + 1) >> 2) << 4) + ((ui.hit_conf == C_MAX) << 1) + ((ui.hit_conf == 0) << 2) + ((ui.alt_bank > 0) << 3) + ((pc ^ (pc >> 2)) << 7)) & ((1 << LOGBIAS) - 1);
#endif
  }

  
  bool predict(uint64_t seq_no, uint8_t piece, uint64_t pc) {
    // checkpoint current hist
    auto key = getUniqueInstrId(seq_no, piece);
    
    ui.tage_pred = Tagepred(pc); // Computes the TAGE table addresses and the partial tags
    ui.pred = ui.tage_pred;
    ui.tage_sc_pred = ui.tage_pred;

#ifdef LOOPPREDICTOR
    ui.loop_conf = 0;
    ui.loop_pred = loop_predictor.predict(pc, ui.loop_conf);
    ui.use_loop = (use_loop_vs_tagesc.getValue(USE_LOOP_HASH) > 0);
#ifdef OPT_LOOP_CONF_FINE
    if (ui.loop_conf >= 3 && ui.use_loop) { // Use loop predictor
#else
    if (ui.loop_conf == 7 && ui.use_loop) {
#endif
      ui.pred = ui.loop_pred; 
      ui.comp = LOOP_COMP;
    }
#endif
    ui.tage_loop_pred = ui.pred;

#ifdef SC

    // Compute the SC prediction
    ui.lsum = 0;

    // integrate BIAS prediction
    ui.lsum += (2 * Bias[get_bias_index(pc, ui)] + 1);
    ui.lsum += (2 * BiasSK[get_biassk_index(pc, ui)] + 1);
    ui.lsum += (2 * BiasBank[get_biasbank_index(pc, ui)] + 1);
#ifdef VARTHRES
    ui.lsum = (1 + (WB[INDUPDS] >= 0)) * ui.lsum;
#endif

    // integrate the GEHL predictions
#ifdef OPT_LOOP_PRED_INDEP
    ui.lsum += Gpredict((pc << 1) + ui.tage_pred, ui.GHIST, Gm, GGEHL, GNB, LOGGNB, WG);
#else
    ui.lsum += Gpredict((pc << 1) + ui.tage_loop_pred, ui.GHIST, Gm, GGEHL, GNB, LOGGNB, WG);
#endif
    ui.lsum += Gpredict(pc, ui.phist, Pm, PGEHL, PNB, LOGPNB, WP);
#ifdef LOCALH
    ui.lsum += Gpredict(pc, ui.L_shist[get_1st_local_index(pc)], Lm, LGEHL, LNB, LOGLNB, WL);
#ifdef LOCALS
    ui.lsum += Gpredict(pc, ui.S_slhist[get_2nd_local_index(pc)], Sm, SGEHL, SNB, LOGSNB, WS);
#endif
#ifdef LOCALT
    ui.lsum += Gpredict(pc, ui.T_slhist[get_3rd_local_index(pc)], Tm, TGEHL, TNB, LOGTNB, WT);
#endif
#endif

#ifdef IMLI
    ui.lsum += Gpredict(pc, ui.IMHIST[(ui.IMLIcount)], IMm, IMGEHL, IMNB, LOGIMNB, WIM);
#ifndef OPT_REDUC_SC
    ui.lsum += Gpredict(pc, ui.IMLIcount, Im, IGEHL, INB, LOGINB, WI);
#endif
#endif
    bool sc_pred = (ui.lsum >= 0);
    
    // An heuristic if the respective contribution of component groups can be multiplied by 2
    ui.thres = (updatethreshold >> 3) + Pupdatethreshold[INDUPD]
#ifdef VARTHRES
            + 12
                  * ((WB[INDUPDS] >= 0) + (WG[INDUPDS] >= 0) + (WP[INDUPDS] >= 0)
#ifdef LOCALH
                     + (WL[INDUPDS] >= 0) + (WS[INDUPDS] >= 0) + (WT[INDUPDS] >= 0)
#endif
#ifdef IMLI
#ifdef OPT_REDUC_SC
                     + (WIM[INDUPDS] >= 0) // + (WIM[INDUPDS] >= 0) // why not?
#else
                     + (WI[INDUPDS] >= 0) // + (WIM[INDUPDS] >= 0) // why not?
#endif
#endif
                  )
#endif
        ;

#ifdef OPT_CHOOSE_LOOP
    if (ui.tage_pred != sc_pred && ui.comp != LOOP_COMP) {
//    if (ui.comp != LOOP_COMP) {
#else
    if (ui.tage_loop_pred != sc_pred) {
#endif

#ifdef OPT_CHOOSER
      ui.use_sc = (use_sc_vs_tage.getValue(USE_SC_HASH) >= 0);
      if (ui.use_sc) {
        ui.pred = sc_pred;
        ui.tage_sc_pred = sc_pred;
        ui.comp = SC_COMP;
      }
#else
      // Chooser uses TAGE confidence and |lsum|
      //    0 ----- thres/4 ----- thres/2 ----- inf
      // H     TAGE          2nd           SC
      // M     1st           SC            SC
      // L     SC            SC            SC
      if ((ui.hit_conf == C_MAX) && abs(ui.lsum) < (ui.thres >> 2)) {
        // Keep TAGE
      } else if ((ui.hit_conf == C_MAX) && abs(ui.lsum) < (ui.thres >> 1)) {
        if (SecondH < 0) {
          ui.pred = sc_pred;
          ui.tage_sc_pred = sc_pred;
          ui.comp = SC_COMP;
        }
      } else if (ui.hit_conf == 2 && abs(ui.lsum) < (ui.thres >> 2)) {
        if (FirstH < 0) {
          ui.pred = sc_pred;
          ui.tage_sc_pred = sc_pred;
          ui.comp = SC_COMP;
        }
      } else {
        ui.pred = sc_pred;
        ui.tage_sc_pred = sc_pred;
        ui.comp = SC_COMP;
      }
#endif
    }
#endif

#ifdef LOOPPREDICTOR
    ui.loop_predictor = loop_predictor;
    ui.use_loop_vs_tagesc = use_loop_vs_tagesc;
#endif
    
    update_info_inflight.emplace(key, ui);
    return ui.pred;
  }

  void historyUpdate(uint64_t pc, int brtype, bool taken, bool pred, uint64_t next_pc) {
    bool cond_branch = brtype & 1;
    bool indirect_branch = brtype & 2;

    if (cond_branch) {
      
#ifdef LOOPPREDICTOR

#ifdef OPT_LOOP_PRED_INDEP
      bool other_pred = ui.tage_sc_pred;
#else
      bool other_pred = ui.tage_pred;
#endif
      loop_predictor.update(pc, pred, pred, other_pred, 0, ui.comp == LOOP_COMP);

#ifdef FIX_LOOP_USEFUL
#ifdef OPT_LOOP_CONF_FINE
      if (ui.loop_pred != other_pred) { // If loop contradicts other prediction
#else
      if (ui.loop_conf == 7 && other_pred != ui.loop_pred) { // If loop contradicts other prediction
#endif
#else
      if (ui.loop_conf == 7 && pred != ui.loop_pred) { // If loop prediction contradicts final prediction
#endif
        use_loop_vs_tagesc.update(USE_LOOP_HASH, ui.loop_pred == pred);
      }

#endif

#ifdef IMLI
      ui.IMHIST[ui.IMLIcount] = (ui.IMHIST[ui.IMLIcount] << 1) + taken;
      if (next_pc < pc) {
        // This branch corresponds to a loop
        if (!taken) {
          // exit of the "loop"
          ui.IMLIcount = 0;
        }
        if (taken) {
          if (ui.IMLIcount < ((1ULL << Im[0]) - 1))
            ui.IMLIcount++;
        }
      }
#endif

      ui.GHIST = (ui.GHIST << 1) + (taken & (next_pc < pc));
      ui.L_shist[get_1st_local_index(pc)] = (ui.L_shist[get_1st_local_index(pc)] << 1) + (taken);
      ui.S_slhist[get_2nd_local_index(pc)] = ((ui.S_slhist[get_2nd_local_index(pc)] << 1) + taken) ^ (pc & 15);
      ui.T_slhist[get_3rd_local_index(pc)] = (ui.T_slhist[get_3rd_local_index(pc)] << 1) + taken;
    }

    int T = ((pc ^ (pc >> 2))) ^ taken;
    int PATH = pc ^ (pc >> 2) ^ (pc >> 4);
    if (cond_branch && indirect_branch && taken) {
      T = (T ^ (next_pc >> 2));
      PATH = PATH ^ (next_pc >> 2) ^ (next_pc >> 4);
    }

    int maxt = (!cond_branch && indirect_branch) ? 3 : 2; // Special treatment for indirect branches
    for (int t = 0; t < maxt; t++) {
      bool DIR = (T & 1);
      T >>= 1;
      int PATHBIT = (PATH & 127);
      PATH >>= 1;
      // update  history
      ui.ptghist--; // ptghist
      ui.ghist[ui.ptghist & (HISTBUFFERLENGTH - 1)] = DIR;
      ui.phist = (ui.phist << 1) ^ PATHBIT; // phist

      // updates to folded histories
      for (int i = 1; i <= NHIST; i++) {
        ui.ch_i[i].update(ui.ghist, ui.ptghist);
        ui.ch_t[0][i].update(ui.ghist, ui.ptghist);
        ui.ch_t[1][i].update(ui.ghist, ui.ptghist);
      }
    }
    ui.phist = (ui.phist & ((1 << PHISTWIDTH) - 1));

    random_phist = ui.phist;
    random_ptghist = ui.ptghist;

  } // END UPDATE HISTORIES

  // PREDICTOR UPDATE
  void update(uint64_t seq_no, uint8_t piece, uint64_t pc, bool resolveDir, bool pred, uint64_t next_pc) {
    const auto key = getUniqueInstrId(seq_no, piece);
    const auto& ui = update_info_inflight.at(key);
    assert(ui.pred == pred);

    if (pred != resolveDir) {
#ifdef LOOPPREDICTOR
      loop_predictor = ui.loop_predictor;
      use_loop_vs_tagesc = ui.use_loop_vs_tagesc;
      int prob_alloc = 3;
#ifdef OPT_LOOP_PRED_INDEP
      bool other_pred = ui.tage_sc_pred;
#else
      bool other_pred = ui.tage_pred;
#endif
      loop_predictor.update(pc, resolveDir, pred, other_pred, prob_alloc, ui.comp == LOOP_COMP);

#ifdef FIX_LOOP_USEFUL
#ifdef OPT_LOOP_CONF_FINE
      if (ui.loop_pred != other_pred) { // If loop contradicts other prediction
#else
      if (ui.loop_conf == 7 && other_pred != ui.loop_pred) { // If loop contradicts other prediction
#endif
#else
      if (ui.loop_conf == 7 && pred != ui.loop_pred) { // If loop prediction contradicts final prediction
#endif
        use_loop_vs_tagesc.update(USE_LOOP_HASH, ui.loop_pred == resolveDir);
      }
#endif 
    }    
      
#ifdef SC
    bool sc_pred = (ui.lsum >= 0);
#ifdef OPT_LOOP_PRED_INDEP
    bool inter_pred = ui.tage_pred;
#else
    bool inter_pred = ui.tage_loop_pred;
#endif
    if (inter_pred != sc_pred) {
      if (ui.hit_conf == C_MAX && abs(ui.lsum) < (ui.thres >> 1) && abs(ui.lsum) >= (ui.thres >> 2)) {
        ctrUpdate(SecondH, (inter_pred == resolveDir), CONFWIDTH);
      }
      if (ui.hit_conf == 2 && abs(ui.lsum) < (ui.thres >> 2)) {
        ctrUpdate(FirstH, (inter_pred == resolveDir), CONFWIDTH);
      }
      use_sc_vs_tage.update(USE_SC_HASH, sc_pred == resolveDir);
    }
  
    if ((sc_pred != resolveDir) || ((abs(ui.lsum) < ui.thres))) {
      ctrUpdate(Pupdatethreshold[INDUPD], sc_pred != resolveDir, WIDTHRESP);
      ctrUpdate(updatethreshold, sc_pred != resolveDir, WIDTHRES);
   
#ifdef VARTHRES
      int xsum = ui.lsum - ((WB[INDUPDS] >= 0) * ((2 * Bias[get_bias_index(pc, ui)] + 1) + (2 * BiasSK[get_biassk_index(pc, ui)] + 1) + (2 * BiasBank[get_biasbank_index(pc, ui)] + 1)));
      if ((xsum + ((2 * Bias[get_bias_index(pc, ui)] + 1) + (2 * BiasSK[get_biassk_index(pc, ui)] + 1) + (2 * BiasBank[get_biasbank_index(pc, ui)] + 1)) >= 0) != (xsum >= 0)) {
        ctrUpdate(WB[INDUPDS], (((2 * Bias[get_bias_index(pc, ui)] + 1) + (2 * BiasSK[get_biassk_index(pc, ui)] + 1) + (2 * BiasBank[get_biasbank_index(pc, ui)] + 1) >= 0) == resolveDir), EWIDTH);
      }
#endif
      ctrUpdate(Bias[get_bias_index(pc, ui)], resolveDir, PERCWIDTH);
      ctrUpdate(BiasSK[get_biassk_index(pc, ui)], resolveDir, PERCWIDTH);
      ctrUpdate(BiasBank[get_biasbank_index(pc, ui)], resolveDir, PERCWIDTH);
      Gupdate((pc << 1) + inter_pred, resolveDir, ui.GHIST, Gm, GGEHL, GNB, LOGGNB, WG, ui.lsum);
      Gupdate(pc, resolveDir, ui.phist, Pm, PGEHL, PNB, LOGPNB, WP, ui.lsum);
#ifdef LOCALH
      Gupdate(pc, resolveDir, ui.L_shist[get_1st_local_index(pc)], Lm, LGEHL, LNB, LOGLNB, WL, ui.lsum);
#ifdef LOCALS
      Gupdate(pc, resolveDir, ui.S_slhist[get_2nd_local_index(pc)], Sm, SGEHL, SNB, LOGSNB, WS, ui.lsum);
#endif
#ifdef LOCALT
      Gupdate(pc, resolveDir, ui.T_slhist[get_3rd_local_index(pc)], Tm, TGEHL, TNB, LOGTNB, WT, ui.lsum);
#endif
#endif

#ifdef IMLI
      Gupdate(pc, resolveDir, ui.IMHIST[(ui.IMLIcount)], IMm, IMGEHL, IMNB, LOGIMNB, WIM, ui.lsum);
#ifndef OPT_REDUC_SC
      Gupdate(pc, resolveDir, ui.IMLIcount, Im, IGEHL, INB, LOGINB, WI, ui.lsum);
#endif
#endif
    }
#endif
    
    // TAGE UPDATE
    auto hit_entry = &gtable[ui.hit_bank][ui.GI[ui.hit_bank]];
    auto alt_entry = &gtable[ui.alt_bank][ui.GI[ui.alt_bank]];
    auto alt2_entry = &gtable[ui.alt2_bank][ui.GI[ui.alt2_bank]];

    // Update chooser between longest matching and alternate matching
#ifdef OPT_CHOOSER
    if (ui.hit_bank > 0 && ui.hit_conf < 2 && ui.hit_pred != ui.alt_pred) {
#else
    if (ui.hit_bank > 0 && ui.hit_conf == 0 && ui.hit_pred != ui.alt_pred) {
#endif
      use_alt_vs_hit.update(USE_ALT_HASH, ui.alt_pred == resolveDir);
    }
    
#ifdef OPT_TAGE_ALLOC
    bool alloc = (ui.tage_loop_pred != resolveDir && ui.hit_bank < NHIST);
    int num_alloc = NNN;
#else
    bool alloc = (ui.tage_pred != resolveDir && ui.hit_bank < NHIST);    
    int num_alloc = NNN;
    if (ui.hit_bank > 0 && abs(2 * hit_entry->ctr + 1) == 1 && ui.hit_pred == resolveDir) {
      alloc = false;
    }
    if (pred == resolveDir && (myRandom() & 31) != 0) {
      alloc = false;
    }
#endif
    
    if (alloc) {
      int penalty = 0;
      int new_alloc = 0;
#ifdef OPT_TAGE_ALLOC
      int DEP = (((ui.hit_bank + 1) >> 1) << 1) ^ (myRandom() & 1);
#else
      int DEP = ((((ui.hit_bank - 1 + 2 * (((myRandom() & 127) < 32) ? 2 : 1)) & 0xffe)) ^ (myRandom() & 1)); // Just a complex formula to choose between X and X+1, when X is odd: sorry
#endif
      for (int I = DEP; I < NHIST; I += 2) {      
        bool Done = false;
        int i = I + 1;
        if (NOSKIP[i]) {
          if (gtable[i][ui.GI[i]].u == 0) {
            if (abs(2 * gtable[i][ui.GI[i]].ctr + 1) <= 3) { // Optimization for the repl policy
              gtable[i][ui.GI[i]].tag = ui.GTAG[i];
              gtable[i][ui.GI[i]].ctr = (resolveDir) ? 0 : -1;
              new_alloc++;
              if (new_alloc > num_alloc) {
                break;
              }
              I += 2;
              Done = true;
            } else { // Optimization for the replacement policy with a single u bit
              gtable[i][ui.GI[i]].ctr += (gtable[i][ui.GI[i]].ctr > 0) ? -1 : 1; // Lower the confidence
            }
          } else {
            penalty++;
          }
        }

        if (!Done) { 
          i = (I ^ 1) + 1;
          if (NOSKIP[i]) {
            if (gtable[i][ui.GI[i]].u == 0) {
              if (abs(2 * gtable[i][ui.GI[i]].ctr + 1) <= 3) { // Optimization for the replacement policy
                gtable[i][ui.GI[i]].tag = ui.GTAG[i];
                gtable[i][ui.GI[i]].ctr = (resolveDir) ? 0 : -1;
                new_alloc++;
                if (new_alloc > num_alloc) {
                  break;
                }
                I += 2;
              } else { // Optimization for the replacement policy with a single u bit
                gtable[i][ui.GI[i]].ctr += (gtable[i][ui.GI[i]].ctr > 0) ? -1 : 1; // Lower the confidence
              }
            } else {
              penalty++;
            }
          }
        }
      }

      // Just the best formula for the Championship
      tick = max(0, tick + penalty - 2 * new_alloc); 
      if (tick >= BORNTICK) {
        for (int i = 1; i <= BORN; i += BORN - 1) {
          for (int j = 0; j < SizeTable[i]; j++) {
            gtable[i][j].u >>= 1;
          }
        }
        tick = 0;
      }
    }

    // Update TAGE tables
    if (!ui.hit_bank) {
      bimodal.update(pc, resolveDir);
    } else {
      if (abs(2 * hit_entry->ctr + 1) == 1 && ui.hit_pred != resolveDir) { // Protection: new alloc
        if (ui.alt_bank) {
          ctrUpdate(alt_entry->ctr, resolveDir, CWIDTH);
#ifdef OPT_ALT2
          if (ui.alt2_bank) {
            ctrUpdate(alt2_entry->ctr, resolveDir, CWIDTH);
          }
#endif
        } else {
          bimodal.update(pc, resolveDir);
        }
      }
      ctrUpdate(hit_entry->ctr, resolveDir, CWIDTH);
      if (abs(2 * hit_entry->ctr + 1) == 1) { // Sign changes: no way it can have been useful
        hit_entry->u = 0; // Perhaps each time is impredicted?
      }
      if (ui.hit_pred == ui.alt_pred) {  
        if (ui.alt_bank && (abs(2 * alt_entry->ctr + 1) >> 1) == C_MAX
            && ui.alt_pred == resolveDir) { // Alt is already a good prediction
          hit_entry->u = 0; 
        }
      } else {
#ifdef OPT_REPL_TAGE
        if (ui.hit_pred == resolveDir) {
          ctrUpdate(hit_entry->u, true, UWIDTH);
        } else if (ui.alt_bank) {
          ctrUpdate(alt_entry->u, true, UWIDTH);
        }
#else
        if (ui.hit_pred == resolveDir) {
          ctrUpdate(hit_entry->u, true, UWIDTH);
        }
#endif
      }
    }
    // END TAGE UPDATE

    update_info_inflight.erase(key);

  } // END PREDICTOR UPDATE AT EXECUTE

#define GINDEX (((uint64_t)pc) ^ bhist ^ (bhist >> (8 - i)) ^ (bhist >> (16 - 2 * i)) ^ (bhist >> (24 - 3 * i)) ^ (bhist >> (32 - 3 * i)) ^ (bhist >> (40 - 4 * i))) & ((1 << (logs - (i >= (NBR - 2)))) - 1)

  int Gpredict(uint64_t pc, uint64_t BHIST, int* length, int8_t** tab, int NBR, int logs, int8_t* W) {
    int sum = 0;
    for (int i = 0; i < NBR; i++) {
      uint64_t bhist = BHIST & ((uint64_t)((1ULL << length[i]) - 1));
      sum += (2 * tab[i][GINDEX] + 1);
    }
#ifdef VARTHRES
    sum = (1 + (W[INDUPDS] >= 0)) * sum;
#endif
    return sum;
  }
  
  void Gupdate(uint64_t pc, bool taken, uint64_t BHIST, int* length, int8_t** tab, int NBR, int logs, int8_t* W, int lsum) {
    int sum = 0;
    for (int i = 0; i < NBR; i++) {
      uint64_t bhist = BHIST & ((uint64_t)((1ULL << length[i]) - 1));
      sum += (2 * tab[i][GINDEX] + 1);
      ctrUpdate(tab[i][GINDEX], taken, PERCWIDTH);
    }
#ifdef VARTHRES
    int xsum = lsum - ((W[INDUPDS] >= 0)) * sum;
    if ((xsum + sum >= 0) != (xsum >= 0)) {
      ctrUpdate(W[INDUPDS], ((sum >= 0) == taken), EWIDTH);
    }
#endif
  }

};

#endif
// ChampSim deviation 2 of 3: the submission's global predictor instance is
// commented out. It exists to be named by cond_branch_predictor_interface.cc,
// which ChampSim does not use; the module owns its instance via
// champsim::cbp6::host instead. Left in, it would construct a second full
// ~192 KB predictor at static-init in every binary and never be read.
// static CBP2016_TAGE_SC_L tage_sc_l;
