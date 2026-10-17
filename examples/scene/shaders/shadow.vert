void main() {
    gl_Position = scene().lightViewProjection * vec4(inPosition, 1.0);
}
