#include "ramulator2_driver.h"

#include <stdexcept>

#include "ramulator2_build.h"
#include "runtime_config.h"

#if CHAMPSIM_WITH_RAMULATOR2
#include <algorithm>
#include <charconv>
#include <cmath>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

#include "defs.h"
#include "ramulator/base/config.h"
#include "ramulator/base/factory.h"
#include "ramulator/dram/dram_spec.h"
#include "ramulator/frontend/i_frontend.h"
#include "ramulator/memory_system/i_memory_system.h"

namespace Ramulator
{
// Upstream External reports one core. Keep its request behavior while providing
// the real dimensions for native per-core counters and policies.
class ChampSimExternal : public IFrontEnd, public Implementation
{
  RAMULATOR_REGISTER_IMPLEMENTATION(IFrontEnd, ChampSimExternal, "ChampSimExternal")
public:
  void init() override { m_clock_ratio = m_config["clock_ratio"].as<unsigned int>(); }
  int get_num_cores() override { return static_cast<int>(champsim::defs::num_cpus); }
  void tick() override {}
  bool is_finished() override { return false; }
  bool receive_external_requests(int type, Addr_t addr, int cpu, std::function<void(Request&)> done, int bytes) override
  {
    return receive_external_requests(type, addr, cpu, -1, std::move(done), bytes);
  }
  bool receive_external_requests(int type, Addr_t addr, int cpu, int ingress, std::function<void(Request&)> done, int bytes) override
  {
    Request request(addr, type, cpu, std::move(done));
    request.ingress_id = ingress;
    request.size_bytes = bytes;
    return m_memory_system->send(request);
  }
};
} // namespace Ramulator

