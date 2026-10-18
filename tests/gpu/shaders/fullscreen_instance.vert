#version 450

// A triangle covering the viewport, from gl_VertexIndex alone (no vertex input), passing the
// instance index to the fragment shader
layout(location = 0) flat out uint instance;

void main() {
    vec2 positions[3] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    instance = gl_InstanceIndex;
}
