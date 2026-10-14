#pragma once

#include <string>
#include <vector>

#include "FileUtil.h"

#ifndef TEST_SHADER_DIR
#error "TEST_SHADER_DIR must be defined to the directory containing the compiled test shaders"
#endif

// Loads SPIR-V compiled by the Makefile, e.g. loadTestShader("pattern.comp")
inline std::vector<char> loadTestShader(const std::string& name) {
    return readFile(std::string(TEST_SHADER_DIR) + "/" + name + ".spv");
}
