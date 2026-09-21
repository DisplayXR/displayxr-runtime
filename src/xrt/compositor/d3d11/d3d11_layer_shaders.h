// Copyright 2024-2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Embedded HLSL shaders for the in-process D3D11 layer compositor.
 * @author David Fattal
 * @ingroup comp_d3d11
 *
 * Moved verbatim out of comp_d3d11_renderer.cpp, which compiled these strings
 * inline. They live in a header for the same reason
 * d3d11_service/d3d11_service_shaders.h and
 * d3d_shared/comp_masked_composite_shaders.h do: the constant-buffer structs
 * below must match the HLSL below them, and nothing but a test can check that.
 * D3DCompile and D3DReflect need no device, so the interface these strings
 * declare is pinnable on the same hardware-free footing as the rest of tests/.
 */

#pragma once

/*!
 * Shader constant buffer layout.
 */
struct LayerConstants
{
	float mvp[16];          // Model-view-projection matrix
	float post_transform[4]; // xy = offset, zw = scale
	float color_scale[4];    // Color multiplier
	float color_bias[4];     // Color offset
	float array_params[4];   // x = array slice (imageArrayIndex) for layered swapchains; yzw pad
};

/*!
 * Local2D flatten constant buffer (#439 Phase 3). One float4: the source
 * sub-rect in normalized [0,1] swapchain-image coords. The viewport's uv [0,1]
 * maps through it as `src_uv = xy + uv*zw`. The caller bakes the dest-clip
 * fractions, the layer's norm_rect, and flip_y (negative zw.y) into it.
 */
struct FlattenParams
{
	float src_rect[4]; // xy = src origin (norm), zw = src size (norm; zw.y < 0 ⇒ flip_y)
};

// Embedded HLSL shader source
static const char *projection_vs_source = R"(
cbuffer LayerCB : register(b0)
{
    float4x4 mvp;
    float4 post_transform;
    float4 color_scale;
    float4 color_bias;
};

struct VS_OUTPUT
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

static const float2 quad_positions[4] = {
    float2(-1.0, -1.0),
    float2(-1.0,  1.0),
    float2( 1.0, -1.0),
    float2( 1.0,  1.0),
};

static const float2 quad_uvs[4] = {
    float2(0.0, 1.0),
    float2(0.0, 0.0),
    float2(1.0, 1.0),
    float2(1.0, 0.0),
};

VS_OUTPUT VSMain(uint vertex_id : SV_VertexID)
{
    VS_OUTPUT output;
    float2 pos = quad_positions[vertex_id];
    float2 uv = quad_uvs[vertex_id];
    output.position = mul(mvp, float4(pos, 0.0, 1.0));
    output.uv = uv * post_transform.zw + post_transform.xy;
    return output;
}
)";

static const char *projection_ps_source = R"(
cbuffer LayerCB : register(b0)
{
    float4x4 mvp;
    float4 post_transform;
    float4 color_scale;
    float4 color_bias;
};

Texture2D layer_tex : register(t0);
SamplerState layer_samp : register(s0);

struct VS_OUTPUT
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 PSMain(VS_OUTPUT input) : SV_Target
{
    float4 color = layer_tex.Sample(layer_samp, input.uv);
    color = color * color_scale + color_bias;
    return color;
}
)";

// Projection pixel shader variant for LAYERED (array) swapchains. Under
// single-pass-instanced the app submits ONE swapchain with arraySize=2 and two
// projection views referencing subImage.imageArrayIndex 0 (left) / 1 (right).
// The whole-array Texture2DArray SRV (FirstArraySlice=0, ArraySize=N) is bound;
// this shader selects the requested slice via array_params.x. A plain Texture2D
// shader (above) always views slice 0, so both eyes would sample the left image
// (flat output) — this is the D3D11 analog of the D3D12 #656 fix. Single-layer
// swapchains keep the Texture2D path unchanged.
static const char *projection_ps_array_source = R"(
cbuffer LayerCB : register(b0)
{
    float4x4 mvp;
    float4 post_transform;
    float4 color_scale;
    float4 color_bias;
    float4 array_params;   // x = array slice
};

Texture2DArray layer_tex : register(t0);
SamplerState layer_samp : register(s0);

struct VS_OUTPUT
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 PSMain(VS_OUTPUT input) : SV_Target
{
    float4 color = layer_tex.Sample(layer_samp, float3(input.uv, array_params.x));
    color = color * color_scale + color_bias;
    return color;
}
)";

// Quad layer vertex shader - positioned 3D quad
static const char *quad_vs_source = R"(
cbuffer LayerCB : register(b0)
{
    float4x4 mvp;
    float4 post_transform;
    float4 color_scale;
    float4 color_bias;
};

struct VS_OUTPUT
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

// Quad centered at origin, 1x1 size in local space
static const float2 quad_positions[4] = {
    float2(0.0, 0.0),   // Bottom-left
    float2(0.0, 1.0),   // Top-left
    float2(1.0, 0.0),   // Bottom-right
    float2(1.0, 1.0),   // Top-right
};

