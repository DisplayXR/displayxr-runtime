#version 450
// #439 Phase 3 masked composite: final = M*weave + (1-M)*twod.
// #491: alpha_over path = premultiplied "over" of the 2D atop the weave
//       (final = twod + (1-twod.a)*weave) — translucent 2D reveals the 3D
//       scene, not the desktop. Used for the IMPLICIT (auto) Local2D mask;
//       the explicit authored mask keeps the hard M-lerp (designer portal).
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 frag;
layout(binding = 0) uniform sampler2D twod_tex;
layout(binding = 1) uniform sampler2D mask_tex;
layout(binding = 2) uniform sampler2D weave_tex;
layout(push_constant) uniform Push {
    vec2 dst_dims;
    vec2 canvas_origin;
    vec2 canvas_size;
    uint use_rect_mask;
    uint alpha_over;
} pc;
void main() {
    if (pc.use_rect_mask != 0u) {
        vec2 px = uv * pc.dst_dims;
        bool inside = px.x >= pc.canvas_origin.x && px.x < pc.canvas_origin.x + pc.canvas_size.x &&
                      px.y >= pc.canvas_origin.y && px.y < pc.canvas_origin.y + pc.canvas_size.y;
        if (inside) discard;            // keep the loaded weave inside the canvas
        frag = texture(twod_tex, uv);   // 2D outside
        return;
    }
    vec4 twod = texture(twod_tex, uv);
    vec4 weave = texture(weave_tex, uv);
    if (pc.alpha_over != 0u) {
        // #491: the 2D layer's own (premultiplied) alpha IS the blend.
        // opaque 2D (a=1) → crisp panel; translucent (a=0.5) → glass over 3D;
        // uncovered (a=0) → full weave.
        frag = twod + (1.0 - twod.a) * weave;
        return;
    }
    float M = clamp(texture(mask_tex, uv).r, 0.0, 1.0);
    frag = M * weave + (1.0 - M) * twod;
}
