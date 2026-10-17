#version 450
#include "bindless.glsl"

// TonemapMaterial's bindings, then its own push constants
layout(push_constant) uniform Indices {
    uint hdr;
    uint hdrSampler;
    float exposure;
} indices;

layout(location = 0) in vec2 uv;

layout(location = 0) out vec4 outColor;

// Krzysztof Narkowicz's fit of the ACES filmic curve
vec3 acesFilmic(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

void main() {
    vec3 hdr = texture(sampler2D(bindlessTextures[indices.hdr], bindlessSamplers[indices.hdrSampler]), uv).rgb;
    // Linear output: the sRGB target encodes it
    outColor = vec4(acesFilmic(hdr * indices.exposure), 1.0);
}
