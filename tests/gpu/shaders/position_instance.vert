#version 450

// Passes a 2D clip space position through, and the instance index to the fragment shader
layout(location = 0) in vec2 position;
layout(location = 0) flat out uint instance;

void main() {
    gl_Position = vec4(position, 0.0, 1.0);
    instance = gl_InstanceIndex;
}
