/*
 * ITTAGE indirect branch target predictor.
 *
 * VENDORED from cbp2025/lib/ittage.h -- Andre Seznec's own implementation,
 * shipped inside the CBP2025 framework ("Modified by A. Seznec
 * (andre.seznec@inria.fr) to include TAGE-SC-L predictor and the ITTAGE indirect
 * branch predictor", cbp2025/lib/bp.cc:23). Algorithm per A. Seznec, "A
 * 64-Kbytes ITTAGE indirect branch predictor", JWAC-2 / CBP-3, 2011.
 *
 * TWO DEVIATIONS from the reference, both documented at their site:
 *
 *   1. The in-class #define configuration block is replaced by a template
 *      parameter CFG, so one copy of the source can be instantiated at several
 *      storage budgets. ONLY the knobs that move storage are parameterised:
 *      NHIST, LOGG, TBITS, MINHIST, MAXHIST, CWIDTH, UWIDTH. Everything else --
 *      HISTBUFFERLENGTH, BORNTICK, ALTWIDTH, NNN, PHISTWIDTH -- stays a fixed
 *      constant with the reference's value, which keeps the diff mechanical.
 *      The substitution is `X` -> `CFG::X` and nothing else; no logic, no
 *      ordering and no arithmetic is altered.
 *
 *   2. A genuine bug in the reference's reinit(): it writes ghist[0] on every
 *      loop iteration instead of ghist[i]. See the comment at that line.
 *
 * That the parameterisation is behaviour-preserving is not asserted, it is
 * TESTED: tools/ittage_equiv/ runs this file at the reference's own default
 * configuration alongside the pristine cbp2025 header over millions of
 * branches and requires bit-identical predictions.
 */

#ifndef INC_ITTAGE_ITTAGE_HPP
#define INC_ITTAGE_ITTAGE_HPP

#include <cmath>
#include <cstdint>
#include <cstdlib>

// The vendored source is warning-clean by CBP2025's standards, not ChampSim's:
// it converts freely between uint64_t/int/long long and uses C-style casts.
// Suppressed here rather than "fixed", because every such edit is a chance to
// change behaviour, and behaviour-preservation is the whole point of the
// equivalence harness. Same treatment the four CBP6 tenants get.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wuseless-cast"

namespace champsim_ittage
{
// Not storage knobs; the reference's values, kept fixed so the parameterised
// diff stays confined to the seven size parameters.
#define HISTBUFFERLENGTH 4096
#define BORNTICK 1024
#define ALTWIDTH 5
#define NNN 1
#define PHISTWIDTH 27

using uint = unsigned int;

// Fast  implementation of the ITTAGE predictor: probably not optimal, but not
// that far
//
// utility class for index computation
// this is the cyclic shift register for folding
// a long global history into a smaller number of bits; see P. Michaud's
// PPM-like predictor at CBP-1
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

  void update(uint8_t *h, int PT) {
    comp = (comp << 1) ^ h[PT & (HISTBUFFERLENGTH - 1)];
    comp ^= h[(PT + OLENGTH) & (HISTBUFFERLENGTH - 1)] << OUTPOINT;
    comp ^= (comp >> CLENGTH);
    comp = (comp) & ((1 << CLENGTH) - 1);
  }
};

class ientry // ITTAGE global table entry
{
public:
  uint64_t target;
  int8_t ctr;
  uint tag;
  int8_t u;

  ientry() {
    target = 0xdeadbeef;

    ctr = 0;
    u = 0;
    tag = 0;
  }
};

template <typename CFG>
class IPREDICTOR {
public:


  // the counter to chose between longest match and alternate prediction on
  // ITTAGE when weak confidence counters

  int8_t use_alt_on_na;
  long long GHIST;

  int TICK; // for the reset of the u counter
  uint8_t ghist[HISTBUFFERLENGTH];
  int ptghist;
  long long phist;                   // path history
  folded_history ch_i[CFG::NHIST + 1];    // utility for computing ITTAGE indices
  folded_history ch_t[2][CFG::NHIST + 1]; // utility for computing ITTAGE tags

  ientry *itable[CFG::NHIST + 1];
  int m[CFG::NHIST + 1];
  int TB[CFG::NHIST + 1];
  int logg[CFG::NHIST + 1];

  int GI[CFG::NHIST + 1]; // indexes to the different tables are computed only once
  uint GTAG[CFG::NHIST + 1]; // tags for the different tables are computed only once
  uint64_t pred_target; // prediction
  uint64_t alt_target;  // alternate  TAGEprediction
  uint64_t tage_target; // TAGE prediction

  uint64_t LongestMatchPred;
  int HitBank; // longest matching bank
  int AltBank; // alternate matching bank
  int Seed;    // for the pseudo-random number generator
  uint64_t target_inter;

  IPREDICTOR(void) { reinit(); }

