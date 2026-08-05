# Standalone -- deliberately not part of the ChampSim build.
#
# Anything under branch/ is compiled and linked into every simulator binary
# (Makefile:294-295), so a translation unit with main() cannot live there.

CHAMPSIM_ROOT ?= ../..
TENANT_DIR    ?= $(CHAMPSIM_ROOT)/branch/cbp6_tagescl64/vendor

CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
INCLUDES  = -I$(CHAMPSIM_ROOT)/inc -I$(TENANT_DIR)

.PHONY: all clean

all: cbp6_replay

cbp6_replay: cbp6_replay.cc
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $<

clean:
	$(RM) cbp6_replay
