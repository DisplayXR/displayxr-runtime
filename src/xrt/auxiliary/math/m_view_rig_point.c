// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The view rig's canvas frame and point mapping (XR_DXR_view_rig, #2189).
 * @ingroup aux_math
 */

#include "math/m_view_rig_point.h"
#include "math/m_api.h"

#include "displayxr_math_xrt.h"

void
m_view_rig_canvas_rebase(const struct m_view_rig_canvas *canvas, const struct xrt_vec3 *p_display, struct xrt_vec3 *out)
{
	struct xrt_vec3 delta = {
	    p_display->x - canvas->center.x,
	    p_display->y - canvas->center.y,
	    p_display->z - canvas->center.z,
	};
	if (canvas->has_orientation) {
		struct xrt_quat inv_q;
		math_quat_invert(&canvas->orientation, &inv_q);
		math_quat_rotate_vec3(&inv_q, &delta, out);
	} else {
		*out = delta;
	}
}

void
m_view_rig_map_canvas_point(const struct m_view_rig_canvas *canvas,
                            const struct m_view_rig *rig,
                            const struct xrt_vec3 *p_canvas,
                            struct xrt_vec3 *out)
{
	if (rig == NULL || rig->kind == M_VIEW_RIG_NONE || canvas == NULL || !canvas->valid) {
		*out = *p_canvas;
		return;
	}
	const struct xrt_vec3 nominal = {0.0f, canvas->nominal_y_m, canvas->nominal_z_m};
	const dxr_screen scr = {canvas->width_m, canvas->height_m};
	struct dxr_xrt_view v;
	if (rig->kind == M_VIEW_RIG_CAMERA) {
		const dxr_camera3d_tunables ct = {
		    .ipd_factor = 1.0f,
		    .parallax_factor = 1.0f,
		    .inv_convergence_distance = rig->inv_convergence_distance,
		    .half_tan_vfov = rig->half_tan_vfov,
		    .m2v = rig->m2v,
		};
		dxr_xrt_camera3d_compute_views(p_canvas, 1, &nominal, &scr, &ct, &rig->pose, &v);
	} else {
		const dxr_display3d_tunables dt = {
		    .ipd_factor = 1.0f,
		    .parallax_factor = 1.0f,
		    .perspective_factor = rig->perspective_factor,
		    .virtual_display_height = rig->virtual_display_height,
		};
		dxr_xrt_display3d_compute_views(p_canvas, 1, &nominal, &scr, &dt, &rig->pose, &v);
	}
	*out = v.eye_world;
}

float
m_view_rig_length_scale(const struct m_view_rig_canvas *canvas, const struct m_view_rig *rig)
{
	if (rig == NULL || rig->kind == M_VIEW_RIG_NONE || canvas == NULL || !canvas->valid) {
		return 1.0f;
	}
	if (rig->kind == M_VIEW_RIG_CAMERA) {
		return rig->m2v > 0.0f ? rig->m2v : 1.0f;
	}
	// The core's display-rig eye scale: perspective x m2v, m2v = vH / canvas height.
	const float m2v = canvas->height_m > 0.0f ? rig->virtual_display_height / canvas->height_m : 1.0f;
	return rig->perspective_factor * m2v;
}

void
m_view_rig_map_relation(const struct m_view_rig_canvas *canvas,
                        const struct m_view_rig *rig,
                        const struct xrt_pose *plane_base,
                        const struct xrt_space_relation *in_base,
                        float radius,
                        struct xrt_space_relation *out,
                        float *out_radius)
{
	*out = *in_base;
	if (out_radius != NULL) {
		*out_radius = radius;
	}
	if (rig == NULL || rig->kind == M_VIEW_RIG_NONE || canvas == NULL || !canvas->valid) {
		return;
	}

	// base -> physical display plane.
	struct xrt_pose plane_inv;
	math_pose_invert(plane_base, &plane_inv);
	struct xrt_pose in_display;
	math_pose_transform(&plane_inv, &in_base->pose, &in_display);

	// display -> canvas -> rig (position: the shared core, as for an eye).
	struct xrt_vec3 p_canvas;
	m_view_rig_canvas_rebase(canvas, &in_display.position, &p_canvas);
	m_view_rig_map_canvas_point(canvas, rig, &p_canvas, &out->pose.position);

	// Rotation base -> rig frame: rig * inverse(canvas) * inverse(plane).
	struct xrt_quat canvas_inv = {0.0f, 0.0f, 0.0f, 1.0f};
	if (canvas->has_orientation) {
		math_quat_invert(&canvas->orientation, &canvas_inv);
	}
	struct xrt_quat rig_canvas;
	math_quat_rotate(&rig->pose.orientation, &canvas_inv, &rig_canvas);
	struct xrt_quat total;
	math_quat_rotate(&rig_canvas, &plane_inv.orientation, &total);

	math_quat_rotate(&total, &in_base->pose.orientation, &out->pose.orientation);
	math_quat_normalize(&out->pose.orientation);

	const float s = m_view_rig_length_scale(canvas, rig);
	struct xrt_vec3 lv;
	math_quat_rotate_vec3(&total, &in_base->linear_velocity, &lv);
	out->linear_velocity = (struct xrt_vec3){lv.x * s, lv.y * s, lv.z * s};
	math_quat_rotate_vec3(&total, &in_base->angular_velocity, &out->angular_velocity);
	if (out_radius != NULL) {
		*out_radius = radius * s;
	}
}
