#version 450
#include "bindless.glsl"

// Compares references against a depth image through a comparison sampler: red is the result for
// reference 0.25 and green for 0.75, at the centre of the image
layout(push_constant) uniform Indices { uint depth; uint compareSampler; } indices;

layout(location = 0) out vec4 outColor;

void main() {
    float low = texture(sampler2DShadow(bindlessTextures[indices.depth], bindlessSamplers[indices.compareSampler]),
                        vec3(0.5, 0.5, 0.25));
    float high = texture(sampler2DShadow(bindlessTextures[indices.depth], bindlessSamplers[indices.compareSampler]),
                         vec3(0.5, 0.5, 0.75));
    outColor = vec4(low, high, 0.0, 1.0);
}
