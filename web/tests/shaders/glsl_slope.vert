// A triangle covering the target, its depth rising along x and, twice as fast, y
void main() {
    vec2 at = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(at * 2.0 - 1.0, 0.25 * (at.x + 2.0 * at.y), 1.0);
}
