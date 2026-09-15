#include <stdexcept>
#include <string>
#include <utility>

#include "dram_controller.h"
#include "memory_backend.h"
#include "runtime_config.h"
#include "util/bits.h"

namespace
{
// Keep the legacy MHz conversion, including integer-picosecond truncation.
champsim::chrono::picoseconds period(const champsim::runtime_config& cfg, const std::string& key, double mhz)
{
  return champsim::chrono::picoseconds{static_cast<champsim::chrono::picoseconds::rep>(1000000.0 / cfg.positive_value<double>(key, mhz))};
}

std::size_t power_of_two_dimension(const champsim::runtime_config& cfg, const std::string& key, std::size_t fallback)
{
  const auto value = cfg.positive_value<std::size_t>(key, fallback);
  if (!champsim::is_power_of_2(value)) {
    throw std::runtime_error{"runtime config: " + key + " must be a power of two"};
  }
  return value;
}

champsim::data::bytes channel_width(const champsim::runtime_config& cfg)
{
  constexpr std::string_view key = "pmem.channel_width";
  const auto value = cfg.positive_value<champsim::data::bytes::rep>(key, 8);
  if (value > BLOCK_SIZE || BLOCK_SIZE % value != 0) {
    throw std::runtime_error{"runtime config: pmem.channel_width must not exceed BLOCK_SIZE and must divide it exactly"};
  }
  return champsim::data::bytes{value};
}

class legacy_memory_backend final : public champsim::memory_backend
{
  MEMORY_CONTROLLER controller;

public:
  legacy_memory_backend(const champsim::runtime_config& cfg, std::vector<champsim::channel*> upper_levels)
      : controller(
            period(cfg, "pmem.data_rate", 3200), period(cfg, "pmem.frequency", 1600.0), cfg.value<std::size_t>("pmem.trp", 24),
            cfg.value<std::size_t>("pmem.trcd", 24), cfg.value<std::size_t>("pmem.tcas", 24), cfg.value<std::size_t>("pmem.tras", 52),
            champsim::chrono::microseconds{static_cast<champsim::chrono::microseconds::rep>(1000.0 * cfg.positive_value<double>("pmem.refresh_period", 32))},
            std::move(upper_levels), cfg.value<std::size_t>("pmem.rq_size", 64), cfg.value<std::size_t>("pmem.wq_size", 64),
            power_of_two_dimension(cfg, "pmem.channels", 1), channel_width(cfg), power_of_two_dimension(cfg, "pmem.bank_rows", 65536),
            power_of_two_dimension(cfg, "pmem.bank_columns", 1024), power_of_two_dimension(cfg, "pmem.ranks", 1),
            power_of_two_dimension(cfg, "pmem.bankgroups", 8), power_of_two_dimension(cfg, "pmem.banks", 4),
            cfg.positive_value<std::size_t>("pmem.refreshes_per_period", 8192))
  {
  }

  champsim::operable& clocked_component() override { return controller; }
  champsim::data::bytes size() const override { return controller.size(); }
  std::string_view name() const override { return "legacy"; }
  champsim::memory_statistics statistics() const override
  {
    champsim::memory_statistics result;
    for (const auto& channel : controller.channels) {
      result.sim_dram.push_back(channel.sim_stats);
      result.roi_dram.push_back(channel.roi_stats);
    }
    return result;
  }
};
} // namespace

namespace champsim
{
std::unique_ptr<memory_backend> make_legacy_memory_backend(const runtime_config& cfg, std::vector<channel*> upper_levels)
{
  return std::make_unique<legacy_memory_backend>(cfg, std::move(upper_levels));
}
} // namespace champsim
