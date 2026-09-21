// Drives both copies through one identical branch stream and requires every
// indirect prediction to match bit-for-bit.
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include "api.h"

#pragma pack(push, 1)
struct rec { uint64_t pc, target; uint8_t indirect; };
#pragma pack(pop)

int main(int argc, char** argv)
{
  if (argc < 2) { std::fprintf(stderr, "usage: %s <stream.bin>\n", argv[0]); return 2; }
  std::FILE* f = std::fopen(argv[1], "rb");
  if (!f) { std::perror("open"); return 2; }

  std::vector<rec> in;
  rec r;
  while (std::fread(&r, sizeof(r), 1, f) == 1) in.push_back(r);
  std::fclose(f);

  unsigned long long nind = 0, mismatch = 0;
  const rec* first_bad = nullptr;
  uint64_t bad_a = 0, bad_b = 0;

  for (const auto& e : in) {
    if (e.indirect) {
      const uint64_t a = pristine::predict(e.pc);
      const uint64_t b = parameterized::predict(e.pc);
      ++nind;
      if (a != b && mismatch++ == 0) { first_bad = &e; bad_a = a; bad_b = b; }
      pristine::update(e.pc, e.target);
      parameterized::update(e.pc, e.target);
    } else {
      pristine::track(e.pc, e.target);
      parameterized::track(e.pc, e.target);
    }
  }

  std::printf("branches=%zu indirect_predictions=%llu mismatches=%llu\n", in.size(), nind, mismatch);
  if (mismatch) {
    std::printf("FAIL first mismatch at pc=%#lx: pristine=%#lx parameterized=%#lx\n",
                (unsigned long)first_bad->pc, (unsigned long)bad_a, (unsigned long)bad_b);
    return 1;
  }
  std::printf("PASS bit-identical over %llu indirect predictions\n", nind);
  return 0;
}
