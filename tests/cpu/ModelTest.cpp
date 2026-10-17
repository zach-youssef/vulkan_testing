// Loading models from glTF and OBJ files (tests/gpu/assets), without a device
#include <doctest/doctest.h>

#include "common/ResultChecks.h"
#include "resources/Model.h"

namespace {

std::filesystem::path asset(const char* name) {
    return std::filesystem::path(TEST_ASSET_DIR) / name;
}

} // namespace

TEST_CASE("glTF models bake their nodes' transforms and keep their base colour materials") {
    const ModelData model = ModelData::load(asset("halves.gltf")).value();
    REQUIRE(model.primitives.size() == 2);
    // The file's two materials, then the default one
    REQUIRE(model.materials.size() == 3);
    CHECK(model.materials[0].name == "quadrants");
    REQUIRE(model.materials[0].baseColorTexture);
    CHECK(model.materials[0].baseColorTexture->path == asset("quadrants.png"));
    CHECK(model.materials[1].baseColorFactor == glm::vec4(0.0f, 0.5f, 0.0f, 1.0f));
    CHECK_FALSE(model.materials[1].baseColorTexture);
    CHECK(model.materials[2].baseColorFactor == glm::vec4(1.0f));

    const ModelPrimitive& left = model.primitives[0];
    CHECK(left.material == 0);
    CHECK(left.indices == std::vector<uint32_t>{0, 1, 2, 2, 3, 0});
    // The unit quad, scaled by (1, 2, 1) and moved by (-1, -1, 0)
    CHECK(left.positions[2] == glm::vec3(0.0f, 1.0f, 0.0f));
    CHECK(left.uvs[2] == glm::vec2(1.0f, 1.0f));
    CHECK(left.normals[0] == glm::vec3(0.0f, 0.0f, 1.0f));
    CHECK(left.colors.empty());
    CHECK(model.primitives[1].material == 1);
    CHECK(model.primitives[1].positions[0] == glm::vec3(0.0f, -1.0f, 0.0f));
}

TEST_CASE("OBJ models split faces by material, flip texture coordinates and read their .mtl") {
    const ModelData model = ModelData::load(asset("quad.obj")).value();
    REQUIRE(model.materials.size() == 2);
    CHECK(model.materials[0].name == "tinted");
    CHECK(model.materials[0].baseColorFactor == glm::vec4(0.5f, 0.25f, 1.0f, 1.0f));
    REQUIRE(model.materials[0].baseColorTexture);
    CHECK(model.materials[0].baseColorTexture->path == asset("quadrants.png"));

    REQUIRE(model.primitives.size() == 2);
    // The triangle before any usemtl has the default material
    const auto withMaterial = [&](uint32_t material) {
        return *std::find_if(model.primitives.begin(), model.primitives.end(),
                             [&](const ModelPrimitive& p) { return p.material == material; });
    };
    CHECK(withMaterial(1).indices.size() == 3);
    const ModelPrimitive quad = withMaterial(0);
    CHECK(quad.indices.size() == 6);
    CHECK(quad.positions.size() == 4);   // Shared corners are one vertex
    CHECK(quad.uvs[0] == glm::vec2(0.0f, 1.0f));
    CHECK(quad.normals.size() == 4);
}

TEST_CASE("Models that can't be loaded say why") {
    CHECK_ERROR(ModelData::load(asset("missing.gltf")), ErrorCode::NotFound, "missing.gltf' not found");
    CHECK_ERROR(ModelData::load(asset("quadrants.png")), ErrorCode::InvalidData, "isn't a .gltf, .glb or .obj file");
    CHECK_ERROR(ModelData::load(asset("broken.gltf")), ErrorCode::InvalidData, "Failed to parse glTF file");
}
