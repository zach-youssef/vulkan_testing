#version 450
#include "bindless.glsl"
#include "scene.glsl"

// ShadowMaterial's bindings
layout(push_constant) uniform Indices {
    uint scene;
} indices;

layout(location = 0) in vec3 inPosition;

void main() {
    gl_Position = scenes[indices.scene].data.lightViewProjection * vec4(inPosition, 1.0);
}
