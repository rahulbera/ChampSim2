#include "ramulator2_memory_backend.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <utility>
#include <fmt/format.h>

#include "channel.h"
#include "defs.h"
#include "operable.h"

namespace
{
class ramulator2_memory_backend final : public champsim::memory_backend, public champsim::operable
{
  using request_type = champsim::channel::request_type;
  using response_type = champsim::channel::response_type;
  using time_point = champsim::chrono::clock::time_point;

  struct parent_request {
    request_type packet;
    std::deque<response_type>* returned;
    bool write;
    uint64_t block_address;
    std::size_t fragments, accepted = 0, completed = 0;
    time_point first_accept{};
  };
  struct queue_state {
    std::deque<request_type>* requests;
    std::deque<response_type>* returned;
    bool write;
    std::optional<uint64_t> head{};
  };
  struct completion {
    uint64_t parent;
    time_point when;
  };
  struct completion_mailbox {
    std::deque<completion> events;
    time_point now{};
    bool open = true;
  };

  std::vector<champsim::channel*> upper_levels;
  std::vector<queue_state> queues;
  std::map<uint64_t, parent_request> parents;
  uint64_t next_parent = 0;
  champsim::ramulator2_statistics counters;
  std::optional<champsim::ramulator2_statistics> roi;
  std::optional<time_point> last_completion;
  bool finalized = false;
  // Callbacks only hold a weak mailbox. Destroy the driver before the mailbox
  // and parent contexts, and never let a callback touch the adapter itself.
  std::shared_ptr<completion_mailbox> mailbox = std::make_shared<completion_mailbox>();
  std::unique_ptr<champsim::ramulator2_driver> driver;
  const champsim::data::bytes capacity;
  const std::size_t transaction_bytes;

  void require_running() const
  {
    if (finalized) {
      throw std::runtime_error{"ramulator2: memory backend is finalized"};
    }
  }

  uint64_t validate(const request_type& packet) const
  {
    if (packet.cpu >= champsim::defs::num_cpus) {
      throw std::runtime_error{fmt::format("ramulator2: invalid core id {} (expected less than {})", packet.cpu, champsim::defs::num_cpus)};
    }
    const auto address = packet.address.to<uint64_t>();
    const auto block = address - address % BLOCK_SIZE;
    const auto bytes = static_cast<uint64_t>(capacity.count());
    if (block >= bytes || BLOCK_SIZE > bytes - block) {
      throw std::runtime_error{fmt::format("ramulator2: request address {:#x} is out of range for {} bytes", address, bytes)};
    }
    return block;
  }

  champsim::ramulator2_statistics live_statistics() const
  {
    auto result = counters;
    for (const auto& [id, parent] : parents) {
      if (parent.accepted != 0) {
        ++result.outstanding_parents;
        result.outstanding_fragments += parent.accepted - parent.completed;
      }
    }
    result.native = driver->statistics();
    return result;
  }

  long complete_requests()
  {
    long progress = 0;
    while (!mailbox->events.empty()) {
      const auto event = mailbox->events.front();
      mailbox->events.pop_front();
      auto found = parents.find(event.parent);
      if (found == parents.end() || found->second.completed == found->second.accepted) {
        throw std::runtime_error{"ramulator2: unexpected native completion"};
      }
      auto& parent = found->second;
      ++parent.completed;
      ++counters.completed_fragments;
      ++progress;
      last_completion = event.when;
      if (parent.completed == parent.fragments) {
        if (parent.write) {
          ++counters.completed_writes;
        } else {
          ++counters.completed_reads;
          counters.total_read_latency_ps += static_cast<uint64_t>((event.when - parent.first_accept).count());
          ++counters.read_latency_samples;
          if (parent.packet.response_requested) {
            parent.returned->emplace_back(parent.packet);
          }
        }
        parents.erase(found);
      }
    }
    return progress;
  }

