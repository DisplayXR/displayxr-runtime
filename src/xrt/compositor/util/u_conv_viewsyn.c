// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Reference view synthesis: pure math (see u_conv_viewsyn.h).
 *
 * Every function here mirrors one in displayxr-web-lift `js/lift/live-dibr.js`
 * or one stage of its shaders, so the GPU port can be pinned host-side.
 *
 * @ingroup comp_util
 */

#include "u_conv_viewsyn.h"

#include <math.h>
#include <string.h>

static double
clampd(double v, double lo, double hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static double
smoothstepd(double e0, double e1, double x)
{
	double t = clampd((x - e0) / (e1 - e0), 0.0, 1.0);
	return t * t * (3.0 - 2.0 * t);
}

void
u_conv_viewsyn_params_defaults(struct u_conv_viewsyn_params *p)
{
	memset(p, 0, sizeof(*p));
	p->depth.kind = U_CONV_VIEWSYN_DEPTH_AFFINE_DISPARITY;
	p->depth.value_scale = 1.0f;
	p->depth.value_shift = 0.0f;
	p->depth.invalid_sample = NAN;
	p->range_near = 1.0f;
	p->range_far = 0.0f;
	p->convergence = 0.5f;
	p->budget = 0.025f;
	p->rear_cap_m = U_CONV_VIEWSYN_REAR_UNLIMITED;
	p->edge_taper = 0.03f;
	p->steps = 32;
	p->dilate = 0; // lift() overrides DEFAULT_PARAMS.dilate (2) with 0: the snap removes the halo
	p->upsample = true;
	p->upsample_sigma_s = 1.0f;
	p->upsample_sigma_r = 0.1f;
	p->upsample_max = 1280;
	p->dilate_hi = 0;
	p->snap = true;
	p->snap_core = 4.0f;
	p->snap_radius = 8.0f;
	p->snap_tau = 0.12f;
	p->debug = 0;
}

double
u_conv_viewsyn_decode(const struct u_conv_viewsyn_depth_desc *d, double sample)
{
	double s = d->value_scale == 0.0f ? 1.0 : (double)d->value_scale;
	return s * sample + (double)d->value_shift;
}

double
u_conv_viewsyn_nearness(uint32_t kind, double decoded)
{
	switch (kind) {
	case U_CONV_VIEWSYN_DEPTH_METRIC: return decoded > 0.0 ? 1.0 / decoded : 0.0;
	case U_CONV_VIEWSYN_DEPTH_AFFINE: return -decoded;
	default: return decoded;
	}
}

double
u_conv_viewsyn_normalise(const struct u_conv_viewsyn_depth_desc *d, double near_lo, double near_hi, double sample)
{
	if (!isnan(d->invalid_sample) && sample == (double)d->invalid_sample) {
		return 0.0;
	}
	double n = u_conv_viewsyn_nearness(d->kind, u_conv_viewsyn_decode(d, sample));
	return (n - near_lo) / (near_hi - near_lo);
}

struct u_conv_viewsyn_vec3
u_conv_viewsyn_eye_from_viewpoint(const float viewpoint_m[3], double rect_height_m)
{
	struct u_conv_viewsyn_vec3 e = {
	    viewpoint_m[0] / rect_height_m,
	    viewpoint_m[1] / rect_height_m,
	    viewpoint_m[2] / rect_height_m,
	};
	return e;
}

struct u_conv_viewsyn_vec3
u_conv_viewsyn_eye_from_projection(const float P[16], double *out_aspect)
{
	double P0 = P[0], P5 = P[5], P8 = P[8], P9 = P[9];
	struct u_conv_viewsyn_vec3 e = {(-P8 * P5) / (2.0 * P0), -P9 / 2.0, P5 / 2.0};
	if (out_aspect != NULL) {
		*out_aspect = P5 / P0;
	}
	return e;
}

void
u_conv_viewsyn_kooima_projection(
    struct u_conv_viewsyn_vec3 eye, double aspect, double near_z, double far_z, float out_P[16])
{
	double d = eye.z;
	double l = ((-aspect / 2.0 - eye.x) * near_z) / d;
	double r = ((aspect / 2.0 - eye.x) * near_z) / d;
	double b = ((-0.5 - eye.y) * near_z) / d;
	double t = ((0.5 - eye.y) * near_z) / d;
	memset(out_P, 0, 16 * sizeof(float));
	out_P[0] = (float)((2.0 * near_z) / (r - l));
	out_P[5] = (float)((2.0 * near_z) / (t - b));
	out_P[8] = (float)((r + l) / (r - l));
	out_P[9] = (float)((t + b) / (t - b));
	out_P[10] = (float)(-(far_z + near_z) / (far_z - near_z));
	out_P[11] = -1.0f;
	out_P[14] = (float)((-2.0 * far_z * near_z) / (far_z - near_z));
}

struct u_conv_viewsyn_vec3
u_conv_viewsyn_source_camera(const struct u_conv_viewsyn_vec3 *eyes,
                             uint32_t count,
                             const struct u_conv_viewsyn_vec3 *anchor,
                             double look_around)
{
	double sx = 0.0, sy = 0.0, sz = 0.0;
	for (uint32_t i = 0; i < count; i++) {
		sx += eyes[i].x;
		sy += eyes[i].y;
		sz += eyes[i].z;
	}
	double n = count > 0 ? (double)count : 1.0;
	struct u_conv_viewsyn_vec3 c = {sx / n, sy / n, sz / n};
	double k = isfinite(look_around) ? clampd(look_around, 0.0, 1.0) : 0.0;
	if (anchor != NULL && k > 0.0) {
		c.x += k * (anchor->x - c.x);
		c.y += k * (anchor->y - c.y);
	}
	return c;
}

double
u_conv_viewsyn_q_scale(double budget, double gain, double aspect, double d0, double kappa)
{
	return (budget * gain * aspect) / (kappa * d0);
}

double
u_conv_viewsyn_rear_k(double q_scale, double conv_n, double cam_z, double rear_cap_wh)
{
	if (rear_cap_wh < 0.0 || !(cam_z > 0.0)) {
		return 1.0;
	}
	double q_far = q_scale * (0.0 - conv_n);
	double q_cap = -rear_cap_wh / (cam_z + rear_cap_wh);
	if (q_far >= q_cap || q_far >= 0.0) {
		return 1.0;
	}
	return q_cap / q_far; // in [0, 1)
}

void
u_conv_viewsyn_src_uv(const double uv_out[2],
                      double q,
                      struct u_conv_viewsyn_vec3 eye,
                      struct u_conv_viewsyn_vec3 cam,
                      double aspect,
                      double out_uv[2])
{
	double px = (uv_out[0] - 0.5) * aspect;
	double py = uv_out[1] - 0.5;
	double z = (q * cam.z) / (1.0 + q);
	double s = (eye.z - z) / eye.z;
	double rx = eye.x + (px - eye.x) * s;
	double ry = eye.y + (py - eye.y) * s;
	double xs = cam.x + (rx - cam.x) * (1.0 + q);
	double ys = cam.y + (ry - cam.y) * (1.0 + q);
	out_uv[0] = xs / aspect + 0.5;
	out_uv[1] = ys + 0.5;
}

double
u_conv_viewsyn_taper(const double uvs[2], double taper, double aspect)
{
	if (!(taper > 0.0)) {
		return 1.0;
	}
	double bx = fmin(uvs[0], 1.0 - uvs[0]);
	double by = fmin(uvs[1], 1.0 - uvs[1]);
	return smoothstepd(0.0, taper, fmin(bx, by / aspect));
}

void
u_conv_viewsyn_snap_radii(
    double snap_core, double snap_radius, uint32_t depth_w, uint32_t *out_core, uint32_t *out_radius)
{
	double k = (double)depth_w / 364.0;
	// JS Math.round = floor(x + 0.5) for the positive values that occur here.
	double core = floor(snap_core * k + 0.5);
	double radius = floor(snap_radius * k + 0.5);
	*out_core = core > 0.0 ? (uint32_t)core : 0u;
	*out_radius = (uint32_t)clampd(radius, 1.0, 12.0);
}

void
u_conv_viewsyn_upsample_size(uint32_t src_w, uint32_t src_h, uint32_t max_edge, uint32_t *out_w, uint32_t *out_h)
{
	double w = src_w > 0 ? (double)src_w : 1.0;
	double h = src_h > 0 ? (double)src_h : 1.0;
	double m = max_edge > 0 ? (double)max_edge : 1.0;
	double k = fmin(1.0, m / fmax(w, h));
	double ow = floor(w * k + 0.5);
	double oh = floor(h * k + 0.5);
	*out_w = ow < 1.0 ? 1u : (uint32_t)ow;
	*out_h = oh < 1.0 ? 1u : (uint32_t)oh;
}

bool
u_conv_viewsyn_resolve(const struct u_conv_viewsyn_params *p, struct u_conv_viewsyn_frame *out)
{
	if (p->view_count == 0 || p->view_count > U_CONV_VIEWSYN_MAX_VIEWS || !(p->rect_width_m > 0.0f) ||
	    !(p->rect_height_m > 0.0f)) {
		return false;
	}
	struct u_conv_viewsyn_frame f;
	memset(&f, 0, sizeof(f));
	f.view_count = p->view_count;
	f.aspect = (double)p->rect_width_m / (double)p->rect_height_m;

	struct u_conv_viewsyn_vec3 eyes[U_CONV_VIEWSYN_MAX_VIEWS];
	for (uint32_t i = 0; i < p->view_count; i++) {
		eyes[i] = u_conv_viewsyn_eye_from_viewpoint(p->viewpoints_m[i], p->rect_height_m);
		if (!(eyes[i].z > 0.0)) {
			return false;
		}
		f.views[i].eye = eyes[i];
	}
	if (p->has_source_camera) {
		f.cam = u_conv_viewsyn_eye_from_viewpoint(p->source_camera_m, p->rect_height_m);
		if (!(f.cam.z > 0.0)) {
			return false;
		}
	} else {
		f.cam = u_conv_viewsyn_source_camera(eyes, p->view_count, NULL, 0.0);
	}

	// Normalisation range in NEARNESS (larger = nearer). The policy hands it
	// as two decoded values; order them, and keep a non-zero span like the
	// JS normaliser does for a flat map.
	double a = u_conv_viewsyn_nearness(p->depth.kind, (double)p->range_near);
	double b = u_conv_viewsyn_nearness(p->depth.kind, (double)p->range_far);
	f.near_lo = fmin(a, b);
	f.near_hi = fmax(a, b);
	if (!(f.near_hi - f.near_lo >= 1e-6)) {
		f.near_hi = f.near_lo + 1e-6;
	}
	double nc = u_conv_viewsyn_nearness(p->depth.kind, (double)p->convergence);
	f.conv_n = isfinite(nc) ? clampd((nc - f.near_lo) / (f.near_hi - f.near_lo), 0.0, 1.0) : 0.5;

	f.taper = p->edge_taper > 0.0f ? (double)p->edge_taper : 0.0;
	f.steps = p->steps < 4 ? 4 : (p->steps > 64 ? 64 : p->steps);

	// live-dibr.js: D0 = the source camera's distance; one qScale per view
	// from that view's aspect (all views share the rect here).
	double qs = u_conv_viewsyn_q_scale((double)p->budget, 1.0, f.aspect, f.cam.z, U_CONV_VIEWSYN_KAPPA);
	for (uint32_t i = 0; i < p->view_count; i++) {
		f.views[i].q_scale = qs;
	}
	double rear_wh = p->rear_cap_m < 0.0f ? -1.0 : (double)p->rear_cap_m / (double)p->rect_height_m;
	f.rear_k = u_conv_viewsyn_rear_k(qs, f.conv_n, f.cam.z, rear_wh);

	*out = f;
	return true;
}

double
u_conv_viewsyn_q_at(const struct u_conv_viewsyn_frame *f,
                    const struct u_conv_viewsyn_view *view,
                    u_conv_viewsyn_sample_fn sample,
                    void *user,
                    const double uvs[2])
{
	double n = clampd(sample(user, uvs[0], uvs[1]), 0.0, 1.0);
	double t = u_conv_viewsyn_taper(uvs, f->taper, f->aspect);
	double q = view->q_scale * (n - f->conv_n) * t;
	return q < 0.0 ? q * f->rear_k : q;
}

void
u_conv_viewsyn_march_bracket(const struct u_conv_viewsyn_frame *f,
                             const struct u_conv_viewsyn_view *view,
                             double *out_q_near,
                             double *out_q_far)
{
	double q_near = view->q_scale * (1.0 - f->conv_n);
	double q_far = view->q_scale * (0.0 - f->conv_n);
	if (q_near < 0.0) {
		q_near *= f->rear_k;
	}
	if (q_far < 0.0) {
		q_far *= f->rear_k;
	}
	// Conservative bracket: the taper pulls borders to 0, so include 0.
	*out_q_near = fmax(q_near, 0.0);
	*out_q_far = fmin(q_far, 0.0);
}

double
u_conv_viewsyn_march(const struct u_conv_viewsyn_frame *f,
                     const struct u_conv_viewsyn_view *view,
                     u_conv_viewsyn_sample_fn sample,
                     void *user,
                     const double uv_out[2],
                     double out_src_uv[2])
{
	double q_near, q_far;
	u_conv_viewsyn_march_bracket(f, view, &q_near, &q_far);
	double uv[2];

	double dq = (q_near - q_far) / (double)f->steps;
	double q = q_near;
	double q_prev = q_near;
	bool hit = false;
	for (uint32_t i = 0; i <= f->steps; i++) {
		q = q_near - dq * (double)i;
		u_conv_viewsyn_src_uv(uv_out, q, view->eye, f->cam, f->aspect, uv);
		if (u_conv_viewsyn_q_at(f, view, sample, user, uv) >= q) {
			hit = true;
			break;
		}
		q_prev = q;
	}
	if (!hit) {
		q = q_far;
	}
	if (hit && q != q_prev) {
		double a = q_prev, b = q;
		for (int k = 0; k < U_CONV_VIEWSYN_REFINE_STEPS; k++) {
			double m = 0.5 * (a + b);
			u_conv_viewsyn_src_uv(uv_out, m, view->eye, f->cam, f->aspect, uv);
			if (u_conv_viewsyn_q_at(f, view, sample, user, uv) >= m) {
				b = m;
			} else {
				a = m;
			}
		}
		q = b;
	}
	u_conv_viewsyn_src_uv(uv_out, q, view->eye, f->cam, f->aspect, out_src_uv);
	return q;
}

static void
filter_pass(const float *in, uint32_t w, uint32_t h, uint32_t r, bool horizontal, bool take_max, float *out)
{
	for (uint32_t y = 0; y < h; y++) {
		for (uint32_t x = 0; x < w; x++) {
			uint32_t c = horizontal ? x : y;
			uint32_t len = horizontal ? w : h;
			uint32_t a = c >= r ? c - r : 0;
			uint32_t b = c + r < len ? c + r : len - 1;
			float m = take_max ? -INFINITY : INFINITY;
			for (uint32_t i = a; i <= b; i++) {
				float v = horizontal ? in[y * w + i] : in[i * w + x];
				if (take_max ? v > m : v < m) {
					m = v;
				}
			}
			out[y * w + x] = m;
		}
	}
}

void
u_conv_viewsyn_ref_dilate_max(const float *in, uint32_t w, uint32_t h, uint32_t r, float *tmp, float *out)
{
	if (r == 0) {
		memcpy(out, in, (size_t)w * h * sizeof(float));
		return;
	}
	filter_pass(in, w, h, r, true, true, tmp);
	filter_pass(tmp, w, h, r, false, true, out);
}

void
u_conv_viewsyn_ref_erode_min(const float *in, uint32_t w, uint32_t h, uint32_t r, float *tmp, float *out)
{
	if (r == 0) {
		memcpy(out, in, (size_t)w * h * sizeof(float));
		return;
	}
	filter_pass(in, w, h, r, true, false, tmp);
	filter_pass(tmp, w, h, r, false, false, out);
}

void
u_conv_viewsyn_ref_snap_pack(
    const float *in, uint32_t w, uint32_t h, uint32_t core, float *tmp, float *tmp2, float *out)
{
	size_t n = (size_t)w * h;
	u_conv_viewsyn_ref_erode_min(in, w, h, core, tmp, tmp2);
	for (size_t i = 0; i < n; i++) {
		out[4 * i] = in[i];
		out[4 * i + 1] = tmp2[i];
		out[4 * i + 3] = 1.0f;
	}
	u_conv_viewsyn_ref_dilate_max(in, w, h, core, tmp, tmp2);
	for (size_t i = 0; i < n; i++) {
		out[4 * i + 2] = tmp2[i];
	}
}
