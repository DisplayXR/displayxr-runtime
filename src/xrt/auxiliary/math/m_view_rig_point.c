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

#include <math.h>
#include <string.h>

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

/*!
 * A camera rig's glass, as the equivalent display rig: the shared core's
 * camera -> display conversion (dxr_view_rig_camera_to_display) puts the
 * virtual display plane at the convergence plane (camera pose moved
 * 1 / convergenceDiopters along the camera's forward axis) with
 * virtualDisplayHeight = 2 tan(vFOV/2) / convergenceDiopters, the frustum's own
 * virtual screen. Points then map through the display core with
 * perspectiveFactor 1: scale = that height / canvas height, glass at zero
 * parallax. Only the plane and its height are taken from the conversion — the
 * converted perspective / ipd / parallax are the eye-pair match, which a point
 * does not use. False (legacy viewer-relative mapping) when the convergence is
 * at infinity or the canvas / FOV are degenerate.
 */
static bool
camera_glass_rig(const struct m_view_rig_canvas *canvas,
                 const struct m_view_rig *rig,
                 struct xrt_pose *out_pose,
                 float *out_vdh)
{
	if (!(rig->inv_convergence_distance > 0.0f) || !(rig->half_tan_vfov > 0.0f) || !(canvas->height_m > 0.0f) ||
	    !(canvas->width_m > 0.0f) || !(canvas->nominal_z_m > 0.0f)) {
		return false;
	}
	dxr_camera_rig cr;
	memset(&cr, 0, sizeof(cr));
	memcpy(&cr.pose, &rig->pose, sizeof(cr.pose)); // layout-identical (displayxr_math_xrt.c asserts)
	cr.ipd_factor = 1.0f;
	cr.parallax_factor = 1.0f;
	cr.inv_convergence_distance = rig->inv_convergence_distance;
	cr.half_tan_vfov = rig->half_tan_vfov;
	cr.m2v = rig->m2v > 0.0f ? rig->m2v : 1.0f;
	const dxr_rig_display_info info = {
	    .physical_height_m = canvas->height_m,
	    .aspect = canvas->width_m / canvas->height_m,
	    .nominal_distance_m = canvas->nominal_z_m,
	};
	dxr_display_rig dr;
	(void)dxr_view_rig_camera_to_display(&cr, &info, &dr);
	if (!(dr.virtual_display_height > 0.0f)) {
		return false;
	}
	memcpy(out_pose, &dr.pose, sizeof(*out_pose));
	*out_vdh = dr.virtual_display_height;
	return true;
}

bool
m_view_rig_camera_glass(const struct m_view_rig_canvas *canvas,
                        const struct m_view_rig *rig,
                        struct xrt_pose *out_glass_pose,
                        float *out_scale)
{
	if (rig == NULL || rig->kind != M_VIEW_RIG_CAMERA || canvas == NULL || !canvas->valid) {
		return false;
	}
	struct xrt_pose pose;
	float vdh = 0.0f;
	if (!camera_glass_rig(canvas, rig, &pose, &vdh)) {
		return false;
	}
	if (out_glass_pose != NULL) {
		*out_glass_pose = pose;
	}
	if (out_scale != NULL) {
		*out_scale = vdh / canvas->height_m;
	}
	return true;
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
	struct xrt_pose glass_pose;
	float glass_vdh = 0.0f;
	if (rig->kind == M_VIEW_RIG_CAMERA && camera_glass_rig(canvas, rig, &glass_pose, &glass_vdh)) {
		// The glass at the convergence plane (zero parallax), scaled by the
		// frustum's virtual screen height / canvas height — the display rig's
		// mapping for the camera rig's own virtual screen.
		const dxr_display3d_tunables dt = {
		    .ipd_factor = 1.0f,
		    .parallax_factor = 1.0f,
		    .perspective_factor = 1.0f,
		    .virtual_display_height = glass_vdh,
		};
		dxr_xrt_display3d_compute_views(p_canvas, 1, &nominal, &scr, &dt, &glass_pose, &v);
	} else if (rig->kind == M_VIEW_RIG_CAMERA) {
		// Convergence at infinity: no glass plane to put the panel on. The
		// camera core's eye mapping (viewer-relative, x metersToVirtual).
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
		float s = 1.0f;
		if (m_view_rig_camera_glass(canvas, rig, NULL, &s)) {
			return s;
		}
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

float
m_view_rig_canvas_scale(const struct m_view_rig_canvas *canvas)
{
	if (canvas == NULL || !canvas->valid || !(canvas->height_m > 0.0f) || !(canvas->display_height_m > 0.0f)) {
		return 1.0f;
	}
	const float s = canvas->height_m / canvas->display_height_m;
	return fabsf(s - 1.0f) <= 1e-4f ? 1.0f : s;
}

void
m_view_rig_canvas_pose(const struct m_view_rig_canvas *canvas, struct xrt_pose *out)
{
	out->position = canvas->center;
	out->orientation = canvas->has_orientation ? canvas->orientation : (struct xrt_quat)XRT_QUAT_IDENTITY;
}

void
m_view_rig_canvas_plane(const struct m_view_rig_canvas *canvas,
                        const struct m_view_rig *rig,
                        const struct xrt_pose *plane_origin,
                        struct xrt_pose *out)
{
	// A full-display canvas IS the physical plane: take it verbatim, so the
	// result is the pre-amendment plane bit for bit.
	struct xrt_pose at = *plane_origin;
	if (canvas->has_orientation || canvas->center.x != 0.0f || canvas->center.y != 0.0f ||
	    canvas->center.z != 0.0f) {
		struct xrt_pose canvas_pose;
		m_view_rig_canvas_pose(canvas, &canvas_pose);
		math_pose_transform(plane_origin, &canvas_pose, &at);
	}
	if (rig == NULL || rig->kind == M_VIEW_RIG_NONE || !canvas->valid) {
		*out = at;
		return;
	}
	// The canvas centre as a joint: rebases to canvas point 0, orientation
	// rig * inverse(canvas) * inverse(plane) * (plane * canvas) = the rig's.
	struct xrt_space_relation in = {0};
	in.pose = at;
	struct xrt_space_relation mapped;
	m_view_rig_map_relation(canvas, rig, plane_origin, &in, 0.0f, &mapped, NULL);
	*out = mapped.pose;
}

void
m_view_rig_scale_relation(const struct xrt_vec3 *origin,
                          float s,
                          const struct xrt_space_relation *in,
                          float radius,
                          struct xrt_space_relation *out,
                          float *out_radius)
{
	*out = *in;
	if (out_radius != NULL) {
		*out_radius = radius;
	}
	if (s == 1.0f) {
		return;
	}
	const struct xrt_vec3 p = in->pose.position;
	out->pose.position = (struct xrt_vec3){
	    origin->x + s * (p.x - origin->x),
	    origin->y + s * (p.y - origin->y),
	    origin->z + s * (p.z - origin->z),
	};
	out->linear_velocity =
	    (struct xrt_vec3){in->linear_velocity.x * s, in->linear_velocity.y * s, in->linear_velocity.z * s};
	if (out_radius != NULL) {
		*out_radius = radius * s;
	}
}
