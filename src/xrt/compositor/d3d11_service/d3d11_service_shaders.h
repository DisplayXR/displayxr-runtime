// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Embedded HLSL shaders for D3D11 service compositor layer rendering.
 * @author David Fattal
 * @ingroup comp_d3d11_service
 */

#pragma once

//! Constant buffer layout for quad layers (must match HLSL)
struct QuadLayerConstants
{
	float mvp[16];           // Model-view-projection matrix
	float post_transform[4]; // xy = offset, zw = scale (UV)
	float color_scale[4];    // RGBA multiplier
	float color_bias[4];     // RGBA offset
};

//! Constant buffer layout for cylinder layers
struct CylinderLayerConstants
{
	float mvp[16];           // Model-view-projection matrix
	float post_transform[4]; // xy = offset, zw = scale (UV)
	float color_scale[4];    // RGBA multiplier
	float color_bias[4];     // RGBA offset
	float radius;            // Cylinder radius (meters)
	float central_angle;     // Angular extent (radians)
	float aspect_ratio;      // Height / arc_length
	float padding;
};

//! Constant buffer layout for equirect2 layers
struct Equirect2LayerConstants
{
	float mv_inverse[16];             // Inverse model-view matrix
	float post_transform[4];          // xy = offset, zw = scale (UV)
	float color_scale[4];             // RGBA multiplier
	float color_bias[4];              // RGBA offset
	float to_tangent[4];              // UV to tangent space conversion
	float radius;                     // Sphere radius
	float central_horizontal_angle;   // Horizontal angle
	float upper_vertical_angle;       // Upper vertical angle
	float lower_vertical_angle;       // Lower vertical angle
};

//! Vertex shader for quad layers - positioned 3D quad
static const char *quad_vs_hlsl = R"(
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

    // Flip Y for OpenXR coordinate system
    pos.y = -pos.y;

    // Transform position by MVP (which includes quad size scaling)
    output.position = mul(mvp, float4(pos, 0.0, 1.0));

    // Apply UV transform for sub-image
    output.uv = in_uv * post_transform.zw + post_transform.xy;

    return output;
}
)";

//! Pixel shader for quad layers
static const char *quad_ps_hlsl = R"(
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

//! Vertex shader for cylinder layers - tessellated curved surface
static const char *cylinder_vs_hlsl = R"(
cbuffer LayerCB : register(b0)
{
    float4x4 mvp;
    float4 post_transform;
    float4 color_scale;
    float4 color_bias;
    float radius;
    float central_angle;
    float aspect_ratio;
    float padding;
};

struct VS_OUTPUT
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

// Number of subdivisions for cylinder tessellation
static const uint SUBDIVISION_COUNT = 64;

float2 get_uv_for_vertex(uint vertex_id)
{
    // One edge on either end and one between each subdivision
    uint edges = SUBDIVISION_COUNT + 1;

    // Goes from [0 .. edges], two vertices per edge
    uint x_idx = vertex_id / 2;

    // Goes from [0 .. 1] every other vertex
    uint y_idx = vertex_id % 2;

    // [0 .. 1] for x
    float x = float(x_idx) / float(edges);

    // [0 .. 1] for y
    float y = float(y_idx);

    return float2(x, y);
}

float3 get_position_for_uv(float2 uv)
{
    // [0 .. 1] to [-0.5 .. 0.5]
    float mixed_u = uv.x - 0.5;

    // [-0.5 .. 0.5] to [-angle/2 .. angle/2]
    float a = mixed_u * central_angle;

    // [0 .. 1] to [0.5 .. -0.5] (flip for OpenXR)
    float mixed_v = 0.5 - uv.y;

    // Total height per spec
    float total_height = (central_angle * radius) / aspect_ratio;

    // Calculate position on cylinder surface
    float x = sin(a) * radius;   // At angle zero, x = 0
    float y = total_height * mixed_v;
    float z = -cos(a) * radius;  // At angle zero, z = -radius

    return float3(x, y, z);
}

VS_OUTPUT VSMain(uint vertex_id : SV_VertexID)
{
    VS_OUTPUT output;

    // Get raw UV for this vertex
    float2 raw_uv = get_uv_for_vertex(vertex_id);

    // Get 3D position on cylinder surface
    float3 pos = get_position_for_uv(raw_uv);

    // Transform to clip space
    output.position = mul(mvp, float4(pos, 1.0));

    // Apply UV transform for sub-image
    output.uv = raw_uv * post_transform.zw + post_transform.xy;

    return output;
}
)";

