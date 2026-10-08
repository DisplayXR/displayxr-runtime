// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_lift app rig (ADR-048 Addendum A) - see u_lift_rig.h.
 * @ingroup aux_util
 */

#include "util/u_lift_rig.h"

// displayxr-common's shared rig core: the same functions xrLocateViews runs.
#include "dxr_view_math.h"

#include <math.h>
#include <string.h>

#define U_LIFT_RIG_MAX_EYES 8

static float
clampf(float v, float lo, float hi, float dflt, bool *clamped)
{
	if (!isfinite(v)) {
		*clamped = true;
		return dflt;
	}
	if (v < lo) {
		*clamped = true;
		return lo;
	}
	if (v > hi) {
		*clamped = true;
		return hi;
	}
	return v;
}

bool
u_lift_rig_sanitize(struct u_lift_rig *rig)
{
	bool clamped = false;
	if (rig->type != U_LIFT_RIG_DISPLAY && rig->type != U_LIFT_RIG_CAMERA) {
		memset(rig, 0, sizeof(*rig));
		rig->type = U_LIFT_RIG_NONE;
		return false;
	}
	rig->ipd_factor = clampf(rig->ipd_factor, 0.0f, 1.0e4f, 1.0f, &clamped);
	rig->parallax_factor = clampf(rig->parallax_factor, 0.0f, 1.0e4f, 1.0f, &clamped);
	rig->perspective_factor = clampf(rig->perspective_factor, 0.1f, 10.0f, 1.0f, &clamped);
	rig->inv_convergence_distance = clampf(rig->inv_convergence_distance, 0.0f, 20.0f, 0.0f, &clamped);
	// tan of half of a [0.01, 3.13] rad full vertical FOV.
	rig->half_tan_vfov = clampf(rig->half_tan_vfov, tanf(0.005f), tanf(1.565f), tanf(0.5f), &clamped);
	if (!(rig->m2v > 0.0f)) {
		rig->m2v = 1.0f; // 0 / unset = identity (XR_DXR_view_rig v3)
	} else {
		rig->m2v = clampf(rig->m2v, 0.0001f, 100000.0f, 1.0f, &clamped);
	}
	return clamped;
}

bool
u_lift_rig_apply(const struct u_lift_rig *rig,
                 float nominal_z,
                 float rect_w,
                 float rect_h,
                 const float *in_xyz,
                 uint32_t count,
                 float *out_xyz)
{
	if (rig == NULL || in_xyz == NULL || out_xyz == NULL || count == 0 || count > U_LIFT_RIG_MAX_EYES) {
		return false;
	}
	if (rig->type == U_LIFT_RIG_NONE) {
		if (out_xyz != in_xyz) {
			memmove(out_xyz, in_xyz, (size_t)count * 3 * sizeof(float));
		}
		return true;
	}
	if (!(rect_w > 0.0f) || !(rect_h > 0.0f)) {
		return false;
	}
	const float nz = nominal_z > 0.0f ? nominal_z : 0.5f;
	const dxr_vec3 nominal = {0.0f, 0.0f, nz};
	const dxr_screen screen = {rect_w, rect_h};

	dxr_vec3 eyes[U_LIFT_RIG_MAX_EYES];
	for (uint32_t i = 0; i < count; i++) {
		eyes[i].x = in_xyz[3 * i + 0];
		eyes[i].y = in_xyz[3 * i + 1];
		eyes[i].z = in_xyz[3 * i + 2];
	}

	if (rig->type == U_LIFT_RIG_DISPLAY) {
		dxr_display3d_tunables t = dxr_display3d_default_tunables();
		t.ipd_factor = rig->ipd_factor;
		t.parallax_factor = rig->parallax_factor;
		t.perspective_factor = rig->perspective_factor;
		t.virtual_display_height = rect_h; // m2v = 1: eye_display stays in metres
		dxr_display3d_view views[U_LIFT_RIG_MAX_EYES];
		dxr_display3d_compute_views(eyes, count, &nominal, &screen, &t, /*display_pose*/ NULL,
		                            /*near_offset*/ 0.0f, /*far_offset*/ 0.0f, /*vulkan_flip_y*/ 0, views);
		for (uint32_t i = 0; i < count; i++) {
			out_xyz[3 * i + 0] = views[i].eye_display.x;
			out_xyz[3 * i + 1] = views[i].eye_display.y;
			out_xyz[3 * i + 2] = views[i].eye_display.z;
		}
		return true;
	}

	// Camera rig: the camera's frustum, re-expressed as the physical viewer
	// whose Kooima frustum onto the rect is the same (see the header).
	dxr_camera3d_tunables t = dxr_camera3d_default_tunables();
	t.ipd_factor = rig->ipd_factor;
	t.parallax_factor = rig->parallax_factor;
	t.inv_convergence_distance = rig->inv_convergence_distance;
	t.half_tan_vfov = rig->half_tan_vfov;
	t.m2v = rig->m2v;
	dxr_camera3d_view views[U_LIFT_RIG_MAX_EYES];
	dxr_camera3d_compute_views(eyes, count, &nominal, &screen, &t, /*camera_pose*/ NULL, 0.1f, 100.0f, views);
	const float z0 = rect_h / (2.0f * t.half_tan_vfov);
	const float k = z0 * t.inv_convergence_distance; // physical metres per world unit
	for (uint32_t i = 0; i < count; i++) {
		out_xyz[3 * i + 0] = k * views[i].eye_world.x;
		out_xyz[3 * i + 1] = k * views[i].eye_world.y;
		out_xyz[3 * i + 2] = z0 + k * views[i].eye_world.z;
	}
	return true;
}
