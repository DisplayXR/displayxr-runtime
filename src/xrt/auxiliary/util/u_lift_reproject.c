// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-058 late placement: pure policy + CPU reference (see u_lift_reproject.h).
 * @ingroup aux_util
 */

#include "u_lift_reproject.h"

#include <math.h>
#include <string.h>


/*
 *
 * (a) Viewpoint delta.
 *
 */

bool
u_lift_reproject_compute_delta(const float (*recorded)[3],
                               const float (*current)[3],
                               uint32_t view_count,
                               struct u_lift_reproject_delta *out)
{
	memset(out, 0, sizeof(*out));
	if (recorded == NULL || current == NULL || view_count == 0 || view_count > U_LIFT_REPROJECT_MAX_VIEWS) {
		return false;
	}
	struct u_lift_reproject_delta d;
	memset(&d, 0, sizeof(d));
	d.view_count = view_count;
	for (uint32_t i = 0; i < view_count; i++) {
		double s = 0.0;
		for (uint32_t k = 0; k < 3; k++) {
			const float a = recorded[i][k], b = current[i][k];
			if (!isfinite(a) || !isfinite(b)) {
				return false;
			}
			d.d_m[i][k] = b - a;
			s += (double)d.d_m[i][k] * (double)d.d_m[i][k];
		}
		d.len_m[i] = (float)sqrt(s);
		if (d.len_m[i] > d.max_m) {
			d.max_m = d.len_m[i];
		}
	}
	*out = d;
	return true;
}


/*
 *
 * (b) Correction policy.
 *
 */

void
u_lift_reproject_policy_defaults(struct u_lift_reproject_policy *p)
{
	p->max_m = U_LIFT_REPROJECT_MAX_M_DEFAULT;
}

bool
u_lift_reproject_policy_set_max_m(struct u_lift_reproject_policy *p, float max_m)
{
	if (!isfinite(max_m) || !(max_m > 0.0f)) {
		return false;
	}
	p->max_m = max_m;
	return true;
}

float
u_lift_reproject_ease(const struct u_lift_reproject_policy *p, float delta_m)
{
	const double lim = p != NULL && isfinite(p->max_m) && p->max_m > 0.0f ? (double)p->max_m
	                                                                      : (double)U_LIFT_REPROJECT_MAX_M_DEFAULT;
	if (!isfinite(delta_m)) {
		return 0.0f;
	}
	const double d = fabs((double)delta_m);
	if (d <= lim) {
		return 1.0f;
	}
	double t = (d - lim) / lim; // 0 at the limit, 1 at twice the limit
	if (t >= 1.0) {
		return 0.0f;
	}
	return (float)(1.0 - t * t * (3.0 - 2.0 * t));
}


/*
 *
 * (c) Late synthesis.
 *
 */

static bool
is_metric(uint32_t kind)
{
	return kind == XRT_CONV_DEPTH_METRIC || kind == XRT_CONV_DEPTH_METRIC_INVERSE;
}

//! A metric depth (metres) as a decoded value of @p kind.
static double
metres_as_decoded(uint32_t kind, double metres)
{
	return kind == XRT_CONV_DEPTH_METRIC_INVERSE ? 1.0 / metres : metres;
}

