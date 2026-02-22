// Copyright 2024-2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0

#include "monado3d_kooima.h"
#include <math.h>

// Quaternion-rotate a vector: v' = q * v * q^-1
static XrVector3f
quat_rotate(const float q[4], XrVector3f v)
{
	// q = (x, y, z, w)
	float qx = q[0], qy = q[1], qz = q[2], qw = q[3];

	// t = 2 * cross(q.xyz, v)
	float tx = 2.0f * (qy * v.z - qz * v.y);
	float ty = 2.0f * (qz * v.x - qx * v.z);
	float tz = 2.0f * (qx * v.y - qy * v.x);

	// v' = v + w*t + cross(q.xyz, t)
	XrVector3f out;
	out.x = v.x + qw * tx + (qy * tz - qz * ty);
	out.y = v.y + qw * ty + (qz * tx - qx * tz);
	out.z = v.z + qw * tz + (qx * ty - qy * tx);
	return out;
}

void
monado3d_apply_scene_transform(const XrVector3f *raw_left,
                               const XrVector3f *raw_right,
                               const Monado3DSceneTransform *xform,
                               XrVector3f *out_left,
                               XrVector3f *out_right)
{
	if (!xform->enabled) {
		*out_left = *raw_left;
		*out_right = *raw_right;
		return;
	}

	// Apply zoom: scale eye positions (closer zoom = stronger depth)
	// This mirrors the test app's: localPos = localPos / zoomScale
	float inv_zoom = 1.0f / xform->zoom_scale;

	XrVector3f left_scaled = {raw_left->x * inv_zoom, raw_left->y * inv_zoom, raw_left->z * inv_zoom};
	XrVector3f right_scaled = {raw_right->x * inv_zoom, raw_right->y * inv_zoom, raw_right->z * inv_zoom};

	// Apply rotation: worldPos = orientation * scaledPos
	XrVector3f left_rotated = quat_rotate(xform->orientation, left_scaled);
	XrVector3f right_rotated = quat_rotate(xform->orientation, right_scaled);

	// Apply translation: worldPos += position
	out_left->x = left_rotated.x + xform->position[0];
	out_left->y = left_rotated.y + xform->position[1];
	out_left->z = left_rotated.z + xform->position[2];

	out_right->x = right_rotated.x + xform->position[0];
	out_right->y = right_rotated.y + xform->position[1];
	out_right->z = right_rotated.z + xform->position[2];
}

XrFovf
monado3d_compute_kooima_fov(XrVector3f eye_pos, float screen_width_m, float screen_height_m)
{
	float ez = eye_pos.z;
	if (ez <= 0.001f) {
		ez = 0.65f; // Fallback: ~arm's length
	}

	float half_w = screen_width_m / 2.0f;
	float half_h = screen_height_m / 2.0f;
	float ex = eye_pos.x;
	float ey = eye_pos.y;

	XrFovf fov;
	fov.angleLeft = atanf((-half_w - ex) / ez);
	fov.angleRight = atanf((half_w - ex) / ez);
	fov.angleDown = atanf((-half_h - ey) / ez);
	fov.angleUp = atanf((half_h - ey) / ez);

	return fov;
}

void
monado3d_apply_tunables(const XrVector3f *raw_left,
                        const XrVector3f *raw_right,
                        const Monado3DTunables *tunables,
                        const Monado3DDisplayInfo *display_info,
                        XrVector3f *out_left,
                        XrVector3f *out_right)
{
	// Compute eye center (midpoint between left and right eyes)
	float center_x = (raw_left->x + raw_right->x) * 0.5f;
	float center_y = (raw_left->y + raw_right->y) * 0.5f;
	float center_z = (raw_left->z + raw_right->z) * 0.5f;

	// Half-IPD vector (from center to right eye)
	float half_ipd_x = (raw_right->x - raw_left->x) * 0.5f;
	float half_ipd_y = (raw_right->y - raw_left->y) * 0.5f;
	float half_ipd_z = (raw_right->z - raw_left->z) * 0.5f;

	// Apply IPD factor: scale inter-eye distance
	half_ipd_x *= tunables->ipd_factor;
	half_ipd_y *= tunables->ipd_factor;
	half_ipd_z *= tunables->ipd_factor;

	// Apply parallax factor: scale X/Y offset from display center
	float parallax_cx = center_x * tunables->parallax_factor;
	float parallax_cy = center_y * tunables->parallax_factor;

	// Apply perspective factor: scale Z only
	float perspective_cz = center_z * tunables->perspective_factor;

	// Reconstruct modified eye positions
	out_left->x = parallax_cx - half_ipd_x;
	out_left->y = parallax_cy - half_ipd_y;
	out_left->z = perspective_cz - half_ipd_z;

	out_right->x = parallax_cx + half_ipd_x;
	out_right->y = parallax_cy + half_ipd_y;
	out_right->z = perspective_cz + half_ipd_z;
}

void
monado3d_camera_centric_extents(float convergence_distance,
                                float fov_override,
                                const Monado3DDisplayInfo *display_info,
                                float *out_width,
                                float *out_height)
{
	if (fov_override > 0.0f) {
		// Compute virtual screen extents from FOV and convergence distance
		float half_fov = fov_override * 0.5f;
		float half_w = convergence_distance * tanf(half_fov);
		float aspect = display_info->display_width_meters / display_info->display_height_meters;
		*out_width = half_w * 2.0f;
		*out_height = *out_width / aspect;
	} else {
		// Scale physical display extents by convergence/nominal ratio
		float nominal_z = display_info->nominal_viewer_z;
		if (nominal_z <= 0.001f) {
			nominal_z = 0.65f;
		}
		float ratio = convergence_distance / nominal_z;
		*out_width = display_info->display_width_meters * ratio;
		*out_height = display_info->display_height_meters * ratio;
	}
}
