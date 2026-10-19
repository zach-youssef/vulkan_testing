void main() {
    gl_Position = transforms()[object()] * vec4(inPosition, 1.0);
    uv = inUv;
    screen = gl_Position.xy / gl_Position.w;
    material = inMaterial;
}
