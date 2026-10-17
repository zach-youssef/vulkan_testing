#pragma once

#include <string>

#include "core/VulkanContext.h"
#include "shaders/ShaderLibrary.h"

#ifndef TEST_SHADER_DIR
#error "TEST_SHADER_DIR must be defined to the directory containing the compiled test shaders"
#endif

// The search path for the SPIR-V the Makefile compiles for the tests
inline std::string testShaderDir() {
    return TEST_SHADER_DIR;
}

// A test shader, e.g. testShader(ctx, "pattern.comp"). The context's shader library must search
// testShaderDir() (the shared GPU test context's does). A shader that fails to load fails the test.
inline Handle<Shader> testShader(VulkanContext& ctx, const std::string& name) {
    return ctx.shaders().get(name).value();
}
