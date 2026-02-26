// Copyright 2025-2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Camera-centric and display-centric stereo math for 3D displays.
 *
 * Canonical implementation ported from test_apps/common/camera3d_view.c.
 * See m_stereo3d.h for API docs.
 *
 * @author David Fattal
 * @ingroup aux_math
 */

#include "m_stereo3d.h"

#include <math.h>

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

struct m_stereo3d_camera_tunables
m_stereo3d_default_camera_tunables(void)
{
	struct m_stereo3d_camera_tunables t;
	t.ipd_factor = 1.0f;
	t.parallax_factor = 1.0f;
	t.inv_convergence_distance = 2.0f; // 1/0.5m convergence
	t.half_tan_vfov = 0.3249f;         // tan(18 deg) -> 36 deg vFOV
	return t;
}

void
m_stereo3d_apply_eye_factors(const struct xrt_vec3 *raw_left,
                             const struct xrt_vec3 *raw_right,
                             const struct xrt_vec3 *nominal_viewer,
                             float ipd_factor,
                             float parallax_factor,
                             struct xrt_vec3 *out_left,
                             struct xrt_vec3 *out_right)
{
	// Default nominal viewer if NULL
	float nom_x = 0.0f, nom_y = 0.0f, nom_z = 0.5f;
	if (nominal_viewer) {
		nom_x = nominal_viewer->x;
		nom_y = nominal_viewer->y;
		nom_z = nominal_viewer->z;
	}

	// Step 1: IPD factor -- scale inter-eye vector, keep center fixed
	float cx = (raw_left->x + raw_right->x) * 0.5f;
	float cy = (raw_left->y + raw_right->y) * 0.5f;
	float cz = (raw_left->z + raw_right->z) * 0.5f;

	float lvx = (raw_left->x - cx) * ipd_factor;
	float lvy = (raw_left->y - cy) * ipd_factor;
	float lvz = (raw_left->z - cz) * ipd_factor;

	float rvx = (raw_right->x - cx) * ipd_factor;
	float rvy = (raw_right->y - cy) * ipd_factor;
	float rvz = (raw_right->z - cz) * ipd_factor;

	// Step 2: Parallax factor -- lerp center toward nominal viewer
	float cx2 = nom_x + parallax_factor * (cx - nom_x);
	float cy2 = nom_y + parallax_factor * (cy - nom_y);
	float cz2 = nom_z + parallax_factor * (cz - nom_z);

	out_left->x = cx2 + lvx;
	out_left->y = cy2 + lvy;
	out_left->z = cz2 + lvz;

	out_right->x = cx2 + rvx;
	out_right->y = cy2 + rvy;
	out_right->z = cz2 + rvz;
}

void
m_stereo3d_camera_compute(const struct xrt_vec3 *raw_left,
                          const struct xrt_vec3 *raw_right,
                          const struct xrt_vec3 *nominal_viewer,
                          const struct m_stereo3d_screen *screen,
                          const struct m_stereo3d_camera_tunables *tunables,
                          const struct xrt_pose *camera_pose,
                          struct xrt_fov *out_fovs,
                          struct xrt_vec3 *out_eye_world)
{
	// Resolve defaults
	struct m_stereo3d_camera_tunables t =
	    tunables ? *tunables : m_stereo3d_default_camera_tunables();

	struct xrt_quat cam_ori = {0, 0, 0, 1};
	struct xrt_vec3 cam_pos = {0, 0, 0};
	if (camera_pose) {
		cam_ori = camera_pose->orientation;
		cam_pos = camera_pose->position;
	}

	// Default nominal viewer
	float nom_z = 0.5f;
	if (nominal_viewer) {
		nom_z = nominal_viewer->z;
	}

	// Step 1: Apply IPD and parallax factors
	struct xrt_vec3 processed[2];
	m_stereo3d_apply_eye_factors(raw_left, raw_right, nominal_viewer,
	                             t.ipd_factor, t.parallax_factor,
	                             &processed[0], &processed[1]);

	// Aspect ratio for horizontal FOV
	float aspect = screen->width_m / screen->height_m;
	float ro = t.half_tan_vfov * aspect; // horizontal half-tangent
	float uo = t.half_tan_vfov;          // vertical half-tangent
	float invd = t.inv_convergence_distance;

	// Process each eye
	for (int i = 0; i < 2; i++) {
		// Step 2: eye_local = displacement from nominal screen plane
		struct xrt_vec3 eye_local;
		eye_local.x = processed[i].x;
		eye_local.y = processed[i].y;
		eye_local.z = processed[i].z - nom_z;

		// Step 3: Transform eye_local to world space via camera_pose
		struct xrt_vec3 eye_world = quat_rotate_vec3(cam_ori, eye_local);
		eye_world.x += cam_pos.x;
		eye_world.y += cam_pos.y;
		eye_world.z += cam_pos.z;
		out_eye_world[i] = eye_world;

		// Step 4: Scale eye_local by inv_convergence_distance
		float dx = eye_local.x * invd;
		float dy = eye_local.y * invd;
		float dz = eye_local.z * invd;

		// Step 5: Compute asymmetric frustum tangent half-angles
		float denom = 1.0f + dz;
		float tan_right = (ro - dx) / denom;
		float tan_left = (ro + dx) / denom;
		float tan_up = (uo - dy) / denom;
		float tan_down = (uo + dy) / denom;

		// Step 6: Convert tangents to xrt_fov (signed angle convention)
		out_fovs[i].angle_left = -atanf(tan_left);
		out_fovs[i].angle_right = atanf(tan_right);
		out_fovs[i].angle_up = atanf(tan_up);
		out_fovs[i].angle_down = -atanf(tan_down);
	}
}
