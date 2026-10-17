# Compiler
CC = /usr/bin/g++
CXX = $(CC)

# Include paths
CFLAGS = -std=c++20 -Wc++11-extensions -Wc++14-extensions -I/usr/local/include -Ithird_party/stb -I/Users/zyoussef/VulkanSDK/1.4.321.0/macOS/include/
CXXFLAGS = $(CFLAGS)

# Library paths
LDFLAGS = -L/opt/homebrew/lib -L/usr/local/lib

LDLIBS = -lvulkan
# Only the app layer (include/app) uses GLFW
GLFW_LDLIBS = -lglfw

OUT = build
INCLUDE_DIR = include

CFLAGS += -I$(INCLUDE_DIR)

HEADERS = $(shell find $(INCLUDE_DIR) -name '*.h')

# Shaders include the library's GLSL (e.g. bindless.glsl)
GLSL_INCLUDES = $(wildcard $(INCLUDE_DIR)/glsl/*.glsl)
GLSLC_FLAGS = -I$(INCLUDE_DIR)/glsl --target-env=vulkan1.3

# ---------------------------------------------------------------------------
# ZGAL (dsl/): the shader interface language. Its compiler, in Racket, generates a C++ header and
# complete shaders from a .zgal file. make docs builds its documentation.
# ---------------------------------------------------------------------------
RACKET = racket
RACO = raco
DSL_DIR = dsl
DSL_SRCS = $(wildcard $(DSL_DIR)/*.rkt $(DSL_DIR)/private/*.rkt)
# The compiler, compiled (Racket keeps compiled code in compiled/ directories beside the sources)
ZGAL_COMPILED = $(DSL_DIR)/compiled/zgal_rkt.zo
ZGAL = $(RACKET) $(DSL_DIR)/zgal.rkt

$(ZGAL_COMPILED): $(DSL_SRCS)
	$(RACO) make $(DSL_DIR)/zgal.rkt

# Generates <out>/<name>.zgal.h and <out>/shaders/<shader> from <dir>/<name>.zgal, and compiles the
# shaders into <spv> (<spv>/<shader>.spv), with a depfile making the stamp depend on the files it
# imports and its GLSL bodies:
#   $(call zgal-rule,<dir>/<name>.zgal,<out>,<spv>)
# GLSL bodies' includes are found in <dir>/shaders. Any change regenerates and recompiles them all.
define zgal-rule
$(2)/$(notdir $(1)).stamp: $(1) $(ZGAL_COMPILED) $(GLSL_INCLUDES)
	@rm -rf $(2)/shaders
	$(ZGAL) --cpp-out $(2) --glsl-out $(2)/shaders --depfile $(2)/$(notdir $(1)).d --dep-target $$@ $$<
	@mkdir -p $(3)
	@for shader in $(2)/shaders/*; do \
		[ -e "$$$$shader" ] || continue; \
		echo glslc $$$$shader; \
		glslc $(GLSLC_FLAGS) -I$(dir $(1))shaders $$$$shader -o $(3)/$$$$(basename $$$$shader).spv || exit 1; \
	done
	@touch $$@
-include $(2)/$(notdir $(1)).d
endef

# ---------------------------------------------------------------------------
# Examples
#   make            - build the simple example into build/examples/simple
# ---------------------------------------------------------------------------
EXAMPLES_DIR = examples
EXAMPLES_OUT = $(OUT)/examples

SIMPLE_DIR = $(EXAMPLES_DIR)/simple
SIMPLE_OUT = $(EXAMPLES_OUT)/simple
# The shaders are in simple.zgal; the graph is built in C++
SIMPLE_GEN = $(SIMPLE_OUT)/generated
SIMPLE_ZGAL_STAMP = $(SIMPLE_GEN)/simple.zgal.stamp
SIMPLE_CFLAGS = $(CFLAGS) -I$(SIMPLE_GEN) \
                -DSIMPLE_EXAMPLE_SHADER_DIR='"$(abspath $(SIMPLE_OUT)/shaders)"' \
                -DSIMPLE_EXAMPLE_TEXTURE_DIR='"$(abspath $(SIMPLE_DIR)/textures)"'

$(eval $(call zgal-rule,$(SIMPLE_DIR)/simple.zgal,$(SIMPLE_GEN),$(SIMPLE_OUT)/shaders))

$(SIMPLE_OUT)/simple_example.o : $(SIMPLE_DIR)/simple_example.cpp $(SIMPLE_ZGAL_STAMP) $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) -c -o $@ $< $(SIMPLE_CFLAGS)

$(SIMPLE_OUT)/simple_example: $(SIMPLE_OUT)/simple_example.o
	$(CC) $^ -o $@ $(LDFLAGS) $(LDLIBS) $(GLFW_LDLIBS)
	install_name_tool -add_rpath /usr/local/lib ./$@

# ---------------------------------------------------------------------------
# Example: image_filter
#   A headless command-line GPU image filter (no GLFW)
#   make test-example-image_filter - smoke test with validation, plus golden image checks
# ---------------------------------------------------------------------------
IMAGE_FILTER_DIR = $(EXAMPLES_DIR)/image_filter
IMAGE_FILTER_OUT = $(EXAMPLES_OUT)/image_filter
IMAGE_FILTER_BIN = $(IMAGE_FILTER_OUT)/image_filter
# The shaders and the graph are in image_filter.zgal
IMAGE_FILTER_GEN = $(IMAGE_FILTER_OUT)/generated
IMAGE_FILTER_ZGAL_STAMP = $(IMAGE_FILTER_GEN)/image_filter.zgal.stamp
IMAGE_FILTER_CFLAGS = $(CFLAGS) -I$(IMAGE_FILTER_GEN) -DIMAGE_FILTER_SHADER_DIR='"$(abspath $(IMAGE_FILTER_OUT)/shaders)"'
IMAGE_FILTER_TEST_DIR = $(IMAGE_FILTER_DIR)/tests
IMAGE_FILTER_TEST_OUT = $(IMAGE_FILTER_OUT)/tests
IMAGE_FILTER_DIFF = $(IMAGE_FILTER_TEST_OUT)/image_diff

$(eval $(call zgal-rule,$(IMAGE_FILTER_DIR)/image_filter.zgal,$(IMAGE_FILTER_GEN),$(IMAGE_FILTER_OUT)/shaders))

$(IMAGE_FILTER_OUT)/image_filter.o : $(IMAGE_FILTER_DIR)/image_filter.cpp $(IMAGE_FILTER_ZGAL_STAMP) $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) -c -o $@ $< $(IMAGE_FILTER_CFLAGS)

$(IMAGE_FILTER_BIN): $(IMAGE_FILTER_OUT)/image_filter.o
	$(CC) $^ -o $@ $(LDFLAGS) $(LDLIBS)
	install_name_tool -add_rpath /usr/local/lib ./$@

$(IMAGE_FILTER_DIFF): $(IMAGE_FILTER_TEST_DIR)/image_diff.cpp
	@mkdir -p $(dir $@)
	$(CC) -o $@ $< $(CFLAGS)

# Every pass on the simple example's texture with validation on (any message fails the run), then
# golden checks on a small input: without filters the output must match the input (colour spaces
# round trip), and blur plus edges must match the approved output within a tolerance. To approve a
# new output, run the blur and edges command below and copy its output over the approved image.
test-example-image_filter: $(IMAGE_FILTER_BIN) $(IMAGE_FILTER_DIFF)
	@mkdir -p $(IMAGE_FILTER_TEST_OUT)
	./$(IMAGE_FILTER_BIN) $(SIMPLE_DIR)/textures/texture.jpg $(IMAGE_FILTER_TEST_OUT)/all_passes.png \
		--blur 2 --edges --threshold 0.15 --validation
	./$(IMAGE_FILTER_BIN) $(IMAGE_FILTER_TEST_DIR)/input.png $(IMAGE_FILTER_TEST_OUT)/identity.png --validation
	./$(IMAGE_FILTER_DIFF) $(IMAGE_FILTER_TEST_DIR)/input.png $(IMAGE_FILTER_TEST_OUT)/identity.png 1
	./$(IMAGE_FILTER_BIN) $(IMAGE_FILTER_TEST_DIR)/input.png $(IMAGE_FILTER_TEST_OUT)/blur_edges.png \
		--blur 2 --edges --validation
	./$(IMAGE_FILTER_DIFF) $(IMAGE_FILTER_TEST_DIR)/blur_edges.approved.png $(IMAGE_FILTER_TEST_OUT)/blur_edges.png 2

# ---------------------------------------------------------------------------
# Example: particles
#   GPU particle simulation drawn in a window
#   make test-example-particles - runs it headless for 120 frames with validation, capturing the last
# ---------------------------------------------------------------------------
PARTICLES_DIR = $(EXAMPLES_DIR)/particles
PARTICLES_OUT = $(EXAMPLES_OUT)/particles
# The shaders and the graph are in particles.zgal
PARTICLES_GEN = $(PARTICLES_OUT)/generated
PARTICLES_ZGAL_STAMP = $(PARTICLES_GEN)/particles.zgal.stamp
PARTICLES_CFLAGS = $(CFLAGS) -I$(PARTICLES_GEN) -DPARTICLES_SHADER_DIR='"$(abspath $(PARTICLES_OUT)/shaders)"'

$(eval $(call zgal-rule,$(PARTICLES_DIR)/particles.zgal,$(PARTICLES_GEN),$(PARTICLES_OUT)/shaders))

$(PARTICLES_OUT)/particles.o : $(PARTICLES_DIR)/particles.cpp $(PARTICLES_ZGAL_STAMP) $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) -c -o $@ $< $(PARTICLES_CFLAGS)

$(PARTICLES_OUT)/particles: $(PARTICLES_OUT)/particles.o
	$(CC) $^ -o $@ $(LDFLAGS) $(LDLIBS) $(GLFW_LDLIBS)
	install_name_tool -add_rpath /usr/local/lib ./$@

test-example-particles: $(PARTICLES_OUT)/particles
	./$< --frames 120 --capture $(PARTICLES_OUT)/smoke.png

# ---------------------------------------------------------------------------
# Example: scene
#   A lit 3D scene: shadow map, forward PBR into HDR, tonemap
#   make test-example-scene - headless smoke test (validation on, fails on any message)
# ---------------------------------------------------------------------------
SCENE_DIR = $(EXAMPLES_DIR)/scene
SCENE_OUT = $(EXAMPLES_OUT)/scene
# The shaders and the graph are in scene.zgal
SCENE_GEN = $(SCENE_OUT)/generated
SCENE_ZGAL_STAMP = $(SCENE_GEN)/scene.zgal.stamp
SCENE_CFLAGS = $(CFLAGS) -I$(SCENE_GEN) -DSCENE_EXAMPLE_SHADER_DIR='"$(abspath $(SCENE_OUT)/shaders)"'

$(eval $(call zgal-rule,$(SCENE_DIR)/scene.zgal,$(SCENE_GEN),$(SCENE_OUT)/shaders))

$(SCENE_OUT)/scene_example.o : $(SCENE_DIR)/scene_example.cpp $(wildcard $(SCENE_DIR)/*.h) $(SCENE_ZGAL_STAMP) $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) -c -o $@ $< $(SCENE_CFLAGS)

$(SCENE_OUT)/scene_example: $(SCENE_OUT)/scene_example.o
	$(CC) $^ -o $@ $(LDFLAGS) $(LDLIBS) $(GLFW_LDLIBS)
	install_name_tool -add_rpath /usr/local/lib ./$@

# Renders frames headless, toggling shadows midway, and captures the last one
test-example-scene: $(SCENE_OUT)/scene_example
	./$< --frames 60 --size 640x360 --capture $(SCENE_OUT)/smoke.png

# All the examples' smoke tests
test-examples: test-example-image_filter test-example-particles test-example-scene

examples: $(SIMPLE_OUT)/simple_example $(IMAGE_FILTER_BIN) $(PARTICLES_OUT)/particles $(SCENE_OUT)/scene_example

shaders: $(SIMPLE_ZGAL_STAMP) $(IMAGE_FILTER_ZGAL_STAMP) $(PARTICLES_ZGAL_STAMP) $(SCENE_ZGAL_STAMP)

.DEFAULT_GOAL := examples

# ---------------------------------------------------------------------------
# Tests
#   make test-cpu   - unit tests that do not need a Vulkan device
#   make test-gpu   - tests that run headless on a Vulkan device (no window)
#   make test       - all tests
#   make test-examples - every example's smoke test (headless, with validation)
#   make approve-screenshots - accept the screenshots from the last failed GPU test run
# Arguments can be forwarded to doctest, e.g. make test-cpu TEST_ARGS='-tc="Handle*"'
# ---------------------------------------------------------------------------
TEST_DIR = tests
TEST_OUT = $(OUT)/tests
TEST_SHADER_OUT = $(TEST_OUT)/shaders
# The GPU tests' ZGAL interfaces (tests/gpu/zgal), generated like an example's
TEST_GEN = $(TEST_OUT)/generated
TEST_ZGAL_DIR = $(TEST_DIR)/gpu/zgal
TEST_ZGAL_STAMP = $(TEST_GEN)/interfaces.zgal.stamp
TEST_CFLAGS = $(CFLAGS) -Ithird_party -Ithird_party/approvaltests -Ithird_party/flip -I$(TEST_DIR) -I$(TEST_GEN) \
              -DTEST_SHADER_DIR='"$(abspath $(TEST_SHADER_OUT))"' \
              -DTEST_TEXTURE_DIR='"$(abspath $(SIMPLE_DIR)/textures)"' \
              -DTEST_ASSET_DIR='"$(abspath $(TEST_DIR)/gpu/assets)"'
TEST_HEADERS = $(HEADERS) $(wildcard $(TEST_DIR)/*/*.h)

