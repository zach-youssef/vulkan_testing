# Building ZGAL programs (dsl/): rules generating a .zgal file's C++ header and shaders, and building
# programs whose main ZGAL generates. This repository's Makefile includes it, and so can other
# projects' (e.g. with the wrapper as a submodule):
#
#     include external/vulkan_test/zgal.mk
#     $(eval $(call zgal-program,game/game.zgal,build/game,game,-lglfw))
#     $(eval $(call zgal-web-program,game/game.zgal,build/web/game))
#
# The including Makefile defines CXX (a C++20 compiler), CFLAGS (its flags), LDFLAGS and LDLIBS
# (with -lvulkan). ZGAL_CFLAGS adds the wrapper's include paths and the platform's (the Vulkan
# headers, GLM, GLFW) to the rules' compilations, and ZGAL_LDFLAGS the platform's library paths to
# their links; a Makefile can use both for its own targets too. ZGAL_ROOT is the wrapper's
# directory, by default this file's.
# ---------------------------------------------------------------------------
ZGAL_ROOT ?= $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))

# The platform. The Vulkan headers and loader are the SDK's when VULKAN_SDK is set (the SDK's
# setup-env.sh sets it), else the system's. On macOS, the SDK's installer puts them in /usr/local
# (where the loader is found at run time through an rpath) and Homebrew has GLFW and GLM; on Linux
# they're in the system's default paths (e.g. the libvulkan-dev, libglfw3-dev and libglm-dev
# packages).
ZGAL_OS := $(shell uname -s)
ZGAL_PLATFORM_CFLAGS = $(if $(VULKAN_SDK),-I$(VULKAN_SDK)/include)
ZGAL_LDFLAGS = $(if $(VULKAN_SDK),-L$(VULKAN_SDK)/lib)
ifeq ($(ZGAL_OS),Darwin)
ZGAL_PLATFORM_CFLAGS += -I/usr/local/include -I/opt/homebrew/include
ZGAL_LDFLAGS += -L/usr/local/lib -L/opt/homebrew/lib -Wl,-rpath,/usr/local/lib
endif

