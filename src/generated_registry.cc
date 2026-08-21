// NOLINTBEGIN(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers): generated
// The runtime module registry: name -> factory over every compiled module,
// emitted by config/module_registry.py from the modules it discovers. A
// separate translation unit keeps the union of all module headers out of the
// environment TU.

#if __has_include("registry.cc.inc")

// Declaration before definitions. The blank lines are load-bearing:
// clang-format sorts within a block, and would otherwise put the definitions
// ahead of the struct they define.
#include "core_inst.inc"
#include "registry.cc.inc"
#include "registry.inc"

#endif

// NOLINTEND(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)
