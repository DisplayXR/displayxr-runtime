// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  HLSL masked 2D-over-3D composite for the D3D11 compositor.
 * @author David Fattal
 * @ingroup comp_d3d11
 *
 * Composites a full-window 2D layer over the weaved 3D output, gated by a
 * per-pixel region mask. Replaces the rectangular strip-copy surround
 * (d3d11_blit_surround_strips) with the general mechanism from
 * docs/roadmap/unified-2d-3d-compositing.md §4.
 *
 * PHASE 0 (this file): the mask is derived analytically from the canvas
 * rect — pixels INSIDE the canvas keep the weave (discard), pixels OUTSIDE
 * are written from the 2D layer at 1:1. With matching DXGI formats + a point
 * sampler + an opaque (no-blend) output state this is pixel-identical to the
 * CopySubresourceRegion strip copy it replaces. Validation: §6 of the plan
 * doc (unified-2d-3d-phase0-impl.md) — A/B capture diff behind
 * DISPLAYXR_SURROUND_SHADER.
 *
 * PHASE 1+ (later): `use_rect_mask` goes false; `mask_tex` (a separate scalar
 * R8 channel, NOT the 2D layer's alpha — see §4.0 of the spec) supplies M in
 * [0,1], and the discard becomes the mask-lerp
 *     final = M·weave + (1-M)·twod        (both rgb and a)
 * which requires the weave bound as an SRV (weave_tex). The fields for that
 * path are present below but unused in Phase 0.
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
    uint   _pad;
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

    // Phase 1+ general path: mask-lerp, preserving each layer's own alpha.
    // (final.a = M·weave.a + (1-M)·twod.a — honors the #225 compose-under-bg
    // contract by carrying whichever layer wins the pixel.)
    float4 twod  = twod_tex.Sample(samp, input.uv);
    float4 weave = weave_tex.Sample(samp, input.uv);
    return M * weave + (1.0 - M) * twod;
}
