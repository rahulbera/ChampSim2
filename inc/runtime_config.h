#ifndef RUNTIME_CONFIG_H
#define RUNTIME_CONFIG_H

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace champsim
{
/*
 * The runtime configuration store: a flat key -> scalar map populated from
 * TOML files (--config) and command-line assignments (--set), applied in argv
 * order so the last definition of a key wins regardless of source.
 *
 * The generated environment's constructor consults it through value<T>(key,
 * fallback), where the fallback is the configure-time value config.sh baked
 * in -- so a binary run without --config/--set behaves exactly as it did
 * before this store existed.
 *
 * This class names no generated symbol, so it links into the test binary.
 */
class runtime_config
{
public:
  using value_type = std::variant<int64_t, double, bool, std::string>;

  // Non-copyable: consulted-key recording is how --knobs enumerates knobs and
  // how main verifies every module-knob key was consumed, and a copy would
  // record on the copy and discard the record. This also makes a module
  // configure(runtime_config cfg, ...) BY VALUE fail the has_configure trait
  // instead of silently mis-recording; the unconsumed-key error then points at
  // the module.
  runtime_config() = default;
  runtime_config(const runtime_config&) = delete;
  runtime_config& operator=(const runtime_config&) = delete;
  runtime_config(runtime_config&&) = default;
  runtime_config& operator=(runtime_config&&) = default;

  // Load one TOML file, flattening nested tables into dotted keys. Later
  // definitions overwrite earlier ones. Throws std::runtime_error on an
  // unreadable or unparseable file, or on a non-scalar value (the schema has
  // no arrays -- matching the statistics document's own rule).
  void load_file(const std::string& path);

  // Apply one "key=value" assignment. The value is typed by parse: true/false
  // -> bool, an integer literal -> int64, a float literal -> double, anything
  // else -> string. Throws std::runtime_error on a malformed assignment.
  void set(std::string_view assignment);

  // The configured value, or the fallback when the key is absent. int64 -> T
  // conversions are range-checked; double -> integer, string -> anything, and
  // int -> bool never convert (they would truncate or reinterpret silently).
  // Records the (key, fallback) pair so --knobs can enumerate what this
  // binary consults. Throws std::runtime_error on a type mismatch.
  template <typename T>
  T value(std::string_view key, T fallback) const;

  // As value(), additionally rejecting zero and negative results. Used by the
  // generated frequency/data-rate lookups, whose value divides into a clock
  // period: zero would be a division by zero and a negative clock runs time
  // backwards -- both silently corrupt a simulation rather than failing it.
  template <typename T>
  T positive_value(std::string_view key, T fallback) const
  {
    T result = value<T>(key, fallback);
    if (!(result > T{})) {
      throw std::runtime_error("runtime config: " + std::string{key} + " must be positive");
    }
    return result;
  }

  // Keys the user set that nothing consulted. The machine reads every key it
  // understands during construction, so a key left unread is one this binary
  // has no use for: a typo, or a knob aimed at a component or module that is
  // not part of this machine. Each is formatted as a ready-to-print
  // complaint, with a did-you-mean when a case-folded match WAS consulted.
  [[nodiscard]] std::vector<std::string> unconsulted_keys() const;

  // Keys the user set that the manifest does not contain, each formatted as a
  // ready-to-print complaint (with a did-you-mean when a case-folded match
  // exists). Empty means the configuration is valid.
  template <typename It>
  [[nodiscard]] std::vector<std::string> unknown_keys(It manifest_begin, It manifest_end) const;

  // Provenance, for [meta] and [config_override]: the files loaded, in order,
  // and the applied key -> value pairs (sorted, values in TOML syntax).
  [[nodiscard]] const std::vector<std::string>& files() const { return files_; }
  [[nodiscard]] std::vector<std::pair<std::string, std::string>> applied() const;

  // Every (key, rendered fallback) pair value<T>() has been asked for, sorted.
  // Constructing a throwaway environment against an empty store and reading
  // this back is how --knobs enumerates the binary's knobs.
  [[nodiscard]] std::vector<std::pair<std::string, std::string>> consulted() const;

  // Record that a key's EFFECTIVE value is not the one the store holds,
  // because something outside the store won a precedence contest for it --
  // today only --heartbeat-frequency over sim.heartbeat_frequency. The key
  // stays consulted (it is not unknown, it merely lost), and [config], which
  // states what the run actually used, reports the winner.
  template <typename T>
  void override_effective(std::string_view key, T effective) const
  {
    note_consulted(key, value_type{static_cast<int64_t>(effective)});
  }

private:
  [[nodiscard]] static std::string render(const value_type& val);
  void note_consulted(std::string_view key, const value_type& fallback) const;
  void note_consulted_raw(std::string_view key, std::string rendered) const;
  [[nodiscard]] static std::string type_name(const value_type& val);

  std::map<std::string, value_type, std::less<>> values_;
  std::vector<std::string> files_;
  // Recording is observation, not state of the configuration itself.
  mutable std::map<std::string, std::string, std::less<>> consulted_;
};

template <typename T>
T runtime_config::value(std::string_view key, T fallback) const
{
  static_assert(std::is_arithmetic_v<T> || std::is_same_v<T, std::string>, "the runtime store holds numeric, boolean, and string scalars");

  // Recorded AFTER the value is resolved, so the record holds what the run
  // actually used -- which is what lets [config] report the effective
  // configuration rather than the defaults it started from. The unsigned case
  // renders directly instead of through the variant's int64_t, which would
  // wrap a value above its maximum.
  const auto note = [this, key](const T& effective) {
    if constexpr (std::is_same_v<T, std::string>) {
      note_consulted(key, value_type{effective});
    } else if constexpr (std::is_same_v<T, bool>) {
      note_consulted(key, value_type{effective});
    } else if constexpr (std::is_floating_point_v<T>) {
      note_consulted(key, value_type{static_cast<double>(effective)});
    } else if constexpr (std::is_unsigned_v<T>) {
      note_consulted_raw(key, std::to_string(effective));
    } else {
      note_consulted(key, value_type{static_cast<int64_t>(effective)});
    }
  };

  auto found = values_.find(key);
  if (found == std::end(values_)) {
    note(fallback);
    return fallback;
  }
  const auto& held = found->second;

  if constexpr (std::is_same_v<T, std::string>) {
    if (const auto* val = std::get_if<std::string>(&held)) {
      note(*val);
      return *val;
    }
  } else if constexpr (std::is_same_v<T, bool>) {
    if (const auto* val = std::get_if<bool>(&held)) {
      note(*val);
      return *val;
    }
  } else if constexpr (std::is_floating_point_v<T>) {
    // An integer is a valid TOML spelling of a whole number of any type.
    if (const auto* val = std::get_if<double>(&held)) {
      note(static_cast<T>(*val));
      return static_cast<T>(*val);
    }
    if (const auto* val = std::get_if<int64_t>(&held)) {
      note(static_cast<T>(*val));
      return static_cast<T>(*val);
    }
  } else if constexpr (std::is_integral_v<T>) {
    if (const auto* val = std::get_if<int64_t>(&held)) {
      // Range-check the narrowing rather than truncate silently.
      using limits = std::numeric_limits<T>;
      const bool below = *val < static_cast<int64_t>(limits::lowest());
      const bool above =
          std::is_unsigned_v<T> ? static_cast<uint64_t>(*val) > static_cast<uint64_t>(limits::max()) : *val > static_cast<int64_t>(limits::max());
      if (*val < 0 && std::is_unsigned_v<T>) {
        throw std::runtime_error("runtime config: " + std::string{key} + " = " + render(held) + " is negative");
      }
      if (below || above) {
        throw std::runtime_error("runtime config: " + std::string{key} + " = " + render(held) + " is out of range");
      }
      note(static_cast<T>(*val));
      return static_cast<T>(*val);
    }
  }
  throw std::runtime_error("runtime config: " + std::string{key} + " holds a " + type_name(held) + ", which does not convert to the expected type");
}

template <typename It>
std::vector<std::string> runtime_config::unknown_keys(It manifest_begin, It manifest_end) const
{
  auto lower = [](std::string_view text) {
    std::string out{};
    for (auto chr : text) {
      out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(chr))));
    }
    return out;
  };

  std::vector<std::string> complaints{};
  for (const auto& [key, val] : values_) {
    if (std::find(manifest_begin, manifest_end, std::string_view{key}) != manifest_end) {
      continue;
    }
    std::string complaint = "unknown configuration key '" + key + "'";
    const auto folded = lower(key);
    auto suggestion = std::find_if(manifest_begin, manifest_end, [&](std::string_view candidate) { return lower(candidate) == folded; });
    if (suggestion != manifest_end) {
      complaint += "; did you mean '" + std::string{*suggestion} + "'?";
    }
    complaints.push_back(std::move(complaint));
  }
  return complaints;
}
} // namespace champsim

#endif
