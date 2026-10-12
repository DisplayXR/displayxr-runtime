// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Embedded HLSL for the runtime-drawn depth cursor (D3D11 + D3D12).
 * @author David Fattal
 * @ingroup comp_util
 *
 * XR_DXR_cursor_depth v5 (ADR-046 Amendment 2 B): after the app's layers are
 * in the atlas and before the display processor, one textured quad per view
 * tile. The corners arrive in TILE-NORMALISED coordinates (origin top-left,
 * y down, 0..1 spans the tile - u_cursor_draw_corners()), so the viewport is
 * the tile and the vertex shader maps them straight to clip space.
 *
 * INTERNAL to the native D3D compositor libraries. API-agnostic SM 5.0
 * (t0 / s0 / b0): D3D11 binds b0 as a constant buffer, D3D12 as 16 root
 * constants - the same reason comp_masked_composite_shaders.h is shared.
 * Compiled on the FIRST cursor request only: a session that never asks for a
 * runtime-drawn cursor never compiles these (ADR-046 section 0).
 *
 * No colour arithmetic, on purpose (ADR-044): the backend picks the source
 * view (decoding or not) and the target view (`_SRGB` or not) exactly as it
 * does for every other layer, and the hardware does the transfer function.
 */

#pragma once

/*!
 * The cursor quad's constants: corners[k].xy = tile-normalised position,
 * corners[k].zw = texture uv, in triangle-strip order (top-left, bottom-left,
 * top-right, bottom-right - u_cursor_draw_corners()'s order). 16 floats.
 */
struct CursorQuadConstants
{
	float corners[4][4];
};

static const char *cursor_quad_vs_source = R"(
cbuffer CursorCB : register(b0)
{
    float4 corners[4];
};

struct VS_OUTPUT
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VS_OUTPUT main(uint vertex_id : SV_VertexID)
{
    float4 c = corners[vertex_id % 4];
    VS_OUTPUT o;
    o.position = float4(c.x * 2.0 - 1.0, 1.0 - c.y * 2.0, 0.0, 1.0);
    o.uv = c.zw;
    return o;
}
)";

// The texel as stored: the blend state carries the alpha convention -
// SrcAlpha / InvSrcAlpha for the app's STRAIGHT sprite (the spec), One /
// InvSrcAlpha for the PREMULTIPLIED default look (comp_cursor_look); alpha
// is One / InvSrcAlpha either way.
// Compiled with `DXR_LAYERED` defined for a layered (arraySize > 1) sprite
// swapchain, whose SRV is a Texture2DArray: the spec samples array layer 0.
//
// Compiled with `DXR_SOFTEN` defined for the lifted cursor's crosstalk
// softening (ADR-046 Amendment 2 B.7): a 13-tap Gaussian blur (centre, 4 at
// r/2, 8 at r; sigma 0.6 r) of the sprite, accumulated premultiplied, times
// an opacity. The quad is padded by the radius (u_cursor_draw_soften_quad)
// and a tap outside 0..1 is transparent. The 16 corner floats (b0 c0..c3)
// are never touched by the parameters:
// - D3D11 puts them in their OWN constant buffer, b1.
// - D3D12 also defines `DXR_SOFTEN_ROOT_B0`: they ride b0 at c4 - root
//   constants 16..19 of the quad root signature's existing 32-constant
//   parameter (the cursor's 16 corner constants are unchanged, and the root
//   signature, shared with every quad pass, gains nothing).
static const char *cursor_quad_ps_source = R"(
#ifdef DXR_LAYERED
Texture2DArray sprite_tex : register(t0);
#else
Texture2D sprite_tex : register(t0);
#endif
SamplerState sprite_samp : register(s0);

float4 sprite_sample(float2 uv)
{
#ifdef DXR_LAYERED
    return sprite_tex.Sample(sprite_samp, float3(uv, 0.0));
#else
    return sprite_tex.Sample(sprite_samp, uv);
#endif
}

#ifdef DXR_SOFTEN
// soften.xy = blur radius in sprite uv, .z = opacity, .w = 1 for a STRAIGHT-
// alpha sprite (the app's), 0 for a premultiplied one (the default look).
#ifdef DXR_SOFTEN_ROOT_B0
cbuffer CursorSoftenCB : register(b0)
{
    float4 soften : packoffset(c4);
};
#else
cbuffer CursorSoftenCB : register(b1)
{
    float4 soften;
};
#endif

// The D3D11 SERVICE's workspace compose pass (ADR-046 Phase 4) runs under the
// ADR-021 colour model of that pass: its blits emit scene-linear when the
// pass's linearize flag (its own constant buffer, bound here at b2 for this
// draw only) is set. Compiled with `DXR_SVC_LINEARIZE` there and nowhere else;
// every other build of this shader leaves the colour untouched.
#ifdef DXR_SVC_LINEARIZE
cbuffer SvcColorCB : register(b2)
{
    float4 svc_color; // x > 0.5: decode display-referred -> linear on output
};
float3 svc_out(float3 c)
{
    return svc_color.x > 0.5 ? ((c <= 0.04045) ? (c / 12.92) : pow((c + 0.055) / 1.055, 2.4)) : c;
}
#else
float3 svc_out(float3 c)
{
    return c;
}
#endif

float4 soft_tap(float2 uv, bool straight)
{
    float4 s = sprite_sample(uv);
    if (straight) {
        s.rgb *= s.a;
    }
    bool inside = uv.x >= 0.0 && uv.y >= 0.0 && uv.x <= 1.0 && uv.y <= 1.0;
    return inside ? s : float4(0.0, 0.0, 0.0, 0.0);
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    const bool straight = soften.w > 0.5;
    const float2 r = soften.xy;
    float4 acc = soft_tap(uv, straight);
    float wsum = 1.0;
    if (r.x > 0.0 || r.y > 0.0) {
        const float wi = 0.7066;
        const float wo = 0.2494;
        acc += wi * soft_tap(uv + float2(0.5, 0.0) * r, straight);
        acc += wi * soft_tap(uv + float2(-0.5, 0.0) * r, straight);
        acc += wi * soft_tap(uv + float2(0.0, 0.5) * r, straight);
        acc += wi * soft_tap(uv + float2(0.0, -0.5) * r, straight);
        [unroll] for (int k = 0; k < 8; k++) {
            float a = 0.3927 + 0.7854 * (float)k;
            acc += wo * soft_tap(uv + float2(cos(a), sin(a)) * r, straight);
        }
        wsum += 4.0 * wi + 8.0 * wo;
    }
    acc /= wsum;
    if (straight) {
        float3 rgb = acc.a > 1e-5 ? acc.rgb / acc.a : float3(0.0, 0.0, 0.0);
        return float4(svc_out(rgb), acc.a * soften.z);
    }
#ifdef DXR_SVC_LINEARIZE
    // Premultiplied: transform the straight colour, then re-multiply.
    float3 prgb = acc.a > 1e-5 ? svc_out(acc.rgb / acc.a) * acc.a : float3(0.0, 0.0, 0.0);
    return float4(prgb, acc.a) * soften.z;
#else
    return acc * soften.z;
#endif
}
#else
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    return sprite_sample(uv);
}
#endif
)";

//! The DXR_SOFTEN variant's constants (4 floats; see above): D3D11's b1,
//! D3D12's root constants 16..19 (kCursorSoftenRootOffset).
struct CursorSoftenConstants
{
	float soften[4];
};

//! D3D12: where CursorSoftenConstants start in the quad root signature's b0
//! root constants (packoffset c4, right after the 16 corner floats).
static const unsigned int kCursorSoftenRootOffset = 16;
