// The reference, untouched, straight out of cbp2025/lib/ittage.h.
// Its macros (NHIST, LOGG, ...) leak at file scope, which is exactly why it
// lives in its own translation unit.
#include <sys/types.h>   // uint
#include <cmath>
#include <cstdlib>
#include "/home/rbera/work/bpeval/cbp2025/lib/ittage.h"
#include "api.h"

// Static storage duration: reinit() leaves ghist[1..4095] unwritten in the
// reference (the bug the vendored copy fixes), so only zero-initialised storage
// makes the two comparable. Both sides use it.
static IPREDICTOR& inst() { static IPREDICTOR p; return p; }

namespace pristine {
uint64_t predict(uint64_t pc)             { return inst().GetPrediction(pc); }
void     update(uint64_t pc, uint64_t t)  { inst().UpdatePredictor(pc, t); }
void     track(uint64_t pc, uint64_t t)   { inst().TrackOtherInst(pc, t); }
}
