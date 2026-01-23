// Copyright 2024-2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  HLSL fullscreen blit shader for D3D11 compositor.
 * @author David Fattal
 * @ingroup comp_d3d11
 *
 * Simple fullscreen blit for copying textures to render targets.
 */

Texture2D source_tex : register(t0);
SamplerState source_samp : register(s0);

struct VS_INPUT
{
    uint vertex_id : SV_VertexID;
};

struct VS_OUTPUT
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

// Full-screen triangle (more efficient than quad)
// Uses oversized triangle to cover the screen
static const float2 positions[3] = {
    float2(-1.0, -1.0),  // Bottom-left
    float2(-1.0,  3.0),  // Top-left (oversized)
    float2( 3.0, -1.0),  // Bottom-right (oversized)
};

static const float2 uvs[3] = {
    float2(0.0, 1.0),  // Bottom-left
    float2(0.0, -1.0), // Top-left (oversized)
    float2(2.0, 1.0),  // Bottom-right (oversized)
};

VS_OUTPUT VSMain(VS_INPUT input)
{
    VS_OUTPUT output;

    output.position = float4(positions[input.vertex_id], 0.0, 1.0);
    output.uv = uvs[input.vertex_id];

    return output;
}

float4 PSMain(VS_OUTPUT input) : SV_Target
{
    return source_tex.Sample(source_samp, input.uv);
}
