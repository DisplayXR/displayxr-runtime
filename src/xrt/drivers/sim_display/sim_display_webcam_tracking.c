// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_display webcam eye tracking (#1855): the pure parts. See the header.
 * @ingroup drv_sim_display
 */

#include "sim_display_webcam_tracking.h"

#include "util/u_stereo_uvc.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void
sim_webcam_config_defaults(struct sim_webcam_config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->hfov_deg = SIM_WEBCAM_DEFAULT_HFOV_DEG;
	cfg->ipd_m = SIM_WEBCAM_DEFAULT_IPD_M;
	cfg->timeout_ns = SIM_WEBCAM_DEFAULT_TIMEOUT_NS;
	cfg->collapse_ns = SIM_WEBCAM_DEFAULT_COLLAPSE_NS;
	cfg->revive_ns = SIM_WEBCAM_DEFAULT_REVIVE_NS;
	cfg->euro_min_cutoff_hz = 1.0f;
	cfg->euro_beta = 1.5f;
	cfg->euro_d_cutoff_hz = 1.0f;
}

static double
focal_px(const struct sim_webcam_config *cfg, uint32_t image_w)
{
	double hfov = (double)cfg->hfov_deg;
	if (!(hfov > 1.0 && hfov < 179.0)) {
		hfov = SIM_WEBCAM_DEFAULT_HFOV_DEG;
	}
	return ((double)image_w * 0.5) / tan(hfov * 0.5 * M_PI / 180.0);
}

static void
camera_position(const struct sim_webcam_config *cfg, float screen_h_m, double out[3])
{
	out[0] = (double)cfg->cam_offset_m[0];
	out[1] = (double)screen_h_m * 0.5 + (double)cfg->cam_offset_m[1];
	out[2] = (double)cfg->cam_offset_m[2];
}

bool
sim_webcam_landmarks_to_eyes(const struct sim_webcam_config *cfg,
                             float screen_h_m,
                             uint32_t image_w,
                             uint32_t image_h,
                             const struct sim_face_landmarks *lm,
                             struct xrt_vec3 out_eyes[2])
{
	if (lm == NULL || !lm->found || image_w == 0 || image_h == 0) {
		return false;
	}
	const double f = focal_px(cfg, image_w);
	const double cx = (double)image_w * 0.5;
	const double cy = (double)image_h * 0.5;

	const double du = (double)lm->eye_px[0][0] - (double)lm->eye_px[1][0];
	const double dv = (double)lm->eye_px[0][1] - (double)lm->eye_px[1][1];
	const double d_px = sqrt(du * du + dv * dv);
	if (!(d_px >= 1.0)) {
		return false;
	}
	const double ipd = (cfg->ipd_m > 0.0f) ? (double)cfg->ipd_m : (double)SIM_WEBCAM_DEFAULT_IPD_M;
	const double z = f * ipd / d_px; // depth along the camera axis
	if (!(z >= 0.1 && z <= 5.0)) {
		return false;
	}

	double cam[3];
	camera_position(cfg, screen_h_m, cam);
	const double sx = cfg->mirrored ? 1.0 : -1.0; // unmirrored: image right = display left

	struct xrt_vec3 e[2];
	for (int i = 0; i < 2; i++) {
		const double xc = ((double)lm->eye_px[i][0] - cx) * z / f; // camera: +x image right
		const double yc = ((double)lm->eye_px[i][1] - cy) * z / f; // camera: +y image down
		e[i].x = (float)(cam[0] + sx * xc);
		e[i].y = (float)(cam[1] - yc);
		e[i].z = (float)(cam[2] + z);
	}
	if (e[0].x <= e[1].x) {
		out_eyes[0] = e[0];
		out_eyes[1] = e[1];
	} else {
		out_eyes[0] = e[1];
		out_eyes[1] = e[0];
	}
	return true;
}

bool
sim_webcam_project(const struct sim_webcam_config *cfg,
                   float screen_h_m,
                   uint32_t image_w,
                   uint32_t image_h,
                   const struct xrt_vec3 *p,
                   float out_px[2])
{
	double cam[3];
	camera_position(cfg, screen_h_m, cam);
	const double z = (double)p->z - cam[2];
	if (!(z > 0.0)) {
		return false;
	}
	const double f = focal_px(cfg, image_w);
	const double sx = cfg->mirrored ? 1.0 : -1.0;
	const double xc = sx * ((double)p->x - cam[0]);
	const double yc = cam[1] - (double)p->y;
	out_px[0] = (float)((double)image_w * 0.5 + xc * f / z);
	out_px[1] = (float)((double)image_h * 0.5 + yc * f / z);
	return true;
}


