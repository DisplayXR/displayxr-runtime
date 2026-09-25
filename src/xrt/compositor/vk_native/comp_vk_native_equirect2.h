// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Push-constant packing for the vk_native equirect2 draw (#1602).
 * @ingroup comp_vk_native
 *
 * The one piece of the equirect2 port that is Vulkan-specific in FORM: the
 * HLSL twin's cbuffer (d3d_shared/comp_equirect2_shaders.h) is 160 bytes, and
 * the compose pass shares ONE 128-byte push-constant block (the guaranteed
 * maxPushConstantsSize) across every layer type. So the frustum tangents are
 * folded into the inverse model-view's 3x3 — the ray is affine in the
 * fullscreen primitive's uv:
 *
 *   ray(uv) = M * (uv.x*tan_w + tan_l, -(uv.y*tan_h + tan_d), -1)
 *           = A*uv.x + B*uv.y + C,        M = mat3(mv_inverse)
 *
 * and the four sphere scalars ride in the otherwise-constant bottom row. The
 * resulting 16 floats go in the block's `mat4` slot; shaders/equirect2.vert
 * documents the same layout from the reading side.
 *
 * A header, not a static in the renderer, so the pixel test
 * (tests_comp_vk_equirect2_draw.cpp) packs through the SAME code the renderer
 * does: a packing bug and a shader bug are then one test, not zero.
 */

#pragma once

#include "xrt/xrt_defines.h"

#include <float.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Pack one view of one equirect2 layer into the compose pass's `mat4` slot.
 *
 * @param mv_inverse Inverse model-view, column-major (`xrt_matrix_4x4::v`).
 * @param fov        This view's camera FOV — the frustum the rays span.
 * @param radius     `XrCompositionLayerEquirect2KHR::radius`; +INFINITY is
 *                   spelled 0 for the shader, which then skips the sphere
 *                   intersection and uses the ray direction.
 * @param central_horizontal_angle ... and the two vertical angles, verbatim.
 * @param out        16 floats, column-major: columns 0..2 xyz = A, B, C;
 *                   column 3 xyz = the camera position in model space; the w
 *                   row = radius, central_horizontal, upper, lower.
 */
static inline void
comp_vk_native_equirect2_pack(const struct xrt_matrix_4x4 *mv_inverse,
                              const struct xrt_fov *fov,
                              float radius,
                              float central_horizontal_angle,
                              float upper_vertical_angle,
                              float lower_vertical_angle,
                              float out[16])
{
	const float tan_l = tanf(fov->angle_left);
	const float tan_d = tanf(fov->angle_down);
	const float tan_w = tanf(fov->angle_right) - tan_l;
	const float tan_h = tanf(fov->angle_up) - tan_d;

	const float *m = mv_inverse->v;
	for (int k = 0; k < 3; k++) {
		out[0 + k] = m[0 + k] * tan_w;                               // A
		out[4 + k] = -m[4 + k] * tan_h;                              // B
		out[8 + k] = m[0 + k] * tan_l - m[4 + k] * tan_d - m[8 + k]; // C
		out[12 + k] = m[12 + k];                                     // camera
	}
	out[3] = radius > FLT_MAX ? 0.0f : radius;
	out[7] = central_horizontal_angle;
	out[11] = upper_vertical_angle;
	out[15] = lower_vertical_angle;
}

#ifdef __cplusplus
}
#endif
