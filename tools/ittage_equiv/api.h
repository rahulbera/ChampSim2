// Identical C API over both predictor copies, so main.cc can drive them
// through the same call sequence without either header being visible to it.
#ifndef ITTAGE_EQUIV_API_H
#define ITTAGE_EQUIV_API_H
#include <cstdint>
namespace pristine { uint64_t predict(uint64_t pc); void update(uint64_t pc, uint64_t t); void track(uint64_t pc, uint64_t t); }
namespace parameterized { uint64_t predict(uint64_t pc); void update(uint64_t pc, uint64_t t); void track(uint64_t pc, uint64_t t); }
#endif
