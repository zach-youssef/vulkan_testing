// A triangle covering the screen, from the vertex index alone (no vertex buffer). Its corners,
// (0, 0), (0, 2) and (2, 0) in texture coordinates, are counter-clockwise in framebuffer
// coordinates (y down), so back face culling keeps it.
void main() {
    uv = vec2(gl_VertexIndex & 2, (gl_VertexIndex << 1) & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
