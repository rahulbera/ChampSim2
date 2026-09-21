#ifndef RAMULATOR2_MEMORY_BACKEND_H
#define RAMULATOR2_MEMORY_BACKEND_H

#include <memory>
#include <vector>

#include "memory_backend.h"
#include "ramulator2_driver.h"

namespace champsim
{
std::unique_ptr<memory_backend> make_ramulator2_memory_backend(std::unique_ptr<ramulator2_driver> driver, std::vector<channel*> upper_levels);
} // namespace champsim

#endif