/*
 * One-euro filter.
 */

void
sim_one_euro_reset(struct sim_one_euro *f)
{
	memset(f, 0, sizeof(*f));
}

static float
euro_alpha(float cutoff_hz, double dt_s)
{
	const double tau = 1.0 / (2.0 * M_PI * (double)(cutoff_hz > 1e-6f ? cutoff_hz : 1e-6f));
	return (float)(1.0 / (1.0 + tau / dt_s));
}

float
sim_one_euro_filter(struct sim_one_euro *f, float min_cutoff_hz, float beta, float d_cutoff_hz, float x, uint64_t t_ns)
{
	if (!f->init) {
		f->init = true;
		f->x_prev = x;
		f->dx_prev = 0.0f;
		f->t_prev_ns = t_ns;
		return x;
	}
	if (t_ns <= f->t_prev_ns) {
		return f->x_prev;
	}
	const double dt = (double)(t_ns - f->t_prev_ns) * 1e-9;
	const float dx = (float)((double)(x - f->x_prev) / dt);
	const float a_d = euro_alpha(d_cutoff_hz, dt);
	const float edx = f->dx_prev + a_d * (dx - f->dx_prev);
	const float cutoff = min_cutoff_hz + beta * fabsf(edx);
	const float a = euro_alpha(cutoff, dt);
	const float out = f->x_prev + a * (x - f->x_prev);
	f->x_prev = out;
	f->dx_prev = edx;
	f->t_prev_ns = t_ns;
	return out;
}


/*
 * Tracking state.
 */

void
sim_webcam_track_reset(struct sim_webcam_track_state *s)
{
	memset(s, 0, sizeof(*s));
}

static float
smoothstep01(double t)
{
	if (t <= 0.0) {
		return 0.0f;
	}
	if (t >= 1.0) {
		return 1.0f;
	}
	return (float)(t * t * (3.0 - 2.0 * t));
}

static float
ramp(uint64_t from_ns, uint64_t at_ns, uint64_t len_ns)
{
	if (at_ns <= from_ns) {
		return 0.0f;
	}
	if (len_ns == 0) {
		return 1.0f;
	}
	return smoothstep01((double)(at_ns - from_ns) / (double)len_ns);
}

//! The blend weight at @p t of a run that began at acquire_ns (t within the run).
static float
revive_weight(const struct sim_webcam_track_state *s, const struct sim_webcam_config *cfg, uint64_t t)
{
	const float r = ramp(s->acquire_ns, t, cfg->revive_ns);
	return s->revive_from + (1.0f - s->revive_from) * r;
}

static bool
within_timeout(const struct sim_webcam_track_state *s, const struct sim_webcam_config *cfg, uint64_t t)
{
	return t <= s->last_face_ns || (t - s->last_face_ns) <= cfg->timeout_ns;
}

void
sim_webcam_track_update(struct sim_webcam_track_state *s,
                        const struct sim_webcam_config *cfg,
                        bool found,
                        const struct xrt_vec3 eyes[2],
                        uint64_t t_ns)
{
	if (!found) {
		return; // the timeout runs off last_face_ns; nothing to record
	}
	if (!s->have_face || !within_timeout(s, cfg, t_ns)) {
		// A new run: continue from wherever the blend is now (0 when never seen).
		float w = 0.0f;
		if (s->have_face) {
			struct sim_webcam_eyes now;
			const struct xrt_vec3 dummy[2] = {{0, 0, 0}, {0, 0, 0}};
			sim_webcam_track_sample(s, cfg, dummy, t_ns, &now);
			w = now.weight;
		}
		for (int i = 0; i < 6; i++) {
			sim_one_euro_reset(&s->filters[i]);
		}
		s->acquire_ns = t_ns;
		s->revive_from = w;
		s->acquisitions++;
		s->have_face = true;
	}
	for (int e = 0; e < 2; e++) {
		const float in[3] = {eyes[e].x, eyes[e].y, eyes[e].z};
		float o[3];
		for (int c = 0; c < 3; c++) {
			o[c] = sim_one_euro_filter(&s->filters[e * 3 + c], cfg->euro_min_cutoff_hz, cfg->euro_beta,
			                           cfg->euro_d_cutoff_hz, in[c], t_ns);
		}
		s->eyes[e] = (struct xrt_vec3){o[0], o[1], o[2]};
	}
	if (t_ns > s->last_face_ns) {
		s->last_face_ns = t_ns;
	}
}

