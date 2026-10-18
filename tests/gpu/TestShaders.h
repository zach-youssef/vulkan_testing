#pragma once

#include <string>

#include "core/Platform.h"
#include "core/VulkanContext.h"
#include "shaders/ShaderLibrary.h"

// Where the Makefile compiles the tests' SPIR-V: beside the test executable, so contexts find it by
// default (defaultShaderSearchPaths)
inline std::string testShaderDir() {
    return (executableDirectory() / "shaders").string();
}

// A test shader, e.g. testShader(ctx, "pattern.comp"). The context's shader library must search
// testShaderDir(), as contexts do by default. A shader that fails to load fails the test.
inline Handle<Shader> testShader(VulkanContext& ctx, const std::string& name) {
    return ctx.shaders().get(name).value();
}
