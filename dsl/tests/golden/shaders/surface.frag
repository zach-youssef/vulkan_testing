void main() {
    vec3 lit = vec3(0.0);
    for (uint i = 0; i < lights().count; ++i) {
        lit += lights().lights[i].color.rgb * lights().lights[i].intensity;
    }
    vec4 base = texture(sampler2D(albedo(), albedoSampler()), uv);
    float masked = imageLoad(mask(), ivec2(screen * 16.0)).r;
    color = vec4(base.rgb * lit * tint() * masked, base.a);
    normalOut = vec4(float(material));
}
