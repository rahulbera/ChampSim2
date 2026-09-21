// Evaluate one BLBP configuration over one or more branch streams.
//
// This is the tuning objective function: it drives the SAME inc/blbp/blbp.h
// predictor the simulator uses (no reimplementation, so tuned constants
// transfer by construction), over streams extracted by extract_stream.py, and
// reports indirect mispredicts counted ONLY after each stream's warmup
// boundary -- the same 50M-instruction warmup a full ChampSim run performs.
//
// Deliberately outside the ChampSim build (standalone Makefile): it must
// compile on the cluster with nothing but g++ and the two blbp headers.
//
// Config file: flat text, one item per line, emitted by tuner.py and parsed
// here -- no JSON dependency on the cluster.
//
//   M 952
//   theta_init 14
//   adaptive 1
//   selective 1
//   transfer 2 4 6 8 11 14 18 24
//   interval 0 13
//   interval 1 33
//   ...                       (N = #intervals + 1; the +1 is local history)
//
// Usage: blbp_eval <config> <out> <stream1> [stream2 ...]
//   For each streamN a sidecar streamN.meta.json must exist (extract_stream.py
//   writes it); only its "warmup_branches" integer is read, with a
//   deliberately narrow parser.
//
// Output <out>: one line per stream "name indirect_branches mispredicts",
// then "TOTAL sum_indirect sum_mispredicts".

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "blbp/blbp.h"

namespace
{
constexpr uint8_t BT_INDIRECT = 1;
constexpr uint8_t BT_CONDITIONAL = 2;
constexpr uint8_t BT_INDIRECT_CALL = 4;

#pragma pack(push, 1)
struct stream_rec {
  uint64_t pc;
  uint64_t next_ip;
  uint8_t branch_type;
  uint8_t taken;
};
#pragma pack(pop)
static_assert(sizeof(stream_rec) == 18, "stream record layout");

champsim::blbp::predictor_config parse_config(const std::string& path)
{
  champsim::blbp::predictor_config cfg{};
  cfg.intervals.clear();
  std::ifstream in(path);
  if (!in) {
    std::fprintf(stderr, "cannot open config %s\n", path.c_str());
    std::exit(2);
  }
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream ls(line);
    std::string key;
    if (!(ls >> key) || key.empty() || key[0] == '#') {
      continue;
    }
    if (key == "M") {
      ls >> cfg.engine.M;
    } else if (key == "theta_init") {
      ls >> cfg.engine.theta_init;
    } else if (key == "adaptive") {
      int v = 1;
      ls >> v;
      cfg.engine.adaptive_theta = (v != 0);
    } else if (key == "selective") {
      int v = 1;
      ls >> v;
      cfg.engine.selective_bits = (v != 0);
    } else if (key == "transfer") {
      cfg.engine.transfer.clear();
      int v = 0;
      while (ls >> v) {
        cfg.engine.transfer.push_back(v);
      }
    } else if (key == "interval") {
      int lo = 0;
      int hi = 0;
      ls >> lo >> hi;
      cfg.intervals.emplace_back(lo, hi);
    } else {
      std::fprintf(stderr, "unknown config key '%s'\n", key.c_str());
      std::exit(2);
    }
  }
  cfg.engine.N = static_cast<int>(cfg.intervals.size()) + 1;
  return cfg;
}

// The meta sidecar is JSON written by extract_stream.py; only one integer is
// needed. A full JSON parser is deliberately avoided -- this reads the value
// following the "warmup_branches" key and fails loudly if absent.
long warmup_branches_of(const std::string& stream_path)
{
  std::ifstream in(stream_path + ".meta.json");
  if (!in) {
    std::fprintf(stderr, "missing sidecar %s.meta.json\n", stream_path.c_str());
    std::exit(2);
  }
  std::stringstream ss;
  ss << in.rdbuf();
  const std::string s = ss.str();
  const auto pos = s.find("\"warmup_branches\"");
  if (pos == std::string::npos) {
    std::fprintf(stderr, "no warmup_branches in %s.meta.json\n", stream_path.c_str());
    std::exit(2);
  }
  return std::strtol(s.c_str() + s.find(':', pos) + 1, nullptr, 10);
}
} // namespace

int main(int argc, char** argv)
{
  if (argc < 4) {
    std::fprintf(stderr, "usage: %s <config> <out> <stream...>\n", argv[0]);
    return 2;
  }
  const auto cfg = parse_config(argv[1]);
  std::ofstream out(argv[2]);

  long total_ind = 0;
  long total_miss = 0;

  for (int a = 3; a < argc; ++a) {
    const std::string path = argv[a];
    const long warmup = warmup_branches_of(path);

    // Fresh predictor per stream, as in a full run.
    champsim::blbp::predictor pred{cfg};

    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
      std::fprintf(stderr, "cannot open %s\n", path.c_str());
      return 2;
    }
    std::vector<stream_rec> buf(1 << 16);
    long seen = 0;
    long ind = 0;
    long miss = 0;
    std::size_t got = 0;
    while ((got = std::fread(buf.data(), sizeof(stream_rec), buf.size(), f)) > 0) {
      for (std::size_t i = 0; i < got; ++i) {
        const auto& r = buf[i];
        if (r.branch_type == BT_CONDITIONAL) {
          pred.note_conditional(r.taken != 0);
        } else if (r.branch_type == BT_INDIRECT || r.branch_type == BT_INDIRECT_CALL) {
          if (seen >= warmup) {
            ++ind;
            const auto p = pred.predict(r.pc);
            if (!p.has_value() || *p != r.next_ip) {
              ++miss;
            }
          }
          pred.update(r.pc, r.next_ip);
        }
        ++seen;
      }
    }
    std::fclose(f);

    out << path << " " << ind << " " << miss << "\n";
    total_ind += ind;
    total_miss += miss;
  }
  out << "TOTAL " << total_ind << " " << total_miss << "\n";
  std::cout << "TOTAL " << total_ind << " " << total_miss << "\n";
  return 0;
}
