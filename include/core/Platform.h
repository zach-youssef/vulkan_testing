#pragma once

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

// The directory holding the running executable, or an empty path where it can't be found (an
// unsupported platform, or the executable was deleted). Symbolic links are resolved, so a link to
// the executable finds the directory of the file it points to.
inline std::filesystem::path executableDirectory() {
    std::filesystem::path executable;
#if defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return {};
    }
    executable = buffer.c_str();
#elif defined(__linux__)
    executable = "/proc/self/exe";
#else
    return {};
#endif
    std::error_code error;
    const auto resolved = std::filesystem::canonical(executable, error);
    if (error) {
        return {};
    }
    return resolved.parent_path();
}

// Where the shader library looks for SPIR-V unless told otherwise: <executable directory>/shaders,
// beside the program (as the Makefile builds the examples and tests)
inline std::vector<std::string> defaultShaderSearchPaths() {
    const auto directory = executableDirectory();
    if (directory.empty()) {
        return {};
    }
    return {(directory / "shaders").string()};
}

// Where assets loaded by relative path (VulkanContext::findAsset) are found unless told otherwise:
// <executable directory>/assets, beside the program (where the Makefile copies a program's assets)
inline std::vector<std::string> defaultAssetSearchPaths() {
    const auto directory = executableDirectory();
    if (directory.empty()) {
        return {};
    }
    return {(directory / "assets").string()};
}
