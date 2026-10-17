// Per-frame data shared by the shadow and lit passes, written by LitMaterial::update
// (SceneUniforms in scene_example.cpp)

#ifndef SCENE_GLSL
#define SCENE_GLSL

struct SceneUniforms {
    mat4 viewProjection;        // Camera
    mat4 lightViewProjection;   // World to shadow map clip space
    vec4 cameraPosition;        // xyz
    vec4 lightDirection;        // xyz: unit vector towards the light
    vec4 lightRadiance;         // rgb: linear colour times intensity
    vec4 skyAmbient;            // rgb: ambient light from above
    vec4 groundAmbient;         // rgb: ambient light from below
};

// Every shader declares the buffer with the same qualifiers (see CLAUDE.md)
BINDLESS_BUFFER(readonly, SceneBuffers, SceneUniforms, scenes);

#endif