  void reinit() {
    m[0] = 0;
    m[1] = CFG::MINHIST;
    m[CFG::NHIST] = CFG::MAXHIST;
    for (int i = 2; i <= CFG::NHIST; i++) {
      m[i] = (int)(((double)CFG::MINHIST * pow((double)(CFG::MAXHIST) / (double)CFG::MINHIST,
                                          (double)(i) / (double)CFG::NHIST)) +
                   0.5);
    }

    for (int i = 0; i <= CFG::NHIST; i++) {
      TB[i] = CFG::TBITS;
      logg[i] = CFG::LOGG;
    }

    for (int i = 0; i <= CFG::NHIST; i++)
      itable[i] = new ientry[(1 << CFG::LOGG)];

    for (int i = 0; i <= CFG::NHIST; i++) {
      ch_i[i].init(m[i], (logg[i]));
      ch_t[0][i].init(ch_i[i].OLENGTH, TB[i]);
      ch_t[1][i].init(ch_i[i].OLENGTH, TB[i] - 1);
    }

    Seed = 0;

    TICK = 0;
    phist = 0;
    Seed = 0;

    // ChampSim deviation 2 of 2: the reference writes ghist[0] on every
    // iteration instead of ghist[i], leaving ghist[1..HISTBUFFERLENGTH-1]
    // uninitialised. Those bytes feed folded_history::update, so every index
    // and tag would depend on indeterminate memory. Harmless only when the
    // predictor has static storage duration (zero-initialised before
    // construction), which is exactly the fragility that produced three
    // uninitialised-member bugs in the RUNLTS port. Fixed here; behaviourally
    // a no-op under static storage, which is how the equivalence harness runs
    // both copies.
    for (int i = 0; i < HISTBUFFERLENGTH; i++)
      ghist[i] = 0;
    ptghist = 0;
    use_alt_on_na = 0;
    GHIST = 0;
    ptghist = 0;
    phist = 0;
  }

  // F serves to mix path history: not very important impact

  int F(long long A, int size, int bank) {
    int A1, A2;
    A = A & ((1 << size) - 1);
    A1 = (A & ((1 << logg[bank]) - 1));
    A2 = (A >> logg[bank]);

    if (bank < logg[bank])
      A2 = ((A2 << bank) & ((1 << logg[bank]) - 1)) +
           (A2 >> (logg[bank] - bank));
    A = A1 ^ A2;
    if (bank < logg[bank])
      A = ((A << bank) & ((1 << logg[bank]) - 1)) + (A >> (logg[bank] - bank));
    return (A);
  }

  // gindex computes a full hash of PC, ghist and phist
  int gindex(unsigned int PC, int bank, long long hist, folded_history *ch_i) {
    int index;
    int M = (m[bank] > PHISTWIDTH) ? PHISTWIDTH : m[bank];
    index = PC ^ (PC >> (abs(logg[bank] - bank) + 1)) ^ ch_i[bank].comp ^
            F(hist, M, bank);

    return (index & ((1 << (logg[bank])) - 1));
  }

  //  tag computation
  uint16_t gtag(unsigned int PC, int bank, folded_history *ch0,
                folded_history *ch1) {
    int tag = (PC) ^ ch0[bank].comp ^ (ch1[bank].comp << 1);
    return (tag & ((1 << (TB[bank])) - 1));
  }

  // up-down saturating counter
  void ctrupdate(int8_t &ctr, bool taken, int nbits) {
    if (taken) {
      if (ctr < ((1 << (nbits - 1)) - 1))
        ctr++;
    } else {
      if (ctr > -(1 << (nbits - 1)))
        ctr--;
    }
  }

  // just a simple pseudo random number generator: use available information
  // to allocate entries  in the loop predictor
  int MYRANDOM() {
    Seed++;
    Seed ^= phist;
    Seed = (Seed >> 21) + (Seed << 11);
    Seed ^= ptghist;
    Seed = (Seed >> 10) + (Seed << 22);
    return (Seed);
  };

  //  ITTAGE PREDICTION: same code at fetch or retire time but the index and
  //  tags must recomputed
  uint64_t GetPrediction(uint64_t PC) {
    HitBank = -1;
    AltBank = -1;
    for (int i = 0; i <= CFG::NHIST; i++) {
      GI[i] = gindex(PC, i, phist, ch_i);
      GTAG[i] = gtag(PC, i, ch_t[0], ch_t[1]);
    }

    alt_target = 0;
    tage_target = 0;

    LongestMatchPred = 0;

    int AltConf = -4;
    int HitConf = -4;
    // Look for the bank with longest matching history
    for (int i = CFG::NHIST; i >= 0; i--) {
      if (itable[i][GI[i]].tag == GTAG[i]) {
        HitBank = i;
        HitConf = itable[HitBank][GI[HitBank]].ctr;
        LongestMatchPred = itable[HitBank][GI[HitBank]].target;
        break;
      }
    }

    // Look for the alternate bank
    for (int i = HitBank - 1; i >= 0; i--) {
      if (itable[i][GI[i]].tag == GTAG[i]) {
        alt_target = itable[i][GI[i]].target;
        AltBank = i;
        AltConf = itable[AltBank][GI[AltBank]].ctr;
        break;
      }
    }
    // computes the prediction and the alternate prediction

    if (HitBank > 0) {

      bool Huse_alt_on_na = (use_alt_on_na >= 0);
      if ((!Huse_alt_on_na) || (HitConf > 0) || (HitConf >= AltConf))
        tage_target = LongestMatchPred;
      else
        tage_target = alt_target;
    }
    if (AltBank < 0)
      tage_target = LongestMatchPred;

    return (tage_target);
  }