namespace
{
using Ramulator::ConfigNode;
void require(bool condition, const std::string& message)
{
  if (!condition)
    throw std::runtime_error("ramulator2: " + message);
}
std::string read_file(const std::filesystem::path& path)
{
  std::ifstream file(path, std::ios::binary);
  require(file.is_open(), "cannot open " + path.string());
  std::ostringstream content;
  content << file.rdbuf();
  require(!file.bad(), "cannot read " + path.string());
  return content.str();
}
std::string fingerprint(const std::string& content)
{
  uint64_t value = 1469598103934665603ULL; // ChampSim config_id's historical basis.
  for (unsigned char byte : content)
    value = (value ^ byte) * 1099511628211ULL;
  std::ostringstream text;
  text << std::hex << std::setfill('0') << std::setw(16) << value;
  return text.str();
}
void verify_library()
{
  Dl_info info{};
  require(dladdr(reinterpret_cast<void*>(&Ramulator::Config::parse_config_string), &info) != 0 && info.dli_fname,
          "cannot identify loaded library; rebuild with the ramulator2 helper");
  require(fingerprint(read_file(info.dli_fname)) == champsim::native_build::fingerprint,
          "loaded library differs from build provenance; rebuild WITH_RAMULATOR2=1");
}
int integer(const ConfigNode& node, const std::string& field)
{
  require(node.is_scalar(), "expected integer " + field);
  const auto& text = node.scalar();
  int value{};
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size(), "invalid integer " + field);
  return value;
}
bool power_of_two(uint64_t value) { return value != 0 && (value & (value - 1)) == 0; }
uint64_t checked_product(uint64_t left, uint64_t right)
{
  require(right != 0 && left <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) / right, "capacity overflow");
  return left * right;
}
int bit_count(uint64_t power)
{
  int result = 0;
  while (power > 1) {
    power >>= 1;
    ++result;
  }
  return result;
}
const champsim::native_build::model& validate_tables(const ConfigNode& controller)
{
  const auto dram = controller["dram"];
  require(dram.is_map() && dram["impl"].is_scalar(), "controller requires a DRAM implementation");
  const auto name = dram["impl"].scalar();
  const auto found =
      std::find_if(champsim::native_build::models.begin(), champsim::native_build::models.end(), [&](const auto& model) { return name == model.name; });
  require(found != champsim::native_build::models.end(), "unsupported DRAM model " + name);
  const auto& model = *found;
  const auto counts = dram["org"]["count"];
  const auto timings = dram["timing"];
  const auto commands = dram["command_cycles"];
  require(counts.is_sequence() && counts.seq().size() == static_cast<std::size_t>(model.levels), "invalid organization dimensions");
  require(timings.is_sequence() && timings.seq().size() == static_cast<std::size_t>(model.timings), "invalid timing dimensions");
  require(commands.is_sequence() && commands.seq().size() == static_cast<std::size_t>(model.commands), "invalid command dimensions");
  for (const auto& count : counts.seq())
    require(power_of_two(static_cast<uint64_t>(integer(count, "organization count"))) && integer(count, "organization count") > 0,
            "organization counts must be positive powers of two");
  require(integer(counts.seq().front(), "channel count") == 1, "each controller must contain one channel");
  require(integer(dram["org"]["dq"], "dq") > 0, "dq must be positive");
  const int width = integer(dram["channel_width"], "channel_width");
  require(width > 0 && width % 8 == 0 && width <= std::numeric_limits<int>::max() / model.prefetch, "invalid channel width");
  if (dram["data_payload_bytes"])
    require(integer(dram["data_payload_bytes"], "data_payload_bytes") > 0, "invalid transaction payload");
  for (const auto& timing : timings.seq())
    require(integer(timing, "timing") >= 0, "negative timing");
  require(integer(timings.seq().at(static_cast<std::size_t>(model.clock)), "tCK_ps") > 0, "nonpositive clock period");
  for (const auto& command : commands.seq())
    require(integer(command, "command cycles") > 0, "nonpositive command cycles");
  require(integer(dram["read_latency"], "read_latency") >= 0, "negative read latency");
  const auto constraints = dram["timing_constraints"];
  require(constraints.is_sequence(), "timing_constraints must be a sequence");
  for (const auto& constraint : constraints.seq()) {
    require(constraint.is_sequence() && constraint.seq().size() >= 4 && constraint.seq().size() <= 7, "invalid timing constraint shape");
    const auto& fields = constraint.seq();
    const int level = integer(fields[0], "constraint level");
    require(level >= 0 && level < model.levels, "timing constraint level out of range");
    for (std::size_t index : {1U, 2U}) {
      require(fields[index].is_sequence(), "constraint commands must be a sequence");
      for (const auto& command : fields[index].seq()) {
        const int id = integer(command, "constraint command");
        require(id >= 0 && id < model.commands, "timing constraint command out of range");
      }
    }
    require(integer(fields[3], "constraint latency") >= 0, "negative constraint latency");
    if (fields.size() > 4)
      require(integer(fields[4], "constraint window") > 0, "invalid constraint window");
    if (fields.size() > 5)
      require(fields[5].is_scalar() && (fields[5].scalar() == "true" || fields[5].scalar() == "false"), "invalid constraint sibling flag");
    if (fields.size() > 6)
      require(integer(fields[6], "constraint history") >= -1, "invalid constraint history");
  }
  for (const auto* field : {"read_buffer_size", "write_buffer_size", "priority_buffer_size"})
    if (controller[field])
      require(integer(controller[field], field) > 0, std::string{field} + " must be positive");
  return model;
}
champsim::native_scalar scalar(const ConfigNode& node, bool text_only)
{
  const auto& text = node.scalar();
  if (text_only)
    return text;
  if (text == "true")
    return true;
  if (text == "false")
    return false;
  const bool negative = !text.empty() && text.front() == '-';
  const auto start = text.begin() + (negative ? 1 : 0);
  const bool decimal_integer = start != text.end() && std::all_of(start, text.end(), [](char c) { return c >= '0' && c <= '9'; });
  if (decimal_integer) {
    if (negative) {
      int64_t value{};
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
      if (parsed.ec == std::errc{})
        return value;
    } else {
      uint64_t value{};
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
      if (parsed.ec == std::errc{}) {
        if (value <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
          return static_cast<int64_t>(value);
        return value;
      }
    }
    return text; // Never round an integer overflow through double.
  }
  double value{};
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size())
    return value;
  return text;
}
void flatten(const ConfigNode& node, std::vector<std::string> path, champsim::native_memory_statistics& stats)
{
  if (node.is_scalar()) {
    stats.values.push_back({path, scalar(node, !path.empty() && (path.back() == "impl" || path.back() == "id"))});
  } else if (node.is_map()) {
    for (const auto& [key, child] : node.map()) {
      auto nested = path;
      nested.push_back(key);
      if (key == "controller") {
        if (child.is_sequence()) {
          for (std::size_t i = 0; i < child.seq().size(); ++i) {
            auto channel = nested;
            channel.push_back("channel" + std::to_string(i));
            flatten(child.seq()[i], std::move(channel), stats);
          }
        } else {
          nested.push_back("channel0");
          flatten(child, std::move(nested), stats);
        }
      } else
        flatten(child, std::move(nested), stats);
    }
  } else if (node.is_sequence()) {
    for (std::size_t i = 0; i < node.seq().size(); ++i) {
      auto nested = path;
      nested.push_back(std::to_string(i));
      flatten(node.seq()[i], std::move(nested), stats);
    }
  }
}
class native_driver final : public champsim::ramulator2_driver
{
  champsim::ramulator2_config_record record_;
  champsim::chrono::picoseconds period_{};
  uint64_t capacity_{};
  std::size_t transaction_{};
  std::unique_ptr<Ramulator::IFrontEnd> frontend_;
  std::unique_ptr<Ramulator::IMemorySystem> memory_;
  bool finalized_ = false;

public:
  explicit native_driver(const champsim::runtime_config& cfg)
  {
    verify_library();
    require(champsim::defs::num_cpus > 0 && champsim::defs::num_cpus <= static_cast<std::size_t>(std::numeric_limits<int>::max()), "invalid core count");
    const auto path = cfg.value<std::string>("ramulator2.config", "");
    require(!path.empty(), "ramulator2.config must name an exported YAML file");
    std::error_code ec;
    const auto normalized = std::filesystem::canonical(path, ec);
    require(!ec, "cannot open config " + path);
    record_ = {normalized.string(),          {}, champsim::native_build::revision, read_file(normalized), champsim::native_build::fingerprint,
               champsim::native_build::build};
    record_.hash = fingerprint(record_.yaml);
    for (const auto& [key, actual] : {std::pair{"ramulator2.config_hash", record_.hash}, std::pair{"ramulator2.revision", record_.revision}}) {
      const auto expected = cfg.value<std::string>(key, "");
      require(expected.empty() || expected == actual, std::string{key} + " mismatch: expected " + expected + ", actual " + actual);
      cfg.override_effective(key, actual);
    }
    cfg.override_effective("ramulator2.config", record_.path);
    auto config = Ramulator::Config::parse_config_string(record_.yaml);
    require(config["frontend"]["impl"].as<std::string>("") == "External", "frontend must be External");
    require(integer(config["frontend"]["clock_ratio"], "frontend clock_ratio") > 0, "invalid frontend clock ratio");
    const auto system = config["memory_system"];
    require(system["impl"].as<std::string>("") == "GenericDRAM", "memory_system must be GenericDRAM");
    require(integer(system["clock_ratio"], "memory clock_ratio") > 0, "invalid memory clock ratio");
    require(system["channel_mapper"]["impl"].as<std::string>("") == "CacheLineInterleave", "channel mapper must be CacheLineInterleave");
    const auto controllers = system["controllers"];
    require(controllers.is_sequence() && power_of_two(controllers.seq().size()), "controllers must be a nonempty power-of-two sequence");
    uint64_t channel_capacity = 0;
    for (const auto& controller : controllers.seq()) {
      const auto& model = validate_tables(controller);
      auto spec = Ramulator::DRAMSpec::create(model.name, controller);
      const int tx = spec->get_tx_bytes();
      require(tx > 0 && power_of_two(static_cast<uint64_t>(tx)), "transaction bytes must be a positive power of two");
      const int columns = spec->get_level_size("Column");
      require(columns >= model.prefetch && columns % model.prefetch == 0, "column count must divide exactly by prefetch size");
      uint64_t bytes = static_cast<uint64_t>(columns / model.prefetch);
      for (int i = 0; i < model.levels; ++i)
        if (i != model.column)
          bytes = checked_product(bytes, static_cast<uint64_t>(spec->organization.level_sizes.at(static_cast<std::size_t>(i))));
      bytes = checked_product(bytes, static_cast<uint64_t>(tx));
      const auto period = champsim::chrono::picoseconds{spec->get_timing_value("tCK_ps")};
      if (channel_capacity != 0)
        require(channel_capacity == bytes && period_ == period && transaction_ == static_cast<std::size_t>(tx),
                "heterogeneous controller capacity, period or transaction size");
      channel_capacity = bytes;
      period_ = period;
      transaction_ = static_cast<std::size_t>(tx);
    }
    capacity_ = checked_product(channel_capacity, controllers.seq().size());
    require(capacity_ >= 1048576 + champsim::defs::page_size && capacity_ % champsim::defs::page_size == 0,
            "capacity must include reserved 1 MiB and whole physical pages");
    const auto interleave_node = system["channel_mapper"]["interleave_bits"];
    const int interleave = interleave_node ? integer(interleave_node, "interleave_bits") : 0;
    require(interleave >= 0 && interleave <= bit_count(channel_capacity) - bit_count(transaction_), "interleave shift exceeds per-channel capacity");
    auto frontend_config = config["frontend"];
    frontend_config.set("impl", ConfigNode("ChampSimExternal"));
    config.set("frontend", frontend_config);
    frontend_.reset(Ramulator::Factory::create_frontend(config));
    memory_.reset(Ramulator::Factory::create_memory_system(config));
    frontend_->connect_memory_system(memory_.get());
    memory_->connect_frontend(frontend_.get());
    const double observed = static_cast<double>(memory_->get_tCK()) * 1000.0;
    const double expected = static_cast<double>(period_.count());
    require(std::isfinite(observed) && std::abs(observed - expected) <= std::max(0.01, expected * 4 * std::numeric_limits<float>::epsilon()),
            "native clock disagrees with integer tCK_ps");
    require(memory_->get_tx_bytes() == static_cast<int>(transaction_), "native transaction size disagrees with geometry");
  }
  champsim::chrono::picoseconds clock_period() const override { return period_; }
  champsim::data::bytes size() const override { return champsim::data::bytes{static_cast<int64_t>(capacity_)}; }
  std::size_t transaction_bytes() const override { return transaction_; }
  bool send(bool write, uint64_t address, uint32_t cpu, std::size_t bytes, std::function<void()> done) override
  {
    require(!finalized_, "request submitted after finalize");
    require(cpu < champsim::defs::num_cpus, "invalid core id");
    require(bytes > 0 && bytes <= transaction_, "invalid request size");
    require(address < capacity_ && bytes <= capacity_ - address, "request address out of range");
    require(bytes <= transaction_ - address % transaction_, "request crosses transaction boundary");
    return frontend_->receive_external_requests(
        write ? Ramulator::Request::Type::Write : Ramulator::Request::Type::Read, static_cast<Ramulator::Addr_t>(address), static_cast<int>(cpu),
        [done = std::move(done)](Ramulator::Request&) {
          if (done)
            done();
        },
        static_cast<int>(bytes));
  }
  void tick() override
  {
    require(!finalized_, "tick after finalize");
    memory_->tick();
  }
  void reset_stats() override
  {
    require(!finalized_, "reset after finalize");
    frontend_->reset_stats_recursive();
    memory_->reset_stats_recursive();
  }
  champsim::native_memory_statistics statistics() override
  {
    frontend_->update_stats_recursive();
    memory_->update_stats_recursive();
    champsim::native_memory_statistics result;
    flatten(frontend_->collect_stats(), {"frontend"}, result);
    flatten(memory_->collect_stats(), {"memory_system"}, result);
    std::ostringstream yaml;
    frontend_->print_stats(yaml);
    memory_->print_stats(yaml);
    result.yaml = yaml.str();
    return result;
  }
  champsim::ramulator2_config_record config_record() const override { return record_; }
  void finalize() override
  {
    if (!finalized_) {
      finalized_ = true;
      frontend_->finalize();
      memory_->finalize();
    }
  }
};
} // namespace
#endif

bool champsim::ramulator2_available() { return CHAMPSIM_WITH_RAMULATOR2 != 0; }
std::unique_ptr<champsim::ramulator2_driver> champsim::make_ramulator2_driver(const runtime_config& cfg)
{
#if CHAMPSIM_WITH_RAMULATOR2
  try {
    return std::make_unique<native_driver>(cfg);
  } catch (const std::exception& error) {
    throw std::runtime_error(std::string{"ramulator2 configuration: "} + error.what());
  }
#else
  (void)cfg;
  throw std::runtime_error("ramulator2 not available: build WITH_RAMULATOR2=1 RAMULATOR2_ROOT=/path/to/ramulator2");
#endif
}
