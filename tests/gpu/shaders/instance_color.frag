#version 450

// Red for instance 0, green otherwise
layout(location = 0) flat in uint instance;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = instance == 0 ? vec4(1, 0, 0, 1) : vec4(0, 1, 0, 1);
}
