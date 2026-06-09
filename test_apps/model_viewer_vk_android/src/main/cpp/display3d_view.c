// Copyright 2025-2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Unified display-centric multiview math for 3D displays
 *
 * Canonical implementation — see display3d_view.h for API docs.
 */

#include "display3d_view.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Internal helpers
// ============================================================================

// Quaternion-rotate a vector: v' = q * v * q^-1
// Uses the efficient cross-product form (no matrix conversion).
static XrVector3f
quat_rotate(XrQuaternionf q, XrVector3f v)
{
	// t = 2 * cross(q.xyz, v)
	float tx = 2.0f * (q.y * v.z - q.z * v.y);
	float ty = 2.0f * (q.z * v.x - q.x * v.z);
	float tz = 2.0f * (q.x * v.y - q.y * v.x);

	// v' = v + w*t + cross(q.xyz, t)
	XrVector3f out;
	out.x = v.x + q.w * tx + (q.y * tz - q.z * ty);
	out.y = v.y + q.w * ty + (q.z * tx - q.x * tz);
	out.z = v.z + q.w * tz + (q.x * ty - q.y * tx);
	return out;
}

// Set a column-major 4x4 matrix to identity.
static void
mat4_identity(float *m)
{
	memset(m, 0, 16 * sizeof(float));
	m[0] = m[5] = m[10] = m[15] = 1.0f;
}

// Build rotation matrix (column-major) from quaternion.
static void
mat4_from_quat(float *m, XrQuaternionf q)
{
	float qx = q.x, qy = q.y, qz = q.z, qw = q.w;

	mat4_identity(m);
	m[0] = 1.0f - 2.0f * (qy * qy + qz * qz);
	m[1] = 2.0f * (qx * qy + qz * qw);
	m[2] = 2.0f * (qx * qz - qy * qw);
	m[4] = 2.0f * (qx * qy - qz * qw);
	m[5] = 1.0f - 2.0f * (qx * qx + qz * qz);
	m[6] = 2.0f * (qy * qz + qx * qw);
	m[8] = 2.0f * (qx * qz + qy * qw);
	m[9] = 2.0f * (qy * qz - qx * qw);
	m[10] = 1.0f - 2.0f * (qx * qx + qy * qy);
}

// Build view matrix from display pose and world-space eye position.
// viewMatrix = transpose(R) * translate(-eye_world)
// where R is the rotation matrix from display_pose.orientation.
static void
build_view_matrix(float *out, XrQuaternionf orientation, XrVector3f eye_world)
{
	// Rotation matrix from quaternion
	float rot[16];
	mat4_from_quat(rot, orientation);

	// Transpose rotation (inverse of orthonormal matrix)
	float inv_rot[16];
	mat4_identity(inv_rot);
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			inv_rot[j * 4 + i] = rot[i * 4 + j];

	// Translation: -eye_world
	float inv_trans[16];
	mat4_identity(inv_trans);
	inv_trans[12] = -eye_world.x;
	inv_trans[13] = -eye_world.y;
	inv_trans[14] = -eye_world.z;

	// viewMatrix = inv_rot * inv_trans (column-major multiplication)
	float tmp[16];
	for (int col = 0; col < 4; col++) {
		for (int row = 0; row < 4; row++) {
			float sum = 0.0f;
			for (int k = 0; k < 4; k++) {
				sum += inv_rot[k * 4 + row] * inv_trans[col * 4 + k];
			}
			tmp[col * 4 + row] = sum;
		}
	}
	memcpy(out, tmp, sizeof(tmp));
}

// ============================================================================
// Public API
// ============================================================================

Display3DTunables
display3d_default_tunables(void)
{
	Display3DTunables t;
	t.ipd_factor = 1.0f;
	t.parallax_factor = 1.0f;
	t.perspective_factor = 1.0f;
	t.virtual_display_height = 0.0f;
	return t;
}

