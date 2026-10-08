// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_lift viewpoint policy (ADR-048) - see u_lift_viewpoint.h.
 * @ingroup aux_util
 */

#include "util/u_lift_viewpoint.h"

#include <math.h>
#include <string.h>

static float
clamp01(float v, float dflt)
{
	if (!isfinite(v)) {
		return dflt;
	}
	return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

void
u_lift_view_control_default(struct u_lift_view_control *vc)
{
	vc->ipd_factor = 1.0f;
	vc->parallax_factor = 1.0f;
	vc->axis_mode = U_LIFT_AXIS_X;
	vc->max_offset_m = 0.0f;
	vc->recenter_mode = U_LIFT_RECENTER_EASE_BACK;
	vc->hold_s = U_LIFT_RECENTER_HOLD_DEFAULT_S;
	vc->tau_s = U_LIFT_RECENTER_TAU_DEFAULT_S;
}

void
u_lift_view_control_sanitize(struct u_lift_view_control *vc)
{
	vc->ipd_factor = clamp01(vc->ipd_factor, 1.0f);
	vc->parallax_factor = clamp01(vc->parallax_factor, 1.0f);
	if (vc->axis_mode < U_LIFT_AXIS_X || vc->axis_mode > U_LIFT_AXIS_XYZ) {
		vc->axis_mode = U_LIFT_AXIS_X;
	}
	if (!isfinite(vc->max_offset_m) || vc->max_offset_m < 0.0f) {
		vc->max_offset_m = 0.0f;
	}
	if (vc->recenter_mode > U_LIFT_RECENTER_EASE_BACK) {
		vc->recenter_mode = U_LIFT_RECENTER_EASE_BACK;
	}
	if (!isfinite(vc->hold_s) || vc->hold_s < 0.0f) {
		vc->hold_s = U_LIFT_RECENTER_HOLD_DEFAULT_S;
	}
	if (!isfinite(vc->tau_s) || vc->tau_s <= 0.0f) {
		vc->tau_s = U_LIFT_RECENTER_TAU_DEFAULT_S;
	}
}

void
u_lift_recenter_reset(struct u_lift_recenter *r)
{
	memset(r, 0, sizeof(*r));
}

void
u_lift_viewpoint_rebase(float *xyz, uint32_t count, const float centre[3])
{
	for (uint32_t i = 0; i < count; i++) {
		xyz[3 * i + 0] -= centre[0];
		xyz[3 * i + 1] -= centre[1];
		xyz[3 * i + 2] -= centre[2];
	}
}

//! Length of @p d over the axes @p axis_mode lets through.
static float
live_len(const float d[3], uint32_t axis_mode)
{
	float s = d[0] * d[0];
	if (axis_mode >= U_LIFT_AXIS_XY) {
		s += d[1] * d[1];
	}
	if (axis_mode >= U_LIFT_AXIS_XYZ) {
		s += d[2] * d[2];
	}
	return sqrtf(s);
}

/*!
 * One step of the ease-back filter on midpoint @p c (in / out). The rendered
 * midpoint is the straight-on viewer (0, 0, nz) plus the head's offset from a
 * REFERENCE that follows the head: after the hold the reference slides to the
 * head, so the camera returns to the scene camera origin; a new movement is an
 * offset from the reference again (temporary look-around). Frame-rate
 * independent: the ease factor per step is 1 - exp(-dt / tau).
 */
static void
recenter_step(const struct u_lift_view_control *vc, struct u_lift_recenter *r, float nz, uint64_t now_ns, float c[3])
{
	const float neutral[3] = {0.0f, 0.0f, nz};
	if (!r->valid) {
		// The reference starts at the straight-on viewer, so the first frames
		// look around exactly as with recentering off.
		r->valid = true;
		r->last_ns = now_ns;
		memcpy(r->ref, neutral, sizeof(r->ref));
		r->beyond_s = 0.0f;
		r->easing = false;
	}
	float dt = now_ns > r->last_ns ? (float)((double)(now_ns - r->last_ns) * 1e-9) : 0.0f;
	if (dt > U_LIFT_RECENTER_MAX_DT_S) {
		dt = U_LIFT_RECENTER_MAX_DT_S;
	}
	r->last_ns = now_ns;
	const float k = 1.0f - expf(-dt / vc->tau_s);

	const float d[3] = {c[0] - r->ref[0], c[1] - r->ref[1], c[2] - r->ref[2]};
	const float len = live_len(d, vc->axis_mode);
	r->beyond_s = len > U_LIFT_RECENTER_THRESHOLD_M ? r->beyond_s + dt : 0.0f;
	if (!r->easing && len > U_LIFT_RECENTER_THRESHOLD_M && r->beyond_s >= vc->hold_s) {
		r->easing = true;
	}
	if (r->easing) {
		for (int i = 0; i < 3; i++) {
			r->ref[i] += d[i] * k;
		}
		const float d2[3] = {c[0] - r->ref[0], c[1] - r->ref[1], c[2] - r->ref[2]};
		if (live_len(d2, vc->axis_mode) < 0.25f * U_LIFT_RECENTER_THRESHOLD_M) {
			r->easing = false; // settled; the next excursion holds again first
			r->beyond_s = 0.0f;
		}
	}
	// Rendered midpoint = the straight-on viewer + the offset from the reference.
	for (int i = 0; i < 3; i++) {
		c[i] = neutral[i] + (c[i] - r->ref[i]);
	}
}

void
u_lift_viewpoint_apply(const struct u_lift_view_control *vc,
                       struct u_lift_recenter *r,
                       float nominal_z,
                       uint64_t now_ns,
                       const float *in_xyz,
                       uint32_t count,
                       float *out_xyz,
                       float *out_baseline_m,
                       float *out_mid)
{
	if (out_baseline_m != NULL) {
		*out_baseline_m = 0.0f;
	}
	if (count == 0 || in_xyz == NULL || out_xyz == NULL) {
		return;
	}
	const uint32_t n = count > U_LIFT_VIEWPOINT_MAX ? U_LIFT_VIEWPOINT_MAX : count;
	const float nz = nominal_z > 0.0f ? nominal_z : U_LIFT_NOMINAL_Z_DEFAULT_M;

	float c[3] = {0.0f, 0.0f, 0.0f};
	for (uint32_t i = 0; i < n; i++) {
		for (int a = 0; a < 3; a++) {
			c[a] += in_xyz[3 * i + a];
		}
	}
	for (int a = 0; a < 3; a++) {
		c[a] /= (float)n;
	}

	// 1. ipd factor: offsets from the midpoint (kept aside: out may alias in).
	float d[3 * U_LIFT_VIEWPOINT_MAX];
	for (uint32_t i = 0; i < n; i++) {
		for (int a = 0; a < 3; a++) {
			d[3 * i + a] = (in_xyz[3 * i + a] - c[a]) * vc->ipd_factor;
		}
	}

	// 2. parallax: lerp the midpoint toward the nominal viewer.
	const float p = vc->parallax_factor;
	c[0] *= p;
	c[1] *= p;
	c[2] = nz + p * (c[2] - nz);

	// 3. axis mask.
	if (vc->axis_mode < U_LIFT_AXIS_XY) {
		c[1] = 0.0f;
	}
	if (vc->axis_mode < U_LIFT_AXIS_XYZ) {
		c[2] = nz;
	}

	// 4. recenter.
	if (r != NULL && vc->recenter_mode != U_LIFT_RECENTER_OFF) {
		recenter_step(vc, r, nz, now_ns, c);
	}

	// 5. clamp the lateral offset.
	if (vc->max_offset_m > 0.0f) {
		for (int a = 0; a < 2; a++) {
			if (c[a] > vc->max_offset_m) {
				c[a] = vc->max_offset_m;
			} else if (c[a] < -vc->max_offset_m) {
				c[a] = -vc->max_offset_m;
			}
		}
	}

	for (uint32_t i = 0; i < n; i++) {
		for (int a = 0; a < 3; a++) {
			out_xyz[3 * i + a] = c[a] + d[3 * i + a];
		}
	}
	if (out_baseline_m != NULL && n >= 2) {
		const float *f = &d[0];
		const float *l = &d[3 * (n - 1)];
		const float dx = l[0] - f[0], dy = l[1] - f[1], dz = l[2] - f[2];
		*out_baseline_m = sqrtf(dx * dx + dy * dy + dz * dz);
	}
	if (out_mid != NULL) {
		memcpy(out_mid, c, sizeof(c));
	}
}
