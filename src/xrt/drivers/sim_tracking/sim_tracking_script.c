// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_tracking's scripted viewer (pure).
 * @ingroup drv_sim_tracking
 */

#include "sim_tracking_script.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>

static bool
ieq(const char *a, const char *lower)
{
	for (size_t i = 0;; i++) {
		const char c = (char)((a[i] >= 'A' && a[i] <= 'Z') ? a[i] + ('a' - 'A') : a[i]);
		if (c != lower[i]) {
			return false;
		}
		if (c == '\0') {
			return true;
		}
	}
}

bool
sim_tracking_parse_script(const char *s, enum sim_tracking_script_kind *out)
{
	if (s == NULL || out == NULL) {
		return false;
	}
	if (ieq(s, "sweep")) {
		*out = SIM_TRACKING_SCRIPT_SWEEP;
		return true;
	}
	if (ieq(s, "offset")) {
		*out = SIM_TRACKING_SCRIPT_OFFSET;
		return true;
	}
	return false;
}

bool
sim_tracking_script_eval(
    const struct sim_tracking_script *s, int64_t t_ns, float left[3], float right[3], float head[3])
{
	const int64_t t = t_ns - s->epoch_ns;
	float c[3] = {s->nominal[0] + s->offset[0], s->nominal[1] + s->offset[1], s->nominal[2] + s->offset[2]};
	if (s->kind == SIM_TRACKING_SCRIPT_SWEEP && s->period_ns > 0) {
		const double phase = (double)(t % s->period_ns) / (double)s->period_ns;
		c[0] += s->amplitude_m * (float)sin(2.0 * 3.14159265358979323846 * phase);
	}
	const float half = s->ipd_m * 0.5f;
	left[0] = c[0] - half;
	left[1] = c[1];
	left[2] = c[2];
	right[0] = c[0] + half;
	right[1] = c[1];
	right[2] = c[2];
	if (head != NULL) {
		head[0] = c[0];
		head[1] = c[1];
		head[2] = c[2];
	}
	if (s->loss_period_ns > 0) {
		const int64_t m = ((t % s->loss_period_ns) + s->loss_period_ns) % s->loss_period_ns;
		return m < s->loss_period_ns / 2;
	}
	return true;
}


/*
 *
 * The scripted camera.
 *
 */

static void
quat_rotate(const double q[4], const double v[3], double out[3])
{
	// out = v + 2 w (qv x v) + 2 qv x (qv x v)
	const double x = q[0], y = q[1], z = q[2], w = q[3];
	const double tx = 2.0 * (y * v[2] - z * v[1]);
	const double ty = 2.0 * (z * v[0] - x * v[2]);
	const double tz = 2.0 * (x * v[1] - y * v[0]);
	out[0] = v[0] + w * tx + (y * tz - z * ty);
	out[1] = v[1] + w * ty + (z * tx - x * tz);
	out[2] = v[2] + w * tz + (x * ty - y * tx);
}

bool
sim_tracking_parse_pose(const char *s, double t[3], double q[4])
{
	if (s == NULL) {
		return false;
	}
	double v[7];
	const char *c = s;
	for (int i = 0; i < 7; i++) {
		char *end = NULL;
		v[i] = strtod(c, &end);
		if (end == c || !isfinite(v[i])) {
			return false;
		}
		c = end;
		while (*c == ' ' || *c == '\t') {
			c++;
		}
		if (i < 6) {
			if (*c != ',') {
				return false;
			}
			c++;
		}
	}
	while (*c == ' ' || *c == '\t') {
		c++;
	}
	if (*c != '\0') {
		return false;
	}
	const double n = sqrt(v[3] * v[3] + v[4] * v[4] + v[5] * v[5] + v[6] * v[6]);
	if (!(n > 1e-9)) {
		return false;
	}
	for (int i = 0; i < 3; i++) {
		t[i] = v[i];
	}
	for (int i = 0; i < 4; i++) {
		q[i] = v[3 + i] / n;
	}
	return true;
}

void
sim_tracking_camera_prior(float display_height_m, double t[3], double q[4])
{
	const double h = display_height_m > 0.0f ? (double)display_height_m : 0.34;
	t[0] = 0.0;
	t[1] = h * 0.5 + 0.008;
	t[2] = 0.0;
	q[0] = q[1] = q[2] = 0.0;
	q[3] = 1.0;
}

void
sim_tracking_camera_measure(const struct sim_tracking_camera *c, const float eye[3], float dir[3], float *distance_m)
{
	const double d[3] = {eye[0] - c->true_t[0], eye[1] - c->true_t[1], eye[2] - c->true_t[2]};
	const double qi[4] = {-c->true_q[0], -c->true_q[1], -c->true_q[2], c->true_q[3]};
	double p[3];
	quat_rotate(qi, d, p);
	const double n = sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
	if (!(n > 1e-9)) {
		dir[0] = dir[1] = 0.0f;
		dir[2] = 1.0f;
		*distance_m = 0.0f;
		return;
	}
	dir[0] = (float)(p[0] / n);
	dir[1] = (float)(p[1] / n);
	dir[2] = (float)(p[2] / n);
	*distance_m = (float)(n * (c->depth_bias > 0.0 ? c->depth_bias : 1.0));
}

void
sim_tracking_camera_to_display(const struct sim_tracking_camera *c, const float dir[3], float distance_m, float out[3])
{
	const double k = c->focal_scale > 0.0 ? c->focal_scale : 1.0;
	double u[3] = {dir[0], dir[1], k * dir[2]};
	const double n = sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
	if (n > 1e-12) {
		u[0] /= n;
		u[1] /= n;
		u[2] /= n;
	}
	const double s = (double)distance_m * (c->depth_scale > 0.0 ? c->depth_scale : 1.0);
	const double p[3] = {s * u[0], s * u[1], s * u[2]};
	double r[3];
	quat_rotate(c->pose_q, p, r);
	out[0] = (float)(c->pose_t[0] + r[0]);
	out[1] = (float)(c->pose_t[1] + r[1]);
	out[2] = (float)(c->pose_t[2] + r[2]);
}
