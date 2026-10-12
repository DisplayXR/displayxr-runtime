// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Conversion policy stages: viewpoints, convergence, budget, range,
 *         observers, eyes snapshot.
 * @ingroup aux_conversion
 */

#include "conv_policy.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>


/*
 *
 * Eyes snapshot.
 *
 */

int
conv_eyes_slot_init(struct conv_eyes_slot *s)
{
	memset(s, 0, sizeof(*s));
	return os_mutex_init(&s->mtx);
}

void
conv_eyes_slot_fini(struct conv_eyes_slot *s)
{
	os_mutex_destroy(&s->mtx);
}

void
conv_eyes_slot_publish(struct conv_eyes_slot *s, const struct conv_eyes *e)
{
	os_mutex_lock(&s->mtx);
	s->eyes = *e;
	if (s->eyes.count > CONV_EYES_MAX) {
		s->eyes.count = CONV_EYES_MAX;
	}
	s->generation++;
	s->live = true;
	os_mutex_unlock(&s->mtx);
}

void
conv_eyes_slot_retire(struct conv_eyes_slot *s)
{
	os_mutex_lock(&s->mtx);
	s->live = false;
	s->generation++;
	os_mutex_unlock(&s->mtx);
}

bool
conv_eyes_slot_read(struct conv_eyes_slot *s, uint64_t now_ns, uint64_t max_age_ns, struct conv_eyes *out)
{
	os_mutex_lock(&s->mtx);
	*out = s->eyes;
	const bool live = s->live;
	os_mutex_unlock(&s->mtx);
	bool fresh = live && out->valid;
	if (fresh && max_age_ns != 0 && now_ns > out->timestamp_ns && now_ns - out->timestamp_ns > max_age_ns) {
		fresh = false;
	}
	if (!fresh) {
		out->valid = false;
	}
	return fresh;
}


/*
 *
 * S3: viewpoints.
 *
 */

static void
mid_baseline(const float *vps, uint32_t n, float mid[3], float *baseline)
{
	mid[0] = mid[1] = mid[2] = 0.0f;
	*baseline = 0.0f;
	if (n == 0) {
		return;
	}
	for (uint32_t i = 0; i < n; i++) {
		for (int a = 0; a < 3; a++) {
			mid[a] += vps[3 * i + a] / (float)n;
		}
	}
	if (n >= 2) {
		const float *f = &vps[0];
		const float *e = &vps[3 * (n - 1)];
		const float dx = e[0] - f[0], dy = e[1] - f[1], dz = e[2] - f[2];
		*baseline = sqrtf(dx * dx + dy * dy + dz * dz);
	}
}

/*! Spread a pair (vps[0], vps[1]) into @p n viewpoints at the pair's own
 *  spacing around its midpoint: view i = mid + (b − a)(i − (n − 1)/2). */
static void
spread_pair(float *vps, uint32_t n)
{
	float a[3], d[3];
	for (int k = 0; k < 3; k++) {
		a[k] = vps[k];
		d[k] = vps[3 + k] - vps[k];
	}
	float mid[3] = {a[0] + d[0] * 0.5f, a[1] + d[1] * 0.5f, a[2] + d[2] * 0.5f};
	for (uint32_t i = 0; i < n; i++) {
		const float t = (float)i - (float)(n - 1) * 0.5f;
		for (int k = 0; k < 3; k++) {
			vps[3 * i + k] = mid[k] + d[k] * t;
		}
	}
}

void
conv_policy_viewpoints(const struct conv_policy_view_in *in,
                       struct conv_policy_view_state *state,
                       struct xrt_conv_params *params,
                       struct conv_policy_view_out *out)
{
	struct conv_policy_view_out o;
	memset(&o, 0, sizeof(o));
	if (in->rect_valid) {
		memcpy(o.centre, in->rect_center, sizeof(o.centre));
	}
	const float rect_w = in->rect_w_m > 0.0f ? in->rect_w_m : 0.0f;
	const float rect_h = in->rect_h_m > 0.0f ? in->rect_h_m : 0.0f;
	const float nz = in->nominal_z_m > 0.0f ? in->nominal_z_m : U_LIFT_NOMINAL_Z_DEFAULT_M;

