/*
 *    Copyright 2023 The ChampSim Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "tracereader.h"

#include <fstream>
#include <stdexcept>
#include <string>

#include "inf_stream.h"
#include "repeatable.h"

namespace champsim
{
uint64_t tracereader::instr_unique_id = 0; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

ooo_model_instr apply_branch_target(ooo_model_instr branch, const ooo_model_instr& target)
{
  branch.branch_target = (branch.is_branch && branch.branch_taken) ? target.ip : champsim::address{};
  return branch;
}

template <template <class, class> typename R, typename T>
champsim::tracereader get_tracereader_for_type(std::string fname, uint8_t cpu)
{
  if (bool is_gzip_compressed = (fname.substr(std::size(fname) - 2) == "gz"); is_gzip_compressed) {
    return champsim::tracereader{R<T, champsim::inf_istream<champsim::decomp_tags::gzip_tag_t<>>>(cpu, fname)};
  }

  if (bool is_lzma_compressed = (fname.substr(std::size(fname) - 2) == "xz"); is_lzma_compressed) {
    return champsim::tracereader{R<T, champsim::inf_istream<champsim::decomp_tags::lzma_tag_t<>>>(cpu, fname)};
  }

  if (bool is_bzip2_compressed = (fname.substr(std::size(fname) - 3) == "bz2"); is_bzip2_compressed) {
    return champsim::tracereader{R<T, champsim::inf_istream<champsim::decomp_tags::bzip2_tag_t>>(cpu, fname)};
  }

  if (bool is_zstd_compressed = (fname.substr(std::size(fname) - 3) == "zst"); is_zstd_compressed) {
    return champsim::tracereader{R<T, champsim::inf_istream<champsim::decomp_tags::zstd_tag_t>>(cpu, fname)};
  }

  return champsim::tracereader{R<T, std::ifstream>(cpu, fname)};
}

// Read the first record of a v2 trace and report whether it declares an explicit
// branch type.
//
// Deliberately NOT done inside bulk_tracereader: champsim::repeatable
// reconstructs that object every time the trace wraps around (repeatable.h:41),
// which re-arms any per-object one-shot. Since main.cc enables repeat whenever
// -i/--simulation-instructions is given -- i.e. on every production run -- a
// per-object flag re-emits the banner once per lap, hundreds of times. This
// runs from get_tracereader(), which is called exactly once per trace file.
//
// Returns true (i.e. "stay quiet") when the file cannot be read or is shorter
// than one record: that is a different problem, reported elsewhere, and a
// second confusing banner would not help.
bool v2_trace_declares_branch_type(const std::string& fname)
{
  auto probe = [](auto stream) {
    input_instr_v2 first{};
    stream.read(reinterpret_cast<char*>(&first), sizeof(first)); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    if (stream.gcount() < static_cast<std::streamsize>(sizeof(first))) {
      return true;
    }
    return champsim::has_explicit_branch_type(first);
  };

  if (fname.size() >= 2 && fname.substr(std::size(fname) - 2) == "gz") {
    return probe(champsim::inf_istream<champsim::decomp_tags::gzip_tag_t<>>{fname});
  }
  if (fname.size() >= 2 && fname.substr(std::size(fname) - 2) == "xz") {
    return probe(champsim::inf_istream<champsim::decomp_tags::lzma_tag_t<>>{fname});
  }
  if (fname.size() >= 3 && fname.substr(std::size(fname) - 3) == "bz2") {
    return probe(champsim::inf_istream<champsim::decomp_tags::bzip2_tag_t>{fname});
  }
  if (fname.size() >= 3 && fname.substr(std::size(fname) - 3) == "zst") {
    return probe(champsim::inf_istream<champsim::decomp_tags::zstd_tag_t>{fname});
  }
  return probe(std::ifstream{fname, std::ios::binary});
}
} // namespace champsim

template <typename T, typename S>
using repeatable_reader_t = champsim::repeatable<champsim::bulk_tracereader<T, S>, uint8_t, std::string>;

champsim::tracereader get_tracereader(const std::string& fname, uint8_t cpu, bool is_cloudsuite, bool repeat, unsigned trace_version)
{
  // The trace format is headerless and unversioned, so reading a v2 file as v1
  // does not fail -- it silently yields wrong statistics (every 512-byte record
  // is consumed as eight 64-byte ones). A size check cannot catch this, since
  // 512 is a multiple of 64. Fall back on the naming convention to catch the
  // likely mistake. This is a convention check, not a format check: a v2 trace
  // named otherwise will still be misread if the version is wrong.
  // Match on the basename: a v1 trace living under a directory called
  // "champsim2-traces" must not trip the v2 guard.
  const auto slash = fname.find_last_of('/');
  const std::string basename = (slash == std::string::npos) ? fname : fname.substr(slash + 1);

  if (basename.find("champsim2") != std::string::npos && trace_version != 2) {
    throw std::invalid_argument{"Trace '" + fname + "' is named as a v2 trace but --trace-version is " + std::to_string(trace_version)};
  }

  // The mirror case: a v1-named trace read as v2 consumes eight 64-byte records
  // per 512-byte read and is just as silently wrong.
  if (basename.find("champsim2") == std::string::npos && basename.find("champsimtrace") != std::string::npos && trace_version == 2) {
    throw std::invalid_argument{"Trace '" + fname + "' is named as a v1 trace but --trace-version is 2"};
  }

  if (trace_version == 2) {
    // There is no v2 cloudsuite record; reading one format as the other would
    // produce plausible-looking garbage rather than an error, so refuse.
    if (is_cloudsuite) {
      throw std::invalid_argument{"The cloudsuite trace format has no version 2"};
    }

    // Branch types inferred from register usage are silently wrong on any trace
    // whose producer did not record the flags register: every conditional
    // branch then looks like an always-taken direct jump, the direction
    // predictor is never consulted, and unrelated predictors report identical,
    // plausible-looking MPKI. That has already happened once. Say so, loudly,
    // once per trace file.
    if (!champsim::v2_trace_declares_branch_type(fname)) {
      fmt::print(stderr,
                 "\n*** WARNING: '{}' carries no explicit branch type ***\n"
                 "  Branch types will be INFERRED from register usage. If this trace was\n"
                 "  produced by a tracer that does not record the flags register, every\n"
                 "  conditional branch is misclassified as an always-taken direct jump, the\n"
                 "  direction predictor is never consulted, and all branch predictors report\n"
                 "  identical, meaningless MPKI.\n"
                 "  Check with: trace_sanity_check -i <trace> -f v2 --check\n\n",
                 fname);
    }

    if (repeat) {
      return champsim::get_tracereader_for_type<repeatable_reader_t, input_instr_v2>(fname, cpu);
    }

    return champsim::get_tracereader_for_type<champsim::bulk_tracereader, input_instr_v2>(fname, cpu);
  }

  if (trace_version != 1) {
    throw std::invalid_argument{"Unknown trace version (expected 1 or 2)"};
  }

  if (is_cloudsuite && repeat) {
    return champsim::get_tracereader_for_type<repeatable_reader_t, cloudsuite_instr>(fname, cpu);
  }

  if (is_cloudsuite && !repeat) {
    return champsim::get_tracereader_for_type<champsim::bulk_tracereader, cloudsuite_instr>(fname, cpu);
  }

  if (!is_cloudsuite && repeat) {
    return champsim::get_tracereader_for_type<repeatable_reader_t, input_instr>(fname, cpu);
  }

  return champsim::get_tracereader_for_type<champsim::bulk_tracereader, input_instr>(fname, cpu);
}
