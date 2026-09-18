# Keep upstream native instrumentation paired with the host. Apply it only in
# the inner Make; named modes already provide their owned debug-symbol policy.
ifeq ($(RAMULATOR2_SANITIZE),1)
ifneq ($(WITH_RAMULATOR2),1)
$(error RAMULATOR2_SANITIZE=1 requires WITH_RAMULATOR2=1 RAMULATOR2_ROOT=/path/to/ramulator2)
endif
sanitizer_options := -fsanitize=address,undefined -fno-omit-frame-pointer
override CXXFLAGS += $(sanitizer_options)
override LDFLAGS += $(sanitizer_options)
else ifneq ($(RAMULATOR2_SANITIZE),0)
$(error RAMULATOR2_SANITIZE must be 0 or 1, not '$(RAMULATOR2_SANITIZE)')
endif
native_sanitize_option = $(if $(filter 1,$(RAMULATOR2_SANITIZE)), --sanitize)

# List all subdirectories of a given directory
# $1 - parent directory
ls_dirs = $(patsubst %/,%,$(filter %/,$(wildcard $1/*/)))

# Migrate names from a source directory (and suffix) to a target directory (and suffix)
# $1 - source directory
# $2 - target directory
# $3 - unique build id
migrate = $(patsubst $1/%.cc,$2/%.o,$(join $(dir $4),$(patsubst %main.cc,$3_%main.cc,$(notdir $4))))
get_object_list = $(call migrate,$1,$2,$3,$(wildcard $1/*.cc)) $(foreach subdir,$(call ls_dirs,$1),$(call $0,$(subdir),$(patsubst $1/%,$2/%,$(subdir)),$3))

# Return the trailing portion of a word sequence
# $1 - the sequence
tail = $(wordlist 2,$(words $1),$1)

# Split a path into a series of words that are path componenents
# $1 - the path to split
_root_standin=__ROOT__
split_path = $(subst /, ,$(patsubst /%,$(_root_standin)/%,$1))

# Join a series of words into a path
# $1 - the path componenents
join_path = $(subst $(eval) $(eval),,$(filter-out $(_root_standin),$(firstword $1) $(addprefix /,$(call tail,$1))))

# Return the common prefix between two paths
# $1 - the first path
# $2 - the second path
common_prefix_impl = $(if $(and $1,$2),$(if $(findstring $(firstword $1),$(firstword $2)),$(firstword $1) $(call $0,$(call tail,$1),$(call tail,$2))))
common_prefix = $(call join_path,$(call $0_impl,$(call split_path,$1),$(call split_path,$2)))

# Remove the given prefix from each word
# $1 - the prefix to remove
# $2 - the words to remove from
remove_prefix_impl = $(if $1,$(if $(findstring $(firstword $1),$(firstword $2)),$(call $0,$(call tail,$1),$(call tail,$2))),$2)
remove_prefix = $(call join_path,$(call $0_impl,$(call split_path,$1),$(call split_path,$2)))

# Given a prefix, return the relative prefix of the same length
# $1 - the prefix
make_relative_prefix = $(call join_path,$(patsubst %,..,$(call split_path,$1)))

# Return the relative path from one path to another
# $1 - the destination path
# $2 - the origin path
#relative_path_impl = $(if $2,$(call make_relative_prefix,$2)/$1,$1)
#relative_path = $(call $0_impl,$(call remove_prefix,$(call common_prefix,$1,$2),$1),$(call remove_prefix,$(call common_prefix,$1,$2),$2))
relative_path = $(shell python3 -c "import os.path; print(os.path.relpath(\"$1\", start=\"$2\"))")

# Recursively find all files matching a pattern within a directory
# $1 - the directory to search
# $2 - the pattern to match
rwildcard = $(foreach d,$(wildcard $(1:=/*)),$(call rwildcard,$d,$2) $(filter $(subst *,%,$2),$d))

# Get the parent directory of a path
# $1 - the path
parent_dir = $(patsubst %/,%,$(dir $1))


sim_key := SIM
executable_name =
nonbase_module_objs =
override MODULE_ROOT += $(ROOT_DIR)
override BRANCH_ROOT += $(addsuffix /branch,$(MODULE_ROOT))
override BTB_ROOT += $(addsuffix /btb,$(MODULE_ROOT))
override PREFETCH_ROOT += $(addsuffix /prefetcher,$(MODULE_ROOT))
override REPLACEMENT_ROOT += $(addsuffix /replacement,$(MODULE_ROOT))
module_dirs = $(foreach d,$(BRANCH_ROOT) $(BTB_ROOT) $(PREFETCH_ROOT) $(REPLACEMENT_ROOT),$(call relative_path,$(abspath $d),$(ROOT_DIR)))
get_module_obj_dir=$(OBJ_ROOT)/modules/$(patsubst ..%,externUPdir%,$(subst /..,_UPdir,$1))
get_module_src_dir=$(patsubst externUPdir%,..%,$(subst _UPdir,/..,$(patsubst $(DEP_ROOT)/modules/%,%,$(patsubst $(OBJ_ROOT)/modules/%,%,$1))))
get_module_list = $(foreach mod_type,$1,$(call get_object_list,$(mod_type),$(call get_module_obj_dir,$(mod_type))))
base_module_objs = $(call get_module_list,$(module_dirs))

# Discovery describes immutable inputs and defaults, never effective policy roots.
configured_bindir ?= bin
registry_dir ?= .csconfig
ifneq (,$(filter configclean compile_commands_clean maketest clean,$(MAKECMDGOALS)))
-include _configuration.mk
else
include _configuration.mk
endif
BIN_ROOT ?= $(configured_bindir)
object_container := $(OBJ_ROOT)
dependency_container := $(DEP_ROOT)
binary_container := $(BIN_ROOT)
publication_binary := $(executable_name)
PUBLISH_ALIAS ?= 1

policy_helper = python3 $(ROOT_DIR)/config/build_config.py
policy_arguments = --obj=$(call shellquote,$(object_container)) --dep=$(call shellquote,$(dependency_container)) --binary=$(call shellquote,$(binary_container)) --registry=$(call shellquote,$(registry_dir)) --cxx=$(call shellquote,$(CXX)) --mode=$(call shellquote,$(BUILD_MODE)) --flavor=$(call shellquote,$(BUILD_FLAVOR)) --isa=$(call shellquote,$(X86_ISA)) $(if $(filter undefined,$(origin X86_ISA)),,--isa-explicit) --triplet=$(call shellquote,$(VCPKG_TARGET_TRIPLET)) --installed=$(call shellquote,$(VCPKG_INSTALLED_DIR)) --native=$(call shellquote,$(WITH_RAMULATOR2)) --native-root=$(call shellquote,$(RAMULATOR2_ROOT)) --native-sanitize=$(call shellquote,$(RAMULATOR2_SANITIZE)) --cppflags=$(call shellquote,$(CPPFLAGS)) --cxxflags=$(call shellquote,$(CXXFLAGS)) --ldflags=$(call shellquote,$(LDFLAGS)) --ldlibs=$(call shellquote,$(LDLIBS)) --loadlibes=$(call shellquote,$(LOADLIBES)) --libraries=$(call shellquote,$(CHAMPSIM_LIBRARIES)) --test-libraries=$(call shellquote,$(CHAMPSIM_TEST_LIBRARIES))
policy_selection := $(shell $(policy_helper) inspect $(policy_arguments))
ifneq ($(words $(policy_selection)),3)
$(error Build policy selection failed; see diagnostic above)
endif
policy_leaf := $(word 1,$(policy_selection))/$(word 2,$(policy_selection))/$(BUILD_MODE)/$(word 3,$(policy_selection))/$(BUILD_FLAVOR)
override OBJ_ROOT := $(object_container)/$(policy_leaf)
override DEP_ROOT := $(dependency_container)/$(policy_leaf)
override BIN_ROOT := $(binary_container)/$(policy_leaf)
canonical_binary := $(if $(filter test,$(BUILD_FLAVOR)),$(BIN_ROOT)/000-test-main,$(BIN_ROOT)/$(notdir $(publication_binary)))
policy_prepare = $(policy_helper) prepare $(policy_arguments) --obj=$(call shellquote,$(OBJ_ROOT)) --registry=$(call shellquote,$(registry_dir))
native_helper = python3 $(ROOT_DIR)/config/ramulator2_build.py --mode=$(call shellquote,$(WITH_RAMULATOR2)) --root=$(call shellquote,$(RAMULATOR2_ROOT)) --obj=$(call shellquote,$(OBJ_ROOT)) --cxx=$(call shellquote,$(CXX)) --flags=$(call shellquote,$(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS)) --abi-flags=$(call shellquote,@$(OBJ_ROOT)/policy.options @$(OBJ_ROOT)/absolute.options) --policy=$(call shellquote,$(OBJ_ROOT)/build-policy.json)$(native_sanitize_option)

# Do not prepare or remake generated dependencies under -n/-q/-t or inspection.
ifeq (,$(make_no_execute))
ifeq (,$(filter print-build-paths clean configclean compile_commands_clean maketest,$(MAKECMDGOALS)))
preparation_status := $(shell $(policy_prepare) >&2 && $(native_helper) >&2 $(if $(filter 1,$(WITH_RAMULATOR2)),&& $(policy_prepare) >&2); echo $$?)
ifneq ($(preparation_status),0)
$(error Build preparation failed; see diagnostic and $(OBJ_ROOT)/ramulator2-native/build.log)
endif
endif
endif

.PHONY: all test clean compile_commands print-build-paths ramulator2 configclean compile_commands_clean
print-build-paths:
	@$(policy_helper) paths --obj=$(call shellquote,$(OBJ_ROOT)) --dep=$(call shellquote,$(DEP_ROOT)) --binary=$(call shellquote,$(canonical_binary))

base_options := $(OBJ_ROOT)/policy.options $(OBJ_ROOT)/absolute.options
metadata := $(base_options) $(OBJ_ROOT)/module-policy.options $(OBJ_ROOT)/link.options $(OBJ_ROOT)/libraries.options $(OBJ_ROOT)/build-policy.json $(OBJ_ROOT)/build_info_generated.h
$(metadata):
	$(policy_prepare)
$(OBJ_ROOT)/compiler.stamp $(OBJ_ROOT)/ramulator2_build.h:
	$(native_helper)

ifeq ($(WITH_RAMULATOR2),1)
native_library := $(abspath $(RAMULATOR2_ROOT))/libramulator.so
$(native_library):
	$(native_helper)
native_private_options := -isystem $(call shellquote,$(abspath $(RAMULATOR2_ROOT))/src) -std=c++20
native_link_options := -Wl,-rpath,$(call shellquote,$(abspath $(RAMULATOR2_ROOT))) -ldl
endif
$(OBJ_ROOT)/ramulator2_driver.o $(DEP_ROOT)/ramulator2_driver.d: private native_options = $(native_private_options)
$(OBJ_ROOT)/ramulator2_driver.o $(DEP_ROOT)/ramulator2_driver.d: $(OBJ_ROOT)/ramulator2_build.h
$(OBJ_ROOT)/build_info.o $(DEP_ROOT)/build_info.d: $(OBJ_ROOT)/build_info_generated.h
ifneq (,$(findstring t,$(make_short_flags)))
# Touch mode must not fabricate provenance headers or a native shared library.
missing_preparation := $(filter-out $(wildcard $(metadata) $(OBJ_ROOT)/compiler.stamp $(OBJ_ROOT)/ramulator2_build.h $(native_library)),$(metadata) $(OBJ_ROOT)/compiler.stamp $(OBJ_ROOT)/ramulator2_build.h $(native_library))
.PHONY: $(missing_preparation)
endif
ramulator2:
	@test "$(WITH_RAMULATOR2)" = 1 || { echo 'Use WITH_RAMULATOR2=1 RAMULATOR2_ROOT=/path/to/ramulator2'; exit 1; }

.SECONDEXPANSION:
base_source_dir := src
test_source_dir := test/cpp/src
get_base_objs = $(call get_object_list,$(base_source_dir),$(OBJ_ROOT),$1)
test_base_objs = $(call get_object_list,$(test_source_dir),$(OBJ_ROOT)/test,TEST)
selected_objects := $(call get_base_objs,$(if $(filter test,$(BUILD_FLAVOR)),TEST,SIM)) $(if $(filter test,$(BUILD_FLAVOR)),$(test_base_objs)) $(base_module_objs) $(nonbase_module_objs)
attach_options = $(addprefix @,$(filter %.options,$^))
define obj_recipe
	$(CXX) $(attach_options) $(native_options) -c -o $@ $(filter %.cc,$^)
endef
define dep_recipe
	$(CXX) $(attach_options) $(native_options) -MM -MP -MT $@ -MT $(patsubst $(DEP_ROOT)/%.d,$(OBJ_ROOT)/%.o,$@) -MF $@ $(filter %.cc,$^)
endef

$(OBJ_ROOT)/%_main.o: src/main.cc $(base_options) | $$(dir $$@)
	$(obj_recipe)
$(DEP_ROOT)/%_main.d: src/main.cc $(base_options) | $$(dir $$@)
	$(dep_recipe)
$(OBJ_ROOT)/%.o: src/%.cc $(base_options) | $$(dir $$@)
	$(obj_recipe)
$(DEP_ROOT)/%.d: src/%.cc $(base_options) | $$(dir $$@)
	$(dep_recipe)
$(OBJ_ROOT)/test/TEST_000-test-main.o: test/cpp/src/000-test-main.cc $(base_options) | $$(dir $$@)
	$(obj_recipe)
$(DEP_ROOT)/test/TEST_000-test-main.d: test/cpp/src/000-test-main.cc $(base_options) | $$(dir $$@)
	$(dep_recipe)
$(OBJ_ROOT)/test/%.o: test/cpp/src/%.cc $(base_options) | $$(dir $$@)
	$(obj_recipe)
$(DEP_ROOT)/test/%.d: test/cpp/src/%.cc $(base_options) | $$(dir $$@)
	$(dep_recipe)
module_prereqs = $(call get_module_src_dir,$(@D))/$(basename $(@F)).cc $(base_options) $(OBJ_ROOT)/module-policy.options
$(OBJ_ROOT)/modules/%.o: $$(module_prereqs) | $$(dir $$@)
	$(obj_recipe)
$(DEP_ROOT)/modules/%.d: $$(module_prereqs) | $$(dir $$@)
	$(dep_recipe)

$(sort $(dir $(selected_objects)) $(dir $(patsubst $(OBJ_ROOT)/%.o,$(DEP_ROOT)/%.d,$(selected_objects))) $(dir $(canonical_binary))):
	mkdir -p $@

$(canonical_binary): $(selected_objects) $(OBJ_ROOT)/ramulator2_build.h $(OBJ_ROOT)/link.options $(OBJ_ROOT)/libraries.options $(native_library) | $$(dir $$@)
	$(CXX) @$(OBJ_ROOT)/link.options -o $@ $(filter %.o,$^) $(native_library) @$(OBJ_ROOT)/libraries.options $(native_link_options)

# Publish by atomic symlink replacement, so running executables retain their inode.
publication_target := $(if $(filter test,$(BUILD_FLAVOR)),$(test_main_name),$(publication_binary))
ifeq ($(PUBLISH_ALIAS),1)
# A newer alias may still point at a different, already-built selection.
ifneq ($(realpath $(publication_target)),$(realpath $(canonical_binary)))
.PHONY: $(publication_target)
endif
$(publication_target): $(canonical_binary)
	$(policy_helper) publish --binary=$(call shellquote,$<) --alias=$(call shellquote,$@)
all: $(publication_target)
else
all: $(canonical_binary)
endif
ifdef TEST_NUM
selected_test = -\# "[$(addprefix \#,$(filter $(addsuffix %,$(TEST_NUM)),$(patsubst %.cc,%,$(notdir $(wildcard $(test_source_dir)/*.cc)))))]"
endif
test: all
	$(canonical_binary) $(selected_test)

compile_commands:
	python3 $(ROOT_DIR)/config/compile_commands/selected.py --policy $(OBJ_ROOT)/build-policy.json --objects $(selected_objects)

clean:
	$(RM) -r $(OBJ_ROOT) $(DEP_ROOT) $(BIN_ROOT)
configclean: clean
	$(RM) _configuration.mk $(registry_dir)/registry.inc $(registry_dir)/registry.cc.inc
compile_commands_clean:
	$(RM) $(OBJ_ROOT)/compile_commands.json

ifeq (,$(filter print-build-paths clean configclean compile_commands_clean compile_commands maketest ramulator2,$(MAKECMDGOALS)))
dependency_files := $(patsubst $(OBJ_ROOT)/%.o,$(DEP_ROOT)/%.d,$(selected_objects))
ifneq (,$(make_no_execute))
dependency_files := $(wildcard $(dependency_files))
.PHONY: $(dependency_files)
endif
-include $(dependency_files)
endif
ifeq (maketest,$(findstring maketest,$(MAKECMDGOALS)))
include $(ROOT_DIR)/test/make/Makefile.test
endif
.NOTINTERMEDIATE: $(dir $(selected_objects))
