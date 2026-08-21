// NOLINTBEGIN(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers): generated
// The runtime module registry: name -> factory over every module config.sh
// discovered. A separate translation unit keeps the union of all module
// headers out of every other one.
//
// registry.cc.inc includes its own guarded declaration, so include order here
// does not matter (clang-format sorts within a block).

#if __has_include("registry.cc.inc")
#include "registry.cc.inc"
#endif

// NOLINTEND(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)
