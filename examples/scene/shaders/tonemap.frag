// Krzysztof Narkowicz's fit of the ACES filmic curve
vec3 acesFilmic(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

void main() {
    vec3 radiance = texture(sampler2D(hdr(), hdrSampler()), uv).rgb;
    // Linear output: the sRGB target encodes it
    outColor = vec4(acesFilmic(radiance * exposure()), 1.0);
}
