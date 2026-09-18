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

#ifndef INF_STREAM_H
#define INF_STREAM_H

#include <array>
#include <bzlib.h>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <lzma.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <zlib.h>
#include <zstd.h>

#include "champsim_assert.h"

namespace champsim
{
namespace decomp_tags
{
enum class status_t { CAN_CONTINUE, END, ERROR };

struct inflate_result {
  status_t status;
  const char* detail;
};

namespace detail
{
template <typename State, typename R, R (*Del)(State*)>
struct end_deleter {
  void operator()(State* s)
  {
    Del(s);
    delete s;
  }
};
} // namespace detail

struct bzip2_tag_t {
  using state_type = bz_stream;
  using in_char_type = std::remove_pointer_t<decltype(state_type::next_in)>;
  using out_char_type = std::remove_pointer_t<decltype(state_type::next_out)>;
  using deflate_state_type = std::unique_ptr<state_type, detail::end_deleter<state_type, int, ::BZ2_bzCompressEnd>>;
  using inflate_state_type = std::unique_ptr<state_type, detail::end_deleter<state_type, int, ::BZ2_bzDecompressEnd>>;
  using status_type = status_t;
  static constexpr const char* name = "bzip2";
  static constexpr bool supports_concatenation = false;

  static status_type deflate(deflate_state_type& x, bool flush)
  {
    auto ret = ::BZ2_bzCompress(x.get(), flush ? BZ_FLUSH : BZ_RUN);
    if (ret == BZ_RUN_OK) {
      return status_type::CAN_CONTINUE;
    }
    if (ret == BZ_FLUSH_OK) {
      return status_type::END;
    }
    return status_type::ERROR;
  }

  static inflate_result inflate(inflate_state_type& x)
  {
    const auto ret = ::BZ2_bzDecompress(x.get());
    if (ret == BZ_OK) {
      return {status_type::CAN_CONTINUE, "continuing"};
    }
    if (ret == BZ_STREAM_END) {
      return {status_type::END, "clean stream end"};
    }
    switch (ret) {
    case BZ_DATA_ERROR:
      return {status_type::ERROR, "data corruption"};
    case BZ_DATA_ERROR_MAGIC:
      return {status_type::ERROR, "invalid stream header"};
    case BZ_MEM_ERROR:
      return {status_type::ERROR, "memory allocation failure"};
    case BZ_PARAM_ERROR:
      return {status_type::ERROR, "invalid decoder parameter"};
    default:
      return {status_type::ERROR, "decoder state error"};
    }
  }

  static deflate_state_type new_deflate_state()
  {
    deflate_state_type state{new state_type};
    *state = state_type{NULL, 0U, 0U, 0U, NULL, 0U, 0U, 0U, NULL, NULL, NULL, NULL};
    const auto ret = ::BZ2_bzCompressInit(state.get(), 9, 0, 0);
    if (ret != BZ_OK) {
      delete state.release();
      throw std::runtime_error{"bzip2 compression initialization failed with code " + std::to_string(ret)};
    }
    return state;
  }

  static inflate_state_type new_inflate_state()
  {
    inflate_state_type state{new state_type};
    *state = state_type{NULL, 0U, 0U, 0U, NULL, 0U, 0U, 0U, NULL, NULL, NULL, NULL};
    const auto ret = ::BZ2_bzDecompressInit(state.get(), 0, 0);
    if (ret != BZ_OK) {
      delete state.release();
      throw std::runtime_error{"bzip2 decompression initialization failed with code " + std::to_string(ret)};
    }
    return state;
  }
};

template <int window = 15 + 16, int compression = Z_DEFAULT_COMPRESSION>
struct gzip_tag_t {
  using state_type = z_stream;
  using in_char_type = std::remove_pointer_t<decltype(state_type::next_in)>;
  using out_char_type = std::remove_pointer_t<decltype(state_type::next_out)>;
  using deflate_state_type = std::unique_ptr<state_type, detail::end_deleter<state_type, int, ::deflateEnd>>;
  using inflate_state_type = std::unique_ptr<state_type, detail::end_deleter<state_type, int, ::inflateEnd>>;
  using status_type = status_t;
  static constexpr const char* name = "gzip";
  static constexpr bool supports_concatenation = false;

