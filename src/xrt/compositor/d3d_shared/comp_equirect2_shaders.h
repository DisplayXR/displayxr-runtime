// Copyright 2025-2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Embedded HLSL for XR_KHR_composition_layer_equirect2, shared by both
 *         D3D11 composition paths.
 * @author David Fattal
 * @ingroup comp_util
 *
 * #1602. This text used to live in d3d11_service/d3d11_service_shaders.h and
 * serve the out-of-process service alone; the in-process renderer had no
 * equirect2 draw at all and dropped the layer with a one-shot WARN. Giving the
 * in-process path its own copy would have made a THIRD hand-maintained twin of
 * ~100 lines of ray-march math — the same argument #1601 already made inside
 * this shader when it chose a `DXR_LAYERED` #ifdef over a second copy, one
 * level up.
 *
 * So it lives in d3d_shared/ for exactly the reason
 * comp_masked_composite_shaders.h does: HLSL that more than one D3D backend
 * compiles, next to the C++ struct whose layout must match it, where a single
 * hardware-free reflection test can pin the pair (D3DCompile and D3DReflect
 * need no device). d3d11_service_shaders.h #includes this, so every existing
 * consumer of the old names keeps compiling unchanged.
 *
 * The vertex shader is convention-correct for D3D11 as it stands — it builds
 * no projection matrix (rays come from `mv_inverse` + `to_tangent`) and the
 * one place raster handedness enters, the [0,1] -> NDC map, already carries
 * the #1580 Y-up fix. There is nothing per-path to re-derive.
 */

#pragma once

//! Constant buffer layout for equirect2 layers
struct Equirect2LayerConstants
{
	float mv_inverse[16];           // Inverse model-view matrix
	float post_transform[4];        // xy = offset, zw = scale (UV)
	float color_scale[4];           // RGBA multiplier
	float color_bias[4];            // RGBA offset
	float to_tangent[4];            // UV to tangent space conversion
	float radius;                   // Sphere radius
	float central_horizontal_angle; // Horizontal angle
	float upper_vertical_angle;     // Upper vertical angle
	float lower_vertical_angle;     // Lower vertical angle
	float array_params[4];          // #1601: x = source array slice; yzw pad
};

//! Vertex shader for equirect2 layers - fullscreen with ray direction
static const char *equirect2_vs_hlsl = R"(
cbuffer LayerCB : register(b0)
{
    float4x4 mv_inverse;
    float4 post_transform;
    float4 color_scale;
    float4 color_bias;
    float4 to_tangent;
    float radius;
    float central_horizontal_angle;
    float upper_vertical_angle;
    float lower_vertical_angle;
};

struct VS_OUTPUT
{
    float4 position : SV_Position;
    float3 camera_position : TEXCOORD0;
    float3 camera_ray : TEXCOORD1;
};

static const float2 positions[4] = {
    float2(0, 0),
    float2(0, 1),
    float2(1, 0),
    float2(1, 1),
};

float3 intersection_with_unit_plane(float2 uv_0_to_1)
{
    // [0 .. 1] to tangent lengths (at unit Z)
    float2 tangent_factors = uv_0_to_1 * to_tangent.zw + to_tangent.xy;

    // With Z at unit plane and flip Y for OpenXR coordinate system
    float3 point_on_unit_plane = float3(tangent_factors.x, -tangent_factors.y, -1);

    return point_on_unit_plane;
}

VS_OUTPUT VSMain(uint vertex_id : SV_VertexID)
{
    VS_OUTPUT output;

    float2 uv = positions[vertex_id % 4];

    // Get camera position in model space
    output.camera_position = mul(mv_inverse, float4(0, 0, 0, 1)).xyz;

    // Get ray direction on unit plane in view space
    float3 ray_in_view_space = intersection_with_unit_plane(uv);

    // Transform to model space (normalize in fragment shader)
    output.camera_ray = mul((float3x3)mv_inverse, ray_in_view_space);

    // Go from [0 .. 1] to NDC. The interpolated ray above is built with
    // -tangent_factors.y, i.e. uv.y == 0 (the frustum's DOWN edge) yields a
    // ray pointing UP -- the Vulkan Y-down raster convention. D3D11 NDC is
    // Y-UP, so map uv.y == 0 to NDC +1 (screen TOP) and the vertex's screen
    // position agrees with its ray again. Flipping the ray sign instead
    // would fix the same mirror twice over -- one fix, here (#1580).
    float2 pos = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    output.position = float4(pos, 0.0, 1.0);

    return output;
}
)";

