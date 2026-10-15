#include "shaders/SpirvReflection.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#ifndef TEST_SHADER_DIR
#error "TEST_SHADER_DIR must be defined to the directory containing the compiled test shaders"
#endif

namespace {

// A compiled GPU test shader (the CPU tests depend on them being built)
std::vector<char> testSpirv(const std::string& name) {
    const auto path = std::filesystem::path(TEST_SHADER_DIR) / (name + ".spv");
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    REQUIRE_MESSAGE(file.is_open(), "Missing " << path);
    std::vector<char> code(static_cast<size_t>(file.tellg()));
    file.seekg(0);
    file.read(code.data(), static_cast<std::streamsize>(code.size()));
    return code;
}

// The universal descriptor set's bindings (bindless.glsl)
constexpr uint32_t kStorageImages = 2;
constexpr uint32_t kStorageBuffers = 3;

} // namespace

TEST_CASE("Reflection finds the descriptor bindings a shader writes through") {
    SUBCASE("An image store") {
        CHECK(reflectDescriptorWrites(testSpirv("pattern.comp")).bindings == std::set<uint32_t>{kStorageImages});
    }
    SUBCASE("Buffer stores") {
        CHECK(reflectDescriptorWrites(testSpirv("fill_buffer.comp")).bindings == std::set<uint32_t>{kStorageBuffers});
        CHECK(reflectDescriptorWrites(testSpirv("small_types.comp")).bindings == std::set<uint32_t>{kStorageBuffers});
    }
    SUBCASE("Reads, and writes to a private variable, aren't writes") {
        CHECK(reflectDescriptorWrites(testSpirv("reflect_reads.comp")).bindings.empty());
    }
    SUBCASE("Through a function parameter, and with an atomic") {
        CHECK(reflectDescriptorWrites(testSpirv("reflect_indirect_writes.comp")).bindings
              == std::set<uint32_t>{kStorageImages, kStorageBuffers});
    }
    SUBCASE("Graphics shaders that write nothing") {
        CHECK(reflectDescriptorWrites(testSpirv("shader.frag")).bindings.empty());
        CHECK(reflectDescriptorWrites(testSpirv("shader.vert")).bindings.empty());
    }
}

TEST_CASE("Reflection rejects data that isn't SPIR-V") {
    CHECK_THROWS_AS(reflectDescriptorWrites(std::vector<char>(20, 0)), std::invalid_argument);
    CHECK_THROWS_AS(reflectDescriptorWrites(std::vector<char>(3, 0)), std::invalid_argument);
    // A header followed by an instruction claiming more words than there are
    std::vector<uint32_t> truncated{0x07230203, 0x00010600, 0, 10, 0, (5u << 16) | 71u, 1};
    CHECK_THROWS_AS(reflectDescriptorWrites(truncated), std::invalid_argument);
}