  long initiate(queue_state& queue)
  {
    long progress = 0;
    while (!queue.requests->empty()) {
      if (!queue.head) {
        const auto block = validate(queue.requests->front());
        if (warmup) {
          if (!queue.write && queue.requests->front().response_requested) {
            queue.returned->emplace_back(queue.requests->front());
          }
          queue.requests->pop_front();
          ++progress;
          continue;
        }
        const auto fragments = std::max<std::size_t>(1, BLOCK_SIZE / transaction_bytes);
        const auto id = next_parent++;
        parents.emplace(id, parent_request{queue.requests->front(), queue.returned, queue.write, block, fragments});
        queue.head = id;
      }
      // A later warmup must not bypass an already partially submitted request.
      // Keep that head for the next measured phase while native clocks advance.
      if (warmup) {
        break;
      }

      const auto id = *queue.head;
      auto& parent = parents.at(id);
      while (parent.accepted < parent.fragments) {
        const auto address = parent.block_address + parent.accepted * transaction_bytes;
        const auto bytes = std::min<std::size_t>(BLOCK_SIZE, transaction_bytes);
        const auto done = [weak = std::weak_ptr<completion_mailbox>{mailbox}, id] {
          if (auto pending = weak.lock(); pending && pending->open) {
            pending->events.push_back({id, pending->now});
          }
        };
        // The context and callback mailbox exist before send. Callbacks only
        // enqueue: acceptance counters commit after true, never provisionally
        // decrement on a rejection. A rejected send must not invoke done.
        if (!driver->send(parent.write, address, parent.packet.cpu, bytes, done)) {
          ++counters.rejected_submissions;
          break;
        }
        if (parent.accepted == 0) {
          parent.first_accept = current_time;
          if (parent.write) {
            ++counters.accepted_writes;
          } else {
            ++counters.accepted_reads;
          }
        }
        ++parent.accepted;
        ++counters.accepted_fragments;
        ++progress;
      }
      const bool all_accepted = parent.accepted == parent.fragments;
      if (all_accepted) {
        queue.requests->pop_front();
        queue.head.reset();
      }
      // In particular, pop the head before a synchronous last-write callback
      // releases its context. Never use the parent reference after this flush.
      progress += complete_requests();
      if (!all_accepted) {
        break;
      }
    }
    return progress;
  }

public:
  ramulator2_memory_backend(std::unique_ptr<champsim::ramulator2_driver> driver_, std::vector<champsim::channel*> upper_levels_)
      : champsim::operable(driver_->clock_period()), upper_levels(std::move(upper_levels_)), driver(std::move(driver_)), capacity(driver->size()),
        transaction_bytes(driver->transaction_bytes())
  {
    for (auto* channel : upper_levels) {
      queues.push_back({&channel->RQ, &channel->returned, false});
      queues.push_back({&channel->PQ, &channel->returned, false});
      queues.push_back({&channel->WQ, &channel->returned, true});
    }
  }
  champsim::operable& clocked_component() override { return *this; }
  champsim::data::bytes size() const override { return capacity; }
  champsim::memory_statistics statistics() const override
  {
    champsim::memory_statistics result;
    result.sim_ramulator2 = live_statistics();
    result.roi_ramulator2 = roi;
    return result;
  }
  std::string_view name() const override { return "ramulator2"; }
  std::optional<champsim::ramulator2_config_record> config_record() const override { return driver->config_record(); }
  void begin_phase() override
  {
    require_running();
    counters = {};
    driver->reset_stats();
    for (auto* channel : upper_levels) {
      channel->sim_stats = {};
      channel->roi_stats = {};
    }
  }
  void end_phase(unsigned /*cpu*/) override { roi = live_statistics(); }
  long operate() override
  {
    require_running();
    mailbox->now = current_time;
    long progress = complete_requests();
    for (auto& queue : queues) {
      progress += initiate(queue);
    }
    driver->tick();
    progress += complete_requests();
    return progress;
  }
  void finalize() override
  {
    if (!finalized) {
      finalized = true;
      mailbox->open = false;
      driver->finalize();
    }
  }
  void print_deadlock() override
  {
    uint64_t live_parents = 0, live_fragments = 0;
    for (const auto& [id, parent] : parents) {
      live_parents += parent.accepted != 0;
      live_fragments += parent.accepted - parent.completed;
    }
    fmt::print("Ramulator2 at {} ps: {} outstanding parents, {} outstanding fragments\n", current_time.time_since_epoch().count(), live_parents,
               live_fragments);
    for (std::size_t i = 0; i < upper_levels.size(); ++i) {
      const auto* channel = upper_levels[i];
      fmt::print("  Feeder {} RQ: {} PQ: {} WQ: {} pending heads: {}\n", i, channel->RQ.size(), channel->PQ.size(), channel->WQ.size(),
                 static_cast<unsigned>(queues[3 * i].head.has_value()) + static_cast<unsigned>(queues[3 * i + 1].head.has_value())
                     + static_cast<unsigned>(queues[3 * i + 2].head.has_value()));
    }
    if (last_completion) {
      fmt::print("  Last completion {} ps ago\n", (current_time - *last_completion).count());
    } else {
      fmt::print("  No native completion yet\n");
    }
  }
};
} // namespace

std::unique_ptr<champsim::memory_backend> champsim::make_ramulator2_memory_backend(std::unique_ptr<ramulator2_driver> driver,
                                                                                   std::vector<channel*> upper_levels)
{
  if (!driver) {
    throw std::runtime_error{"ramulator2: memory backend requires a driver"};
  }
  const auto transaction = driver->transaction_bytes();
  if (transaction == 0 || (transaction & (transaction - 1)) != 0) {
    throw std::runtime_error{"ramulator2: transaction bytes must be a positive power of two"};
  }
  if (driver->clock_period() <= chrono::picoseconds::zero() || driver->size().count() < BLOCK_SIZE) {
    throw std::runtime_error{"ramulator2: driver must supply a positive clock and capacity of at least one cache block"};
  }
  if (std::any_of(upper_levels.begin(), upper_levels.end(), [](const auto* channel) { return channel == nullptr; })) {
    throw std::runtime_error{"ramulator2: feeder channel must not be null"};
  }
  return std::make_unique<ramulator2_memory_backend>(std::move(driver), std::move(upper_levels));
}
