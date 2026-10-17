#version 450
#include "bindless.glsl"
#include "scene.glsl"

// LitMaterial's bindings, then its own push constants (shared with lit.frag)
layout(push_constant) uniform Indices {
    uint scene;
    uint shadowMap;
    uint shadowSampler;
    uint shadowsEnabled;
} indices;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inAlbedoRoughness;

layout(location = 0) out vec3 worldPosition;
layout(location = 1) out vec3 worldNormal;
layout(location = 2) out vec4 albedoRoughness;

void main() {
    SceneUniforms scene = scenes[indices.scene].data;
    gl_Position = scene.viewProjection * vec4(inPosition, 1.0);
    worldPosition = inPosition;
    worldNormal = inNormal;
    albedoRoughness = inAlbedoRoughness;
}
