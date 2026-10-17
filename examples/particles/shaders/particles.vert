// Two triangles per particle, without a vertex or index buffer: vertex 6 * i + c is corner c of
// particle i's quad
const vec2 corners[6] = vec2[](vec2(-1, -1), vec2(1, -1), vec2(1, 1),
                               vec2(1, 1), vec2(-1, 1), vec2(-1, -1));

void main() {
    SimParams params = params();
    Particle p = particles()[gl_VertexIndex / 6];
    corner = corners[gl_VertexIndex % 6];

    vec2 position = p.position + corner * params.particleRadius;
    gl_Position = vec4(position.x / params.aspect, position.y, 0.0, 1.0);

    // Slow particles are deep blue, fast ones orange; all fade in and out over their lives
    float speed = length(p.velocity);
    vec3 tint = mix(vec3(0.05, 0.15, 0.6), vec3(1.0, 0.45, 0.1), clamp(speed * 0.6, 0.0, 1.0));
    float age = 1.0 - p.life / p.maxLife;
    float fade = smoothstep(0.0, 0.1, age) * smoothstep(0.0, 0.3, p.life / p.maxLife);
    color = tint * fade;
}
