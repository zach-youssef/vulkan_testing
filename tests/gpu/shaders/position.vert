#version 450

// Passes a 2D clip space position through (vertex input at location 0)
layout(location = 0) in vec2 position;

void main() {
    gl_Position = vec4(position, 0.0, 1.0);
}
