#version 450

layout(binding = 0) uniform UniformBufferObject {
    mat4 mvp;
} ubo;

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec2 inTexCoord0;

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec2 fragTexCoord0;

void main() {
    gl_Position = ubo.mvp * vec4(inPosition, 0.0, 1.0);
    fragColor = inColor;
    fragTexCoord0 = inTexCoord0;
}
