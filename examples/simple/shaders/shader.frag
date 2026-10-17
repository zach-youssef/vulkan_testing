#version 450
#include "bindless.glsl"

// The material's bindings, in order (shared with shader.vert)
layout(push_constant) uniform Indices {
    uint ubo;
    uint texture;
    uint textureSampler;
} indices;

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec2 fragTexCoord0;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(sampler2D(bindlessTextures[indices.texture], bindlessSamplers[indices.textureSampler]), fragTexCoord0);
}
