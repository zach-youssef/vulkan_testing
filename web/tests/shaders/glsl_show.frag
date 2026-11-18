void main() {
    outColor = texture(sampler2D(source(), sourceSampler()), uv) * tint();
}
