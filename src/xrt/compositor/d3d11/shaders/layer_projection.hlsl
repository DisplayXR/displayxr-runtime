// Copyright 2024-2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  HLSL projection layer shader for D3D11 compositor.
 * @author David Fattal
 * @ingroup comp_d3d11
 *
 * Renders projection layers (stereo views from XrCompositionLayerProjection).
 * This shader handles texture sampling with UV transforms.
 */

// Per-layer constant buffer
cbuffer LayerCB : register(b0)
{
    float4x4 mvp;           // Model-view-projection matrix
    float4 post_transform;  // xy = offset, zw = scale (for UV adjustment)
};

// Input texture and sampler
Texture2D layer_tex : register(t0);
SamplerState layer_samp : register(s0);

// Vertex shader input/output
struct VS_INPUT
{
    uint vertex_id : SV_VertexID;
};

struct VS_OUTPUT
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

// Full-screen quad vertices (triangle strip)
static const float2 quad_positions[4] = {
    float2(-1.0, -1.0),  // Bottom-left
    float2(-1.0,  1.0),  // Top-left
    float2( 1.0, -1.0),  // Bottom-right
    float2( 1.0,  1.0),  // Top-right
};

static const float2 quad_uvs[4] = {
    float2(0.0, 1.0),  // Bottom-left (flip Y)
    float2(0.0, 0.0),  // Top-left
    float2(1.0, 1.0),  // Bottom-right
    float2(1.0, 0.0),  // Top-right
};

VS_OUTPUT VSMain(VS_INPUT input)
{
    VS_OUTPUT output;

    // Get quad vertex
    float2 pos = quad_positions[input.vertex_id];
    float2 uv = quad_uvs[input.vertex_id];

    // Apply MVP transform
    output.position = mul(mvp, float4(pos, 0.0, 1.0));

    // Apply UV transform: uv = uv * scale + offset
    output.uv = uv * post_transform.zw + post_transform.xy;

    return output;
}

float4 PSMain(VS_OUTPUT input) : SV_Target
{
    return layer_tex.Sample(layer_samp, input.uv);
}
