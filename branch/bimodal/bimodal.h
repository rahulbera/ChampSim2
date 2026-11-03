#ifndef BRANCH_BIMODAL_H
#define BRANCH_BIMODAL_H

#include <array>

#include "address.h"
#include "modules.h"
#include "msl/fwcounter.h"

class bimodal : champsim::modules::branch_predictor
{
  [[nodiscard]] static constexpr auto hash(champsim::address ip, champsim::address_space_id asid)
  {
    // PRIME is not a power of two, so XOR-ing the asid in first would change
    // which addresses share a counter; adding it after the modulus only rotates
    // the table, and asid {0,0} leaves the index unchanged.
    return (ip.to<unsigned long>() % PRIME + champsim::asid_word(asid)) % PRIME;
  }

  static constexpr std::size_t TABLE_SIZE = 16384;
  static constexpr std::size_t PRIME = 16381;
  static constexpr std::size_t BITS = 2;

  std::array<champsim::msl::fwcounter<BITS>, TABLE_SIZE> bimodal_table;

public:
  using branch_predictor::branch_predictor;

  // void initialize_branch_predictor();
  bool predict_branch(champsim::address ip, champsim::address predicted_target, bool always_taken, uint8_t branch_type, champsim::address_space_id asid);
  void last_branch_result(champsim::address ip, champsim::address branch_target, bool taken, uint8_t branch_type, champsim::address_space_id asid);
};

#endif
