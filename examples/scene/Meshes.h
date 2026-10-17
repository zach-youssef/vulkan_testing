#pragma once

// SceneVertex, generated from scene.zgal
#include "scene.zgal.h"

#include <vulkan/vulkan.h>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cassert>
#include <vector>

/*
 Procedural meshes for the scene example. Shapes are built in world space (their transforms are
 baked into the vertices), since nothing in the scene moves but the light and the camera. Each
 vertex carries its surface's material: a linear albedo and a roughness.

 The vertex format, SceneVertex, is declared in scene.zgal.

 Front faces are counter-clockwise seen from outside, as in OpenGL; the example's projections flip Y
 for Vulkan, which keeps them counter-clockwise on screen.
 */

struct SurfaceMaterial {
    glm::vec3 albedo;
    float roughness;
};

struct MeshData {
    std::vector<SceneVertex> vertices;
    std::vector<uint16_t> indices;   // MeshRenderable draws 16-bit indices
};

namespace meshes {

// Adds a vertex transformed by `transform`. Normals use the inverse transpose so non-uniform scales
// keep them perpendicular to the surface.
inline uint16_t addVertex(MeshData& mesh, const glm::mat4& transform, glm::vec3 position, glm::vec3 normal,
                          const SurfaceMaterial& material) {
    // The scene's meshes are small enough for 16-bit indices
    assert(mesh.vertices.size() < 0xFFFF && "Mesh exceeds 16-bit indices");
    const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(transform)));
    mesh.vertices.push_back({glm::vec3(transform * glm::vec4(position, 1.0f)),
                             glm::normalize(normalMatrix * normal),
                             glm::vec4(material.albedo, material.roughness)});
    return static_cast<uint16_t>(mesh.vertices.size() - 1);
}

// A quad from corner, spanning edges u and v (counter-clockwise from u to v seen from the front)
inline void addQuad(MeshData& mesh, const glm::mat4& transform, glm::vec3 corner, glm::vec3 u, glm::vec3 v,
                    const SurfaceMaterial& material) {
    const glm::vec3 normal = glm::normalize(glm::cross(u, v));
    const uint16_t a = addVertex(mesh, transform, corner, normal, material);
    const uint16_t b = addVertex(mesh, transform, corner + u, normal, material);
    const uint16_t c = addVertex(mesh, transform, corner + u + v, normal, material);
    const uint16_t d = addVertex(mesh, transform, corner + v, normal, material);
    mesh.indices.insert(mesh.indices.end(), {a, b, c, c, d, a});
}

// A square in the XZ plane facing +Y, centred on the origin
inline MeshData plane(float size, const SurfaceMaterial& material, const glm::mat4& transform = glm::mat4(1.0f)) {
    MeshData mesh;
    const float h = size * 0.5f;
    addQuad(mesh, transform, {-h, 0.0f, h}, {size, 0.0f, 0.0f}, {0.0f, 0.0f, -size}, material);
    return mesh;
}

// A unit cube centred on the origin, then transformed (scale it for boxes)
inline MeshData cube(const SurfaceMaterial& material, const glm::mat4& transform) {
    MeshData mesh;
    const glm::vec3 x{1, 0, 0}, y{0, 1, 0}, z{0, 0, 1};
    // Each face: a corner, then two edges whose cross product points out of the cube
    addQuad(mesh, transform, {-0.5f, -0.5f, 0.5f}, x, y, material);    // +Z
    addQuad(mesh, transform, {0.5f, -0.5f, -0.5f}, -x, y, material);   // -Z
    addQuad(mesh, transform, {0.5f, -0.5f, 0.5f}, -z, y, material);    // +X
    addQuad(mesh, transform, {-0.5f, -0.5f, -0.5f}, z, y, material);   // -X
    addQuad(mesh, transform, {-0.5f, 0.5f, 0.5f}, x, -z, material);    // +Y
    addQuad(mesh, transform, {-0.5f, -0.5f, -0.5f}, x, z, material);   // -Y
    return mesh;
}

// A UV sphere
inline MeshData sphere(glm::vec3 centre, float radius, const SurfaceMaterial& material,
                       uint32_t segments = 48, uint32_t rings = 24) {
    MeshData mesh;
    const glm::mat4 identity(1.0f);
    for (uint32_t ring = 0; ring <= rings; ++ring) {
        const float polar = glm::pi<float>() * static_cast<float>(ring) / static_cast<float>(rings);
        for (uint32_t segment = 0; segment <= segments; ++segment) {
            const float azimuth = glm::two_pi<float>() * static_cast<float>(segment) / static_cast<float>(segments);
            const glm::vec3 normal{std::sin(polar) * std::cos(azimuth), std::cos(polar), -std::sin(polar) * std::sin(azimuth)};
            addVertex(mesh, identity, centre + radius * normal, normal, material);
        }
    }
    // Rings run from the top (+Y) down, and segments counter-clockwise seen from above
    const uint32_t stride = segments + 1;
    for (uint32_t ring = 0; ring < rings; ++ring) {
        for (uint32_t segment = 0; segment < segments; ++segment) {
            const auto top = static_cast<uint16_t>(ring * stride + segment);
            const auto bottom = static_cast<uint16_t>(top + stride);
            mesh.indices.insert(mesh.indices.end(), {top, bottom, static_cast<uint16_t>(bottom + 1),
                                                     static_cast<uint16_t>(bottom + 1), static_cast<uint16_t>(top + 1), top});
        }
    }
    return mesh;
}

} // namespace meshes