	params->viewpoint_frame = XRT_CONV_VIEWPOINT_FRAME_RECT;
	params->rect_width_m = rect_w;
	params->rect_height_m = rect_h;
	params->nominal_z_m = nz;
	params->viewpoint_count = 0;
	params->viewpoint_source = XRT_CONV_VIEWPOINTS_TRACKED;
	params->axis_mode = XRT_CONV_AXIS_X;
	params->baseline_m = 0.0f;
	params->max_offset_m = 0.0f;
	memset(params->viewpoints_xyz, 0, sizeof(params->viewpoints_xyz));
	o.mid[2] = nz;
	o.source = XRT_CONV_VIEWPOINTS_TRACKED;

	const uint32_t n = in->view_count > XRT_CONV_MAX_VIEWS ? XRT_CONV_MAX_VIEWS : in->view_count;
	if (n == 0) {
		// Depth / splat streams get no viewpoints (resolve_viewpoints: DEPTH / GAUSSIANS).
		if (out != NULL) {
			*out = o;
		}
		return;
	}

	float vps[3 * XRT_CONV_MAX_VIEWS];
	memset(vps, 0, sizeof(vps));
	float baseline = 0.0f;

	if (in->explicit_count >= 1) {
		// EXPLICIT: the app's own viewpoints, only rebased.
		uint32_t m = in->explicit_count > XRT_CONV_MAX_VIEWS ? XRT_CONV_MAX_VIEWS : in->explicit_count;
		memcpy(vps, in->explicit_xyz, sizeof(float) * 3 * m);
		u_lift_viewpoint_rebase(vps, m, o.centre);
		if (m != n) {
			if (m == 1) {
				// One point: the straight-on viewer pair through it.
				vps[3] = vps[0] + CONV_POLICY_NOMINAL_IPD_M * 0.5f;
				vps[0] -= CONV_POLICY_NOMINAL_IPD_M * 0.5f;
				vps[4] = vps[1];
				vps[5] = vps[2];
			}
			spread_pair(vps, n);
		}
		params->axis_mode = XRT_CONV_AXIS_XYZ;
		o.source = XRT_CONV_VIEWPOINTS_EXPLICIT;
	} else {
		struct u_lift_view_control vc = in->vc;
		u_lift_view_control_sanitize(&vc);
		if (in->recenter_off) {
			vc.recenter_mode = U_LIFT_RECENTER_OFF;
		}
		struct u_lift_rig rig = in->rig;
		u_lift_rig_sanitize(&rig);
		if (rig.type != U_LIFT_RIG_NONE) {
			// The rig owns the eye factors: the policy keeps axis mask, recenter, clamp.
			vc.ipd_factor = 1.0f;
			vc.parallax_factor = 1.0f;
		}
		params->axis_mode = vc.axis_mode;
		params->max_offset_m = vc.max_offset_m;

		uint32_t m = 2;
		struct u_lift_recenter *rc = state != NULL ? &state->recenter : NULL;
		if (in->eyes == NULL || !in->eyes->valid || in->eyes->count < 2) {
			// Stale snapshot (no panel DP, DP gone): the nominal viewer, as today.
			o.nominal = true;
			vps[0] = -CONV_POLICY_NOMINAL_IPD_M * 0.5f;
			vps[3] = CONV_POLICY_NOMINAL_IPD_M * 0.5f;
			vps[2] = vps[5] = nz;
			if (rc != NULL) {
				u_lift_recenter_reset(rc);
			}
			rc = NULL;
		} else {
			if (!in->eyes->is_tracking && rc != NULL) {
				u_lift_recenter_reset(rc); // ADR-048: reset on tracking loss
			}
			m = in->eyes->count == n ? n : 2;
			for (uint32_t i = 0; i < m; i++) {
				vps[3 * i + 0] = in->eyes->xyz[i][0];
				vps[3 * i + 1] = in->eyes->xyz[i][1];
				vps[3 * i + 2] = in->eyes->xyz[i][2];
			}
			u_lift_viewpoint_rebase(vps, m, o.centre);
		}
		float mid[3];
		u_lift_viewpoint_apply(&vc, rc, nz, in->now_ns, vps, m, vps, &baseline, mid);
		if (rig.type != U_LIFT_RIG_NONE && u_lift_rig_apply(&rig, nz, rect_w, rect_h, vps, m, vps)) {
			o.source = rig.type == U_LIFT_RIG_CAMERA ? XRT_CONV_VIEWPOINTS_CAMERA_RIG
			                                         : XRT_CONV_VIEWPOINTS_DISPLAY_RIG;
		}
		if (m != n) {
			spread_pair(vps, n);
		}
	}