void
sim_webcam_track_sample(const struct sim_webcam_track_state *s,
                        const struct sim_webcam_config *cfg,
                        const struct xrt_vec3 nominal[2],
                        uint64_t now_ns,
                        struct sim_webcam_eyes *out)
{
	memset(out, 0, sizeof(*out));
	if (!s->have_face) {
		out->eyes[0] = nominal[0];
		out->eyes[1] = nominal[1];
		return;
	}
	const bool tracking = within_timeout(s, cfg, now_ns);
	float w;
	if (tracking) {
		w = revive_weight(s, cfg, now_ns);
	} else {
		const uint64_t lost_at = s->last_face_ns + cfg->timeout_ns;
		w = revive_weight(s, cfg, lost_at) * (1.0f - ramp(lost_at, now_ns, cfg->collapse_ns));
	}
	for (int e = 0; e < 2; e++) {
		out->eyes[e].x = nominal[e].x + w * (s->eyes[e].x - nominal[e].x);
		out->eyes[e].y = nominal[e].y + w * (s->eyes[e].y - nominal[e].y);
		out->eyes[e].z = nominal[e].z + w * (s->eyes[e].z - nominal[e].z);
	}
	out->is_tracking = tracking;
	out->weight = w;
	// Every run rose once; every run but a live one has fallen once.
	out->edges = 2u * s->acquisitions - (tracking ? 1u : 0u);
}


/*
 * Device + mode selection.
 */

static bool
contains_ci(const char *hay, const char *needle)
{
	const size_t n = strlen(needle);
	if (n == 0) {
		return true;
	}
	for (const char *h = hay; *h != '\0'; h++) {
		size_t i = 0;
		while (i < n && h[i] != '\0' && tolower((unsigned char)h[i]) == tolower((unsigned char)needle[i])) {
			i++;
		}
		if (i == n) {
			return true;
		}
	}
	return false;
}

int32_t
sim_webcam_select_device(const struct u_stereo_uvc_device *devs, uint32_t count, const char *sel)
{
	if (count == 0) {
		return -1;
	}
	if (sel == NULL || sel[0] == '\0') {
		return 0;
	}
	bool digits = true;
	for (const char *p = sel; *p != '\0'; p++) {
		if (!isdigit((unsigned char)*p)) {
			digits = false;
			break;
		}
	}
	if (digits) {
		const unsigned long idx = strtoul(sel, NULL, 10);
		return idx < count ? (int32_t)idx : -1;
	}
	for (uint32_t i = 0; i < count; i++) {
		if (contains_ci(devs[i].name, sel)) {
			return (int32_t)i;
		}
	}
	return -1;
}

bool
sim_webcam_select_mode(const struct u_stereo_uvc_mode *modes, uint32_t count, struct u_stereo_uvc_mode *out)
{
	int32_t best = -1;
	for (uint32_t i = 0; i < count; i++) {
		const struct u_stereo_uvc_mode *m = &modes[i];
		if (m->width == 0 || m->height == 0) {
			continue;
		}
		if (best < 0) {
			best = (int32_t)i;
			continue;
		}
		const struct u_stereo_uvc_mode *b = &modes[best];
		const uint32_t dm = m->width > SIM_WEBCAM_TARGET_WIDTH ? m->width - SIM_WEBCAM_TARGET_WIDTH
		                                                       : SIM_WEBCAM_TARGET_WIDTH - m->width;
		const uint32_t db = b->width > SIM_WEBCAM_TARGET_WIDTH ? b->width - SIM_WEBCAM_TARGET_WIDTH
		                                                       : SIM_WEBCAM_TARGET_WIDTH - b->width;
		const float fm = m->fps > 60.0f ? 60.0f : m->fps;
		const float fb = b->fps > 60.0f ? 60.0f : b->fps;
		if (dm < db || (dm == db && (fm > fb || (fm == fb && m->height < b->height)))) {
			best = (int32_t)i;
		}
	}
	if (best < 0) {
		return false;
	}
	*out = modes[best];
	return true;
}

bool
sim_webcam_parse_offset_mm(const char *s, float out_m[3])
{
	if (s == NULL) {
		return false;
	}
	float v[3];
	char tail;
	if (sscanf(s, " %f , %f , %f %c", &v[0], &v[1], &v[2], &tail) != 3) {
		return false;
	}
	for (int i = 0; i < 3; i++) {
		out_m[i] = v[i] / 1000.0f;
	}
	return true;
}
