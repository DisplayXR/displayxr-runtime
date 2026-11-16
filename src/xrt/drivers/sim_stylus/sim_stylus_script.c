// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_stylus's script: pure functions of the requested timestamp. See
 *         the header for the frame, the pen axes and the schedules.
 * @ingroup drv_sim_stylus
 */

#include "sim_stylus_script.h"

#include "math/m_mathinclude.h"

#include <string.h>


/*
 *
 * Small vector / quaternion helpers (kept local so the script stays a pure C
 * unit with no aux_math dependency).
 *
 */

static struct xrt_vec3
v3(float x, float y, float z)
{
	struct xrt_vec3 v = {x, y, z};
	return v;
}

static struct xrt_vec3
v3_normalize(struct xrt_vec3 v)
{
	const float n = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
	if (n <= 0.0f) {
		return v3(0.0f, 0.0f, 1.0f);
	}
	return v3(v.x / n, v.y / n, v.z / n);
}

static struct xrt_vec3
v3_cross(struct xrt_vec3 a, struct xrt_vec3 b)
{
	return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

static float
v3_dot(struct xrt_vec3 a, struct xrt_vec3 b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

/*!
 * The rotation whose +Z axis is @p z (unit) and whose +Y axis is @p up_hint
 * made orthogonal to it; +X = Y x Z (right-handed).
 */
static struct xrt_quat
quat_from_z_up(struct xrt_vec3 z, struct xrt_vec3 up_hint)
{
	const float d = v3_dot(up_hint, z);
	struct xrt_vec3 y = v3_normalize(v3(up_hint.x - d * z.x, up_hint.y - d * z.y, up_hint.z - d * z.z));
	struct xrt_vec3 x = v3_cross(y, z);

	// Rotation matrix with columns x, y, z -> quaternion.
	const float m00 = x.x, m01 = y.x, m02 = z.x;
	const float m10 = x.y, m11 = y.y, m12 = z.y;
	const float m20 = x.z, m21 = y.z, m22 = z.z;
	struct xrt_quat q;
	const float tr = m00 + m11 + m22;
	if (tr > 0.0f) {
		const float s = sqrtf(tr + 1.0f) * 2.0f;
		q.w = 0.25f * s;
		q.x = (m21 - m12) / s;
		q.y = (m02 - m20) / s;
		q.z = (m10 - m01) / s;
	} else if (m00 > m11 && m00 > m22) {
		const float s = sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
		q.w = (m21 - m12) / s;
		q.x = 0.25f * s;
		q.y = (m01 + m10) / s;
		q.z = (m02 + m20) / s;
	} else if (m11 > m22) {
		const float s = sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
		q.w = (m02 - m20) / s;
		q.x = (m01 + m10) / s;
		q.y = 0.25f * s;
		q.z = (m12 + m21) / s;
	} else {
		const float s = sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
		q.w = (m10 - m01) / s;
		q.x = (m02 + m20) / s;
		q.y = (m12 + m21) / s;
		q.z = 0.25f * s;
	}
	const float n = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
	q.x /= n;
	q.y /= n;
	q.z /= n;
	q.w /= n;
	return q;
}

static bool
in_window(double t, double period, double start, double len)
{
	const double u = fmod(t, period);
	return u >= start && u < start + len;
}


/*
 *
 * The script.
 *
 */

static float
cfg_width(const struct sim_stylus_script_config *cfg)
{
	return cfg->display_width_m > 0.0f ? cfg->display_width_m : SIM_STYLUS_DISPLAY_WIDTH_FALLBACK_M;
}

static float
cfg_height(const struct sim_stylus_script_config *cfg)
{
	return cfg->display_height_m > 0.0f ? cfg->display_height_m : SIM_STYLUS_DISPLAY_HEIGHT_FALLBACK_M;
}

static double
cfg_period(const struct sim_stylus_script_config *cfg)
{
	return cfg->period_s > 0.0f ? (double)cfg->period_s : (double)SIM_STYLUS_PERIOD_DEFAULT_S;
}

//! Docked at @p t_ns, and for how long (ns) the current dock has lasted.
static bool
docked_at(const struct sim_stylus_script_config *cfg, int64_t t_ns, int64_t *out_in_dock_ns)
{
	*out_in_dock_ns = 0;
	if (cfg->dock_ms <= 0 || t_ns < 0) {
		return false;
	}
	const int64_t half_ns = cfg->dock_ms * 1000000LL;
	const int64_t k = t_ns / half_ns;
	if ((k & 1) == 0) {
		return false;
	}
	*out_in_dock_ns = t_ns - k * half_ns;
	return true;
}

//! Tip position of the free (undocked) pen at @p t_s, plus contact + force.
static struct xrt_vec3
tip_position(const struct sim_stylus_script_config *cfg, double t_s, bool *out_contact, float *out_force)
{
	const double period = cfg_period(cfg);
	const double u = fmod(t_s, period) / period; // 0..1 lap phase
	const double theta = 2.0 * M_PI * u;
	const float a = SIM_STYLUS_FIGURE_W_FRAC * cfg_width(cfg);
	const float b = SIM_STYLUS_FIGURE_H_FRAC * cfg_height(cfg);
	const float x = a * (float)sin(theta);
	const float y = b * (float)sin(2.0 * theta);
	float z = 0.0f;

	*out_contact = false;
	*out_force = 0.0f;

	if (cfg->mode == SIM_STYLUS_MODE_HOVER) {
		z = cfg->hover_height_m >= 0.0f ? cfg->hover_height_m : SIM_STYLUS_HOVER_HEIGHT_DEFAULT_M;
	} else {
		const double c = (double)SIM_STYLUS_DRAW_CONTACT_FRAC;
		if (u < c) {
			// On the glass: the force rises from MIN to 1 and falls back.
			*out_contact = true;
			*out_force = SIM_STYLUS_FORCE_MIN + (1.0f - SIM_STYLUS_FORCE_MIN) * (float)sin(M_PI * u / c);
		} else {
			// Pen-up gap: a smooth arc off the glass, back down at the next stroke.
			z = SIM_STYLUS_LIFT_HEIGHT_M * (float)sin(M_PI * (u - c) / (1.0 - c));
		}
	}
	return v3(x, y, z);
}

static struct xrt_pose
grip_from_tip(const struct xrt_pose *tip, struct xrt_vec3 back_dir)
{
	struct xrt_pose g = *tip;
	g.position.x += back_dir.x * SIM_STYLUS_GRIP_OFFSET_M;
	g.position.y += back_dir.y * SIM_STYLUS_GRIP_OFFSET_M;
	g.position.z += back_dir.z * SIM_STYLUS_GRIP_OFFSET_M;
	return g;
}

void
sim_stylus_script_eval(const struct sim_stylus_script_config *cfg, int64_t t_ns, struct sim_stylus_state *out)
{
	memset(out, 0, sizeof(*out));
	const double t_s = (double)t_ns * 1e-9;

	int64_t in_dock_ns = 0;
	const bool docked = docked_at(cfg, t_ns, &in_dock_ns);

	if (docked) {
		// Standing tip-down in a dock beside the panel's lower-right corner.
		const struct xrt_vec3 back = v3(0.0f, 1.0f, 0.0f);
		out->tip.position = v3(0.5f * cfg_width(cfg) + 0.04f, -0.5f * cfg_height(cfg), 0.03f);
		out->tip.orientation = quat_from_z_up(back, v3(0.0f, 0.0f, 1.0f));
		out->grip = grip_from_tip(&out->tip, back);
		out->docked = true;
		out->tracked = !(cfg->dock_sleep_ms > 0 && in_dock_ns >= cfg->dock_sleep_ms * 1000000LL);
		return;
	}

	// The barrel leans back toward the viewer, up, and to the holding hand's side.
	const float side = cfg->left_handed ? -1.0f : 1.0f;
	const struct xrt_vec3 back = v3_normalize(v3(side * 0.35f, 0.40f, 0.85f));

	bool contact = false;
	float force = 0.0f;
	out->tip.position = tip_position(cfg, t_s, &contact, &force);
	// +Z of the pose is the back direction (-Z points out of the tip); +Y the
	// cluster's normal, taken as the world up made orthogonal to the barrel.
	out->tip.orientation = quat_from_z_up(back, v3(0.0f, 1.0f, 0.0f));
	out->grip = grip_from_tip(&out->tip, back);
	out->tip_force = force;
	out->contact = contact;
	out->tracked = true;

	// Linear velocity by a 1 ms forward difference of the same pure function.
	{
		bool c2 = false;
		float f2 = 0.0f;
		const struct xrt_vec3 p2 = tip_position(cfg, t_s + 1e-3, &c2, &f2);
		out->tip_linear_velocity =
		    v3((p2.x - out->tip.position.x) * 1000.0f, (p2.y - out->tip.position.y) * 1000.0f,
		       (p2.z - out->tip.position.z) * 1000.0f);
	}

	if (cfg->buttons) {
		out->front_click = in_window(t_s, 4.0, 1.0, 0.15);
		out->back_click = in_window(t_s, 6.0, 3.0, 0.15);
		out->front_double_tap = in_window(t_s, 8.0, 5.0, 0.10);
		out->back_double_tap = in_window(t_s, 12.0, 9.0, 0.10);
		if (in_window(t_s, 10.0, 6.0, 1.0)) {
			const double v = fmod(t_s, 10.0) - 6.0; // 0..1
			out->middle_force = (float)(1.0 - fabs(2.0 * v - 1.0));
		}
	}
}

bool
sim_stylus_script_tracked(const struct sim_stylus_script_config *cfg, int64_t t_ns)
{
	int64_t in_dock_ns = 0;
	if (!docked_at(cfg, t_ns, &in_dock_ns)) {
		return true;
	}
	return !(cfg->dock_sleep_ms > 0 && in_dock_ns >= cfg->dock_sleep_ms * 1000000LL);
}

bool
sim_stylus_parse_mode(const char *s, enum sim_stylus_mode *out)
{
	if (s == NULL) {
		return false;
	}
	if (strcmp(s, "draw") == 0) {
		*out = SIM_STYLUS_MODE_DRAW;
		return true;
	}
	if (strcmp(s, "hover") == 0) {
		*out = SIM_STYLUS_MODE_HOVER;
		return true;
	}
	return false;
}