	mid_baseline(vps, n, o.mid, &baseline);
	memcpy(params->viewpoints_xyz, vps, sizeof(float) * 3 * n);
	params->viewpoint_count = n;
	params->viewpoint_source = o.source;
	params->baseline_m = baseline;
	if (out != NULL) {
		*out = o;
	}
}


/*
 *
 * Depth units.
 *
 */

bool
conv_policy_kind_is_metric(uint32_t depth_kind)
{
	return depth_kind == XRT_CONV_DEPTH_METRIC || depth_kind == XRT_CONV_DEPTH_METRIC_INVERSE;
}

double
conv_policy_nearness(uint32_t depth_kind, double decoded)
{
	switch (depth_kind) {
	case XRT_CONV_DEPTH_METRIC: return decoded > 0.0 ? 1.0 / decoded : 0.0;
	case XRT_CONV_DEPTH_AFFINE: return -decoded;
	default: return decoded;
	}
}

double
conv_policy_from_nearness(uint32_t depth_kind, double nearness)
{
	switch (depth_kind) {
	case XRT_CONV_DEPTH_METRIC: return nearness > 0.0 ? 1.0 / nearness : (double)FLT_MAX;
	case XRT_CONV_DEPTH_AFFINE: return -nearness;
	default: return nearness;
	}
}

bool
conv_policy_depth_range(const struct xrt_conv_depth_info *di, float *out_near, float *out_far)
{
	if (di == NULL) {
		return false;
	}
	float a, b;
	if (di->range_p05 != 0.0f || di->range_p95 != 0.0f) {
		a = di->range_p05;
		b = di->range_p95;
	} else if (di->range_min != 0.0f || di->range_max != 0.0f) {
		a = di->range_min;
		b = di->range_max;
	} else {
		return false;
	}
	if (!isfinite(a) || !isfinite(b) || a == b) {
		return false;
	}
	const bool a_nearer = conv_policy_nearness(di->kind, a) >= conv_policy_nearness(di->kind, b);
	*out_near = a_nearer ? a : b;
	*out_far = a_nearer ? b : a;
	return true;
}

static int
cmp_float(const void *pa, const void *pb)
{
	const float a = *(const float *)pa, b = *(const float *)pb;
	return a < b ? -1 : (a > b ? 1 : 0);
}

#define CONV_STATS_MAX_SAMPLES 4096u

bool
conv_policy_depth_stats(
    struct xrt_conv_depth_info *di, const float *samples, uint32_t w, uint32_t h, uint32_t stride_floats)
{
	const bool have_pct = di->range_p05 != 0.0f || di->range_p95 != 0.0f;
	const bool have_minmax = di->range_min != 0.0f || di->range_max != 0.0f;
	if (have_pct && have_minmax) {
		return true;
	}
	if (samples == NULL || w == 0 || h == 0) {
		return have_pct || have_minmax;
	}
	if (stride_floats < w) {
		stride_floats = w;
	}
	const float scale = di->value_scale != 0.0f ? di->value_scale : 1.0f;
	const float shift = di->value_shift;
	const bool has_invalid = !isnan(di->invalid_sample);

