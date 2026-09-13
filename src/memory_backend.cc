#include "memory_backend.h"

#include <stdexcept>
#include <string>
#include <utility>
#include <fmt/format.h>

#include "runtime_config.h"

namespace champsim
{
std::unique_ptr<memory_backend> make_legacy_memory_backend(const runtime_config& cfg, std::vector<channel*> upper_levels);

std::unique_ptr<memory_backend> make_memory_backend(const runtime_config& cfg, std::vector<channel*> upper_levels)
{
  const auto model = cfg.value<std::string>("dram-model", "legacy");
  if (model != "legacy" && model != "ramulator2") {
    throw std::runtime_error{fmt::format("runtime config: dram-model = '{}': expected legacy or ramulator2", model)};
  }

  const std::string_view inactive_prefix = model == "legacy" ? "ramulator2." : "pmem.";
  for (const auto& [key, value] : cfg.applied()) {
    if (key.compare(0, inactive_prefix.size(), inactive_prefix) == 0) {
      throw std::runtime_error{fmt::format("runtime config: {} is not supported with dram-model = '{}'", key, model)};
    }
  }

  if (model == "legacy") {
    return make_legacy_memory_backend(cfg, std::move(upper_levels));
  }
  throw std::runtime_error{"runtime config: dram-model = 'ramulator2' is not available in this build"};
}
} // namespace champsim
