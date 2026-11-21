// A hash of a and b (web/tests/extern_glsl.test.ts computes it too)
uint weave(uint a, uint b) {
    return (a * 2654435761u) ^ (b << 3u) ^ (a >> 2u);
}