//! Pixel shader for cylinder layers (same as quad)
static const char *cylinder_ps_hlsl = R"(
cbuffer LayerCB : register(b0)
{
    float4x4 mvp;
    float4 post_transform;
    float4 color_scale;
    float4 color_bias;
    float radius;
    float central_angle;
    float aspect_ratio;
    float padding;
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

    // Go from [0 .. 1] to [-1 .. 1] for fullscreen NDC
    float2 pos = uv * 2.0 - 1.0;
    output.position = float4(pos, 0.0, 1.0);

    return output;
}
)";

//! Pixel shader for equirect2 layers - spherical UV mapping
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
};

Texture2D layer_tex : register(t0);
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

        if (distances.y < 0) {
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

        float4 color = layer_tex.Sample(layer_samp, uv_sub);
        return color * color_scale + color_bias;
    } else {
        return float4(0, 0, 0, 0);
    }
}
)";

//! Vertex shader for cube layers - fullscreen with view direction
static const char *cube_vs_hlsl = R"(
cbuffer LayerCB : register(b0)
{
    float4x4 mv_inverse;
    float4 post_transform;
    float4 color_scale;
    float4 color_bias;
    float4 to_tangent;
};

struct VS_OUTPUT
{
    float4 position : SV_Position;
    float3 view_dir : TEXCOORD0;
};

static const float2 positions[4] = {
    float2(0, 0),
    float2(0, 1),
    float2(1, 0),
    float2(1, 1),
};

VS_OUTPUT VSMain(uint vertex_id : SV_VertexID)
{
    VS_OUTPUT output;

    float2 uv = positions[vertex_id % 4];

    // [0 .. 1] to tangent lengths (at unit Z)
    float2 tangent_factors = uv * to_tangent.zw + to_tangent.xy;

    // View direction on unit plane, flip Y for OpenXR
    float3 view_dir_view = float3(tangent_factors.x, -tangent_factors.y, -1);

    // Transform to model space
    output.view_dir = mul((float3x3)mv_inverse, view_dir_view);

    // Fullscreen quad
    float2 pos = uv * 2.0 - 1.0;
    output.position = float4(pos, 0.0, 1.0);

    return output;
}
)";

//! Pixel shader for cube layers - cubemap sampling
static const char *cube_ps_hlsl = R"(
cbuffer LayerCB : register(b0)
{
    float4x4 mv_inverse;
    float4 post_transform;
    float4 color_scale;
    float4 color_bias;
    float4 to_tangent;
};

TextureCube layer_tex : register(t0);
SamplerState layer_samp : register(s0);

struct VS_OUTPUT
{
    float4 position : SV_Position;
    float3 view_dir : TEXCOORD0;
};

float4 PSMain(VS_OUTPUT input) : SV_Target
{
    float3 dir = normalize(input.view_dir);
    float4 color = layer_tex.Sample(layer_samp, dir);
    return color * color_scale + color_bias;
}
)";