  void HistoryUpdate(uint64_t PC, uint64_t target, long long &X, int &Y,
                     folded_history *H, folded_history *G, folded_history *J) {

    int maxt = 3;
    int T = (PC >> 2) ^ (PC >> 6);
    int PATH = (target >> 2) ^ (target >> 6);

    for (int t = 0; t < maxt; t++) {
      bool DIR = (T & 1);
      T >>= 1;
      int PATHBIT = (PATH & 127);
      PATH >>= 1;
      // update  history
      Y--;
      ghist[Y & (HISTBUFFERLENGTH - 1)] = DIR;
      X = (X << 1) ^ PATHBIT;

      for (int i = 1; i <= CFG::NHIST; i++) {

        H[i].update(ghist, Y);
        G[i].update(ghist, Y);
        J[i].update(ghist, Y);
      }
    }

    X = (X & ((1 << PHISTWIDTH) - 1));

    // END UPDATE  HISTORIES
  }

  void TrackOtherInst(uint64_t PC, uint64_t branchTarget) {

    HistoryUpdate(PC, branchTarget, phist, ptghist, ch_i, ch_t[0], ch_t[1]);
  }
  // PREDICTOR UPDATE

  void UpdatePredictor(uint64_t PC, uint64_t branchTarget) {

    // TAGE UPDATE
    bool ALLOC = ((tage_target != branchTarget) & (HitBank < CFG::NHIST));

    // do not allocate too often if the overall prediction is correct

    if (HitBank > 0)
      if (AltBank >= 0) {
        // Manage the selection between longest matching and alternate matching
        // for "pseudo"-newly allocated longest matching entry
        // this is extremely important for TAGE only, not that important when
        // the overall predictor is implemented
        bool PseudoNewAlloc = (itable[HitBank][GI[HitBank]].ctr <= 0);
        // an entry is considered as newly allocated if its prediction counter
        // is weak
        if (PseudoNewAlloc) {
          if (LongestMatchPred == branchTarget)
            ALLOC = false;
          // if it was delivering the correct prediction, no need to allocate a
          // new entry
          // even if the overall prediction was false
          if (LongestMatchPred != alt_target)
            if ((LongestMatchPred == branchTarget) ||
                (alt_target == branchTarget)) {
              ctrupdate(use_alt_on_na, (alt_target == branchTarget), ALTWIDTH);
            }
        }
      }

    if (ALLOC) {

      int T = NNN;
      int A = 1;
      if ((MYRANDOM() & 127) < 32)
        A = 2;
      int Penalty = 0;
      int NA = 0;
      int DEP = HitBank + A;
      for (int i = DEP; i <= CFG::NHIST; i++) {
        if (itable[i][GI[i]].u == 0) {
          itable[i][GI[i]].tag = GTAG[i];
          itable[i][GI[i]].target = branchTarget;
          itable[i][GI[i]].ctr = 0;
          NA++;
          if (T <= 0) {
            break;
          }
          i += 1;
          T -= 1;
        }

        else {
          Penalty++;
        }
      }

      TICK += (Penalty - 2 * NA);

      // just the best formula for the Championship:
      // In practice when one out of two entries are useful
      if (TICK < 0)
        TICK = 0;
      if (TICK >= BORNTICK) {

        for (int i = 0; i <= CFG::NHIST; i++)
          for (int j = 0; j < (1 << CFG::LOGG); j++)
            itable[i][j].u >>= 1;
        TICK = 0;
      }
    }

    // update predictions
    if (HitBank >= 0) {
      if (itable[HitBank][GI[HitBank]].ctr <= 0)
        if (LongestMatchPred != branchTarget)

        {
          if (alt_target == branchTarget)
            if (AltBank >= 0) {
              ctrupdate(itable[AltBank][GI[AltBank]].ctr,
                        (alt_target == branchTarget), CFG::CWIDTH);
            }
        }

      ctrupdate(itable[HitBank][GI[HitBank]].ctr,
                (LongestMatchPred == branchTarget), CFG::CWIDTH);
      if (LongestMatchPred != branchTarget)
        if (itable[HitBank][GI[HitBank]].ctr < 0)
          itable[HitBank][GI[HitBank]].target = branchTarget;
    }
    if (LongestMatchPred != alt_target)
      if (LongestMatchPred == branchTarget) {
        if (itable[HitBank][GI[HitBank]].u < (1 << CFG::UWIDTH) - 1)
          itable[HitBank][GI[HitBank]].u++;
      }
    // END TAGE UPDATE

    HistoryUpdate(PC, branchTarget, phist, ptghist, ch_i, ch_t[0], ch_t[1]);

    // END PREDICTOR UPDATE
  }
};

#undef HISTBUFFERLENGTH
#undef BORNTICK
#undef ALTWIDTH
#undef NNN
#undef PHISTWIDTH
} // namespace champsim_ittage

#pragma GCC diagnostic pop

#endif
