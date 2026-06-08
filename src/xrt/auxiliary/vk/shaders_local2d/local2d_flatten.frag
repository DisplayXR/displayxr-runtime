#version 450
// #439 Phase 3 Local2D flatten: sample one layer image through src_rect.
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 frag;
layout(binding = 0) uniform sampler2D src_tex;
layout(push_constant) uniform Push {
    vec4 src_rect; // xy = src origin (norm), zw = src size (norm; zw.y < 0 = flip_y)
} pc;
void main() {
    vec2 src_uv = pc.src_rect.xy + uv * pc.src_rect.zw;
    frag = texture(src_tex, src_uv);
}
