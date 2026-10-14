# Compiler
CC = /usr/bin/g++
CXX = $(CC)

# Include paths
CFLAGS = -std=c++20 -Wc++11-extensions -Wc++14-extensions -I/usr/local/include -I/Users/zyoussef/stb/ -I/Users/zyoussef/VulkanSDK/1.4.321.0/macOS/include/
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

.PHONY: clean shaders

shaders: $(SHADERS)

clean:
	rm -rf $(OUT)
