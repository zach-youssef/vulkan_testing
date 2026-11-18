// A triangle covering the target, uv (0, 0) at its first row's first pixel (Vulkan's convention,
// which the web keeps: naga flips y)
void main() {
    uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
