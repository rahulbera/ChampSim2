#ifndef CHAMPSIM_RAMULATOR2_DRIVER_H
#define CHAMPSIM_RAMULATOR2_DRIVER_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "champsim.h"
#include "chrono.h"
#include "util/units.h"
namespace champsim
{
class runtime_config;
using native_scalar = std::variant<int64_t, uint64_t, double, bool, std::string>;
// Paths are owned components: a dot inside a component is never a separator.
// Native ConfigNode erases scalar types; normalization preserves integer values.
struct native_statistic {
  std::vector<std::string> path;
  native_scalar value;
};
struct native_memory_statistics {
  std::vector<native_statistic> values;
  std::string yaml;
};
struct ramulator2_config_record {
  std::string path, hash, revision, yaml;
  // Project FNV content fingerprint and owned JSON compiler/dependency provenance.
  std::string library_hash, build;
};
class ramulator2_driver
{
public:
  virtual ~ramulator2_driver() = default;
  virtual chrono::picoseconds clock_period() const = 0;
  virtual data::bytes size() const = 0;
  virtual std::size_t transaction_bytes() const = 0;
  virtual bool send(bool write, uint64_t address, uint32_t cpu, std::size_t bytes, std::function<void()> done) = 0;
  virtual void tick() = 0;
  virtual void reset_stats() = 0;
  virtual native_memory_statistics statistics() = 0;
  virtual ramulator2_config_record config_record() const = 0;
  virtual void finalize() = 0;
};
std::unique_ptr<ramulator2_driver> make_ramulator2_driver(const runtime_config&);
bool ramulator2_available();
} // namespace champsim
#endif
