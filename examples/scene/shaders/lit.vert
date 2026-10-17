// The world is the model space (nothing moves), so positions and normals pass through
void main() {
    gl_Position = scene().viewProjection * vec4(inPosition, 1.0);
    worldPosition = inPosition;
    worldNormal = inNormal;
    albedoRoughness = inAlbedoRoughness;
}
