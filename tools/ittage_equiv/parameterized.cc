// Our parameterised copy, instantiated at the REFERENCE'S OWN defaults. Any
// difference in prediction is therefore a defect introduced by the
// parameterisation, which is the whole point of this harness.
#include "ittage/ittage.hpp"
#include "api.h"

struct reference_cfg {
  static constexpr int NHIST = 8, LOGG = 10, TBITS = 11;
  static constexpr int MINHIST = 2, MAXHIST = 300;
  static constexpr int CWIDTH = 3, UWIDTH = 2;
};

static champsim_ittage::IPREDICTOR<reference_cfg>& inst()
{
  static champsim_ittage::IPREDICTOR<reference_cfg> p;
  return p;
}

namespace parameterized {
uint64_t predict(uint64_t pc)            { return inst().GetPrediction(pc); }
void     update(uint64_t pc, uint64_t t) { inst().UpdatePredictor(pc, t); }
void     track(uint64_t pc, uint64_t t)  { inst().TrackOtherInst(pc, t); }
}
