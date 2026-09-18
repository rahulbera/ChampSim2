#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

#include "ramulator/base/config.h"
#include "ramulator/base/factory.h"
#include "ramulator/frontend/i_frontend.h"
#include "ramulator/memory_system/i_memory_system.h"

struct Transaction {
  int id, parent, fragment;
  unsigned release_tick;
  int type;
  std::int64_t address;
  int source, size;
  bool accepted = false, completed = false;
};

void write_raw(const Ramulator::ConfigNode& node, std::vector<std::string> path, nlohmann::json& out)
{
  if (node.is_scalar()) {
    out.push_back({{"path", path}, {"text", node.scalar()}});
  } else if (node.is_map()) {
    for (const auto& [name, child] : node.map()) {
      auto nested = path;
      nested.push_back(name);
      if (name == "controller") {
        if (child.is_sequence()) {
          for (std::size_t i = 0; i < child.seq().size(); ++i) {
            auto channel = nested;
            channel.push_back("channel" + std::to_string(i));
            write_raw(child.seq()[i], channel, out);
          }
        } else {
          nested.push_back("channel0");
          write_raw(child, nested, out);
        }
      } else {
        write_raw(child, nested, out);
      }
    }
  } else if (node.is_sequence()) {
    for (std::size_t i = 0; i < node.seq().size(); ++i) {
      auto nested = path;
      nested.push_back(std::to_string(i));
      write_raw(node.seq()[i], nested, out);
    }
  }
}

int main(int argc, char** argv)
{
  try {
    if (argc != 4)
      throw std::runtime_error("usage: direct_replay config.yaml stream.csv stats.yaml");
    const auto cfg = Ramulator::Config::parse_config_file(argv[1]);
    std::unique_ptr<Ramulator::IFrontEnd> frontend(Ramulator::Factory::create_frontend(cfg));
    std::unique_ptr<Ramulator::IMemorySystem> memory(Ramulator::Factory::create_memory_system(cfg));
    frontend->connect_memory_system(memory.get());
    memory->connect_frontend(frontend.get());
    std::vector<Transaction> transactions;
    std::ifstream stream(argv[2]);
    if (!stream)
      throw std::runtime_error("cannot open stream");
    std::string line;
    std::getline(stream, line); // header
    while (std::getline(stream, line)) {
      for (char& c : line)
        if (c == ',')
          c = ' ';
      std::istringstream row(line);
      Transaction tx{};
      if (!(row >> tx.id >> tx.parent >> tx.fragment >> tx.release_tick >> tx.type >> tx.address >> tx.source >> tx.size))
        throw std::runtime_error("invalid stream row");
      if (tx.id != static_cast<int>(transactions.size()) || tx.source != 0 || tx.size <= 0 || tx.size > memory->get_tx_bytes())
        throw std::runtime_error("invalid stream transaction");
      transactions.push_back(tx);
    }
    if (transactions.empty())
      throw std::runtime_error("empty stream");
    unsigned tick = 0, attempts = 0, rejects = 0, completions = 0, synchronous = 0;
    int sending_id = -1;
    std::cout << "EVENT,tick,id,type,address,source,size,accepted,parent,fragment,synchronous\n";
    while (tick < 10000 && completions < transactions.size()) {
      // At clock boundary N: all due, unaccepted IDs are attempted in CSV order;
      // then one native tick advances to boundary N+1. Accepted IDs are never retried.
      for (auto& tx : transactions) {
        if (tx.accepted || tx.release_tick > tick)
          continue;
        sending_id = tx.id;
        auto callback = [&, id = tx.id](Ramulator::Request& req) {
          auto& done = transactions.at(static_cast<std::size_t>(id));
          if (done.completed)
            throw std::runtime_error("duplicate callback");
          if (req.addr != done.address || req.source_id != done.source || req.size_bytes != done.size || req.type_id != done.type)
            throw std::runtime_error("callback metadata mismatch");
          done.completed = true;
          ++completions;
          const bool sync = sending_id == id;
          synchronous += sync;
          std::cout << "CALLBACK," << tick << ',' << id << ',' << done.type << ',' << done.address << ',' << done.source << ',' << done.size << ",,"
                    << done.parent << ',' << done.fragment << ',' << sync << '\n';
        };
        const bool accepted = frontend->receive_external_requests(tx.type, tx.address, tx.source, callback, tx.size);
        sending_id = -1;
        if (!accepted && tx.completed)
          throw std::runtime_error("rejected send completed synchronously");
        tx.accepted = accepted;
        ++attempts;
        rejects += !accepted;
        std::cout << "SEND," << tick << ',' << tx.id << ',' << tx.type << ',' << tx.address << ',' << tx.source << ',' << tx.size << ',' << accepted << ','
                  << tx.parent << ',' << tx.fragment << ",\n";
      }
      if (completions == transactions.size())
        break;
      ++tick; // Native callbacks caused by this tick observe the advanced clock boundary.
      memory->tick();
    }
    if (completions != transactions.size() || rejects == 0 || synchronous == 0)
      throw std::runtime_error("bounded completion/reject/synchronous callback requirement failed");
    frontend->update_stats_recursive();
    memory->update_stats_recursive();
    frontend->finalize();
    memory->finalize();
    std::ofstream stats(argv[3]);
    if (!stats)
      throw std::runtime_error("cannot open stats output");
    memory->print_stats(stats);
    nlohmann::json values = nlohmann::json::array();
    write_raw(memory->collect_stats(), {"memory_system"}, values);
    std::ofstream raw(std::string(argv[3]) + ".raw.json");
    raw << values.dump(2) << '\n';
    stats.flush();
    raw.flush();
    if (!stats || !raw)
      throw std::runtime_error("cannot write native statistics");
    std::cout << "SUMMARY ticks=" << tick << " transactions=" << transactions.size() << " attempts=" << attempts << " rejects=" << rejects
              << " completions=" << completions << " synchronous=" << synchronous << " tx_bytes=" << memory->get_tx_bytes() << '\n';
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
