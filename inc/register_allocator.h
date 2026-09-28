#include <cstdint>
#include <list>
#include <optional>
#include <queue>
#include <vector>

#ifndef REG_ALLOC_H
#define REG_ALLOC_H

#include "instruction.h"

namespace champsim {
struct core_context;
}

struct physical_register {
  uint16_t arch_reg_index;
  uint64_t producing_instruction_id;
  bool valid; // has the producing instruction committed yet?
  bool busy;  // is this register in use anywhere in the pipeline?
};

// The core-shared half of register renaming: the physical register file and
// its free list. The per-context alias tables (frontend_RAT/backend_RAT) and
// the rename methods that touch them live on champsim::core_context, which is
// granted private access here because a rename reads and writes this pool.
class RegisterAllocator
{
private:
  std::queue<PHYSICAL_REGISTER_ID> free_registers;
  std::vector<physical_register> physical_register_file;

public:
  RegisterAllocator(size_t num_physical_registers);
  void complete_dest_register(PHYSICAL_REGISTER_ID physreg);
  void free_register(PHYSICAL_REGISTER_ID physreg);
  bool isValid(PHYSICAL_REGISTER_ID physreg) const;
  unsigned long count_free_registers() const;
  int count_reg_dependencies(const ooo_model_instr& instr) const;
  void print_deadlock();

  friend struct champsim::core_context;
};
#endif
