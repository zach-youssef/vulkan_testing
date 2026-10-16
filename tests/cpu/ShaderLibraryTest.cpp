#include "shaders/ShaderLibrary.h"

#include <doctest/doctest.h>

TEST_CASE("Shader stages come from the shader's name") {
    CHECK(shaderStageFromName("shader.vert") == VK_SHADER_STAGE_VERTEX_BIT);
    CHECK(shaderStageFromName("dir/shader.frag") == VK_SHADER_STAGE_FRAGMENT_BIT);
    CHECK(shaderStageFromName("copy.comp") == VK_SHADER_STAGE_COMPUTE_BIT);
    CHECK(shaderStageFromName("a.geom") == VK_SHADER_STAGE_GEOMETRY_BIT);
    CHECK_FALSE(shaderStageFromName("shader.glsl"));
    CHECK_FALSE(shaderStageFromName("shader"));
}

TEST_CASE("checkSpirv rejects data that isn't a SPIR-V module") {
    std::vector<char> valid(20, 0);
    const uint32_t magic = 0x07230203;
    memcpy(valid.data(), &magic, sizeof(magic));
    CHECK(checkSpirv(valid).empty());

    CHECK(checkSpirv({}).find("not SPIR-V") != std::string::npos);
    auto oddSize = valid;
    oddSize.push_back(0);
    CHECK_FALSE(checkSpirv(oddSize).empty());
    auto wrongMagic = valid;
    wrongMagic[0] = 0;
    CHECK(checkSpirv(wrongMagic).find("magic") != std::string::npos);
}
