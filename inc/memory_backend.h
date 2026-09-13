#ifndef MEMORY_BACKEND_H
#define MEMORY_BACKEND_H

#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "champsim.h"
#include "memory_stats.h"
#include "ramulator2_driver.h"
#include "util/units.h"

namespace champsim
{
class channel;
class operable;
class runtime_config;

// Owns memory without adding a second clock: the selected component itself is
// scheduled by the environment and receives the ordinary operable phase hooks.
class memory_backend
{
public:
  virtual ~memory_backend() = default;
  virtual operable& clocked_component() = 0;
  virtual data::bytes size() const = 0;
  virtual memory_statistics statistics() const = 0;
  virtual std::string_view name() const = 0;
  virtual std::optional<ramulator2_config_record> config_record() const { return std::nullopt; }
  virtual void finalize() {}
};

std::unique_ptr<memory_backend> make_memory_backend(const runtime_config& cfg, std::vector<channel*> upper_levels);
} // namespace champsim

#endif
