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
static const char *cursor_quad_ps_source = R"(
#ifdef DXR_LAYERED
Texture2DArray sprite_tex : register(t0);
#else
Texture2D sprite_tex : register(t0);
#endif
SamplerState sprite_samp : register(s0);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
#ifdef DXR_LAYERED
    return sprite_tex.Sample(sprite_samp, float3(uv, 0.0));
#else
    return sprite_tex.Sample(sprite_samp, uv);
#endif
}
)";