CPU_TEST_SRCS = $(wildcard $(TEST_DIR)/cpu/*.cpp)
CPU_TEST_OBJS = $(patsubst $(TEST_DIR)/%.cpp,$(TEST_OUT)/%.o,$(CPU_TEST_SRCS))

GPU_TEST_SRCS = $(wildcard $(TEST_DIR)/gpu/*.cpp)
GPU_TEST_OBJS = $(patsubst $(TEST_DIR)/%.cpp,$(TEST_OUT)/%.o,$(GPU_TEST_SRCS))

# SPIR-V used by the GPU tests, compiled to $(TEST_SHADER_OUT)/<name>.<stage>.spv: test-only shaders,
# and those of the tests' ZGAL file (with the stamp, as its shaders' names aren't files)
GPU_TEST_SHADER_SRCS = $(wildcard $(TEST_DIR)/gpu/shaders/*)
GPU_TEST_SHADERS = $(patsubst %,$(TEST_SHADER_OUT)/%.spv,$(notdir $(GPU_TEST_SHADER_SRCS))) $(TEST_ZGAL_STAMP)

$(eval $(call zgal-rule,$(TEST_ZGAL_DIR)/interfaces.zgal,$(TEST_GEN),$(TEST_SHADER_OUT)))

$(TEST_OUT)/gpu/ZgalTest.o: $(TEST_ZGAL_STAMP)

$(TEST_SHADER_OUT)/%.spv : $(TEST_DIR)/gpu/shaders/% $(GLSL_INCLUDES)
	@mkdir -p $(dir $@)
	glslc $(GLSLC_FLAGS) $< -o $@

# Compiled from absolute paths so __FILE__ is absolute: ApprovalTests uses it to find approved images
$(TEST_OUT)/%.o : $(TEST_DIR)/%.cpp $(TEST_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) -c -o $@ $(abspath $<) $(TEST_CFLAGS)

# The CPU tests reflect on compiled test shaders
$(TEST_OUT)/cpu_tests: $(CPU_TEST_OBJS) | $(GPU_TEST_SHADERS)
	$(CC) $^ -o $@ $(LDFLAGS) $(LDLIBS)
	install_name_tool -add_rpath /usr/local/lib ./$@

# The GPU tests cover VulkanApp, which uses GLFW (though only headless)
$(TEST_OUT)/gpu_tests: $(GPU_TEST_OBJS) | $(GPU_TEST_SHADERS)
	$(CC) $^ -o $@ $(LDFLAGS) $(LDLIBS) $(GLFW_LDLIBS)
	install_name_tool -add_rpath /usr/local/lib ./$@

test-cpu: $(TEST_OUT)/cpu_tests
	./$< $(TEST_ARGS)

test-gpu: $(TEST_OUT)/gpu_tests
	./$< $(TEST_ARGS)

# The ZGAL compiler's tests, and its documentation, whose examples are compiled as it's built.
# The tests compile generated code with the build's tools and flags.
DOCS_OUT = $(OUT)/docs
ZGAL_TEST_ENV = ZGAL_GLSLC='glslc' ZGAL_GLSLC_FLAGS='$(GLSLC_FLAGS:-I%=-I$(CURDIR)/%)' \
                 ZGAL_CXX='$(CXX)' ZGAL_CXXFLAGS='$(subst -Iinclude,-I$(CURDIR)/include,$(subst -Ithird_party,-I$(CURDIR)/third_party,$(CXXFLAGS)))'

test-dsl: $(ZGAL_COMPILED) docs
	$(ZGAL_TEST_ENV) $(RACO) test $(DSL_DIR)/tests

# build/docs/zgal.html, and zgal.txt for reading in a terminal
docs: $(ZGAL_COMPILED)
	@mkdir -p $(DOCS_OUT)
	$(RACO) scribble --html --dest $(DOCS_OUT) $(DSL_DIR)/docs/zgal.scrbl
	$(RACO) scribble --text --dest $(DOCS_OUT) $(DSL_DIR)/docs/zgal.scrbl

test: test-cpu test-gpu test-dsl

SCREENSHOT_DIR = $(TEST_DIR)/gpu/approved

approve-screenshots:
	@find $(SCREENSHOT_DIR) -name '*.received.png' 2>/dev/null | while read -r received; do \
		approved="$${received%.received.png}.approved.png"; \
		mv "$$received" "$$approved" && echo "Approved $$approved"; \
	done
	@find $(SCREENSHOT_DIR) -name '*.flip.png' -delete 2>/dev/null || true

.PHONY: clean examples shaders test test-cpu test-gpu test-dsl docs approve-screenshots test-example-image_filter test-example-particles test-example-scene test-examples


clean:
	rm -rf $(OUT) $(DSL_DIR)/compiled $(DSL_DIR)/*/compiled
