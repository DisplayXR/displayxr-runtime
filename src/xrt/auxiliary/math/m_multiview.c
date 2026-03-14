// Copyright 2025-2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Camera-centric and display-centric multiview math for 3D displays.
 *
 * Canonical implementation ported from test_apps/common/camera3d_view.c.
 * See m_multiview.h for API docs.
 *
 * @author David Fattal
 * @ingroup aux_math
 */

#include "m_multiview.h"

#include <math.h>
#include <stdint.h>

// Quaternion-rotate a vector: v' = q * v * q^-1
// Uses the efficient cross-product form (no matrix conversion).
static struct xrt_vec3
quat_rotate_vec3(struct xrt_quat q, struct xrt_vec3 v)
{
	float tx = 2.0f * (q.y * v.z - q.z * v.y);
	float ty = 2.0f * (q.z * v.x - q.x * v.z);
	float tz = 2.0f * (q.x * v.y - q.y * v.x);

	struct xrt_vec3 out;
	out.x = v.x + q.w * tx + (q.y * tz - q.z * ty);
	out.y = v.y + q.w * ty + (q.z * tx - q.x * tz);
	out.z = v.z + q.w * tz + (q.x * ty - q.y * tx);
	return out;
}

struct m_multiview_camera_tunables
m_multiview_default_camera_tunables(void)
{
	struct m_multiview_camera_tunables t;
	t.ipd_factor = 1.0f;
	t.parallax_factor = 1.0f;
	t.inv_convergence_distance = 2.0f; // 1/0.5m convergence
	t.half_tan_vfov = 0.3249f;         // tan(18 deg) -> 36 deg vFOV
	return t;
}

void
m_multiview_apply_eye_factors(const struct xrt_vec3 *raw_eyes,
                              uint32_t count,
                              const struct xrt_vec3 *nominal_viewer,
                              float ipd_factor,
                              float parallax_factor,
                              struct xrt_vec3 *out_eyes)
{
	if (count == 0) {
		return;
	}

	float nom_z = 0.5f;
	if (nominal_viewer) {
		nom_z = nominal_viewer->z;
	}

	// Compute centroid of all eyes
	float cx = 0.0f, cy = 0.0f, cz = 0.0f;
	for (uint32_t i = 0; i < count; i++) {
		cx += raw_eyes[i].x;
		cy += raw_eyes[i].y;
		cz += raw_eyes[i].z;
	}
	float inv_n = 1.0f / (float)count;
	cx *= inv_n;
	cy *= inv_n;
	cz *= inv_n;

	// Parallax factor: lerp center toward (0, 0, nom_z)
	float cx2 = parallax_factor * cx;
	float cy2 = parallax_factor * cy;
	float cz2 = nom_z + parallax_factor * (cz - nom_z);

	// View spread factor: scale each eye's offset from center
	for (uint32_t i = 0; i < count; i++) {
		out_eyes[i].x = cx2 + (raw_eyes[i].x - cx) * ipd_factor;
		out_eyes[i].y = cy2 + (raw_eyes[i].y - cy) * ipd_factor;
		out_eyes[i].z = cz2 + (raw_eyes[i].z - cz) * ipd_factor;
	}
}

void
m_multiview_camera_compute_view(const struct xrt_vec3 *processed_eye,
                                float nominal_z,
                                const struct m_multiview_screen *screen,
                                const struct m_multiview_camera_tunables *tunables,
                                const struct xrt_pose *camera_pose,
                                struct xrt_fov *out_fov,
                                struct xrt_vec3 *out_eye_world)
{
	struct m_multiview_camera_tunables t =
	    tunables ? *tunables : m_multiview_default_camera_tunables();

	struct xrt_quat cam_ori = {0, 0, 0, 1};
	struct xrt_vec3 cam_pos = {0, 0, 0};
	if (camera_pose) {
		cam_ori = camera_pose->orientation;
		cam_pos = camera_pose->position;
	}

	float aspect = screen->width_m / screen->height_m;
	float ro = t.half_tan_vfov * aspect;
	float uo = t.half_tan_vfov;
	float invd = t.inv_convergence_distance;

	// eye_local = displacement from nominal screen plane
	struct xrt_vec3 eye_local;
	eye_local.x = processed_eye->x;
	eye_local.y = processed_eye->y;
	eye_local.z = processed_eye->z - nominal_z;

	// Transform to world space
	struct xrt_vec3 eye_world = quat_rotate_vec3(cam_ori, eye_local);
	eye_world.x += cam_pos.x;
	eye_world.y += cam_pos.y;
	eye_world.z += cam_pos.z;
	*out_eye_world = eye_world;

	// Scale by inv_convergence_distance
	float dx = eye_local.x * invd;
	float dy = eye_local.y * invd;
	float dz = eye_local.z * invd;

	// Asymmetric frustum tangent half-angles
	float denom = 1.0f + dz;
	float tan_right = (ro - dx) / denom;
	float tan_left = (ro + dx) / denom;
	float tan_up = (uo - dy) / denom;
	float tan_down = (uo + dy) / denom;

	// Convert tangents to xrt_fov (signed angle convention)
	out_fov->angle_left = -atanf(tan_left);
	out_fov->angle_right = atanf(tan_right);
	out_fov->angle_up = atanf(tan_up);
	out_fov->angle_down = -atanf(tan_down);
}

void
m_multiview_camera_compute(const struct xrt_vec3 *raw_eyes,
                           uint32_t count,
                           const struct xrt_vec3 *nominal_viewer,
                           const struct m_multiview_screen *screen,
                           const struct m_multiview_camera_tunables *tunables,
                           const struct xrt_pose *camera_pose,
                           struct xrt_fov *out_fovs,
                           struct xrt_vec3 *out_eye_world)
{
	struct m_multiview_camera_tunables t =
	    tunables ? *tunables : m_multiview_default_camera_tunables();

	float nom_z = 0.5f;
	if (nominal_viewer) {
		nom_z = nominal_viewer->z;
	}

	// Apply view spread and parallax factors
	struct xrt_vec3 processed[8]; // XRT_MAX_VIEWS
	m_multiview_apply_eye_factors(raw_eyes, count, nominal_viewer,
	                              t.ipd_factor, t.parallax_factor,
	                              processed);

	// Compute each view via single-view primitive
	for (uint32_t i = 0; i < count; i++) {
		m_multiview_camera_compute_view(&processed[i], nom_z, screen,
		                                &t, camera_pose,
		                                &out_fovs[i], &out_eye_world[i]);
	}
}
