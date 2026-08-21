#ifndef CHAMPSIM_DEFS_H
#define CHAMPSIM_DEFS_H

#include <cstddef>
#include <cstdint>

/*
 * Compile-time machine constants.
 *
 * These are the parameters that cannot be runtime knobs: they size types and
 * are read during static initialization (champsim::defaults' namespace-scope
 * builders consume LOG2_BLOCK_SIZE/LOG2_PAGE_SIZE before main runs), so a
 * value parsed at startup would be captured stale.
 *
 * Everything else is configured at run time from a TOML file -- see
 * configs/README.md. Edit here and rebuild only to change the numbers below,
 * or the machine's shape in src/environment.cc.
 */
namespace champsim::defs
{
// One binary per core count: multi-core development follows single-core
// iteration, so this is deliberately not a runtime knob.
inline constexpr std::size_t num_cpus = 1;

inline constexpr std::size_t block_size = 64;
inline constexpr std::size_t page_size = 4096;

// The default heartbeat interval; sim.heartbeat_frequency and the
// --heartbeat-frequency flag both override it at run time.
inline constexpr std::uint64_t heartbeat_frequency = 10000000;

// The modules a binary uses unless a runtime key selects another. These are
// the champsim::defaults packs; changing them here changes what a
// configuration-free run simulates.
inline constexpr const char* default_branch_predictor = "hashed_perceptron";
inline constexpr const char* default_btb = "basic_btb";
inline constexpr const char* default_prefetcher = "no";
inline constexpr const char* default_replacement = "lru";
} // namespace champsim::defs

#endif
