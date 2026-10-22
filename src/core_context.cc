#include "core_context.h"

#include <algorithm>
#include <cstddef>
#include <fmt/core.h>

#include "champsim_assert.h"

// The per-context rename methods, moved line-for-line from RegisterAllocator.
// The shared physical-register pool and its free list live behind
// reg_allocator, which the core guarantees outlives this context.

PHYSICAL_REGISTER_ID champsim::core_context::rename_dest_register(int16_t reg, champsim::program_ordered<ooo_model_instr>::id_type producer_id)
{
  CHAMPSIM_ASSERT(!reg_allocator->free_registers.empty());

  PHYSICAL_REGISTER_ID phys_reg = reg_allocator->free_registers.front();
  reg_allocator->free_registers.pop();
  frontend_RAT[reg] = phys_reg;
  reg_allocator->physical_register_file.at(phys_reg) = {(uint16_t)reg, producer_id, false, true, this->ctx_index}; // arch_reg_index, valid, busy, context

  return phys_reg;
}

PHYSICAL_REGISTER_ID champsim::core_context::rename_src_register(int16_t reg)
{
  PHYSICAL_REGISTER_ID phys = frontend_RAT[reg];

  if (phys < 0) {
    // allocate the register if it hasn't yet been mapped
    // (common due to the traces being slices in the middle of a program)
    CHAMPSIM_ASSERT(!reg_allocator->free_registers.empty());
    phys = reg_allocator->free_registers.front();
    reg_allocator->free_registers.pop();
    frontend_RAT[reg] = phys;
    backend_RAT[reg] = phys;                                              // we assume this register's last write has been committed
    reg_allocator->physical_register_file.at(phys) = {(uint16_t)reg, 0, true, true, this->ctx_index}; // arch_reg_index, producing_inst_id, valid, busy, context
  } else {
    // An already-mapped source must have been allocated by this thread's own
    // rename. Keyed on context, not asid: a shared address space still has a
    // RAT per thread, so a sibling's write to the same asid is a different
    // physical register.
    CHAMPSIM_ASSERT(reg_allocator->physical_register_file.at(phys).context.value == this->ctx_index.value);
  }

  return phys;
}

void champsim::core_context::retire_dest_register(PHYSICAL_REGISTER_ID physreg)
{
  // The committing instruction's destination must have been renamed by this
  // thread. Thread identity, not asid, is the key (see rename_src_register).
  CHAMPSIM_ASSERT(reg_allocator->physical_register_file.at(physreg).context.value == this->ctx_index.value);

  // grab the arch reg index, find old phys reg in backend RAT
  uint16_t arch_reg = reg_allocator->physical_register_file.at(physreg).arch_reg_index;
  PHYSICAL_REGISTER_ID old_phys_reg = backend_RAT[arch_reg];

  // update the backend RAT with the new phys reg
  backend_RAT[arch_reg] = physreg;

  // free the old phys reg
  if (old_phys_reg != -1) {
    reg_allocator->free_register(old_phys_reg);
  }
}

void champsim::core_context::reset_frontend_RAT()
{
  std::copy(std::begin(backend_RAT), std::end(backend_RAT), std::begin(frontend_RAT));
  // once wrong path is implemented:
  // find registers allocated by wrong-path instructions and free them
}

bool champsim::core_context::isAllocated(PHYSICAL_REGISTER_ID archreg) const
{
  // An architectural ID. A renamed instruction's operands hold physical IDs,
  // which can exceed the RAT, so asking this about them is a caller bug.
  CHAMPSIM_ASSERT(archreg >= 0 && static_cast<std::size_t>(archreg) < std::size(frontend_RAT));
  return frontend_RAT[static_cast<std::size_t>(archreg)] != -1;
}

void champsim::core_context::reclaim_registers()
{
  // After a full drain frontend_RAT == backend_RAT: every destination write has
  // retired, so each register this thread holds is exactly one committed
  // backend_RAT entry. Free them all and clear both tables. The producer-
  // identity check is the same one retire_dest_register relies on -- a register
  // is private to its thread, so this can never free a sibling's register.
  for (auto phys : backend_RAT) {
    if (phys != -1) {
      CHAMPSIM_ASSERT(reg_allocator->physical_register_file.at(phys).context.value == this->ctx_index.value);
      reg_allocator->free_register(phys);
    }
  }
  frontend_RAT.fill(-1);
  backend_RAT.fill(-1);
}

void champsim::core_context::flush_pipeline()
{
  // Discard every in-flight (uncommitted) instruction. Memory requests already
  // issued keep flying in the channel -- this clears the tracking, not the
  // request (value-less model, bmt-design.md section 2 step 1).
  IFETCH_BUFFER.clear();
  DECODE_BUFFER.clear();
  DISPATCH_BUFFER.clear();
  ROB.clear();
  DIB_HIT_BUFFER.clear();
  for (auto& entry : LQ)
    entry.reset();
  SQ.clear();

  // Roll the rename alias table back to the committed mapping. This is the RAT
  // half of the scoped flush; the physical-register half is the loop below.
  reset_frontend_RAT();

  // Release every in-flight physical register this thread holds. After the
  // buffers are cleared and frontend_RAT rolled back, the only registers this
  // context should still own are the committed backend_RAT values: a register is
  // either the committed mapping for its architectural register (in backend_RAT)
  // or the not-yet-committed destination of a younger write (busy but absent from
  // backend_RAT). Free the latter.
  for (std::size_t phys = 0; phys < reg_allocator->physical_register_file.size(); ++phys) {
    const auto& pr = reg_allocator->physical_register_file[phys];
    const bool committed = std::find(std::begin(backend_RAT), std::end(backend_RAT), static_cast<PHYSICAL_REGISTER_ID>(phys)) != std::end(backend_RAT);
    if (pr.busy && pr.context.value == this->ctx_index.value && !committed)
      reg_allocator->free_register(static_cast<PHYSICAL_REGISTER_ID>(phys));
  }
}

void champsim::core_context::rsave_registers(std::array<PHYSICAL_REGISTER_ID, std::numeric_limits<uint8_t>::max() + 1>& saved_rat)
{
  // Snapshot the committed mapping first (the value-less IRB entry), then release
  // exactly the registers that mapping holds -- the release loop is copied
  // line-for-line from reclaim_registers (bmt-design.md section 2 step 3).
  saved_rat = backend_RAT;
  for (auto phys : backend_RAT) {
    if (phys != -1) {
      CHAMPSIM_ASSERT(reg_allocator->physical_register_file.at(phys).context.value == this->ctx_index.value);
      reg_allocator->free_register(phys);
    }
  }
  frontend_RAT.fill(-1);
  backend_RAT.fill(-1);
}

// The RAT half of the deadlock printer, printed back-to-back with
// RegisterAllocator::print_deadlock() (the physical-file half) so the combined
// output matches the original single-function version.
void champsim::core_context::print_deadlock()
{
  fmt::print("Frontend Register Allocation Table        Backend Register Allocation Table\n");
  for (size_t i = 0; i < frontend_RAT.size(); ++i) {
    fmt::print("Arch reg: {:3}    Phys reg: {:3}            Arch reg: {:3}    Phys reg: {:3}\n", i, frontend_RAT[i], i, backend_RAT[i]);
  }

  if (reg_allocator->count_free_registers() == 0) {
    fmt::print("\n**WARNING!! WARNING!!** THE PHYSICAL REGISTER FILE IS COMPLETELY OCCUPIED.\n");
    fmt::print("It is extremely likely your register file size is too small.\n");
  }
}