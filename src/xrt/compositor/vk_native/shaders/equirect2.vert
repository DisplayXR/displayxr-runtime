// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
//
// XR_KHR_composition_layer_equirect2 vertex shader (#1602), the Vulkan twin of
// d3d_shared/comp_equirect2_shaders.h's VSMain and of GL's VS_EQUIRECT2.
//
// Same mechanism: no model geometry and no MVP. A fullscreen primitive carries
// a per-pixel CAMERA RAY in the layer's model space, and the fragment shader
// intersects it with the layer's sphere. The per-view viewport + scissor
// confine it to its tile.
//
// ── THE PUSH-CONSTANT PACKING (one difference in FORM, none in math) ──
//
// The HLSL cbuffer is mv_inverse + to_tangent + four sphere scalars + the
// shared post_transform / colour channels + an array slice = 160 bytes, over
// the 128-byte guaranteed maxPushConstantsSize this pass is sized to. So the
// CPU folds to_tangent INTO mat3(mv_inverse) — the ray is affine in uv:
//
//   ray = mat3(mv_inv) * (uv.x*zw.x + xy.x, -(uv.y*zw.y + xy.y), -1)
//       = A*uv.x + B*uv.y + C
//
// with A = M*(zw.x, 0, 0), B = M*(0, -zw.y, 0), C = M*(xy.x, -xy.y, -1). The
// `mvp` slot of the shared ComposeParams block then holds, column-major:
//
//   eq[0].xyz = A   eq[1].xyz = B   eq[2].xyz = C
//   eq[3].xyz = the camera position in model space (mv_inv's translation)
//   eq[0].w = radius (0 == +INFINITY)   eq[1].w = centralHorizontalAngle
//   eq[2].w = upperVerticalAngle        eq[3].w = lowerVerticalAngle
//
// so the pipeline layout is shared with the projection / zone / quad draws
// and nothing grows. Identical rays to the HLSL's, interpolated identically
// (the ray is affine in uv, so per-vertex evaluation is exact).
//
// ── THE ONE Y DIVERGENCE FROM THE HLSL: clip space ──
//
// uv.y == 0 yields a ray pointing UP (the -tangent.y above), so it must land
// at the TOP of the viewport. D3D and GL NDC +y is the top, hence their
// `1.0 - uv.y * 2.0`; Vulkan NDC +y is the BOTTOM, so here it is
// `uv.y * 2.0 - 1.0` — exactly the fullscreen triangle zone_blit.vert already
// draws. Sampling needs NO flip (unlike GL): a Vulkan image's v == 0 is its
// top row, as a D3D texture's is.

#version 450

layout(push_constant) uniform ComposeParams {
	mat4 eq;          // packed ray basis + camera + sphere scalars, see above
	vec4 src_rect;    // post_transform: xy = offset, zw = scale (UV)
	vec4 params;      // x = array slice (fragment); yzw unused here
	vec4 color_scale;
	vec4 color_bias;
} pc;

layout(location = 0) out vec3 out_camera_position;
layout(location = 1) out vec3 out_camera_ray;

void main()
{
	// Fullscreen triangle over uv [0,2]; the viewport/scissor clip it to the
	// tile, and the rays past uv 1 are simply never rasterised.
	vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);

	out_camera_position = pc.eq[3].xyz;
	out_camera_ray = mat3(pc.eq) * vec3(uv, 1.0);

	gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
