// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Depth-aware cursor on lifted content (ADR-046 Amendment 1).
 * @ingroup aux_util
 */

#include "util/u_lift_cursor.h"

#include <math.h>

bool
u_lift_cursor_relief_z(float sample,
                       bool encoding_inverse,
                       float value_scale,
                       float value_offset,
                       float relief_scale,
                       float relief_offset,
                       float *out_z)
{
	const float d = (value_scale != 0.0f ? value_scale : 1.0f) * sample + value_offset;
	if (!isfinite(d) || d <= 0.0f) {
		return false;
	}
	// INVERSE: the decoded value already is 1 / depth.
	const float inv_depth = encoding_inverse ? d : 1.0f / d;
	const float z = relief_scale * inv_depth + relief_offset;
	if (!isfinite(z)) {
		return false;
	}
	*out_z = z;
	return true;
}

bool
u_lift_cursor_nearest_z(const float *samples,
                        uint32_t count,
                        bool encoding_inverse,
                        float value_scale,
                        float value_offset,
                        float relief_scale,
                        float relief_offset,
                        float *out_z)
{
	bool any = false;
	float best = 0.0f;
	for (uint32_t i = 0; i < count; i++) {
		float z;
		if (!u_lift_cursor_relief_z(samples[i], encoding_inverse, value_scale, value_offset, relief_scale,
		                            relief_offset, &z)) {
			continue;
		}
		if (!any || z > best) {
			best = z;
			any = true;
		}
	}
	if (any) {
		*out_z = best;
	}
	return any;
}

bool
u_lift_cursor_eye_midpoint(const float *viewpoints, uint32_t count, float out_e[3])
{
	if (viewpoints == NULL || count == 0) {
		return false;
	}
	float e[3] = {0.0f, 0.0f, 0.0f};
	for (uint32_t i = 0; i < count; i++) {
		e[0] += viewpoints[3 * i + 0];
		e[1] += viewpoints[3 * i + 1];
		e[2] += viewpoints[3 * i + 2];
	}
	for (int k = 0; k < 3; k++) {
		e[k] /= (float)count;
	}
	if (!isfinite(e[0]) || !isfinite(e[1]) || !isfinite(e[2]) || e[2] <= 0.0f) {
		return false;
	}
	out_e[0] = e[0];
	out_e[1] = e[1];
	out_e[2] = e[2];
	return true;
}

bool
u_lift_cursor_disparity_of_z(const float e[3], float z, float *out_disparity)
{
	if (e[2] <= 0.0f || !isfinite(z) || z >= e[2]) {
		return false;
	}
	// t: the point's distance from E along the display normal, over S's.
	const float t = (e[2] - z) / e[2];
	*out_disparity = 1.0f - 1.0f / t;
	return true;
}

bool
u_lift_cursor_project(const float e[3],
                      float sx,
                      float sy,
                      float disparity,
                      const float *viewpoints,
                      uint32_t count,
                      float *out_xy)
{
	if (viewpoints == NULL || count == 0 || count > U_LIFT_CURSOR_MAX_VIEWS || e[2] <= 0.0f ||
	    !isfinite(disparity) || disparity >= 1.0f) {
		return false;
	}
	// d = 1 - 1/t  ->  t = 1 / (1 - d); C = E + t (S - E), S = (sx, sy, 0).
	const float t = 1.0f / (1.0f - disparity);
	const float c[3] = {e[0] + t * (sx - e[0]), e[1] + t * (sy - e[1]), e[2] * (1.0f - t)};
	for (uint32_t i = 0; i < count; i++) {
		const float *v = &viewpoints[3 * i];
		const float dz = v[2] - c[2];
		if (v[2] <= 0.0f || dz <= 1e-6f) {
			return false; // the viewpoint is not in front of C
		}
		// The ray v -> C crosses z = 0 at k = v.z / (v.z - C.z).
		const float k = v[2] / dz;
		out_xy[2 * i + 0] = v[0] + k * (c[0] - v[0]);
		out_xy[2 * i + 1] = v[1] + k * (c[1] - v[1]);
		if (!isfinite(out_xy[2 * i + 0]) || !isfinite(out_xy[2 * i + 1])) {
			return false;
		}
	}
	return true;
}
