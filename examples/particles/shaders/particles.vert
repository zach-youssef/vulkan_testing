#version 450
#include "bindless.glsl"

// The material's bindings, in order (shared with particles.comp and particles.frag)
layout(push_constant) uniform Indices {
    uint params;
    uint particles;
} indices;

// Matches SimParams in particles.comp
struct SimParams {
    vec2 attractor;
    float dt;
    float time;
    float aspect;
    float attractorStrength;
    uint step;
    uint reset;
    uint particleCount;
    float particleRadius;
};

struct Particle {
    vec2 position;
    vec2 velocity;
    float life;
    float maxLife;
    vec2 padding;
};

BINDLESS_BUFFER(readonly, ParamBuffers, SimParams, paramBuffers);
BINDLESS_BUFFER_ARRAY(readonly, ParticleBuffers, Particle, particleBuffers);

layout(location = 0) out vec2 outCorner;   // [-1, 1] across the quad
layout(location = 1) out vec3 outColor;

// Two triangles per particle, without a vertex or index buffer: vertex 6 * i + c is corner c of
// particle i's quad
const vec2 corners[6] = vec2[](vec2(-1, -1), vec2(1, -1), vec2(1, 1),
                               vec2(1, 1), vec2(-1, 1), vec2(-1, -1));

void main() {
    SimParams params = paramBuffers[indices.params].data;
    Particle p = particleBuffers[indices.particles].data[gl_VertexIndex / 6];
    vec2 corner = corners[gl_VertexIndex % 6];

    vec2 position = p.position + corner * params.particleRadius;
    gl_Position = vec4(position.x / params.aspect, position.y, 0.0, 1.0);
    outCorner = corner;

    // Slow particles are deep blue, fast ones orange; all fade in and out over their lives
    float speed = length(p.velocity);
    vec3 color = mix(vec3(0.05, 0.15, 0.6), vec3(1.0, 0.45, 0.1), clamp(speed * 0.6, 0.0, 1.0));
    float age = 1.0 - p.life / p.maxLife;
    float fade = smoothstep(0.0, 0.1, age) * smoothstep(0.0, 0.3, p.life / p.maxLife);
    outColor = color * fade;
}
