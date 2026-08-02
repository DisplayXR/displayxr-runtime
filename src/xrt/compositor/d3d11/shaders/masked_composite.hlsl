// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  HLSL masked 2D-over-3D composite for the D3D11 compositor.
 * @author David Fattal
 * @ingroup comp_d3d11
 *
 * Composites a 2D layer (Local2D / display-zones content) over the weaved 3D
 * output, gated by a per-pixel region mask. This is the general 2D-over-3D
 * mechanism from docs/roadmap/unified-2d-3d-compositing.md §4.
 *
 * RECT-MASK PATH (`use_rect_mask` true): the mask is derived analytically from
 * the canvas rect — pixels INSIDE the canvas keep the weave (discard), pixels
 * OUTSIDE are written from the 2D layer at 1:1. With matching DXGI formats + a
 * point sampler + an opaque (no-blend) output state this is a hard-edged 1:1
 * fill of the non-canvas region.
 *
 * AUTHORED-MASK PATH: `use_rect_mask` goes false; `mask_tex` (a separate scalar
 * R8 channel, NOT the 2D layer's alpha — see §4.0 of the spec) supplies M in
 * [0,1], and the blend follows `composite_mode`:
 *     0 (LERP)       final = M·weave + (1-M)·twod         (explicit authored
 *                    mask — designer cutout/portal; both rgb and a)
 *     1 (ALPHA_OVER) final = twod + (1-twod.a)·weave      (#491 implicit
 *                    legacy mask: the 2D's own premultiplied alpha IS the
 *                    blend; mask unused)
 *     2 (ZONES)      final = twod + (1-twod.a)·(M·weave)  (ADR-027/#801: M
 *                    gates only the WEAVE by zone geometry — binary zone
 *                    raster, or the #803 opt-in feather ramp — and the 2D
 *                    composites on top by its own alpha)
 * which requires the weave bound as an SRV (weave_tex).
 *
 * OPAQUE PRESENT (`opaque_present`, runtime #833 / plugin #116): DWM completes
 * no blends, so the composite never emits alpha < 1. The DP's flattened gate
 * already baked the captured desktop into the weave wherever the atlas was
 * transparent, so the weave IS the background: ZONES and ALPHA_OVER collapse
 * to a premul-over of the 2D onto the weave; LERP keeps M but completes its
 * 2D side the same way. α=1 everywhere; ignored on the rect path.
 */

Texture2D twod_tex   : register(t0);   // the 2D layer (RGBA, premultiplied)
Texture2D mask_tex   : register(t1);   // Phase 1+: scalar region mask, M in [0,1]
Texture2D weave_tex  : register(t2);   // Phase 1+: weaved 3D, read for the lerp
SamplerState samp    : register(s0);   // point sampler (1:1, no filtering)

cbuffer CompositeParams : register(b0)
{
    float2 dst_dims;       // destination width,height in pixels
    float2 canvas_origin;  // canvas sub-rect top-left (px) — the 3D region
    float2 canvas_size;    // canvas sub-rect size (px)
    uint   use_rect_mask;  // 1 = Phase 0 analytic rect mask; 0 = sample mask_tex
    uint   composite_mode; // 0 = hard M-lerp, 1 = #491 premul over, 2 = zones (ADR-027)
    uint   opaque_present; // #833/#116: 1 = flatten (DWM completes no blends)
};

struct VS_OUTPUT
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

// Full-screen triangle (matches fullscreen_blit.hlsl): uv spans [0,1] across dst.
static const float2 positions[3] = {
    float2(-1.0, -1.0),
    float2(-1.0,  3.0),
    float2( 3.0, -1.0),
};
static const float2 uvs[3] = {
    float2(0.0, 1.0),
    float2(0.0, -1.0),
    float2(2.0, 1.0),
};

VS_OUTPUT VSMain(uint vertex_id : SV_VertexID)
{
    VS_OUTPUT o;
    o.position = float4(positions[vertex_id], 0.0, 1.0);
    o.uv = uvs[vertex_id];
    return o;
}

// Returns 3D-ness M in [0,1]: 1 = fully 3D (keep weave), 0 = fully 2D.
float region_mask(float2 px, float2 uv)
{
    if (use_rect_mask)
    {
        // Phase 0: hard rect. Inside the canvas → 3D (M=1), outside → 2D (M=0).
        bool inside =
            px.x >= canvas_origin.x && px.x < canvas_origin.x + canvas_size.x &&
            px.y >= canvas_origin.y && px.y < canvas_origin.y + canvas_size.y;
        return inside ? 1.0 : 0.0;
    }
    // Phase 1+: separate scalar mask, sampled 1:1.
    return saturate(mask_tex.Sample(samp, uv).r);
}

float4 PSMain(VS_OUTPUT input) : SV_Target
{
    float2 px = input.uv * dst_dims;
    float M = region_mask(px, input.uv);

    // Phase 0 fast path: hard mask, no weave read. M==1 → keep the weave
    // (discard so the bound RTV — which already holds the weave — is
    // untouched); M==0 → write the 2D layer at 1:1. Byte-identical to the
    // strip CopySubresourceRegion under matched formats + point sampling.
    if (use_rect_mask)
    {
        if (M >= 0.5)
            discard;                       // inside canvas: weave stays
        return twod_tex.Sample(samp, input.uv);
    }

    // Phase 1+ general path, by composite_mode (see the header comment).
    float4 twod  = twod_tex.Sample(samp, input.uv);
    float4 weave = weave_tex.Sample(samp, input.uv);
    if (opaque_present == 1)
    {
        // #833/#116 flatten (see the header comment).
        float3 over = twod.rgb + (1.0 - twod.a) * weave.rgb;
        if (composite_mode == 0)
            return float4(M * weave.rgb + (1.0 - M) * over, 1.0);
        return float4(over, 1.0);
    }
    if (composite_mode == 1)
    {
        // #491: the 2D layer's own (premultiplied) alpha IS the blend.
        return twod + (1.0 - twod.a) * weave;
    }
    if (composite_mode == 2)
    {
        // XR_DXR_display_zones (ADR-027, #801): M gates only the weave.
        return twod + (1.0 - twod.a) * (M * weave);
    }
    // Hard M-lerp, preserving each layer's own alpha. (final.a = M·weave.a +
    // (1-M)·twod.a — honors the #225 compose-under-bg contract by carrying
    // whichever layer wins the pixel.)
    return M * weave + (1.0 - M) * twod;
}