  static status_type deflate(deflate_state_type& x, bool flush)
  {
    auto ret = ::deflate(x.get(), flush ? Z_FINISH : Z_NO_FLUSH);
    if (ret == Z_OK) {
      return status_type::CAN_CONTINUE;
    }
    if (ret == Z_STREAM_END) {
      return status_type::END;
    }
    return status_type::ERROR;
  }

  static inflate_result inflate(inflate_state_type& x)
  {
    const auto ret = ::inflate(x.get(), Z_NO_FLUSH);
    if (ret == Z_OK) {
      return {status_type::CAN_CONTINUE, "continuing"};
    }
    if (ret == Z_STREAM_END) {
      return {status_type::END, "clean stream end"};
    }
    if (x->msg != nullptr) {
      return {status_type::ERROR, x->msg};
    }
    switch (ret) {
    case Z_DATA_ERROR:
      return {status_type::ERROR, "data corruption"};
    case Z_MEM_ERROR:
      return {status_type::ERROR, "memory allocation failure"};
    case Z_NEED_DICT:
      return {status_type::ERROR, "preset dictionary required"};
    case Z_BUF_ERROR:
      return {status_type::ERROR, "decoder made no progress"};
    default:
      return {status_type::ERROR, "decoder state error"};
    }
  }

  static deflate_state_type new_deflate_state()
  {
    deflate_state_type state{new state_type};
    *state = state_type{Z_NULL, 0, 0, Z_NULL, 0, 0, NULL, NULL, Z_NULL, Z_NULL, Z_NULL, 0, 0UL, 0UL};
    const auto ret = ::deflateInit(state.get(), compression);
    if (ret != Z_OK) {
      delete state.release();
      throw std::runtime_error{"gzip compression initialization failed with code " + std::to_string(ret)};
    }
    return state;
  }

  static inflate_state_type new_inflate_state()
  {
    inflate_state_type state{new state_type};
    *state = state_type{Z_NULL, 0, 0, Z_NULL, 0, 0, NULL, NULL, Z_NULL, Z_NULL, Z_NULL, 0, 0UL, 0UL};
    const auto ret = ::inflateInit2(state.get(), window);
    if (ret != Z_OK) {
      delete state.release();
      throw std::runtime_error{"gzip decompression initialization failed with code " + std::to_string(ret)};
    }
    return state;
  }
};

template <uint32_t flags = 0>
struct lzma_tag_t {
  using state_type = lzma_stream;
  using in_char_type = std::remove_const_t<std::remove_pointer_t<decltype(state_type::next_in)>>;
  using out_char_type = std::remove_pointer_t<decltype(state_type::next_out)>;
  using deflate_state_type = std::unique_ptr<state_type, detail::end_deleter<state_type, void, ::lzma_end>>;
  using inflate_state_type = std::unique_ptr<state_type, detail::end_deleter<state_type, void, ::lzma_end>>;
  using status_type = status_t;
  static constexpr const char* name = "xz";
  static constexpr bool supports_concatenation = false;

  static status_type deflate(deflate_state_type& x, bool flush)
  {
    auto ret = ::lzma_code(x.get(), flush ? LZMA_FULL_FLUSH : LZMA_RUN);
    if (ret == LZMA_OK) {
      return status_type::CAN_CONTINUE;
    } else if (ret == LZMA_STREAM_END) {
      return status_type::END;
    } else {
      return status_type::ERROR;
    }
  }