RACKET = racket
RACO = raco
ZGAL_DSL_DIR = $(ZGAL_ROOT)/dsl
ZGAL_DSL_SRCS = $(wildcard $(ZGAL_DSL_DIR)/*.rkt $(ZGAL_DSL_DIR)/private/*.rkt)
# The compiler, compiled (Racket keeps compiled code in compiled/ directories beside the sources)
ZGAL_COMPILED = $(ZGAL_DSL_DIR)/compiled/zgal_rkt.zo
ZGAL = $(RACKET) $(ZGAL_DSL_DIR)/zgal.rkt

# The wrapper's headers and the libraries it compiles (stb, cgltf, tinyobjloader)
ZGAL_CFLAGS = -I$(ZGAL_ROOT)/include -I$(ZGAL_ROOT)/third_party/stb -I$(ZGAL_ROOT)/third_party/cgltf \
              -I$(ZGAL_ROOT)/third_party/tinyobjloader $(ZGAL_PLATFORM_CFLAGS)
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
# GLSL bodies' includes are found in <dir>/shaders. The compiler only rewrites the shaders that
# changed, so only those are recompiled, unless GLSL they include (or a GLSL body) changed: then all.
define zgal-rule
$(2)/$(notdir $(1)).stamp: $(1) $(ZGAL_COMPILED) $(ZGAL_GLSL_INCLUDES)
	$(ZGAL) --cpp-out $(2) --glsl-out $(2)/shaders --depfile $(2)/$(notdir $(1)).d --dep-target $$@ $$<
	@mkdir -p $(3)
	@all=; for f in $$?; do case $$$$f in *.zgal|$(ZGAL_COMPILED)) ;; *) all=1 ;; esac; done; \
	for shader in $(2)/shaders/*; do \
		[ -e "$$$$shader" ] || continue; \
		spv=$(3)/$$$$(basename $$$$shader).spv; \
		[ -n "$$$$all" ] || [ ! -e "$$$$spv" ] || [ "$$$$shader" -nt "$$$$spv" ] || continue; \
		echo glslc $$$$shader; \
		glslc $(ZGAL_GLSLC_FLAGS) -I$(dir $(1))shaders $$$$shader -o $$$$spv || exit 1; \
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
	$$(CXX) -c -o $$@ $(2)/generated/$(notdir $(1)).main.cpp $$(CFLAGS) $$(ZGAL_CFLAGS) -I$(2)/generated
$(if $(wildcard $(dir $(1))*.cpp),$(patsubst $(dir $(1))%.cpp,$(2)/%.o,$(wildcard $(dir $(1))*.cpp)): $(2)/%.o: $(dir $(1))%.cpp $(2)/generated/$(notdir $(1)).stamp $$(ZGAL_HEADERS)
	@mkdir -p $$(dir $$@)
	$$(CXX) -c -o $$@ $$< $$(CFLAGS) $$(ZGAL_CFLAGS) -I$(2)/generated)
$(2)/assets/%: $(dir $(1))assets/%
	@mkdir -p $$(dir $$@)
	cp $$< $$@
$(2)/$(3): $(2)/$(notdir $(1)).main.o $(patsubst $(dir $(1))%.cpp,$(2)/%.o,$(wildcard $(dir $(1))*.cpp)) | $(patsubst $(dir $(1))assets/%,$(2)/assets/%,$(shell find $(dir $(1))assets -type f 2>/dev/null))
	$$(CXX) $$(filter %.o,$$^) -o $$@ $$(LDFLAGS) $$(ZGAL_LDFLAGS) $$(LDLIBS) $(4)
endef

# The web backend's npm packages (TypeScript, and Dawn, WebGPU for Node), which building and running
# programs for the web uses
NODE ?= node
NPM ?= npm
ZGAL_WEB_DIR = $(ZGAL_ROOT)/web
ZGAL_WEB_PACKAGES = $(ZGAL_WEB_DIR)/node_modules/.package-lock.json
ZGAL_WEB_RUNTIME = $(wildcard $(ZGAL_WEB_DIR)/zgal/*.ts)

$(ZGAL_WEB_PACKAGES): $(ZGAL_WEB_DIR)/package.json $(ZGAL_WEB_DIR)/package-lock.json
	cd $(ZGAL_WEB_DIR) && $(NPM) ci
	@touch $@

# A program for the web (a .zgal file with a main): generates its TypeScript into <out>/src beside a
# copy of the runtime (web/zgal) and the program's own TypeScript, the .ts files beside the .zgal file
# (<name>.inputs.ts defining its mainInputs, if it has MainInputs), and builds it to JavaScript in
# <out>: <name>.zgal.main.js, the program, and for one with a window its page, <name>.zgal.html. The
# files under <dir>/assets are copied to <out>/assets, where it finds them. Serve <out> and open the page, or run it headless
# under Node: node <out>/<name>.zgal.main.js --frames N.
#   $(call zgal-web-program,<dir>/<name>.zgal,<out>)
define zgal-web-program
$(2)/src/$(notdir $(1)).stamp: $(1) $(wildcard $(dir $(1))*.ts) $(ZGAL_COMPILED) $(ZGAL_WEB_RUNTIME) $(ZGAL_WEB_DIR)/program.tsconfig.json
	rm -rf $(2)/src
	@mkdir -p $(2)/src
	$(ZGAL) --web-out $(2)/src --depfile $(2)/src/$(notdir $(1)).d --dep-target $$@ $$<
	cp -R $(ZGAL_WEB_DIR)/zgal $(2)/src/zgal
	$(if $(wildcard $(dir $(1))*.ts),cp $(wildcard $(dir $(1))*.ts) $(2)/src/)
	cp $(ZGAL_WEB_DIR)/program.tsconfig.json $(2)/src/tsconfig.json
	echo '{"type": "module"}' > $(2)/src/package.json
	ln -sfn $$(abspath $(ZGAL_WEB_DIR)/node_modules) $(2)/src/node_modules
	@touch $$@
-include $(2)/src/$(notdir $(1)).d
$(2)/$(notdir $(1)).main.js: $(2)/src/$(notdir $(1)).stamp $(ZGAL_WEB_PACKAGES)
	$(NODE) $(ZGAL_WEB_DIR)/node_modules/typescript/bin/tsc -p $(2)/src --outDir $(2) --rootDir $(2)/src
	for page in $(2)/src/*.html; do [ -e "$$$$page" ] && cp "$$$$page" $(2)/ || true; done
	echo '{"type": "module"}' > $(2)/package.json
	ln -sfn $$(abspath $(ZGAL_WEB_DIR)/node_modules) $(2)/node_modules
$(2)/assets/%: $(dir $(1))assets/%
	@mkdir -p $$(dir $$@)
	cp $$< $$@
$(2): $(2)/$(notdir $(1)).main.js $(patsubst $(dir $(1))assets/%,$(2)/assets/%,$(shell find $(dir $(1))assets -type f 2>/dev/null))
.PHONY: $(2)
endef