	// Every sample for min / max; a regular subsample for the percentiles.
	const uint64_t total = (uint64_t)w * h;
	uint64_t step = total / CONV_STATS_MAX_SAMPLES;
	if (step == 0) {
		step = 1;
	}
	float *pick = (float *)malloc(sizeof(float) * CONV_STATS_MAX_SAMPLES);
	if (pick == NULL) {
		return have_pct || have_minmax;
	}
	uint32_t k = 0;
	float lo = FLT_MAX, hi = -FLT_MAX;
	uint64_t idx = 0;
	for (uint32_t y = 0; y < h; y++) {
		const float *row = samples + (size_t)y * stride_floats;
		for (uint32_t x = 0; x < w; x++, idx++) {
			const float s = row[x];
			if (!isfinite(s) || (has_invalid && s == di->invalid_sample)) {
				continue;
			}
			const float v = scale * s + shift;
			lo = v < lo ? v : lo;
			hi = v > hi ? v : hi;
			if (idx % step == 0 && k < CONV_STATS_MAX_SAMPLES) {
				pick[k++] = v;
			}
		}
	}
	bool ok = false;
	if (k > 0) {
		if (!have_minmax) {
			di->range_min = lo;
			di->range_max = hi;
		}
		if (!have_pct) {
			qsort(pick, k, sizeof(float), cmp_float);
			di->range_p05 = pick[(uint32_t)((double)(k - 1) * 0.05 + 0.5)];
			di->range_p95 = pick[(uint32_t)((double)(k - 1) * 0.95 + 0.5)];
		}
		ok = true;
	}
	free(pick);
	return ok || have_pct || have_minmax;
}


/*
 *
 * Convergence.
 *
 */

static float
clamp01(float v)
{
	if (!(v >= 0.0f)) {
		return 0.0f;
	}
	return v > 1.0f ? 1.0f : v;
}

bool
conv_policy_convergence_to_decoded(float rel01, const struct xrt_conv_depth_info *last, float *out_decoded)
{
	float near_v, far_v;
	if (!conv_policy_depth_range(last, &near_v, &far_v)) {
		return false;
	}
	const double nn = conv_policy_nearness(last->kind, near_v);
	const double nf = conv_policy_nearness(last->kind, far_v);
	const double n = nn + (double)clamp01(rel01) * (nf - nn);
	*out_decoded = (float)conv_policy_from_nearness(last->kind, n);
	return true;
}

bool
conv_policy_convergence_from_decoded(float decoded, const struct xrt_conv_depth_info *last, float *out_rel01)
{
	float near_v, far_v;
	if (!conv_policy_depth_range(last, &near_v, &far_v)) {
		return false;
	}
	const double nn = conv_policy_nearness(last->kind, near_v);
	const double nf = conv_policy_nearness(last->kind, far_v);
	if (nf == nn) {
		return false;
	}
	*out_rel01 = (float)((conv_policy_nearness(last->kind, decoded) - nn) / (nf - nn));
	return true;
}

void
conv_policy_convergence(const struct conv_policy_conv_in *in,
                        struct xrt_conv_params *params,
                        struct conv_policy_conv_out *out)
{
	struct conv_policy_conv_out o = {CONV_CONVERGENCE_SRC_AUTO, 0.0f};
	params->convergence_mode = XRT_CONV_CONVERGENCE_AUTO;
	params->convergence = 0.0f;

