// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Per-view camera selection for non-projection composition layers.
 * @author David Fattal
 * @ingroup comp_util
 *
 * See comp_layer_view_camera.h for the invariant this implements (#1580).
 */

#include "util/comp_layer_view_camera.h"
#include "util/comp_layer_accum.h"
#include "util/u_logging.h"

#include "xrt/xrt_compiler.h" // ARRAY_SIZE

#include "dxr_view_math.h" // dxr_display3d_compute_fov — the shared Kooima core

#include <stddef.h>

//! Legacy placeholder half-baseline, metres. Kept only for branch (c).
#define COMP_LAYER_VIEW_CAMERA_FALLBACK_HALF_BASELINE_M 0.032f

//! Legacy placeholder symmetric half-FOV, radians (~45 degrees).
#define COMP_LAYER_VIEW_CAMERA_FALLBACK_HALF_FOV_RAD 0.785f

/*!
 * The frame's first projection-class layer that covers @p view_index.
 *
 * Projection, projection+depth and 3D-zone layers all start with a
 * @ref xrt_layer_projection_data at offset 0 of the layer-data union, so one
 * accessor serves all three.
 */
static const struct xrt_layer_projection_view_data *
find_projection_view(const struct comp_layer_accum *accum, uint32_t view_index)
{
	if (accum == NULL || view_index >= XRT_MAX_VIEWS) {
		return NULL;
	}

	for (uint32_t i = 0; i < accum->layer_count; i++) {
		const struct xrt_layer_data *data = &accum->layers[i].data;

		switch (data->type) {
		case XRT_LAYER_PROJECTION:
		case XRT_LAYER_PROJECTION_DEPTH:
		case XRT_LAYER_ZONE_3D: break;
		default: continue;
		}

		// view_count is 0 on submitters that never filled it in; those are
		// always stereo.
		uint32_t view_count = data->view_count != 0 ? data->view_count : 2;
		if (view_index >= view_count) {
			continue; // A later layer may cover this view.
		}

		return &data->proj.v[view_index];
	}

	return NULL;
}

/*!
 * Branch (b)'s render eye: the state tracker's two eye-set rules, verbatim.
 *
 * See comp_layer_view_camera_select_eyes() in the header for why each exists
 * and where its twin lives in the state tracker / IPC server.
 *
 * @return false when there is no eye to render from (skip branch (b)).
 */
static bool
resolve_render_eye(const struct xrt_eye_positions *eyes,
                   uint32_t view_index,
                   uint32_t active_view_count,
                   struct xrt_vec3 *out_eye)
{
	if (eyes == NULL || eyes->count == 0) {
		return false;
	}

	const uint32_t cap = (uint32_t)ARRAY_SIZE(eyes->eyes);
	uint32_t n = eyes->count;
	if (n > cap) {
		n = cap;
	}

	const uint32_t avc = active_view_count != 0 ? active_view_count : n;

	// Rule 1: mono collapse — one view, many eyes => the centroid.
	if (avc == 1 && n >= 2) {
		struct xrt_vec3 c = {0.0f, 0.0f, 0.0f};
		for (uint32_t i = 0; i < n; i++) {
			c.x += eyes->eyes[i].x;
			c.y += eyes->eyes[i].y;
			c.z += eyes->eyes[i].z;
		}
		const float inv = 1.0f / (float)n;
		out_eye->x = c.x * inv;
		out_eye->y = c.y * inv;
		out_eye->z = c.z * inv;
		return true;
	}

	// Rule 2: view i renders from eye i; surplus views reuse the last eye.
	const uint32_t idx = view_index < n ? view_index : n - 1;
	out_eye->x = eyes->eyes[idx].x;
	out_eye->y = eyes->eyes[idx].y;
	out_eye->z = eyes->eyes[idx].z;
	return true;
}

bool
comp_layer_view_camera_select_eyes(const struct comp_layer_accum *accum,
                                   uint32_t view_index,
                                   const struct xrt_eye_positions *eyes,
                                   uint32_t active_view_count,
                                   const struct xrt_vec3 *canvas_center,
                                   float canvas_w_m,
                                   float canvas_h_m,
                                   struct comp_layer_view_camera *out)
{
	struct xrt_vec3 eye = {0.0f, 0.0f, 0.0f};
	const bool have_eye = resolve_render_eye(eyes, view_index, active_view_count, &eye);

	return comp_layer_view_camera_select_ex(accum, view_index, have_eye ? &eye : NULL, canvas_center, canvas_w_m,
	                                        canvas_h_m, out);
}

bool
comp_layer_view_camera_select_ex(const struct comp_layer_accum *accum,
                                 uint32_t view_index,
                                 const struct xrt_vec3 *eye_pos,
                                 const struct xrt_vec3 *canvas_center,
                                 float canvas_w_m,
                                 float canvas_h_m,
                                 struct comp_layer_view_camera *out)
{
	if (out == NULL) {
		return false;
	}

	/*
	 * (a) The app's own camera. data.proj.v[i].pose and data.quad.pose come
	 * out of the SAME handle_space() call in the state tracker, so they are
	 * already one head-relative space — no re-basing, no new plumbing.
	 */
	const struct xrt_layer_projection_view_data *vd = find_projection_view(accum, view_index);
	if (vd != NULL) {
		out->pose = vd->pose;
		out->fov = vd->fov;
		out->source = COMP_LAYER_VIEW_CAMERA_FROM_PROJECTION;
		return true;
	}

	/*
	 * (b) No projection layer this frame (a quad-only frame). Synthesize the
	 * camera the state tracker would have handed out: the DP's eye with
	 * identity orientation, and the Kooima off-axis FOV from the SHARED core
	 * — the same dxr_display3d_compute_fov() oxr_session.c runs, so a quad
	 * lands where a projection layer at the same pose would have.
	 *
	 * The FOV is a function of the eye relative to the CANVAS centre; the
	 * pose is expressed in the layer space. Those two origins differ whenever
	 * the app's window is off the display centre, hence the rebase here and
	 * the untouched pose below.
	 */
	if (eye_pos != NULL && canvas_w_m > 0.0f && canvas_h_m > 0.0f) {
		dxr_vec3 eye_canvas = {eye_pos->x, eye_pos->y, eye_pos->z};
		if (canvas_center != NULL) {
			eye_canvas.x -= canvas_center->x;
			eye_canvas.y -= canvas_center->y;
			eye_canvas.z -= canvas_center->z;
		}

		dxr_fov fov = dxr_display3d_compute_fov(eye_canvas, canvas_w_m, canvas_h_m);

		out->pose.orientation = (struct xrt_quat)XRT_QUAT_IDENTITY;
		out->pose.position = *eye_pos;
		out->fov.angle_left = fov.angle_left;
		out->fov.angle_right = fov.angle_right;
		out->fov.angle_up = fov.angle_up;
		out->fov.angle_down = fov.angle_down;
		out->source = COMP_LAYER_VIEW_CAMERA_FROM_DISPLAY3D;
		return true;
	}

	/*
	 * (c) Neither. Keep drawing with the historical placeholder rather than
	 * dropping the layer, but say so ONCE per process — never per frame
	 * (docs/reference/debug-logging.md).
	 */
	{
		static bool warned = false;
		if (!warned) {
			warned = true;
			U_LOG_W(
			    "#1580: no projection layer and no %s — composing 3D-positioned layers with "
			    "the legacy placeholder camera (+-%.0f mm, symmetric +-%.0f deg); quads may "
			    "not register with projection content",
			    eye_pos == NULL ? "eye position" : "canvas metrics",
			    (double)(COMP_LAYER_VIEW_CAMERA_FALLBACK_HALF_BASELINE_M * 1000.0f),
			    (double)(COMP_LAYER_VIEW_CAMERA_FALLBACK_HALF_FOV_RAD * 57.29578f));
		}
	}

	out->pose.orientation = (struct xrt_quat)XRT_QUAT_IDENTITY;
	out->pose.position.x = view_index == 0 ? -COMP_LAYER_VIEW_CAMERA_FALLBACK_HALF_BASELINE_M
	                                       : COMP_LAYER_VIEW_CAMERA_FALLBACK_HALF_BASELINE_M;
	out->pose.position.y = 0.0f;
	out->pose.position.z = 0.0f;
	out->fov.angle_left = -COMP_LAYER_VIEW_CAMERA_FALLBACK_HALF_FOV_RAD;
	out->fov.angle_right = COMP_LAYER_VIEW_CAMERA_FALLBACK_HALF_FOV_RAD;
	out->fov.angle_up = COMP_LAYER_VIEW_CAMERA_FALLBACK_HALF_FOV_RAD;
	out->fov.angle_down = -COMP_LAYER_VIEW_CAMERA_FALLBACK_HALF_FOV_RAD;
	out->source = COMP_LAYER_VIEW_CAMERA_FALLBACK;
	return false;
}

bool
comp_layer_view_camera_select(const struct comp_layer_accum *accum,
                              uint32_t view_index,
                              const struct xrt_vec3 *eye_pos,
                              float canvas_w_m,
                              float canvas_h_m,
                              struct comp_layer_view_camera *out)
{
	return comp_layer_view_camera_select_ex(accum, view_index, eye_pos, NULL, canvas_w_m, canvas_h_m, out);
}
