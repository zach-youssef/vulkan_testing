#version 450

layout(location = 0) in vec2 inCorner;
layout(location = 1) in vec3 inColor;

layout(location = 0) out vec4 outColor;

void main() {
    // A soft round sprite. Blending is additive, so dense regions glow.
    float falloff = exp(-4.0 * dot(inCorner, inCorner));
    outColor = vec4(inColor * falloff * 0.35, 1.0);
}
