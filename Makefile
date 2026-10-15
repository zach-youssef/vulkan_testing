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

examples: $(SIMPLE_OUT)/simple_example

shaders: $(SIMPLE_SHADERS)

.DEFAULT_GOAL := examples

# ---------------------------------------------------------------------------
# Tests
#   make test-cpu   - unit tests that do not need a Vulkan device
#   make test-gpu   - tests that run headless on a Vulkan device (no window)
#   make test       - all tests
#   make approve-screenshots - accept the screenshots from the last failed GPU test run
# Arguments can be forwarded to doctest, e.g. make test-cpu TEST_ARGS='-tc="Handle*"'
# ---------------------------------------------------------------------------
TEST_DIR = tests
TEST_OUT = $(OUT)/tests
TEST_SHADER_OUT = $(TEST_OUT)/shaders
TEST_CFLAGS = $(CFLAGS) -Ithird_party -Ithird_party/approvaltests -Ithird_party/flip -I$(TEST_DIR) \
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

$(TEST_OUT)/cpu_tests: $(CPU_TEST_OBJS)
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

test: test-cpu test-gpu

SCREENSHOT_DIR = $(TEST_DIR)/gpu/approved

approve-screenshots:
	@find $(SCREENSHOT_DIR) -name '*.received.png' 2>/dev/null | while read -r received; do \
		approved="$${received%.received.png}.approved.png"; \
		mv "$$received" "$$approved" && echo "Approved $$approved"; \
	done
	@find $(SCREENSHOT_DIR) -name '*.flip.png' -delete 2>/dev/null || true

.PHONY: clean examples shaders test test-cpu test-gpu approve-screenshots


clean:
	rm -rf $(OUT)
