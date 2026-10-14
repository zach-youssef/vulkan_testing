# Compiler
CC = /usr/bin/g++
CXX = $(CC)

# Include paths
CFLAGS = -std=c++20 -Wc++11-extensions -Wc++14-extensions -I/usr/local/include -Ithird_party/stb -I/Users/zyoussef/VulkanSDK/1.4.321.0/macOS/include/
CXXFLAGS = $(CFLAGS)

# Library paths
LDFLAGS = -L/opt/homebrew/lib -L/usr/local/lib

LDLIBS = -lglfw -lvulkan

OUT = build
SRC = vulkan_test
SHADER_DIR = $(SRC)/shaders

HEADERS = $(wildcard $(SRC)/*.h)

# Rule for building Cpp objects
$(OUT)/%.o : $(SRC)/%.cpp $(HEADERS) | $(OUT)
	$(CC) -c -o $@ $< $(CFLAGS)

$(OUT):
	mkdir -p $@

# Rules for building shader spirv (file names match those loaded in main.cpp)
$(SHADER_DIR)/vert.spv : $(SHADER_DIR)/shader.vert
	glslc $^ -o $@
$(SHADER_DIR)/frag.spv : $(SHADER_DIR)/shader.frag
	glslc $^ -o $@
$(SHADER_DIR)/compTest.spv : $(SHADER_DIR)/test.comp
	glslc $^ -o $@

SHADERS = $(SHADER_DIR)/vert.spv $(SHADER_DIR)/frag.spv $(SHADER_DIR)/compTest.spv

_OBJS = main.o
OBJS = $(patsubst %,$(OUT)/%,$(_OBJS))

$(OUT)/vulkan_test: $(OBJS) | $(SHADERS)
	$(CC) $^ -o $@ $(LDFLAGS) $(LDLIBS)
	install_name_tool -add_rpath /usr/local/lib ./$@

.DEFAULT_GOAL := $(OUT)/vulkan_test

# ---------------------------------------------------------------------------
# Tests
#   make test-cpu   - unit tests that do not need a Vulkan device
#   make test-gpu   - tests that run headless on a Vulkan device (no window)
#   make test       - all tests
# Arguments can be forwarded to doctest, e.g. make test-cpu TEST_ARGS='-tc="Handle*"'
# ---------------------------------------------------------------------------
TEST_DIR = tests
TEST_OUT = $(OUT)/tests
TEST_SHADER_OUT = $(TEST_OUT)/shaders
TEST_CFLAGS = $(CFLAGS) -I$(SRC) -Ithird_party -I$(TEST_DIR) -DTEST_SHADER_DIR='"$(abspath $(TEST_SHADER_OUT))"'
TEST_HEADERS = $(HEADERS) $(wildcard $(TEST_DIR)/*/*.h)

CPU_TEST_SRCS = $(wildcard $(TEST_DIR)/cpu/*.cpp)
CPU_TEST_OBJS = $(patsubst $(TEST_DIR)/%.cpp,$(TEST_OUT)/%.o,$(CPU_TEST_SRCS))

GPU_TEST_SRCS = $(wildcard $(TEST_DIR)/gpu/*.cpp)
GPU_TEST_OBJS = $(patsubst $(TEST_DIR)/%.cpp,$(TEST_OUT)/%.o,$(GPU_TEST_SRCS))

# SPIR-V used by the GPU tests: test-only shaders plus the example's shaders,
# compiled to $(TEST_SHADER_OUT)/<name>.<stage>.spv
GPU_TEST_SHADER_SRCS = $(wildcard $(TEST_DIR)/gpu/shaders/*) $(wildcard $(SHADER_DIR)/*.vert $(SHADER_DIR)/*.frag $(SHADER_DIR)/*.comp)
GPU_TEST_SHADERS = $(patsubst %,$(TEST_SHADER_OUT)/%.spv,$(notdir $(GPU_TEST_SHADER_SRCS)))

$(TEST_SHADER_OUT)/%.spv : $(TEST_DIR)/gpu/shaders/%
	@mkdir -p $(dir $@)
	glslc $< -o $@
$(TEST_SHADER_OUT)/%.spv : $(SHADER_DIR)/%
	@mkdir -p $(dir $@)
	glslc $< -o $@

$(TEST_OUT)/%.o : $(TEST_DIR)/%.cpp $(TEST_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) -c -o $@ $< $(TEST_CFLAGS)

$(TEST_OUT)/cpu_tests: $(CPU_TEST_OBJS)
	$(CC) $^ -o $@ $(LDFLAGS) $(LDLIBS)
	install_name_tool -add_rpath /usr/local/lib ./$@

$(TEST_OUT)/gpu_tests: $(GPU_TEST_OBJS) | $(GPU_TEST_SHADERS)
	$(CC) $^ -o $@ $(LDFLAGS) $(LDLIBS)
	install_name_tool -add_rpath /usr/local/lib ./$@

test-cpu: $(TEST_OUT)/cpu_tests
	./$< $(TEST_ARGS)

test-gpu: $(TEST_OUT)/gpu_tests
	./$< $(TEST_ARGS)

test: test-cpu test-gpu

.PHONY: clean shaders test test-cpu test-gpu

shaders: $(SHADERS)

clean:
	rm -rf $(OUT)