  static inflate_result inflate(inflate_state_type& x)
  {
    const auto ret = ::lzma_code(x.get(), LZMA_RUN);
    if (ret == LZMA_OK) {
      return {status_type::CAN_CONTINUE, "continuing"};
    }
    if (ret == LZMA_STREAM_END) {
      return {status_type::END, "clean stream end"};
    }
    switch (ret) {
    case LZMA_FORMAT_ERROR:
      return {status_type::ERROR, "invalid stream format"};
    case LZMA_OPTIONS_ERROR:
      return {status_type::ERROR, "unsupported stream options"};
    case LZMA_DATA_ERROR:
      return {status_type::ERROR, "data corruption"};
    case LZMA_BUF_ERROR:
      return {status_type::ERROR, "decoder made no progress"};
    case LZMA_MEM_ERROR:
    case LZMA_MEMLIMIT_ERROR:
      return {status_type::ERROR, "memory allocation or limit failure"};
    default:
      return {status_type::ERROR, "decoder state error"};
    }
  }

  static deflate_state_type new_deflate_state()
  {
    deflate_state_type state{new state_type};
    *state = LZMA_STREAM_INIT;
    auto ret = ::lzma_easy_encoder(state.get(), LZMA_PRESET_DEFAULT, LZMA_CHECK_CRC64);
    if (ret != LZMA_OK) {
      throw std::runtime_error{"xz compression initialization failed with code " + std::to_string(ret)};
    }
    return state;
  }

  static inflate_state_type new_inflate_state()
  {
    inflate_state_type state{new state_type};
    *state = LZMA_STREAM_INIT;
    auto ret = ::lzma_stream_decoder(state.get(), std::numeric_limits<uint64_t>::max(), flags);
    if (ret != LZMA_OK) {
      throw std::runtime_error{"xz decompression initialization failed with code " + std::to_string(ret)};
    }
    return state;
  }
};
// zstd's streaming API does not expose a zlib-shaped state object, so this tag
// supplies one: inf_streambuf drives decompression purely through the
// next_in/avail_in/next_out/avail_out/total_out members, which are translated
// into ZSTD_inBuffer/ZSTD_outBuffer on each call.
//
// Only the inflate direction is implemented, since nothing in the tree
// compresses. Concatenated frames are handled for free: ZSTD_decompressStream
// reports a frame boundary with 0 and begins the next frame on the following
// call, so no explicit reset is required.
struct zstd_tag_t {
  using in_char_type = unsigned char;
  using out_char_type = unsigned char;

  struct state_type {
    const in_char_type* next_in = nullptr;
    std::size_t avail_in = 0;
    out_char_type* next_out = nullptr;
    std::size_t avail_out = 0;
    std::size_t total_out = 0;
    ::ZSTD_DCtx* dctx = nullptr;
  };

  struct state_deleter {
    void operator()(state_type* s) const
    {
      if (s != nullptr) {
        ::ZSTD_freeDCtx(s->dctx);
        delete s; // NOLINT(cppcoreguidelines-owning-memory)
      }
    }
  };

  using inflate_state_type = std::unique_ptr<state_type, state_deleter>;
  using status_type = status_t;
  static constexpr const char* name = "zstd";
  static constexpr bool supports_concatenation = true;

  static inflate_result inflate(inflate_state_type& x)
  {
    ::ZSTD_inBuffer in{x->next_in, x->avail_in, 0};
    ::ZSTD_outBuffer out{x->next_out, x->avail_out, 0};

    const std::size_t ret = ::ZSTD_decompressStream(x->dctx, &out, &in);

    x->next_in += in.pos;
    x->avail_in -= in.pos;
    x->next_out += out.pos;
    x->avail_out -= out.pos;
    x->total_out += out.pos;

    if (::ZSTD_isError(ret) != 0U) {
      return {status_type::ERROR, ::ZSTD_getErrorName(ret)};
    }
    // A return of zero means the frame completed cleanly.
    return (ret == 0) ? inflate_result{status_type::END, "clean frame end"} : inflate_result{status_type::CAN_CONTINUE, "continuing"};
  }

