#define CATCH_CONFIG_MAIN
#include <catch.hpp>

#include "champsim.h"
#include "defs.h"

// Derived, not restated: 501-static-environment.cc builds a real
// champsim::static_environment, which is sized by champsim::defs. Hardcoding
// these left the test binary claiming one core while the environment under
// test built two, the moment anyone edited inc/defs.h.
const std::size_t NUM_CPUS = champsim::defs::num_cpus;

const unsigned BLOCK_SIZE = champsim::defs::block_size;
const unsigned PAGE_SIZE = champsim::defs::page_size;
