// NOLINTBEGIN(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers): generated
// The runtime module registry: name -> factory over every compiled module,
// generated per build id by config/module_registry.py. A separate translation
// unit keeps the union of all module headers out of the environment TU.

#if __has_include("registry.cc.inc")
// core_inst.inc is included here, once, rather than inside registry.cc.inc:
// a multi-executable configure joins one registry fragment per build id into
// that file, and core_inst.inc has no include guard.
#include "core_inst.inc"
#include "registry.cc.inc"
#endif

// NOLINTEND(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)