  static inflate_state_type new_inflate_state()
  {
    inflate_state_type state{new state_type};
    state->dctx = ::ZSTD_createDCtx();
    if (state->dctx == nullptr) {
      throw std::bad_alloc{};
    }

    // A bare DCtx refuses frames whose window exceeds ZSTD_WINDOWLOG_LIMIT_DEFAULT
    // (27), which any trace compressed with --long or --ultra will. Raise the
    // limit to whatever this build of zstd allows. ZSTD_WINDOWLOG_MAX itself is
    // behind ZSTD_STATIC_LINKING_ONLY, so query the bound instead.
    const auto bounds = ::ZSTD_dParam_getBounds(ZSTD_d_windowLogMax);
    if (::ZSTD_isError(bounds.error) != 0U) {
      throw std::runtime_error{"zstd decompression initialization failed while querying the window bound: " + std::string{::ZSTD_getErrorName(bounds.error)}};
    }
    const auto ret = ::ZSTD_DCtx_setParameter(state->dctx, ZSTD_d_windowLogMax, bounds.upperBound);
    if (::ZSTD_isError(ret) != 0U) {
      throw std::runtime_error{"zstd decompression initialization failed: " + std::string{::ZSTD_getErrorName(ret)}};
    }
    return state;
  }
};
} // namespace decomp_tags

template <typename Tag, typename StreamType = std::ifstream>
struct inf_istream {
  template <typename IStrm>
  class inf_streambuf : public std::basic_streambuf<typename IStrm::char_type, std::char_traits<typename IStrm::char_type>>
  {
  private:
    using base_type = std::basic_streambuf<typename IStrm::char_type, std::char_traits<typename IStrm::char_type>>;
    using int_type = typename base_type::int_type;
    using char_type = typename base_type::char_type;
    using strm_in_buf_type = typename Tag::in_char_type;
    using strm_out_buf_type = typename Tag::out_char_type;

    constexpr static std::size_t CHUNK = (1 << 16);

    std::array<strm_in_buf_type, CHUNK> in_buf;
    std::array<char_type, CHUNK> out_buf;
    typename std::add_pointer<IStrm>::type src;
    std::string source_label;
    typename Tag::inflate_state_type strm;
    bool physical_eof = false;
    bool clean_end = false;

    static typename Tag::inflate_state_type make_inflate_state(const std::string& label)
    {
      try {
        return Tag::new_inflate_state();
      } catch (const std::exception& err) {
        throw std::runtime_error{"trace '" + label + "': " + Tag::name + " decompression initialization failed: " + err.what()};
      }
    }

  public:
    explicit inf_streambuf(IStrm* in, std::string label) : src(in), source_label(std::move(label)), strm(make_inflate_state(source_label)) {}
    explicit inf_streambuf(Tag /*tag*/, IStrm* in, std::string label) : inf_streambuf(in, std::move(label)) {}

    [[nodiscard]] std::size_t bytes_read() const { return strm->total_out - (this->egptr() - this->gptr()); }

  protected:
    int_type underflow() override;
  };

  std::string source_label;
  std::unique_ptr<StreamType> underlying;
  std::unique_ptr<inf_streambuf<StreamType>> buffer;
  std::streamsize gcount_ = 0;
  bool eof_ = false;

  inf_istream& read(char* s, std::streamsize count)
  {
    std::istream inflated{buffer.get()};
    inflated.exceptions(std::ios::badbit);
    inflated.read(s, count);
    gcount_ = inflated.gcount();
    eof_ = inflated.eof();
    return *this;
  }

  [[nodiscard]] bool eof() const { return eof_; }
  [[nodiscard]] std::streamsize gcount() const { return gcount_; }

