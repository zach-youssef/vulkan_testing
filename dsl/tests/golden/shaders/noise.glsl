// A function written in GLSL, which ZGAL bodies call (extern-glsl)
float hashNoise(vec2 p) {
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}
