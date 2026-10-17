const float PI = 3.14159265359;

// GLSL can't store a combined sampler in a local variable
#define SHADOW_MAP sampler2DShadow(shadowMap(), shadowSampler())

// Fraction of light reaching the point, filtered over 3x3 bilinear comparisons (PCF)
float shadowFactor(vec3 position, vec3 normal, vec3 toLight) {
    const vec2 texelSize = 1.0 / vec2(textureSize(SHADOW_MAP, 0));

    // Offset along the normal, more at grazing angles, so surfaces don't shadow themselves
    const float cosTheta = clamp(dot(normal, toLight), 0.0, 1.0);
    const vec3 offsetPosition = position + normal * (0.02 + 0.04 * (1.0 - cosTheta));
    vec4 clip = scene().lightViewProjection * vec4(offsetPosition, 1.0);
    vec3 ndc = clip.xyz / clip.w;
    // Vulkan NDC y points down the image, matching texture coordinates
    vec2 uv = ndc.xy * 0.5 + 0.5;
    const float reference = ndc.z - 0.001;

    float lit = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            lit += texture(SHADOW_MAP, vec3(uv + vec2(x, y) * texelSize, reference));
        }
    }
    return lit / 9.0;
}

// Cook-Torrance GGX specular for a dielectric (F0 = 0.04) plus Lambert diffuse
vec3 brdf(vec3 albedo, float roughness, vec3 n, vec3 v, vec3 l) {
    const vec3 h = normalize(v + l);
    const float nDotL = max(dot(n, l), 0.0);
    const float nDotV = max(dot(n, v), 1e-4);
    const float nDotH = max(dot(n, h), 0.0);
    const float vDotH = max(dot(v, h), 0.0);

    const float alpha = max(roughness * roughness, 1e-3);
    const float alpha2 = alpha * alpha;
    const float denom = nDotH * nDotH * (alpha2 - 1.0) + 1.0;
    const float distribution = alpha2 / (PI * denom * denom);

    const float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    const float geometry = (nDotV / (nDotV * (1.0 - k) + k)) * (nDotL / (nDotL * (1.0 - k) + k));

    const vec3 fresnel = vec3(0.04) + (1.0 - 0.04) * pow(1.0 - vDotH, 5.0);

    const vec3 specular = distribution * geometry * fresnel / max(4.0 * nDotV * nDotL, 1e-4);
    const vec3 diffuse = (1.0 - fresnel) * albedo / PI;
    return diffuse + specular;
}

void main() {
    SceneUniforms scene = scene();
    const vec3 albedo = albedoRoughness.rgb;
    const float roughness = albedoRoughness.a;

    const vec3 n = normalize(worldNormal);
    const vec3 v = normalize(scene.cameraPosition - worldPosition);
    const vec3 l = scene.lightDirection;

    float shadow = 1.0;
    if (shadowsEnabled() != 0) {
        shadow = shadowFactor(worldPosition, n, l);
    }

    const vec3 direct = brdf(albedo, roughness, n, v, l) * scene.lightRadiance * max(dot(n, l), 0.0) * shadow;
    // Hemisphere ambient: sky from above, light bounced off the ground from below
    const vec3 ambient = mix(scene.groundAmbient, scene.skyAmbient, n.y * 0.5 + 0.5) * albedo;
    outColor = vec4(direct + ambient, 1.0);
}