void
display3d_apply_eye_factors(const XrVector3f *raw_left,
                            const XrVector3f *raw_right,
                            const XrVector3f *nominal_viewer,
                            float ipd_factor,
                            float parallax_factor,
                            XrVector3f *out_left,
                            XrVector3f *out_right)
{
	// Default nominal viewer if NULL (only z is used — x/y lerp toward origin)
	float nom_z = 0.5f;
	if (nominal_viewer) {
		nom_z = nominal_viewer->z;
	}

	// Step 1: IPD factor — scale inter-eye vector, keep center fixed
	float cx = (raw_left->x + raw_right->x) * 0.5f;
	float cy = (raw_left->y + raw_right->y) * 0.5f;
	float cz = (raw_left->z + raw_right->z) * 0.5f;

	float lvx = (raw_left->x - cx) * ipd_factor;
	float lvy = (raw_left->y - cy) * ipd_factor;
	float lvz = (raw_left->z - cz) * ipd_factor;

	float rvx = (raw_right->x - cx) * ipd_factor;
	float rvy = (raw_right->y - cy) * ipd_factor;
	float rvz = (raw_right->z - cz) * ipd_factor;

	// Step 2: Parallax factor — lerp center toward (0, 0, nom_z).
	// We use origin for x/y so that reducing parallax drives the viewpoint
	// to the display-center axis rather than to an arbitrary nominal offset.
	float cx2 = parallax_factor * cx;
	float cy2 = parallax_factor * cy;
	float cz2 = nom_z + parallax_factor * (cz - nom_z);

	out_left->x = cx2 + lvx;
	out_left->y = cy2 + lvy;
	out_left->z = cz2 + lvz;

	out_right->x = cx2 + rvx;
	out_right->y = cy2 + rvy;
	out_right->z = cz2 + rvz;
}

XrFovf
display3d_compute_fov(XrVector3f eye_pos, float screen_width_m, float screen_height_m)
{
	float ez = eye_pos.z;
	if (ez <= 0.001f)
		ez = 0.65f; // Fallback: ~arm's length

	float half_w = screen_width_m / 2.0f;
	float half_h = screen_height_m / 2.0f;
	float ex = eye_pos.x;
	float ey = eye_pos.y;

	XrFovf fov;
	fov.angleLeft = atanf((-half_w - ex) / ez);
	fov.angleRight = atanf((half_w - ex) / ez);
	fov.angleUp = atanf((half_h - ey) / ez);
	fov.angleDown = atanf((-half_h - ey) / ez);

	return fov;
}

void
display3d_compute_projection(XrVector3f eye_pos,
                             float screen_width_m,
                             float screen_height_m,
                             float near_z,
                             float far_z,
                             float *out_matrix)
{
	float ez = eye_pos.z;
	if (ez <= 0.001f)
		ez = 0.65f;

	float half_w = screen_width_m / 2.0f;
	float half_h = screen_height_m / 2.0f;
	float ex = eye_pos.x;
	float ey = eye_pos.y;

	// Near-plane edge distances (similar triangles: project screen edges through eye)
	float left = near_z * (-half_w - ex) / ez;
	float right = near_z * (half_w - ex) / ez;
	float bottom = near_z * (-half_h - ey) / ez;
	float top = near_z * (half_h - ey) / ez;

	float w = right - left;
	float h = top - bottom;

	// Column-major asymmetric frustum projection matrix
	memset(out_matrix, 0, 16 * sizeof(float));
	out_matrix[0] = 2.0f * near_z / w;
	out_matrix[5] = 2.0f * near_z / h;
	out_matrix[8] = (right + left) / w;
	out_matrix[9] = (top + bottom) / h;
	out_matrix[10] = -(far_z + near_z) / (far_z - near_z);
	out_matrix[11] = -1.0f;
	out_matrix[14] = -(2.0f * far_z * near_z) / (far_z - near_z);
}

void
display3d_apply_eye_factors_n(const XrVector3f *raw_eyes,
                              uint32_t count,
                              const XrVector3f *nominal_viewer,
                              float ipd_factor,
                              float parallax_factor,
                              XrVector3f *out_eyes)
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

	// IPD factor: scale each eye's offset from center
	for (uint32_t i = 0; i < count; i++) {
		out_eyes[i].x = cx2 + (raw_eyes[i].x - cx) * ipd_factor;
		out_eyes[i].y = cy2 + (raw_eyes[i].y - cy) * ipd_factor;
		out_eyes[i].z = cz2 + (raw_eyes[i].z - cz) * ipd_factor;
	}
}