VS_OUTPUT VSMain(uint vertex_id : SV_VertexID)
{
    VS_OUTPUT output;

    float2 in_uv = quad_positions[vertex_id % 4];

    // Center the quad at origin
    float2 pos = in_uv - 0.5;

    // Flip Y into OpenXR/model space. in_uv.y == 0 is the texture's TOP row
    // (D3D texture origin is top-left) and OpenXR quad model space is Y-up,
    // so the top row must sit at +Y. Under the Y-up (D3D) projection that
    // puts the texture's top at the top of the view.
    // (#1580: this flip used to be absent and the Vulkan Y-DOWN projection
    // supplied it instead. The texture then came out upright, but the quad's
    // PLACEMENT was mirrored about the view's horizontal centre line -- a
    // Y-down projection negates the quad's world-space Y offset too, which
    // the identity-blit projection layer in the same tile never gets.)
    pos.y = -pos.y;

    // Transform position by MVP (which includes quad size scaling)
    output.position = mul(mvp, float4(pos, 0.0, 1.0));

    // Apply UV transform for sub-image
    output.uv = in_uv * post_transform.zw + post_transform.xy;

    return output;
}
)";

// Quad layer pixel shader
static const char *quad_ps_source = R"(
cbuffer LayerCB : register(b0)
{
    float4x4 mvp;
    float4 post_transform;
    float4 color_scale;
    float4 color_bias;
};

Texture2D layer_tex : register(t0);
SamplerState layer_samp : register(s0);

struct VS_OUTPUT
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 PSMain(VS_OUTPUT input) : SV_Target
{
    float4 color = layer_tex.Sample(layer_samp, input.uv);
    color = color * color_scale + color_bias;
    return color;
}
)";

// #1601 — quad pixel shader variant for LAYERED (arraySize>1) swapchains. Same
// relationship to quad_ps_source as projection_ps_array_source has to
// projection_ps_source, and for the same reason: comp_d3d11_swapchain creates a
// WHOLE-ARRAY Texture2DArray SRV whenever ArraySize > 1, so binding it to the
// Texture2D shader above is a view-dimension mismatch that reads slice 0 no
// matter what subImage.imageArrayIndex asked for. This selects the requested
// slice via array_params.x.
//
// The gate is the SWAPCHAIN's shape, not array_index != 0 — what must match the
// shader is the view dimension, so slice 0 OF AN ARRAY swapchain belongs here
// too. Single-layer swapchains keep the Texture2D path unchanged.
static const char *quad_ps_array_source = R"(
cbuffer LayerCB : register(b0)
{
    float4x4 mvp;
    float4 post_transform;
    float4 color_scale;
    float4 color_bias;
    float4 array_params;   // x = array slice
};

Texture2DArray layer_tex : register(t0);
SamplerState layer_samp : register(s0);

struct VS_OUTPUT
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 PSMain(VS_OUTPUT input) : SV_Target
{
    float4 color = layer_tex.Sample(layer_samp, float3(input.uv, array_params.x));
    color = color * color_scale + color_bias;
    return color;
}
)";

// #439 Phase 3 — Local2D flatten. Draws one app Local2D layer image into the
// runtime 2D scratch. The per-draw viewport (RSSetViewports, set by the caller)
// restricts output to the clipped dest sub-rect; uv [0,1] over that viewport
// maps through src_rect into the source swapchain image (dest-clip fractions,
// the layer norm_rect, and flip_y are all baked into src_rect by the caller).
// Premultiplied-vs-unpremultiplied is the caller's blend-state choice; the
// shader passes the sampled texel straight through (sRGB-passthrough: the
// source SRV is the swapchain's UNORM sibling, so no auto-decode).
static const char *local2d_flatten_vs_source = R"(
struct VS_OUTPUT
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

// Fullscreen triangle, uv [0,1] with top-left origin (uv grows right/down),
// matching D3D texture-sampling convention.
static const float2 positions[3] = {
    float2(-1.0,  1.0),
    float2(-1.0, -3.0),
    float2( 3.0,  1.0),
};
static const float2 uvs[3] = {
    float2(0.0, 0.0),
    float2(0.0, 2.0),
    float2(2.0, 0.0),
};

VS_OUTPUT VSMain(uint vertex_id : SV_VertexID)
{
    VS_OUTPUT o;
    o.position = float4(positions[vertex_id], 0.0, 1.0);
    o.uv = uvs[vertex_id];
    return o;
}
)";

static const char *local2d_flatten_ps_source = R"(
Texture2D src_tex  : register(t0);
SamplerState samp  : register(s0);

cbuffer FlattenParams : register(b0)
{
    float4 src_rect; // xy = src origin (norm), zw = src size (norm; zw.y < 0 = flip_y)
};

struct VS_OUTPUT
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 PSMain(VS_OUTPUT input) : SV_Target
{
    float2 src_uv = src_rect.xy + input.uv * src_rect.zw;
    return src_tex.Sample(samp, src_uv);
}
)";