//! Constant buffer layout for projection blit (SRGB conversion)
struct BlitConstants
{
	float src_rect[4];       // x, y, width, height in pixels
	float dst_offset[2];     // x, y destination offset in pixels
	float src_size[2];       // source texture size
	float dst_size[2];       // destination texture size
	float convert_srgb;      // 1.0 if source is SRGB, 0.0 otherwise; 2.0 = solid color mode
	float quad_mode;         // > 0.5: use quad_corners instead of dst_offset + dst_rect_wh
	float dst_rect_wh[2];   // destination quad width, height (0 = use src_rect.zw)
	float corner_radius;    // corner radius as fraction of quad height (0 = sharp)
	float corner_aspect;    // width/height aspect ratio for circular corners.
	                        // Sign encodes which corners: >0 = top both, <0 = see corner_radius sign.
	                        // When corner_radius > 0 && corner_aspect > 0: top-left + top-right
	                        // When corner_radius > 0 && corner_aspect < 0: top-right only
	                        // When corner_radius < 0: bottom-left + bottom-right (abs values used)
	// Perspective quad corners packed as two float4s (avoids HLSL array padding).
	// TL = top-left, BL = bottom-left, TR = top-right, BR = bottom-right.
	float quad_corners_01[4]; // [TL.x, TL.y, BL.x, BL.y]
	float quad_corners_23[4]; // [TR.x, TR.y, BR.x, BR.y]
	// Per-corner W for perspective-correct UV interpolation.
	// W = depth from eye to corner (eye_z - corner_z). Required because
	// projecting a 3D quad to 2D produces a trapezoid — linear UV interpolation
	// distorts straight lines without perspective correction.
	float quad_w[4];          // [TL_w, BL_w, TR_w, BR_w]
	float edge_feather;       // feather width as fraction of quad height (0 = off)
	float glow_intensity;     // 0 = off, 0-1 = glow strength
	float glow_extent;        // glow margin as fraction of oversized quad
	float glow_falloff;       // Gaussian tightness (e.g. 3.0)
	float glow_color[4];      // RGB + unused alpha (float4 for HLSL alignment)
	// Phase 2.K: per-corner SV_Position.z value, normalised to [0, 1] (closer
	// to eye = smaller value, LESS depth-test). Window content sets all 4 to
	// (eye_z - window_z) / WORKSPACE_DEPTH_FAR_M; rotated quads use per-corner
	// (eye_z - corner_world_z) / WORKSPACE_DEPTH_FAR_M. Chrome biases by a
	// small epsilon toward the eye so it occludes its own window's content
	// while still depth-testing against other windows. Set to 0 (front) for
	// passes that bind depth_disabled (background, launcher, glow halos).
	float corner_depth_ndc[4]; // [TL_d, BL_d, TR_d, BR_d]
	// Phase 2.K Commit 8.C: per-slot multiplier on the final fragment alpha.
	// Used by chrome blits to fade in/out on hover. Default 1.0 = passthrough
	// (every blit site that doesn't set this leaves the discarded-buffer
	// memory; the shader saturates it via saturate() so values outside [0,1]
	// don't clip the geometry). Three padding floats keep the cb 16-byte
	// aligned for HLSL.
	float chrome_alpha;
	float _pad_chrome[3];
};

//! Vertex shader for projection blit - draws a quad at specified destination
static const char *blit_vs_hlsl = R"(
cbuffer BlitCB : register(b0)
{
    float4 src_rect;       // x, y, width, height in pixels
    float2 dst_offset;     // x, y destination offset in pixels
    float2 src_size;       // source texture size
    float2 dst_size;       // destination texture size
    float convert_srgb;    // 1.0 if source is SRGB; 2.0 = solid color mode
    float quad_mode;       // > 0.5: use quad_corners for perspective quad
    float2 dst_rect_wh;   // destination quad width, height (0 = use src_rect.zw)
    float corner_radius;  // corner radius fraction (0=sharp, >0=top, <0=bottom)
    float corner_aspect;  // w/h aspect (>0=both top, <0=top-right only)
    float4 quad_corners_01; // TL.xy, BL.xy (packed as float4 to avoid HLSL array padding)
    float4 quad_corners_23; // TR.xy, BR.xy
    float4 quad_w;          // per-corner W for perspective-correct interpolation: TL, BL, TR, BR
    float edge_feather;     // feather width as fraction of quad height (0 = off)
    float glow_intensity;   // 0 = off, 0-1 = glow strength
    float glow_extent;      // glow margin as fraction of oversized quad
    float glow_falloff;     // Gaussian tightness
    float4 glow_color;      // RGB + unused alpha
    float4 corner_depth_ndc; // Phase 2.K: per-corner depth in [0,1] for SV_Position.z (TL,BL,TR,BR)
    float4 chrome_alpha;     // Phase 2.K Commit 8.C: x = alpha multiplier; yzw padding
};

struct VS_OUTPUT
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float2 quad_uv : TEXCOORD1;  // 0-1 position within the quad (for corner rounding)
};

// Quad vertex positions (also used as UV coords): TL, BL, TR, BR
static const float2 positions[4] = {
    float2(0, 0),
    float2(0, 1),
    float2(1, 0),
    float2(1, 1),
};