void
display3d_compute_view(const XrVector3f *processed_eye,
                       const Display3DScreen *screen,
                       const Display3DTunables *tunables,
                       const XrPosef *display_pose,
                       float near_z,
                       float far_z,
                       Display3DView *out)
{
	Display3DTunables t = tunables ? *tunables : display3d_default_tunables();

	XrQuaternionf disp_ori = {0, 0, 0, 1};
	XrVector3f disp_pos = {0, 0, 0};
	if (display_pose) {
		disp_ori = display_pose->orientation;
		disp_pos = display_pose->position;
	}

	float m2v = t.virtual_display_height / screen->height_m;

	// Apply perspective * m2v to eye XYZ
	float es = t.perspective_factor * m2v;
	XrVector3f eye_scaled;
	eye_scaled.x = processed_eye->x * es;
	eye_scaled.y = processed_eye->y * es;
	eye_scaled.z = processed_eye->z * es;
	out->eye_display = eye_scaled;

	// Apply m2v to screen dimensions
	float kScreenW = screen->width_m * m2v;
	float kScreenH = screen->height_m * m2v;

	// Transform display-space eye -> world-space via display_pose
	XrVector3f eye_world = quat_rotate(disp_ori, eye_scaled);
	eye_world.x += disp_pos.x;
	eye_world.y += disp_pos.y;
	eye_world.z += disp_pos.z;
	out->eye_world = eye_world;

	// Build view matrix + projection + FOV
	build_view_matrix(out->view_matrix, disp_ori, eye_world);
	out->orientation = disp_ori;
	display3d_compute_projection(eye_scaled, kScreenW, kScreenH,
	                             near_z, far_z, out->projection_matrix);
	out->fov = display3d_compute_fov(eye_scaled, kScreenW, kScreenH);
}

