#version 450
// Android weave un-squeeze (rect-scoped weave): copy one source sub-rect into
// the viewport (blend OFF), stretching as the viewport/src ratio dictates, with
// the sample point CLAMPED to the sub-rect's outermost texel centres. That is
// exactly what a LINEAR vkCmdBlitImage out of an image of EXACTLY the sub-rect's
// size produces (its clamp-to-edge is the image edge) — without the staging copy.
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 frag;
layout(binding = 0) uniform sampler2D src_tex;
layout(push_constant) uniform Push {
    vec4 src_rect;   // xy = src origin (norm), zw = src size (norm)
    vec4 clamp_rect; // xy = min sample point (norm), zw = max sample point (norm)
} pc;
void main() {
    vec2 src_uv = pc.src_rect.xy + uv * pc.src_rect.zw;
    frag = texture(src_tex, clamp(src_uv, pc.clamp_rect.xy, pc.clamp_rect.zw));
}
