#version 450
// #439 Phase 3 masked composite: final = M*weave + (1-M)*twod.
// #491: alpha_over path = premultiplied "over" of the 2D atop the weave
//       (final = twod + (1-twod.a)*weave) — translucent 2D reveals the 3D
//       scene, not the desktop. Used for the IMPLICIT (auto) Local2D mask;
//       the explicit authored mask keeps the hard M-lerp (designer portal).
// XR_DXR_display_zones (mode 2): final = twod + (1-twod.a)*(M*weave).
//       ADR-027 — the wish is HARDWARE-only; composition follows zone
//       geometry (M gates only the WEAVE, binary zone raster) + alpha (the
//       2D composites by its own premultiplied alpha ON TOP). Zone interior
//       with no 2D → weave; a Local2D overlay inside a zone → glass over the
//       weave; a 2D band outside every zone → the 2D with its own alpha
//       (alpha 0 where uncovered, so a transparent present still reaches the
//       desktop). Replaces the zones frames' former hard M-lerp, which
//       multiplied overlays away in the M=1 interior.
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
    uint opaque_present; // #833/#116: 1 = flatten (DWM completes no blends)
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
    if (pc.opaque_present == 1u) {
        // Opaque present (runtime #833 / plugin #116): DWM completes no
        // blends, so never emit alpha < 1. The DP's flattened gate already
        // baked the captured desktop into the weave wherever the atlas was
        // transparent (2D bands, outside every zone), so the weave IS the
        // background here: ZONES and ALPHA_OVER collapse to a premul-over of
        // the 2D onto the flattened weave (the M weave-gate would discard
        // that baked desktop); LERP keeps M but completes the 2D side the
        // same way. Zone-edge feather becomes a no-op (both mix ends hold
        // woven content inside the ramp) — a documented semantic of the mode.
        vec3 over = twod.rgb + (1.0 - twod.a) * weave.rgb;
        if (pc.alpha_over == 0u) {
            float M = clamp(texture(mask_tex, uv).r, 0.0, 1.0);
            frag = vec4(M * weave.rgb + (1.0 - M) * over, 1.0);
        } else {
            frag = vec4(over, 1.0);
        }
        return;
    }
    if (pc.alpha_over == 1u) {
        // #491: the 2D layer's own (premultiplied) alpha IS the blend.
        // opaque 2D (a=1) → crisp panel; translucent (a=0.5) → glass over 3D;
        // uncovered (a=0) → full weave.
        frag = twod + (1.0 - twod.a) * weave;
        return;
    }
    float M = clamp(texture(mask_tex, uv).r, 0.0, 1.0);
    if (pc.alpha_over == 2u) {
        // Zones mode (ADR-027): M gates the WEAVE (zone geometry); the 2D
        // composites on top by its own premultiplied alpha.
        frag = twod + (1.0 - twod.a) * (M * weave);
        return;
    }
    frag = M * weave + (1.0 - M) * twod;
}