  explicit inf_istream(std::string s)
      : source_label(s), underlying(std::make_unique<StreamType>(s)), buffer(std::make_unique<inf_streambuf<StreamType>>(underlying.get(), source_label))
  {
  }
  explicit inf_istream(StreamType&& str, std::string label = "<memory>")
      : source_label(std::move(label)), underlying(std::make_unique<StreamType>(std::move(str))),
        buffer(std::make_unique<inf_streambuf<StreamType>>(underlying.get(), source_label))
  {
  }
};

template <typename T, typename S>
template <typename I>
auto inf_istream<T, S>::inf_streambuf<I>::underflow() -> int_type
{
  std::array<strm_out_buf_type, std::tuple_size<decltype(out_buf)>::value> uns_out_buf;

  strm->avail_out = uns_out_buf.size();
  strm->next_out = uns_out_buf.data();
  do {
    if (clean_end && !T::supports_concatenation) {
      if (strm->avail_in != 0) {
        throw std::runtime_error{"trace '" + source_label + "': unexpected data after the " + T::name + " stream"};
      }
      if (!physical_eof) {
        const auto next = src->peek();
        if (next != base_type::traits_type::eof()) {
          throw std::runtime_error{"trace '" + source_label + "': unexpected data after the " + T::name + " stream"};
        }
        if (src->bad() || (src->fail() && !src->eof())) {
          throw std::runtime_error{"trace '" + source_label + "': input read failed while decoding " + T::name};
        }
        physical_eof = src->eof();
      }
      this->setg(this->out_buf.data(), this->out_buf.data(), this->out_buf.data());
      return base_type::traits_type::eof();
    }

    // Check to see if we have consumed all available input
    if (strm->avail_in == 0) {
      if (physical_eof) {
        if (!clean_end) {
          throw std::runtime_error{"trace '" + source_label + "': truncated " + T::name + " stream"};
        }
        this->setg(this->out_buf.data(), this->out_buf.data(), this->out_buf.data());
        return base_type::traits_type::eof();
      }

      // Read data from the stream and convert to zlib-appropriate format
      std::array<char_type, std::tuple_size<decltype(in_buf)>::value> sig_in_buf;
      src->read(sig_in_buf.data(), sig_in_buf.size());
      const auto bytes_read = src->gcount();
      CHAMPSIM_ASSERT(bytes_read >= 0);
      if (src->bad() || (src->fail() && !src->eof())) {
        throw std::runtime_error{"trace '" + source_label + "': input read failed while decoding " + T::name};
      }
      physical_eof = src->eof();
      std::memcpy(in_buf.data(), sig_in_buf.data(), static_cast<std::size_t>(bytes_read));

      // Record that bytes are available in in_buf
      strm->avail_in = static_cast<decltype(strm->avail_in)>(bytes_read);
      strm->next_in = in_buf.data();

      // If we failed to get any data
      if (strm->avail_in == 0) {
        if (!clean_end) {
          throw std::runtime_error{"trace '" + source_label + "': truncated " + T::name + " stream"};
        }
        this->setg(this->out_buf.data(), this->out_buf.data(), this->out_buf.data());
        return base_type::traits_type::eof();
      }
    }

    // Perform inflation
    const auto avail_in_before = strm->avail_in;
    const auto avail_out_before = strm->avail_out;
    const auto result = T::inflate(strm);
    if (result.status == T::status_type::ERROR) {
      throw std::runtime_error{"trace '" + source_label + "': " + T::name + " decompression failed: " + result.detail};
    }
    clean_end = result.status == T::status_type::END;
    if (result.status == T::status_type::CAN_CONTINUE && strm->avail_in == avail_in_before && strm->avail_out == avail_out_before) {
      throw std::runtime_error{"trace '" + source_label + "': " + T::name + " decompression failed: decoder made no progress"};
    }
  }
  // Repeat until we actually get new output
  while (strm->avail_out == uns_out_buf.size());

  // Copy into a format appropriate for the stream
  std::memcpy(this->out_buf.data(), uns_out_buf.data(), uns_out_buf.size() - strm->avail_out);

  auto bytes_remaining = std::size(uns_out_buf) - strm->avail_out;
  CHAMPSIM_ASSERT(bytes_remaining <= std::numeric_limits<std::make_signed_t<decltype(bytes_remaining)>>::max());
  this->setg(this->out_buf.data(), this->out_buf.data(),
             std::next(this->out_buf.data(), static_cast<std::make_signed_t<decltype(bytes_remaining)>>(bytes_remaining)));
  return base_type::traits_type::to_int_type(this->out_buf.front());
}
} // namespace champsim

#endif