void
display3d_pose_slerp(const XrPosef *from, const XrPosef *to, float t, XrPosef *out)
{
	// Linear lerp position
	out->position.x = from->position.x + (to->position.x - from->position.x) * t;
	out->position.y = from->position.y + (to->position.y - from->position.y) * t;
	out->position.z = from->position.z + (to->position.z - from->position.z) * t;

	// Shortest-arc slerp orientation
	XrQuaternionf q0 = from->orientation;
	XrQuaternionf q1 = to->orientation;
	float dot = q0.x * q1.x + q0.y * q1.y + q0.z * q1.z + q0.w * q1.w;
	if (dot < 0.0f) {
		q1.x = -q1.x; q1.y = -q1.y; q1.z = -q1.z; q1.w = -q1.w;
		dot = -dot;
	}
	float k0, k1;
	if (dot > 0.9995f) {
		// Nearly identical — fall back to linear to avoid numerical issues
		k0 = 1.0f - t;
		k1 = t;
	} else {
		float theta = acosf(dot);
		float sin_theta = sinf(theta);
		k0 = sinf((1.0f - t) * theta) / sin_theta;
		k1 = sinf(t * theta) / sin_theta;
	}
	XrQuaternionf r;
	r.x = k0 * q0.x + k1 * q1.x;
	r.y = k0 * q0.y + k1 * q1.y;
	r.z = k0 * q0.z + k1 * q1.z;
	r.w = k0 * q0.w + k1 * q1.w;
	float norm = sqrtf(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
	if (norm > 1e-8f) {
		float inv = 1.0f / norm;
		r.x *= inv; r.y *= inv; r.z *= inv; r.w *= inv;
	} else {
		r.w = 1.0f; r.x = r.y = r.z = 0.0f;
	}
	out->orientation = r;
}

static XrVector3f
vec3_normalize(XrVector3f v)
{
	float n = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
	if (n < 1e-8f) {
		XrVector3f z = {0, 0, 0};
		return z;
	}
	float inv = 1.0f / n;
	XrVector3f r = {v.x * inv, v.y * inv, v.z * inv};
	return r;
}

static XrVector3f
vec3_cross(XrVector3f a, XrVector3f b)
{
	XrVector3f r;
	r.x = a.y * b.z - a.z * b.y;
	r.y = a.z * b.x - a.x * b.z;
	r.z = a.x * b.y - a.y * b.x;
	return r;
}

void
display3d_align_pose_to_ray(XrVector3f hit_world,
                            XrVector3f ray_dir_world,
                            XrVector3f up_hint,
                            XrPosef *out)
{
	// Display convention: viewer sits at display-local +Z; eyes look toward
	// local -Z (into the display).  For the viewer to remain approximately in
	// place after focus, the new display's local +Z (in world) must point from
	// the hit back toward the old viewer — i.e. opposite the click ray.
	XrVector3f z_axis = {-ray_dir_world.x, -ray_dir_world.y, -ray_dir_world.z};
	z_axis = vec3_normalize(z_axis);
	if (z_axis.x == 0 && z_axis.y == 0 && z_axis.z == 0) {
		z_axis.x = 0; z_axis.y = 0; z_axis.z = 1;
	}

	// X = normalize(cross(up_hint, Z)); fall back if parallel
	XrVector3f x_axis = vec3_cross(up_hint, z_axis);
	float x_len2 = x_axis.x * x_axis.x + x_axis.y * x_axis.y + x_axis.z * x_axis.z;
	if (x_len2 < 1e-8f) {
		XrVector3f alt = {1, 0, 0};
		x_axis = vec3_cross(alt, z_axis);
		x_len2 = x_axis.x * x_axis.x + x_axis.y * x_axis.y + x_axis.z * x_axis.z;
		if (x_len2 < 1e-8f) {
			XrVector3f alt2 = {0, 0, 1};
			x_axis = vec3_cross(alt2, z_axis);
		}
	}
	x_axis = vec3_normalize(x_axis);

	// Y = cross(Z, X) — right-handed, already unit length
	XrVector3f y_axis = vec3_cross(z_axis, x_axis);

	// Rotation matrix columns = [X, Y, Z]; extract quaternion (trace method).
	float m00 = x_axis.x, m10 = x_axis.y, m20 = x_axis.z;
	float m01 = y_axis.x, m11 = y_axis.y, m21 = y_axis.z;
	float m02 = z_axis.x, m12 = z_axis.y, m22 = z_axis.z;

	float tr = m00 + m11 + m22;
	XrQuaternionf q;
	if (tr > 0.0f) {
		float s = sqrtf(tr + 1.0f) * 2.0f;
		q.w = 0.25f * s;
		q.x = (m21 - m12) / s;
		q.y = (m02 - m20) / s;
		q.z = (m10 - m01) / s;
	} else if (m00 > m11 && m00 > m22) {
		float s = sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
		q.w = (m21 - m12) / s;
		q.x = 0.25f * s;
		q.y = (m01 + m10) / s;
		q.z = (m02 + m20) / s;
	} else if (m11 > m22) {
		float s = sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
		q.w = (m02 - m20) / s;
		q.x = (m01 + m10) / s;
		q.y = 0.25f * s;
		q.z = (m12 + m21) / s;
	} else {
		float s = sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
		q.w = (m10 - m01) / s;
		q.x = (m02 + m20) / s;
		q.y = (m12 + m21) / s;
		q.z = 0.25f * s;
	}

	out->position = hit_world;
	out->orientation = q;
}

void
display3d_unproject_ndc_to_ray(float ndc_x,
                               float ndc_y,
                               const float *V,
                               const float *P,
                               XrVector3f *out_origin_world,
                               XrVector3f *out_dir_world)
{
	// View-space direction using Kooima-friendly shortcut.
	float vx = (ndc_x + P[8]) / P[0];
	float vy = (ndc_y + P[9]) / P[5];
	float vz = -1.0f;

	// World direction = R^T * view_dir (R = upper-left 3x3 of V, col-major).
	float wx = V[0] * vx + V[1] * vy + V[2] * vz;
	float wy = V[4] * vx + V[5] * vy + V[6] * vz;
	float wz = V[8] * vx + V[9] * vy + V[10] * vz;
	float len = sqrtf(wx * wx + wy * wy + wz * wz);
	if (len < 1e-8f) {
		out_dir_world->x = 0; out_dir_world->y = 0; out_dir_world->z = -1;
	} else {
		float inv = 1.0f / len;
		out_dir_world->x = wx * inv;
		out_dir_world->y = wy * inv;
		out_dir_world->z = wz * inv;
	}

	// World eye = -R^T * V_translation (V[12], V[13], V[14]).
	float tx = V[12], ty = V[13], tz = V[14];
	out_origin_world->x = -(V[0] * tx + V[1] * ty + V[2] * tz);
	out_origin_world->y = -(V[4] * tx + V[5] * ty + V[6] * tz);
	out_origin_world->z = -(V[8] * tx + V[9] * ty + V[10] * tz);
}

void
display3d_compute_views(const XrVector3f *raw_eyes,
                               uint32_t count,
                               const XrVector3f *nominal_viewer,
                               const Display3DScreen *screen,
                               const Display3DTunables *tunables,
                               const XrPosef *display_pose,
                               float near_z,
                               float far_z,
                               Display3DView *out_views)
{
	Display3DTunables t = tunables ? *tunables : display3d_default_tunables();

	// Apply IPD and parallax factors (N-view path)
	XrVector3f stack_processed[8];
	XrVector3f *processed = (count <= 8) ? stack_processed : (XrVector3f *)malloc(count * sizeof(XrVector3f));
	display3d_apply_eye_factors_n(raw_eyes, count, nominal_viewer,
	                              t.ipd_factor, t.parallax_factor,
	                              processed);

	// Compute each view via single-eye primitive
	for (uint32_t i = 0; i < count; i++) {
		display3d_compute_view(&processed[i], screen, &t,
		                       display_pose, near_z, far_z,
		                       &out_views[i]);
	}

	if (count > 8)
		free(processed);
}