//! Pixel shader for equirect2 layers - spherical UV mapping.
//!
//! #1601: compiled TWICE — once plain, once with DXR_LAYERED defined, which
//! yields the Texture2DArray variant that samples subImage.imageArrayIndex.
//! The two short layer shaders (quad, cylinder) each got a hand-written array
//! twin, matching the blit_ps/blit_ps_array precedent in this file; this one
//! does not, because a second copy of ~100 lines of ray-march math would drift
//! from the original the first time either is touched. Same reason the variant
//! is a #ifdef over the declaration and the sample rather than a #ifdef over
//! the whole body.
static const char *equirect2_ps_hlsl = R"(
cbuffer LayerCB : register(b0)
{
    float4x4 mv_inverse;
    float4 post_transform;
    float4 color_scale;
    float4 color_bias;
    float4 to_tangent;
    float radius;
    float central_horizontal_angle;
    float upper_vertical_angle;
    float lower_vertical_angle;
    float4 array_params;   // x = array slice; read only under DXR_LAYERED
};

#ifdef DXR_LAYERED
Texture2DArray layer_tex : register(t0);
#else
Texture2D layer_tex : register(t0);
#endif
SamplerState layer_samp : register(s0);

struct VS_OUTPUT
{
    float4 position : SV_Position;
    float3 camera_position : TEXCOORD0;
    float3 camera_ray : TEXCOORD1;
};

static const float PI = 3.14159265359;

float2 sphere_intersect(float3 ray_origin, float3 ray_direction, float3 sphere_center, float r)
{
    float3 ray_sphere_diff = ray_origin - sphere_center;
    float B = dot(ray_sphere_diff, ray_direction);
    float3 QC = ray_sphere_diff - B * ray_direction;
    float H = r * r - dot(QC, QC);

    if (H < 0.0) {
        return float2(-1.0, -1.0);  // No intersection
    }

    H = sqrt(H);
    return float2(-B - H, -B + H);
}

float4 PSMain(VS_OUTPUT input) : SV_Target
{
    float3 ray_origin = input.camera_position;
    float3 ray_dir = normalize(input.camera_ray);

    float3 dir_from_sph;

    // CPU code sets +INFINITY to zero radius
    if (radius == 0) {
        dir_from_sph = ray_dir;
    } else {
        float2 distances = sphere_intersect(ray_origin, ray_dir, float3(0, 0, 0), radius);

        // The ray misses the sphere: this fragment is not part of the layer at
        // all. DISCARD, never "return transparent black" -- see the note at the
        // foot of this shader. The return below is unreachable and is there
        // only because HLSL requires every path of a value-returning function
        // to end in one.
        if (distances.y < 0) {
            discard;
            return float4(0, 0, 0, 0);
        }

        float3 pos = ray_origin + (ray_dir * distances.y);
        dir_from_sph = normalize(pos);
    }

    // Calculate spherical coordinates
    float lon = atan2(dir_from_sph.x, -dir_from_sph.z) / (2 * PI) + 0.5;
    float lat = acos(dir_from_sph.y) / PI;

    float chan = central_horizontal_angle / (PI * 2.0);

    // Normalize [0, 2π] to [0, 1]
    float uhan = 0.5 + chan / 2.0;
    float lhan = 0.5 - chan / 2.0;

    // Normalize [-π/2, π/2] to [0, 1]
    float uvan = upper_vertical_angle / PI + 0.5;
    float lvan = lower_vertical_angle / PI + 0.5;

    if (lat < uvan && lat > lvan && lon < uhan && lon > lhan) {
        // Map configured display region to whole texture
        float2 ll_offset = float2(lhan, lvan);
        float2 ll_extent = float2(uhan - lhan, uvan - lvan);
        float2 sample_point = (float2(lon, lat) - ll_offset) / ll_extent;

        float2 uv_sub = sample_point * post_transform.zw + post_transform.xy;

#ifdef DXR_LAYERED
        float4 color = layer_tex.Sample(layer_samp, float3(uv_sub, array_params.x));
#else
        float4 color = layer_tex.Sample(layer_samp, uv_sub);
#endif
        return color * color_scale + color_bias;
    } else {
        // OUTSIDE the layer's angular extent. Same rule as the sphere miss
        // above, and this is the one that bites in practice.
        //
        // An equirect2 layer paints only the sphere section it covers, so it is
        // a SUB-RECT of the tile: it may mark the tile composited, but it can
        // never be the tile's base. A layer with no
        // BLEND_TEXTURE_SOURCE_ALPHA_BIT resolves to OPAQUE_COVER, whose blend
        // state has blending DISABLED -- so a `return float4(0,0,0,0)` here is
        // not a no-op, it OVERWRITES the destination with transparent black and
        // erases whatever the tile already held, everywhere the section does
        // not reach. On a narrow centralHorizontalAngle that is most of the
        // tile: a sub-rect layer behaving as a full-tile base, which is exactly
        // what the rule forbids.
        //
        // `discard` is correct in every mode, not just that one: under both
        // blended modes a source of (0,0,0,0) was already a no-op, so nothing
        // that composited before composites differently now.
        discard;
        return float4(0, 0, 0, 0);
    }
}
)";