bool
u_lift_reproject_display_params(const struct u_conv_viewsyn_params *base,
                                const struct u_lift_reproject_display *d,
                                const float (*viewpoints)[3],
                                uint32_t view_count,
                                struct u_conv_viewsyn_params *out,
                                float out_rect_center[3])
{
	if (base == NULL || d == NULL || out == NULL || viewpoints == NULL || view_count == 0 ||
	    view_count > U_LIFT_REPROJECT_MAX_VIEWS || !d->transform_valid || !is_metric(base->depth.kind)) {
		return false;
	}
	const float *m = d->depth_to_display;
	const double W = d->rect_w_m, H = d->rect_h_m;
	if (!(W > 0.0) || !(H > 0.0) || !isfinite(W) || !isfinite(H) || !(m[0] > 0.0f) || !isfinite(m[0])) {
		return false;
	}
	// col 0 = (W/dc, 0, 0, 0), col 1 = (0, -H/dc, 0, 0),
	// col 2 = ((-W/2 - Cx)/dc, (H/2 - Cy)/dc, -Cz/dc, 0), col 3 = R + C.
	const double dc = W / (double)m[0];
	if (!isfinite(dc) || !(dc > 0.0) || fabs((double)m[5] * dc + H) > 1e-4 * H) {
		return false;
	}
	const double tcx = -(double)m[8] * dc - 0.5 * W;
	const double tcy = 0.5 * H - (double)m[9] * dc;
	const double tcz = -(double)m[10] * dc;
	if (!isfinite(tcx) || !isfinite(tcy) || !(tcz > 0.0)) {
		return false;
	}
	const double near_m = d->near_m, far_m = d->far_m;
	if (!(near_m > 0.0) || !(far_m > 0.0) || !isfinite(near_m) || !isfinite(far_m)) {
		return false;
	}
	// The source camera: the viewpoints' midpoint, as S5 resolves it.
	double cz = 0.0;
	for (uint32_t i = 0; i < view_count; i++) {
		cz += (double)viewpoints[i][2];
	}
	cz /= (double)view_count;
	if (!(cz > 0.0)) {
		return false;
	}

	struct u_conv_viewsyn_params p = *base;
	p.view_count = view_count;
	memcpy(p.viewpoints_m, viewpoints, sizeof(float) * 3 * view_count);
	p.rect_width_m = (float)W;
	p.rect_height_m = (float)H;
	p.has_source_camera = false;

	// Range in nearness (1 / metres) covering the content AND the plane, so
	// S5's clamps never bite: n = (N - lo) / (hi - lo), n_conv at N = 1 / dc.
	const double n_dc = 1.0 / dc;
	const double lo = fmin(fmin(1.0 / near_m, 1.0 / far_m), n_dc);
	const double hi = fmax(fmax(1.0 / near_m, 1.0 / far_m), n_dc);
	if (!(hi - lo > 1e-6)) {
		return false; // a flat map on the plane: nothing to place
	}
	p.range_near = (float)metres_as_decoded(p.depth.kind, 1.0 / hi);
	p.range_far = (float)metres_as_decoded(p.depth.kind, 1.0 / lo);
	p.convergence = (float)metres_as_decoded(p.depth.kind, dc);
	// S5: q = qScale (n - n_conv) and qScale = budget A / (kappa C.z_wh), with
	// A = W / H and C.z_wh = cz / H. The texel at depth d lies on the ray from
	// C through its rect position at the fraction d / dc, i.e. at
	// q = dc / d - 1 = dc (N - n_dc) = dc (hi - lo) (n - n_conv).
	const double q_scale = dc * (hi - lo);
	p.budget = (float)(q_scale * U_CONV_VIEWSYN_KAPPA * cz / W);
	// Metric placement is exact: the ADR-040 cap is the caller's to re-apply.
	p.rear_cap_m = U_CONV_VIEWSYN_REAR_UNLIMITED;

	if (out_rect_center != NULL) {
		out_rect_center[0] = (float)((double)m[12] - tcx);
		out_rect_center[1] = (float)((double)m[13] - tcy);
		out_rect_center[2] = (float)((double)m[14] - tcz);
	}
	*out = p;
	return true;
}

static bool
resolve_source(const struct u_lift_reproject_source *src,
               const float (*viewpoints)[3],
               uint32_t view_count,
               struct u_conv_viewsyn_params *out)
{
	switch (src->geometry) {
	case U_LIFT_REPROJECT_COMPOSED: {
		if (src->params.depth.kind == XRT_CONV_DEPTH_UNKNOWN || view_count != src->params.view_count ||
		    view_count == 0 || view_count > U_LIFT_REPROJECT_MAX_VIEWS) {
			return false;
		}
		struct u_conv_viewsyn_params p = src->params;
		memcpy(p.viewpoints_m, viewpoints, sizeof(float) * 3 * view_count);
		*out = p;
		return true;
	}
	case U_LIFT_REPROJECT_DISPLAY:
		return view_count == src->params.view_count &&
		       u_lift_reproject_display_params(&src->params, &src->display, viewpoints, view_count, out, NULL);
	default: return false;
	}
}

static void
hold(struct u_lift_reproject_decision *d)
{
	if (d != NULL) {
		memset(d, 0, sizeof(*d));
		d->verdict = U_LIFT_REPROJECT_HOLD;
		d->ease = 0.0f;
	}
}

enum u_lift_reproject_verdict
u_lift_reproject_late_params(const struct u_lift_reproject_policy *pol,
                             const struct u_lift_reproject_source *src,
                             const float (*current)[3],
                             uint32_t current_count,
                             struct u_conv_viewsyn_params *out_params,
                             struct u_lift_reproject_decision *out_decision)
{
	hold(out_decision);
	if (src == NULL || current == NULL || out_params == NULL) {
		return U_LIFT_REPROJECT_HOLD;
	}
	// The source as recorded (its own viewpoints) must be valid too: a result
	// that could not have been synthesized is never re-placed.
	const float(*recorded)[3] = (const float(*)[3])src->params.viewpoints_m;
	struct u_conv_viewsyn_params rec, late;
	struct u_conv_viewsyn_frame f;
	if (!resolve_source(src, recorded, src->params.view_count, &rec) || rec.view_count != current_count ||
	    !u_conv_viewsyn_resolve(&rec, &f) || !resolve_source(src, current, current_count, &late)) {
		return U_LIFT_REPROJECT_HOLD;
	}
	struct u_lift_reproject_decision dec;
	memset(&dec, 0, sizeof(dec));
	if (!u_lift_reproject_compute_delta(recorded, current, current_count, &dec.delta)) {
		return U_LIFT_REPROJECT_HOLD;
	}

