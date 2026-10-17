#version 450

// Outputs the colour pushed as the material's push constants (it has no bindings)
layout(push_constant) uniform Constants { vec4 color; } constants;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = constants.color;
}
