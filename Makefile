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

# Generates <out>/<name>.zgal.h and <out>/shaders/<shader> from <dir>/<name>.zgal, and a depfile
# making the stamp depend on the shader bodies:
#   $(call zgal-rule,<dir>/<name>.zgal,<out>)
# Shaders compile from <out>/shaders with -I<dir>/shaders, for files their bodies include.
define zgal-rule
$(2)/$(notdir $(1)).stamp: $(1) $(ZGAL_COMPILED)
	$(ZGAL) --cpp-out $(2) --glsl-out $(2)/shaders --depfile $(2)/$(notdir $(1)).d --dep-target $$@ $$<
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
SIMPLE_SHADER_SRCS = $(wildcard $(SIMPLE_DIR)/shaders/*)
SIMPLE_SHADERS = $(patsubst $(SIMPLE_DIR)/shaders/%,$(SIMPLE_OUT)/shaders/%.spv,$(SIMPLE_SHADER_SRCS))
SIMPLE_CFLAGS = $(CFLAGS) \
                -DSIMPLE_EXAMPLE_SHADER_DIR='"$(abspath $(SIMPLE_OUT)/shaders)"' \
                -DSIMPLE_EXAMPLE_TEXTURE_DIR='"$(abspath $(SIMPLE_DIR)/textures)"'

# SPIR-V is written to <out>/shaders/<name>.<stage>.spv
$(SIMPLE_OUT)/shaders/%.spv : $(SIMPLE_DIR)/shaders/% $(GLSL_INCLUDES)
	@mkdir -p $(dir $@)
	glslc $(GLSLC_FLAGS) $< -o $@

$(SIMPLE_OUT)/simple_example.o : $(SIMPLE_DIR)/simple_example.cpp $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) -c -o $@ $< $(SIMPLE_CFLAGS)

$(SIMPLE_OUT)/simple_example: $(SIMPLE_OUT)/simple_example.o | $(SIMPLE_SHADERS)
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
IMAGE_FILTER_SHADER_SRCS = $(wildcard $(IMAGE_FILTER_DIR)/shaders/*.comp)
IMAGE_FILTER_SHADER_INCLUDES = $(wildcard $(IMAGE_FILTER_DIR)/shaders/*.glsl)
IMAGE_FILTER_SHADERS = $(patsubst $(IMAGE_FILTER_DIR)/shaders/%,$(IMAGE_FILTER_OUT)/shaders/%.spv,$(IMAGE_FILTER_SHADER_SRCS))
IMAGE_FILTER_CFLAGS = $(CFLAGS) -DIMAGE_FILTER_SHADER_DIR='"$(abspath $(IMAGE_FILTER_OUT)/shaders)"'
IMAGE_FILTER_TEST_DIR = $(IMAGE_FILTER_DIR)/tests
IMAGE_FILTER_TEST_OUT = $(IMAGE_FILTER_OUT)/tests
IMAGE_FILTER_DIFF = $(IMAGE_FILTER_TEST_OUT)/image_diff

$(IMAGE_FILTER_OUT)/shaders/%.spv : $(IMAGE_FILTER_DIR)/shaders/% $(IMAGE_FILTER_SHADER_INCLUDES) $(GLSL_INCLUDES)
	@mkdir -p $(dir $@)
	glslc $(GLSLC_FLAGS) $< -o $@

$(IMAGE_FILTER_OUT)/image_filter.o : $(IMAGE_FILTER_DIR)/image_filter.cpp $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) -c -o $@ $< $(IMAGE_FILTER_CFLAGS)

$(IMAGE_FILTER_BIN): $(IMAGE_FILTER_OUT)/image_filter.o | $(IMAGE_FILTER_SHADERS)
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
PARTICLES_SHADER_SRCS = $(wildcard $(PARTICLES_DIR)/shaders/*)
PARTICLES_SHADERS = $(patsubst $(PARTICLES_DIR)/shaders/%,$(PARTICLES_OUT)/shaders/%.spv,$(PARTICLES_SHADER_SRCS))
PARTICLES_CFLAGS = $(CFLAGS) -DPARTICLES_SHADER_DIR='"$(abspath $(PARTICLES_OUT)/shaders)"'

$(PARTICLES_OUT)/shaders/%.spv : $(PARTICLES_DIR)/shaders/% $(GLSL_INCLUDES)
	@mkdir -p $(dir $@)
	glslc $(GLSLC_FLAGS) $< -o $@

$(PARTICLES_OUT)/particles.o : $(PARTICLES_DIR)/particles.cpp $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) -c -o $@ $< $(PARTICLES_CFLAGS)

$(PARTICLES_OUT)/particles: $(PARTICLES_OUT)/particles.o | $(PARTICLES_SHADERS)
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
# The shaders' interfaces are in scene.zgal: the generated shaders are its bodies (shaders/) with
# the interface prepended
SCENE_GEN = $(SCENE_OUT)/generated
SCENE_ZGAL_STAMP = $(SCENE_GEN)/scene.zgal.stamp
SCENE_SHADER_SRCS = $(wildcard $(SCENE_DIR)/shaders/*.vert $(SCENE_DIR)/shaders/*.frag)
SCENE_SHADERS = $(patsubst $(SCENE_DIR)/shaders/%,$(SCENE_OUT)/shaders/%.spv,$(SCENE_SHADER_SRCS))
SCENE_CFLAGS = $(CFLAGS) -I$(SCENE_GEN) -DSCENE_EXAMPLE_SHADER_DIR='"$(abspath $(SCENE_OUT)/shaders)"'

$(eval $(call zgal-rule,$(SCENE_DIR)/scene.zgal,$(SCENE_GEN)))

$(SCENE_SHADERS): $(SCENE_OUT)/shaders/%.spv : $(SCENE_ZGAL_STAMP) $(GLSL_INCLUDES)
	@mkdir -p $(dir $@)
	glslc $(GLSLC_FLAGS) -I$(SCENE_DIR)/shaders $(SCENE_GEN)/shaders/$* -o $@

$(SCENE_OUT)/scene_example.o : $(SCENE_DIR)/scene_example.cpp $(wildcard $(SCENE_DIR)/*.h) $(SCENE_ZGAL_STAMP) $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) -c -o $@ $< $(SCENE_CFLAGS)

$(SCENE_OUT)/scene_example: $(SCENE_OUT)/scene_example.o | $(SCENE_SHADERS)
	$(CC) $^ -o $@ $(LDFLAGS) $(LDLIBS) $(GLFW_LDLIBS)
	install_name_tool -add_rpath /usr/local/lib ./$@

# Renders frames headless, toggling shadows midway, and captures the last one
test-example-scene: $(SCENE_OUT)/scene_example
	./$< --frames 60 --size 640x360 --capture $(SCENE_OUT)/smoke.png

# All the examples' smoke tests
test-examples: test-example-image_filter test-example-particles test-example-scene

examples: $(SIMPLE_OUT)/simple_example $(IMAGE_FILTER_BIN) $(PARTICLES_OUT)/particles $(SCENE_OUT)/scene_example

shaders: $(SIMPLE_SHADERS) $(IMAGE_FILTER_SHADERS) $(PARTICLES_SHADERS) $(SCENE_SHADERS)

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
              -DTEST_TEXTURE_DIR='"$(abspath $(SIMPLE_DIR)/textures)"'
TEST_HEADERS = $(HEADERS) $(wildcard $(TEST_DIR)/*/*.h)

