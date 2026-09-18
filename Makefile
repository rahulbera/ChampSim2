override ROOT_DIR := $(patsubst %/,%,$(dir $(abspath $(firstword $(MAKEFILE_LIST)))))
.DEFAULT_GOAL := all
BUILD_MODE ?= release
BUILD_FLAVOR ?= sim
WITH_RAMULATOR2 ?= 0
RAMULATOR2_ROOT ?=
RAMULATOR2_SANITIZE ?= 0
OBJ_ROOT ?= .csconfig
DEP_ROOT ?= $(OBJ_ROOT)
VCPKG_INSTALLED_DIR ?= $(ROOT_DIR)/vcpkg_installed
CHAMPSIM_LIBRARIES ?= -lCLI11 -llzma -lz -lbz2 -lzstd -lfmt
CHAMPSIM_TEST_LIBRARIES ?= -lCatch2Main -lCatch2
test_main_name ?= test/bin/000-test-main
shellquote = '$(subst ','"'"',$1)'
make_short_flags := $(if $(findstring =,$(firstword $(MAKEFLAGS))),,$(filter-out --%,$(firstword $(MAKEFLAGS))))
make_no_execute := $(strip $(foreach flag,n q t,$(findstring $(flag),$(make_short_flags))))

# One inner Make owns exactly one mode and one production/test flavor. Recursive
# Make preserves the jobserver and independently honors -n/-q/-t.
ifndef CHAMPSIM_INNER
named_goals := $(filter debug release fast,$(MAKECMDGOALS))
ifneq (,$(named_goals))
ifneq (,$(filter-out debug release fast,$(MAKECMDGOALS)))
$(error Do not mix named modes and ordinary targets; use BUILD_MODE=fast all test)
endif
ifneq ($(origin BUILD_MODE),file)
ifneq (,$(filter-out $(BUILD_MODE),$(named_goals)))
$(error BUILD_MODE conflicts with the named mode target)
endif
endif
endif
.PHONY: all test debug release fast clean compile_commands print-build-paths ramulator2 pytest configclean compile_commands_clean maketest
all test clean compile_commands print-build-paths ramulator2:
	+@$(MAKE) --no-print-directory CHAMPSIM_INNER=1 BUILD_FLAVOR=$(if $(filter test,$@),test,$(BUILD_FLAVOR)) $@
debug release fast:
	+@$(MAKE) --no-print-directory CHAMPSIM_INNER=1 BUILD_MODE=$@ BUILD_FLAVOR=sim PUBLISH_ALIAS=0 all
# Explicit compatibility paths (notably CI's test executable) are ordinary builds.
other_goals := $(filter-out all test clean compile_commands print-build-paths ramulator2 debug release fast pytest configclean compile_commands_clean maketest,$(MAKECMDGOALS))
.PHONY: $(other_goals)
$(other_goals):
	+@$(MAKE) --no-print-directory CHAMPSIM_INNER=1 BUILD_FLAVOR=$(if $(filter $(test_main_name),$@),test,$(BUILD_FLAVOR)) $@
pytest:
	PYTHONPATH=$(PYTHONPATH):$(ROOT_DIR) python3 -m unittest discover -v --start-directory=test/python
configclean compile_commands_clean maketest:
	+@$(MAKE) --no-print-directory CHAMPSIM_INNER=1 $@
else
include $(ROOT_DIR)/config/build_rules.mk
endif
