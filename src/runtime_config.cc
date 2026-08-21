#include "runtime_config.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <fmt/core.h>

#include <toml++/toml.hpp>

namespace
{
// One TOML scalar node into the store's variant. Anything non-scalar is a
// schema violation: the configuration language deliberately has no arrays,
// matching the statistics document's own rule.
champsim::runtime_config::value_type to_value(const toml::node& node, const std::string& key)
{
  if (const auto* val = node.as_integer()) {
    return val->get();
  }
  if (const auto* val = node.as_floating_point()) {
    return val->get();
  }
  if (const auto* val = node.as_boolean()) {
    return val->get();
  }
  if (const auto* val = node.as_string()) {
    return val->get();
  }
  throw std::runtime_error(fmt::format("runtime config: key '{}' holds a {}, but only integer, float, boolean, and string values are accepted", key,
                                       node.is_array() ? "array" : "non-scalar value"));
}

void flatten(const toml::table& table, const std::string& prefix, std::map<std::string, champsim::runtime_config::value_type, std::less<>>& into)
{
  for (const auto& [key, node] : table) {
    const auto path = prefix.empty() ? std::string{key.str()} : prefix + "." + std::string{key.str()};
    if (const auto* sub = node.as_table()) {
      flatten(*sub, path, into);
    } else {
      into.insert_or_assign(path, to_value(node, path));
    }
  }
}
} // namespace

void champsim::runtime_config::load_file(const std::string& path)
{
  toml::table table{};
  try {
    table = toml::parse_file(path);
  } catch (const toml::parse_error& err) {
    throw std::runtime_error(fmt::format("runtime config: cannot parse '{}': {}", path, err.description()));
  } catch (const std::exception& err) {
    throw std::runtime_error(fmt::format("runtime config: cannot read '{}': {}", path, err.what()));
  }
  flatten(table, {}, values_);
  files_.push_back(path);
}

void champsim::runtime_config::set(std::string_view assignment)
{
  const auto equals = assignment.find('=');
  if (equals == std::string_view::npos || equals == 0) {
    throw std::runtime_error(fmt::format("runtime config: malformed --set '{}': expected key=value", assignment));
  }
  const std::string key{assignment.substr(0, equals)};
  const std::string text{assignment.substr(equals + 1)};

  // Type by parse: bool, then integer, then float, else string. The full text
  // must consume -- "12abc" is a string, not the integer 12.
  if (text == "true" || text == "false") {
    values_.insert_or_assign(key, text == "true");
    return;
  }
  char* end = nullptr;
  // strtoll/strtod clamp on overflow and signal only through errno; without
  // the check an out-of-range --set would silently become INT64_MAX or inf,
  // while the same value in a --config file is a fatal toml++ parse error.
  errno = 0;
  const long long as_int = std::strtoll(text.c_str(), &end, 10);
  if (end != text.c_str() && *end == '\0') {
    if (errno == ERANGE) {
      throw std::runtime_error(fmt::format("runtime config: '{}': {} is out of range", assignment, text));
    }
    values_.insert_or_assign(key, static_cast<int64_t>(as_int));
    return;
  }
  errno = 0;
  const double as_double = std::strtod(text.c_str(), &end);
  if (end != text.c_str() && *end == '\0') {
    if (errno == ERANGE) {
      throw std::runtime_error(fmt::format("runtime config: '{}': {} is out of range", assignment, text));
    }
    values_.insert_or_assign(key, as_double);
    return;
  }
  values_.insert_or_assign(key, text);
}

std::string champsim::runtime_config::render(const value_type& val)
{
  return std::visit(
      [](const auto& held) -> std::string {
        using held_type = std::decay_t<decltype(held)>;
        if constexpr (std::is_same_v<held_type, bool>) {
          return held ? "true" : "false";
        } else if constexpr (std::is_same_v<held_type, double>) {
          // A TOML float keeps a decimal point; fmt's shortest form drops it
          // for whole numbers, which would read back as an integer.
          auto text = fmt::format("{}", held);
          if (text.find('.') == std::string::npos && text.find('e') == std::string::npos && text.find("inf") == std::string::npos
              && text.find("nan") == std::string::npos) {
            text += ".0";
          }
          return text;
        } else if constexpr (std::is_same_v<held_type, std::string>) {
          // A TOML basic string, escaped exactly as ::quote() in
          // src/toml_printer.cc and toml_string() in config/config_record.py
          // escape it -- three implementations of one document's syntax.
          std::string out{"\""};
          for (auto chr : held) {
            switch (chr) {
            case '"':
              out += "\\\"";
              break;
            case '\\':
              out += "\\\\";
              break;
            case '\b':
              out += "\\b";
              break;
            case '\f':
              out += "\\f";
              break;
            case '\n':
              out += "\\n";
              break;
            case '\r':
              out += "\\r";
              break;
            case '\t':
              out += "\\t";
              break;
            default:
              if (static_cast<unsigned char>(chr) < 0x20 || chr == 0x7f) {
                out += fmt::format("\\u{:04X}", static_cast<unsigned>(static_cast<unsigned char>(chr)));
              } else {
                out.push_back(chr);
              }
              break;
            }
          }
          out += "\"";
          return out;
        } else {
          return fmt::format("{}", held);
        }
      },
      val);
}

std::string champsim::runtime_config::type_name(const value_type& val)
{
  return std::visit(
      [](const auto& held) -> std::string {
        using held_type = std::decay_t<decltype(held)>;
        if constexpr (std::is_same_v<held_type, bool>) {
          return "boolean";
        } else if constexpr (std::is_same_v<held_type, double>) {
          return "float";
        } else if constexpr (std::is_same_v<held_type, std::string>) {
          return "string";
        } else {
          return "integer";
        }
      },
      val);
}

void champsim::runtime_config::note_consulted(std::string_view key, const value_type& fallback) const
{
  consulted_.insert_or_assign(std::string{key}, render(fallback));
}

void champsim::runtime_config::note_consulted_raw(std::string_view key, std::string rendered) const
{
  consulted_.insert_or_assign(std::string{key}, std::move(rendered));
}

std::vector<std::pair<std::string, std::string>> champsim::runtime_config::applied() const
{
  std::vector<std::pair<std::string, std::string>> out{};
  std::transform(std::begin(values_), std::end(values_), std::back_inserter(out),
                 [](const auto& entry) { return std::pair{entry.first, render(entry.second)}; });
  return out;
}

std::vector<std::pair<std::string, std::string>> champsim::runtime_config::consulted() const
{
  std::vector<std::pair<std::string, std::string>> out{};
  std::copy(std::begin(consulted_), std::end(consulted_), std::back_inserter(out));
  return out;
}

std::vector<std::string> champsim::runtime_config::unconsulted_keys() const
{
  const auto lower = [](std::string_view text) {
    std::string out{};
    std::transform(std::begin(text), std::end(text), std::back_inserter(out),
                   [](char chr) { return static_cast<char>(std::tolower(static_cast<unsigned char>(chr))); });
    return out;
  };

  std::vector<std::string> complaints{};
  for (const auto& [key, val] : values_) {
    if (consulted_.find(key) != std::end(consulted_)) {
      continue;
    }
    std::string complaint = "nothing consumed configuration key '" + key + "'";
    const auto folded = lower(key);
    const auto suggestion = std::find_if(std::begin(consulted_), std::end(consulted_), [&](const auto& entry) { return lower(entry.first) == folded; });
    if (suggestion != std::end(consulted_)) {
      complaint += "; did you mean '" + suggestion->first + "'?";
    }
    complaints.push_back(std::move(complaint));
  }
  return complaints;
}