VS_OUTPUT VSMain(uint vertex_id : SV_VertexID)
{
    VS_OUTPUT output;

    float2 uv = positions[vertex_id % 4];
    uint vid = vertex_id % 4;

    float2 dst_pos;
    float w = 1.0;
    if (quad_mode > 0.5) {
        // Perspective quad mode: use pre-projected corner positions.
        // Unpack from two float4s: TL=01.xy, BL=01.zw, TR=23.xy, BR=23.zw
        float2 corners[4] = {
            quad_corners_01.xy,  // TL (vertex 0)
            quad_corners_01.zw,  // BL (vertex 1)
            quad_corners_23.xy,  // TR (vertex 2)
            quad_corners_23.zw   // BR (vertex 3)
        };
        float ws[4] = { quad_w.x, quad_w.y, quad_w.z, quad_w.w };
        dst_pos = corners[vid];
        w = ws[vid];
    } else {
        // Axis-aligned mode (existing path)
        float2 quad_size = (dst_rect_wh.x > 0) ? dst_rect_wh : src_rect.zw;
        dst_pos = dst_offset + uv * quad_size;
    }

    // Convert to NDC [-1, 1]
    float2 ndc = (dst_pos / dst_size) * 2.0 - 1.0;
    ndc.y = -ndc.y;  // Flip Y for D3D

    // Phase 2.K: per-corner depth for hardware depth test (LESS, [0,1]).
    // CPU encodes (eye_z - corner_z) / WORKSPACE_DEPTH_FAR_M for window
    // content, with a small bias toward the eye for chrome. After
    // perspective divide the rasterizer gives SV_Position.z = corner_depth_ndc.
    // saturate() is defense-in-depth: blit sites that don't initialize the
    // field (most overlay / chrome paths in CPU code) leave it as whatever
    // memory was in the discarded constant buffer. Without saturate, NaN or
    // out-of-range values cause the viewport z-clip to drop the vertex even
    // when depth_disabled is bound — observed as missing tiles / mismatched
    // disparity in the empty workspace logo and standalone hot-switch.
    float depths[4] = { corner_depth_ndc.x, corner_depth_ndc.y, corner_depth_ndc.z, corner_depth_ndc.w };
    float depth_ndc = saturate(depths[vid]);

    // Set w for perspective-correct UV interpolation.
    // w=1 for axis-aligned (affine), w=depth for perspective quads.
    // D3D11 rasterizer automatically divides interpolated attributes by w.
    output.position = float4(ndc * w, depth_ndc * w, w);

    // Calculate source UV — always from src_rect (independent of dest size)
    float2 src_pos = src_rect.xy + uv * src_rect.zw;
    output.uv = src_pos / src_size;

    // Pass through raw 0-1 quad position for corner rounding
    output.quad_uv = uv;

    return output;
}
)";

//! Pixel shader for projection blit with SRGB conversion
static const char *blit_ps_hlsl = R"(
cbuffer BlitCB : register(b0)
{
    float4 src_rect;
    float2 dst_offset;
    float2 src_size;
    float2 dst_size;
    float convert_srgb;
    float quad_mode;
    float2 dst_rect_wh;
    float corner_radius;  // corner radius fraction (0=sharp, >0=top, <0=bottom)
    float corner_aspect;  // w/h aspect (>0=both top, <0=top-right only)
    float4 quad_corners_01;
    float4 quad_corners_23;
    float4 quad_w;
    float edge_feather;
    float glow_intensity;
    float glow_extent;
    float glow_falloff;
    float4 glow_color;
    float4 corner_depth_ndc; // Phase 2.K: per-corner depth (unused in PS but keeps cb layout in sync)
    float4 chrome_alpha;     // Phase 2.K Commit 8.C: x = alpha multiplier
};

Texture2D src_tex : register(t0);
SamplerState src_samp : register(s0);

struct VS_OUTPUT
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float2 quad_uv : TEXCOORD1;
};

// Linear to sRGB encode (gamma compression)
// When sampling from an SRGB SRV, the GPU auto-linearizes the values.
// We need to re-encode to sRGB before writing to the non-SRGB stereo texture
// so the weaver receives sRGB-encoded values (matching SR Hydra's behavior).
float3 linear_to_srgb(float3 linear_color)
{
    // Use SR Hydra's gamma exponent (2.333) for consistent color output
    return pow(abs(linear_color), 1.0 / 2.333);
}