CPU_TEST_SRCS = $(wildcard $(TEST_DIR)/cpu/*.cpp)
CPU_TEST_OBJS = $(patsubst $(TEST_DIR)/%.cpp,$(TEST_OUT)/%.o,$(CPU_TEST_SRCS))

GPU_TEST_SRCS = $(wildcard $(TEST_DIR)/gpu/*.cpp)
GPU_TEST_OBJS = $(patsubst $(TEST_DIR)/%.cpp,$(TEST_OUT)/%.o,$(GPU_TEST_SRCS))

# SPIR-V used by the GPU tests: test-only shaders plus the simple example's shaders,
# compiled to $(TEST_SHADER_OUT)/<name>.<stage>.spv
GPU_TEST_SHADER_SRCS = $(wildcard $(TEST_DIR)/gpu/shaders/*) $(SIMPLE_SHADER_SRCS)
GPU_TEST_SHADERS = $(patsubst %,$(TEST_SHADER_OUT)/%.spv,$(notdir $(GPU_TEST_SHADER_SRCS)))
TEST_ZGAL_SHADERS = $(patsubst %,$(TEST_SHADER_OUT)/%.spv,$(notdir $(wildcard $(TEST_ZGAL_DIR)/shaders/*)))
GPU_TEST_SHADERS += $(TEST_ZGAL_SHADERS)

$(eval $(call zgal-rule,$(TEST_ZGAL_DIR)/interfaces.zgal,$(TEST_GEN)))

$(TEST_ZGAL_SHADERS): $(TEST_SHADER_OUT)/%.spv : $(TEST_ZGAL_STAMP) $(GLSL_INCLUDES)
	@mkdir -p $(dir $@)
	glslc $(GLSLC_FLAGS) -I$(TEST_ZGAL_DIR)/shaders $(TEST_GEN)/shaders/$* -o $@

$(TEST_OUT)/gpu/ZgalTest.o: $(TEST_ZGAL_STAMP)

$(TEST_SHADER_OUT)/%.spv : $(TEST_DIR)/gpu/shaders/% $(GLSL_INCLUDES)
	@mkdir -p $(dir $@)
	glslc $(GLSLC_FLAGS) $< -o $@
$(TEST_SHADER_OUT)/%.spv : $(SIMPLE_DIR)/shaders/% $(GLSL_INCLUDES)
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
