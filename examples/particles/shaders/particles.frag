void main() {
    // A soft round sprite. Blending is additive, so dense regions glow.
    float falloff = exp(-4.0 * dot(corner, corner));
    outColor = vec4(color * falloff * 0.35, 1.0);
}