	dec.ease = u_lift_reproject_ease(pol, dec.delta.max_m);
	if (dec.ease < 1.0f) {
		// Toward the plane: zero parallax at the convergence value, so the
		// eased content flattens onto the glass instead of tearing.
		late.budget = late.budget * dec.ease;
		dec.verdict = U_LIFT_REPROJECT_EASE;
	} else {
		dec.verdict = U_LIFT_REPROJECT_PLACE;
	}
	if (!u_conv_viewsyn_resolve(&late, &f)) {
		return U_LIFT_REPROJECT_HOLD; // e.g. a current eye at or behind the glass
	}
	*out_params = late;
	if (out_decision != NULL) {
		*out_decision = dec;
	}
	return dec.verdict;
}

enum u_lift_reproject_verdict
u_lift_reproject_render(const struct u_lift_reproject_policy *pol,
                        const struct u_lift_reproject_source *src,
                        const float (*current)[3],
                        uint32_t current_count,
                        uint32_t out_w,
                        uint32_t out_h,
                        float *out,
                        struct u_lift_reproject_decision *out_decision)
{
	struct u_conv_viewsyn_params late;
	struct u_lift_reproject_decision dec;
	enum u_lift_reproject_verdict v = u_lift_reproject_late_params(pol, src, current, current_count, &late, &dec);
	if (v == U_LIFT_REPROJECT_HOLD ||
	    !u_conv_viewsyn_ref_render(&late, src->color, src->color_w, src->color_h, src->depth, src->depth_w,
	                               src->depth_h, out_w, out_h, out)) {
		hold(out_decision);
		return U_LIFT_REPROJECT_HOLD;
	}
	if (out_decision != NULL) {
		*out_decision = dec;
	}
	return v;
}


/*
 *
 * Re-run rule.
 *
 */

bool
u_lift_reproject_needs_rerun(
    const float (*prev)[3], const float (*cur)[3], uint32_t view_count, float prev_ease, float cur_ease)
{
	if (prev == NULL || cur == NULL || view_count == 0 || view_count > U_LIFT_REPROJECT_MAX_VIEWS ||
	    !isfinite(prev_ease) || !isfinite(cur_ease)) {
		return true;
	}
	if (fabs((double)cur_ease - (double)prev_ease) > (double)U_LIFT_REPROJECT_RERUN_EASE) {
		return true;
	}
	double pc[3] = {0, 0, 0}, cc[3] = {0, 0, 0};
	for (uint32_t i = 0; i < view_count; i++) {
		for (uint32_t k = 0; k < 3; k++) {
			if (!isfinite(prev[i][k]) || !isfinite(cur[i][k])) {
				return true;
			}
			pc[k] += prev[i][k];
			cc[k] += cur[i][k];
		}
	}
	for (uint32_t k = 0; k < 3; k++) {
		pc[k] /= (double)view_count;
		cc[k] /= (double)view_count;
	}
	if (fabs(cc[2] - pc[2]) > (double)U_LIFT_REPROJECT_RERUN_DISTANCE_M) {
		return true;
	}
	const double thr = (double)U_LIFT_REPROJECT_RERUN_SPREAD_M;
	double tilt = 0.0; // the largest view depth offset from the midpoint, over its distance
	for (uint32_t i = 0; i < view_count; i++) {
		double s = 0.0;
		for (uint32_t k = 0; k < 3; k++) {
			const double d = ((double)cur[i][k] - cc[k]) - ((double)prev[i][k] - pc[k]);
			s += d * d;
		}
		if (s > thr * thr) {
			return true;
		}
		if (cc[2] > 0.0) {
			const double t = fabs((double)cur[i][2] - cc[2]) / cc[2];
			tilt = t > tilt ? t : tilt;
		}
	}
	// A common (x, y) move is exact only for a pair parallel to the glass: a
	// turned head (views at different depths) shifts each view by about
	// move * (dz / z) * q, q <= 1. Bound it by the same threshold.
	const double lx = cc[0] - pc[0], ly = cc[1] - pc[1];
	return sqrt(lx * lx + ly * ly) * tilt > thr;
}


/*
 *
 * Ages.
 *
 */

void
u_lift_reproject_compute_ages(uint64_t now_ns,
                              uint64_t content_ns,
                              uint64_t placement_ns,
                              struct u_lift_reproject_ages *out)
{
	out->content_age_ns = now_ns > content_ns ? now_ns - content_ns : 0;
	if (placement_ns == 0) {
		out->placement_age_ns = out->content_age_ns;
	} else {
		out->placement_age_ns = now_ns > placement_ns ? now_ns - placement_ns : 0;
	}
}
