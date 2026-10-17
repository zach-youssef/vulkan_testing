void main() {
    outColor = texture(sampler2D(albedo(), albedoSampler()), uv);
}
