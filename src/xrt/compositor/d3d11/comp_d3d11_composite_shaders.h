// Copyright 2024-2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Embedded HLSL for the masked 2D-over-3D composite (D3D11).
 * @author David Fattal
 * @ingroup comp_d3d11
 *
 * INTERNAL to the D3D11 compositor library — not part of any public interface.
 *
 * The masked composite is compiled per DEVICE, and #918 Phase 2 gives the
 * compositor two of them (the app device and, under the output-device split,
 * the scanout device). The sources therefore live here, in ONE place, so a
 * second consumer includes them instead of copying: a divergent copy would
 * weave one device's frames with a different shader than the other's, which is
 * exactly the class of bug the split cannot afford.
 *
 * Keep byte-aligned with the reference `shaders/masked_composite.hlsl` (that
 * file documents the pass; these strings are what actually compiles).
 */

#pragma once

// Masked 2D-over-3D composite (#439 Phase 0). Keep byte-aligned with
// shaders/masked_composite.hlsl. Phase 0 derives a hard mask from the canvas
// rect (discard inside → weave kept; sample 2D outside at 1:1). The Phase 1+
// lerp path (sample mask_tex t1, lerp against weave_tex t2) is present but
// gated by use_rect_mask.
static const char *masked_composite_vs_source = R"(
struct VS_OUTPUT
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

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
)";

static const char *masked_composite_ps_source = R"(
Texture2D twod_tex   : register(t0);
Texture2D mask_tex   : register(t1);
Texture2D weave_tex  : register(t2);
SamplerState samp    : register(s0);

cbuffer CompositeParams : register(b0)
{
    float2 dst_dims;
    float2 canvas_origin;
    float2 canvas_size;
    uint   use_rect_mask;
    uint   composite_mode; // 0 = hard M-lerp, 1 = #491 premul over, 2 = zones (ADR-027)
    uint   opaque_present; // #833/#116: 1 = flatten (DWM completes no blends)
};

struct VS_OUTPUT
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float region_mask(float2 px, float2 uv)
{
    if (use_rect_mask)
    {
        bool inside =
            px.x >= canvas_origin.x && px.x < canvas_origin.x + canvas_size.x &&
            px.y >= canvas_origin.y && px.y < canvas_origin.y + canvas_size.y;
        return inside ? 1.0 : 0.0;
    }
    return saturate(mask_tex.Sample(samp, uv).r);
}

float4 PSMain(VS_OUTPUT input) : SV_Target
{
    float2 px = input.uv * dst_dims;

    if (use_rect_mask)
    {
        float M = region_mask(px, input.uv);
        if (M >= 0.5)
            discard;
        return twod_tex.Sample(samp, input.uv);
    }

    float4 twod  = twod_tex.Sample(samp, input.uv);
    float4 weave = weave_tex.Sample(samp, input.uv);
    if (opaque_present == 1)
    {
        // Opaque present (runtime #833 / plugin #116): DWM completes no
        // blends, so never emit alpha < 1. The DP's flattened gate already
        // baked the captured desktop into the weave wherever the atlas was
        // transparent (2D bands, outside every zone), so the weave IS the
        // background here: ZONES and ALPHA_OVER collapse to a premul-over
        // of the 2D onto the flattened weave (the M weave-gate would
        // discard that baked desktop); LERP keeps M but completes the 2D
        // side the same way. Zone-edge feather becomes a no-op (both mix
        // ends hold woven content inside the ramp) — a documented semantic
        // of the mode.
        float3 over = twod.rgb + (1.0 - twod.a) * weave.rgb;
        if (composite_mode == 0)
        {
            float M = saturate(mask_tex.Sample(samp, input.uv).r);
            return float4(M * weave.rgb + (1.0 - M) * over, 1.0);
        }
        return float4(over, 1.0);
    }
    if (composite_mode == 1)
    {
        // #491: the 2D layer's own (premultiplied) alpha IS the blend —
        // translucent 2D reveals the 3D scene, not the desktop.
        return twod + (1.0 - twod.a) * weave;
    }
    float M = saturate(mask_tex.Sample(samp, input.uv).r);
    if (composite_mode == 2)
    {
        // XR_DXR_display_zones (ADR-027, #801): M gates only the WEAVE by
        // zone geometry (binary zone raster, or the #803 opt-in feather
        // ramp); the 2D composites on top by its own premultiplied alpha.
        // Zone interior with no 2D -> weave; a Local2D overlay inside a
        // zone -> glass over the weave; a 2D band outside every zone ->
        // the 2D with its own alpha (alpha 0 where uncovered, so a
        // transparent present still reaches the desktop).
        return twod + (1.0 - twod.a) * (M * weave);
    }
    return M * weave + (1.0 - M) * twod;
}
)";