float4 PSMain(VS_OUTPUT input) : SV_Target
{
    float2 uv01 = input.quad_uv;

    // --- Glow mode (convert_srgb >= 3.0): soft halo around focused window ---
    if (convert_srgb > 2.5) {
        // Separate X/Y extents: glow_extent = X fraction, edge_feather = Y fraction.
        // When edge_feather == 0, fall back to glow_extent for both (legacy).
        float ext_x = glow_extent;
        float ext_y = (edge_feather > 0.001) ? edge_feather : ext_x;
        if (ext_x < 0.001) discard;
        // Distance from inner boundary (positive = outside inner rect)
        float dx = max(ext_x - uv01.x, uv01.x - (1.0 - ext_x));
        float dy = max(ext_y - uv01.y, uv01.y - (1.0 - ext_y));
        // Normalize both axes to 0-1 range within the margin
        float ndx = dx / ext_x;
        float ndy = dy / ext_y;
        float dist;
        if (ndx > 0 && ndy > 0)
            dist = length(float2(ndx, ndy));
        else
            dist = max(max(ndx, ndy), 0.0);
        if (dist <= 0.0) discard;  // inside inner rect — content draws on top
        float falloff = exp(-glow_falloff * dist * dist);
        float a = glow_intensity * falloff;
        return float4(glow_color.rgb * a, a);  // premultiplied alpha
    }

    // --- Corner rounding with smooth alpha falloff ---
    // corner_radius: fraction of height. >0 = top corners, <0 = bottom corners.
    // corner_aspect: abs = w/h aspect ratio. sign selects corners.
    float corner_alpha = 1.0;
    if (corner_radius != 0) {
        float ry = abs(corner_radius);
        float aspect = abs(corner_aspect);
        if (aspect < 0.001) aspect = 10.0;
        float rx = ry / aspect;
        // corner_radius > 0, corner_aspect > 0: top-left + top-right
        // corner_radius > 0, corner_aspect < 0: top-right only
        // corner_radius < 0, corner_aspect > 0: bottom-left + bottom-right
        // corner_radius < 0, corner_aspect < 0: ALL four corners
        bool all_corners = (corner_radius < 0 && corner_aspect < 0);
        bool do_top = (corner_radius > 0 || all_corners);
        bool do_bottom = (corner_radius < 0);
        bool do_top_left = do_top && (corner_aspect > 0 || all_corners);
        bool do_top_right = do_top;
        bool do_bottom_left = (corner_radius < 0);
        bool do_bottom_right = (corner_radius < 0);
        float corner_dist = -1.0;  // < 0 means not in a corner region
        if (do_top_left && uv01.x < rx && uv01.y < ry)
            corner_dist = length(float2((rx - uv01.x) / rx, (ry - uv01.y) / ry));
        if (do_top_right && uv01.x > 1.0 - rx && uv01.y < ry)
            corner_dist = length(float2((uv01.x - (1.0 - rx)) / rx, (ry - uv01.y) / ry));
        if (do_bottom_left && uv01.x < rx && uv01.y > 1.0 - ry)
            corner_dist = length(float2((rx - uv01.x) / rx, (uv01.y - (1.0 - ry)) / ry));
        if (do_bottom_right && uv01.x > 1.0 - rx && uv01.y > 1.0 - ry)
            corner_dist = length(float2((uv01.x - (1.0 - rx)) / rx, (uv01.y - (1.0 - ry)) / ry));
        if (corner_dist >= 0.0) {
            if (corner_dist > 1.0) discard;
            // Smooth falloff in the last pixel-equivalent of the corner
            float feather_band = (edge_feather > 0.0) ? edge_feather / ry : 0.02;
            corner_alpha = saturate((1.0 - corner_dist) / feather_band);
        }
    }

    // --- Edge feathering: smooth alpha fade at quad boundaries ---
    float feather_alpha = 1.0;
    if (edge_feather > 0.0) {
        float d = min(min(uv01.x, 1.0 - uv01.x), min(uv01.y, 1.0 - uv01.y));
        feather_alpha = saturate(d / edge_feather);
    }
    float alpha = corner_alpha * feather_alpha;

    // Phase 2.K Commit 8.C: optional alpha multiplier for chrome fade-in/out.
    // Inverted semantic: chrome_alpha.x = 0 means "no fade" (full opacity).
    // chrome_alpha.x ∈ (0, 1] means "fade by that amount" (1 - x = output
    // alpha multiplier). This way uninit / discarded-buffer garbage in
    // chrome_alpha.x simply lands at "≈ 0 means no fade" most of the time
    // (negative garbage clamps to 0; positive garbage > 1 clamps to 1 →
    // fully hidden, but those blits aren't reached because CPU skips them
    // when alpha is fully hidden — so the bad case is rare).
    // Chrome blits explicitly set chrome_alpha.x = 1 - actual_alpha so that
    // 0 → no fade, 1 → fully transparent.
    float fade = saturate(chrome_alpha.x);
    float a_mul = 1.0 - fade;

    // Solid color mode: convert_srgb >= 2.0 outputs src_rect.rgb as solid color
    if (convert_srgb > 1.5)
        return float4(src_rect.xyz, alpha * a_mul);

    float4 color = src_tex.Sample(src_samp, input.uv);
    return float4(color.rgb, color.a * alpha * a_mul);
}
)";
