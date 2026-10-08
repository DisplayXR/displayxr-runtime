// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_lift auxiliary depth transform (ADR-048 Addendum A) - see
 *         u_lift_depth.h.
 * @ingroup aux_util
 */

#include "util/u_lift_depth.h"

#include <math.h>
#include <string.h>

static void
identity(float m[16])
{
	memset(m, 0, 16 * sizeof(float));
	m[0] = m[5] = m[10] = m[15] = 1.0f;
}

bool
u_lift_depth_to_display(const float viewpoint[3],
                        const float rect_center[3],
                        float rect_w,
                        float rect_h,
                        float convergence_depth,
                        float out_m[16])
{
	identity(out_m);
	if (viewpoint == NULL || rect_center == NULL || !(rect_w > 0.0f) || !(rect_h > 0.0f) ||
	    !(convergence_depth > 0.0f) || !isfinite(rect_w) || !isfinite(rect_h) || !isfinite(convergence_depth) ||
	    !(viewpoint[2] > 0.0f) || !isfinite(viewpoint[0]) || !isfinite(viewpoint[1]) || !isfinite(viewpoint[2])) {
		return false;
	}
	const float inv = 1.0f / convergence_depth;
	const float cx = viewpoint[0], cy = viewpoint[1], cz = viewpoint[2];
	memset(out_m, 0, 16 * sizeof(float));
	// col 0: coefficient of un*d
	out_m[0] = rect_w * inv;
	// col 1: coefficient of vn*d
	out_m[5] = -rect_h * inv;
	// col 2: coefficient of d
	out_m[8] = (-0.5f * rect_w - cx) * inv;
	out_m[9] = (0.5f * rect_h - cy) * inv;
	out_m[10] = -cz * inv;
	// col 3: constant
	out_m[12] = rect_center[0] + cx;
	out_m[13] = rect_center[1] + cy;
	out_m[14] = rect_center[2] + cz;
	out_m[15] = 1.0f;
	return true;
}

void
u_lift_depth_transform_point(const float m[16], float un, float vn, float d, float out_xyz[3])
{
	const float v[4] = {un * d, vn * d, d, 1.0f};
	for (int r = 0; r < 3; r++) {
		out_xyz[r] = m[0 * 4 + r] * v[0] + m[1 * 4 + r] * v[1] + m[2 * 4 + r] * v[2] + m[3 * 4 + r] * v[3];
	}
}

float
u_lift_depth_decode(float sample, float scale, float offset, bool inverse)
{
	const float d = (scale != 0.0f ? scale : 1.0f) * sample + offset;
	if (!inverse) {
		return d;
	}
	return d != 0.0f ? 1.0f / d : 0.0f;
}
