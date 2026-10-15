// The universal descriptor set (see descriptors/DescriptorHeap.h). Include with
//     #include "bindless.glsl"
// and compile with glslc -I <library>/include/glsl.
//
// Materials push the indices of their descriptor entries as push constants, in the order of their
// bindings, as 32-bit unsigned integers:
//     layout(push_constant) uniform Indices { uint ubo; uint texture; uint sampler; } indices;
// and shaders use them to index the arrays below, e.g.
//     texture(sampler2D(bindlessTextures[indices.texture], bindlessSamplers[indices.sampler]), uv)

#ifndef BINDLESS_GLSL
#define BINDLESS_GLSL

#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_shader_image_load_formatted : require

layout(set = 0, binding = 0) uniform texture2D bindlessTextures[];
layout(set = 0, binding = 1) uniform sampler bindlessSamplers[];
// Declared without a format: loads and stores use the image's own format
layout(set = 0, binding = 2) uniform image2D bindlessStorageImages[];

// Declares an array of storage buffers holding `Block`, named `name`, e.g.
//     struct Camera { mat4 view; mat4 projection; };
//     BINDLESS_BUFFER(readonly, CameraBuffer, Camera, cameras);
//     ... cameras[indices.camera].data.view ...
#define BINDLESS_BUFFER(qualifiers, BlockName, Type, name) \
    layout(set = 0, binding = 3) qualifiers buffer BlockName { Type data; } name[]

#endif
