#version 450
#include "bindless.glsl"

// The material's bindings, in order (shared with shader.frag)
layout(push_constant) uniform Indices {
    uint ubo;
    uint texture;
    uint textureSampler;
} indices;

struct UniformBufferObject {
    mat4 mvp;
};
BINDLESS_BUFFER(readonly, UniformBuffers, UniformBufferObject, ubos);

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec2 inTexCoord0;

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec2 fragTexCoord0;

void main() {
    gl_Position = ubos[indices.ubo].data.mvp * vec4(inPosition, 0.0, 1.0);
    fragColor = inColor;
    fragTexCoord0 = inTexCoord0;
}
