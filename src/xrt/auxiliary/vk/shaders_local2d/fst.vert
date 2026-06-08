#version 450
// Fullscreen triangle; uv in [0,1], top-left origin (Vulkan NDC y-down).
layout(location = 0) out vec2 out_uv;
void main() {
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    out_uv = uv;
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