	float decoded = 0.0f;
	if (in->app_set && conv_policy_convergence_to_decoded(in->app_rel01, in->last, &decoded)) {
		o.source = CONV_CONVERGENCE_SRC_APP;
		o.rel01 = clamp01(in->app_rel01);
	} else if (!in->app_set && in->observer_valid &&
	           conv_policy_convergence_to_decoded(in->observer_rel01, in->last, &decoded)) {
		o.source = CONV_CONVERGENCE_SRC_OBSERVER;
		o.rel01 = clamp01(in->observer_rel01);
	} else if (!in->app_set && in->hint_allowed && in->last != NULL && (in->last->suggest_flags & 1u) != 0 &&
	           isfinite(in->last->suggested_convergence)) {
		decoded = in->last->suggested_convergence; // already in decoded units
		o.source = CONV_CONVERGENCE_SRC_HINT;
		float rel = 0.0f;
		o.rel01 = conv_policy_convergence_from_decoded(decoded, in->last, &rel) ? clamp01(rel) : 0.0f;
	}
	if (o.source != CONV_CONVERGENCE_SRC_AUTO) {
		params->convergence_mode = XRT_CONV_CONVERGENCE_DECODED;
		params->convergence = decoded;
	}
	if (out != NULL) {
		*out = o;
	}
}


/*
 *
 * Budget.
 *
 */

void
conv_policy_budget(const struct conv_policy_budget_in *in, struct conv_policy_budget_out *out)
{
	const float strength = in->strength > 0.0f && isfinite(in->strength) ? in->strength : 1.0f;
	const float def = in->default_budget > 0.0f ? in->default_budget : CONV_POLICY_BUDGET_DEFAULT;
	const float d = in->viewer_distance_m > 0.0f ? in->viewer_distance_m : U_LIFT_NOMINAL_Z_DEFAULT_M;
	const float ipd = in->ipd_m > 0.0f ? in->ipd_m : CONV_POLICY_NOMINAL_IPD_M;

	struct conv_policy_budget_out o;
	memset(&o, 0, sizeof(o));
	o.rear_cap_m = in->rear_cap_m;

	float near_v, far_v;
	if (conv_policy_kind_is_metric(in->depth_kind) && in->display_width_m > 0.0f &&
	    conv_policy_depth_range(in->last, &near_v, &far_v)) {
		// Real-world disparity a nominal pair would see, shown on this display.
		const double inv_near = conv_policy_nearness(in->depth_kind, near_v); // 1/z
		const double inv_far = conv_policy_nearness(in->depth_kind, far_v);
		double natural = (double)ipd * (inv_near - inv_far) * (double)d / (double)in->display_width_m;
		const double cap = (double)def * CONV_POLICY_BUDGET_METRIC_MAX_FACTOR;
		natural = natural < 0.0 ? 0.0 : (natural > cap ? cap : natural);
		o.budget = (float)natural * strength;
		o.source = CONV_BUDGET_SRC_METRIC_RANGE;
	} else {
		// Affine (scale unknown by definition): never derived from the range.
		o.budget = def * strength;
		o.source = CONV_BUDGET_SRC_STRENGTH;
		if (in->corpus_validated && in->last != NULL && (in->last->suggest_flags & 2u) != 0 &&
		    in->last->suggested_strength > 0.0f && isfinite(in->last->suggested_strength)) {
			o.budget *= in->last->suggested_strength;
			o.source = CONV_BUDGET_SRC_VALIDATED_HINT;
		}
	}
	o.strength = o.budget / def;
	*out = o;
}


/*
 *
 * Observers.
 *
 */

bool
conv_policy_observe(struct conv_observer *obs,
                    const struct xrt_conv_depth_info *di,
                    const struct conv_depth_samples *s,
                    const float active[4],
                    uint64_t now_ns)
{
	if (obs == NULL || obs->ops == NULL || obs->ops->observe == NULL || di == NULL) {
		return false;
	}
	if (di->same_inference != 1u) {
		return false; // a different inference than the frame shown: "depth late vs image"
	}
	obs->ops->observe(obs->ctx, di, s, active, now_ns);
	return true;
}

void
conv_policy_observer_reset(struct conv_observer *obs)
{
	if (obs != NULL && obs->ops != NULL && obs->ops->reset != NULL) {
		obs->ops->reset(obs->ctx);
	}
}
