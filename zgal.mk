# Building ZGAL programs (dsl/): rules generating a .zgal file's C++ header and shaders, and building
# programs whose main ZGAL generates. This repository's Makefile includes it, and so can other
# projects' (e.g. with the wrapper as a submodule):
#
#     include external/vulkan_test/zgal.mk
#     $(eval $(call zgal-program,game/game.zgal,build/game,game,-lglfw))
#
# The including Makefile defines CC, CFLAGS (a C++20 compiler's flags, with the Vulkan SDK's include
# path), LDFLAGS and LDLIBS (with -lvulkan); ZGAL_CFLAGS adds the wrapper's include paths to the
# rules' compilations. ZGAL_ROOT is the wrapper's directory, by default this file's.
# ---------------------------------------------------------------------------
ZGAL_ROOT ?= $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))

RACKET = racket
RACO = raco
ZGAL_DSL_DIR = $(ZGAL_ROOT)/dsl
ZGAL_DSL_SRCS = $(wildcard $(ZGAL_DSL_DIR)/*.rkt $(ZGAL_DSL_DIR)/private/*.rkt)
# The compiler, compiled (Racket keeps compiled code in compiled/ directories beside the sources)
ZGAL_COMPILED = $(ZGAL_DSL_DIR)/compiled/zgal_rkt.zo
ZGAL = $(RACKET) $(ZGAL_DSL_DIR)/zgal.rkt

# The wrapper's headers and the libraries it compiles (stb, cgltf, tinyobjloader)
ZGAL_CFLAGS = -I$(ZGAL_ROOT)/include -I$(ZGAL_ROOT)/third_party/stb -I$(ZGAL_ROOT)/third_party/cgltf \
              -I$(ZGAL_ROOT)/third_party/tinyobjloader
ZGAL_HEADERS = $(shell find $(ZGAL_ROOT)/include -name '*.h')
# Shaders include the wrapper's GLSL (e.g. bindless.glsl)
ZGAL_GLSL_INCLUDES = $(wildcard $(ZGAL_ROOT)/include/glsl/*.glsl)
ZGAL_GLSLC_FLAGS = -I$(ZGAL_ROOT)/include/glsl --target-env=vulkan1.3

$(ZGAL_COMPILED): $(ZGAL_DSL_SRCS)
	$(RACO) make $(ZGAL_DSL_DIR)/zgal.rkt

# Generates <out>/<name>.zgal.h and <out>/shaders/<shader> from <dir>/<name>.zgal, and compiles the
# shaders into <spv> (<spv>/<shader>.spv), with a depfile making the stamp depend on the files it
# imports and its GLSL bodies:
#   $(call zgal-rule,<dir>/<name>.zgal,<out>,<spv>)
# GLSL bodies' includes are found in <dir>/shaders. Any change regenerates and recompiles them all.
define zgal-rule
$(2)/$(notdir $(1)).stamp: $(1) $(ZGAL_COMPILED) $(ZGAL_GLSL_INCLUDES)
	@rm -rf $(2)/shaders
	$(ZGAL) --cpp-out $(2) --glsl-out $(2)/shaders --depfile $(2)/$(notdir $(1)).d --dep-target $$@ $$<
	@mkdir -p $(3)
	@for shader in $(2)/shaders/*; do \
		[ -e "$$$$shader" ] || continue; \
		echo glslc $$$$shader; \
		glslc $(ZGAL_GLSLC_FLAGS) -I$(dir $(1))shaders $$$$shader -o $(3)/$$$$(basename $$$$shader).spv || exit 1; \
	done
	@touch $$@
-include $(2)/$(notdir $(1)).d
endef

# A program whose main is generated (a .zgal file with a main): builds <out>/<binary> from the
# generated <name>.zgal.main.cpp and the program's own C++, the .cpp files beside the .zgal file
# (defining its MainInputs, if it has any), compiled with <out>/generated on the include path. Its
# SPIR-V goes in <out>/shaders, and the files under <dir>/assets are copied to <out>/assets: beside
# it, where the context finds them. <libraries>: e.g. -lglfw for a program with a window.
#   $(call zgal-program,<dir>/<name>.zgal,<out>,<binary>,<libraries>)
define zgal-program
$(call zgal-rule,$(1),$(2)/generated,$(2)/shaders)
$(2)/$(notdir $(1)).main.o: $(2)/generated/$(notdir $(1)).stamp $$(ZGAL_HEADERS)
	@mkdir -p $$(dir $$@)
	$$(CC) -c -o $$@ $(2)/generated/$(notdir $(1)).main.cpp $$(CFLAGS) $$(ZGAL_CFLAGS) -I$(2)/generated
$(if $(wildcard $(dir $(1))*.cpp),$(patsubst $(dir $(1))%.cpp,$(2)/%.o,$(wildcard $(dir $(1))*.cpp)): $(2)/%.o: $(dir $(1))%.cpp $(2)/generated/$(notdir $(1)).stamp $$(ZGAL_HEADERS)
	@mkdir -p $$(dir $$@)
	$$(CC) -c -o $$@ $$< $$(CFLAGS) $$(ZGAL_CFLAGS) -I$(2)/generated)
$(2)/assets/%: $(dir $(1))assets/%
	@mkdir -p $$(dir $$@)
	cp $$< $$@
$(2)/$(3): $(2)/$(notdir $(1)).main.o $(patsubst $(dir $(1))%.cpp,$(2)/%.o,$(wildcard $(dir $(1))*.cpp)) | $(patsubst $(dir $(1))assets/%,$(2)/assets/%,$(shell find $(dir $(1))assets -type f 2>/dev/null))
	$$(CC) $$(filter %.o,$$^) -o $$@ $$(LDFLAGS) $$(LDLIBS) $(4)
	install_name_tool -add_rpath /usr/local/lib ./$$@
endef
