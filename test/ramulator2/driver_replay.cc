#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

#include "ramulator2_driver.h"
#include "runtime_config.h"

struct Transaction {
  int id, parent, fragment;
  unsigned release_tick;
  int type;
  std::int64_t address;
  int source, size;
  bool accepted = false, completed = false;
};

int main(int argc, char** argv)
{
  try {
    if (argc != 4)
      throw std::runtime_error("usage: driver_replay config.yaml stream.csv stats.yaml");
    champsim::runtime_config cfg;
    cfg.set(std::string("ramulator2.config=") + argv[1]);
    auto memory = champsim::make_ramulator2_driver(cfg);
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
      if (tx.id != static_cast<int>(transactions.size()) || tx.source != 0 || tx.size <= 0 || static_cast<std::size_t>(tx.size) > memory->transaction_bytes())
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
        auto callback = [&, id = tx.id]() {
          auto& done = transactions.at(static_cast<std::size_t>(id));
          if (done.completed)
            throw std::runtime_error("duplicate callback");
          done.completed = true;
          ++completions;
          const bool sync = sending_id == id;
          synchronous += sync;
          std::cout << "CALLBACK," << tick << ',' << id << ',' << done.type << ',' << done.address << ',' << done.source << ',' << done.size << ",,"
                    << done.parent << ',' << done.fragment << ',' << sync << '\n';
        };
        const bool accepted = memory->send(tx.type == 1, tx.address, tx.source, tx.size, callback);
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
    const auto before = memory->statistics();
    memory->finalize();
    const auto after = memory->statistics();
    if (before.yaml != after.yaml)
      throw std::runtime_error("finalization changed native counters");
    std::ofstream stats(argv[3]);
    if (!stats)
      throw std::runtime_error("cannot open stats output");
    stats << before.yaml;
    nlohmann::json values = nlohmann::json::array();
    for (const auto& stat : before.values) {
      std::ostringstream text;
      std::visit([&](const auto& value) { text << std::setprecision(17) << value; }, stat.value);
      values.push_back({{"path", stat.path}, {"kind", stat.value.index()}, {"text", text.str()}});
    }
    std::ofstream typed(std::string(argv[3]) + ".typed.json");
    typed << values.dump(2) << '\n';
    stats.flush();
    typed.flush();
    if (!stats || !typed)
      throw std::runtime_error("cannot write driver statistics");
    std::cout << "SUMMARY ticks=" << tick << " transactions=" << transactions.size() << " attempts=" << attempts << " rejects=" << rejects
              << " completions=" << completions << " synchronous=" << synchronous << " tx_bytes=" << memory->transaction_bytes() << '\n';
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
