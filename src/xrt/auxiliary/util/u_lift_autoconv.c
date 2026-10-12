// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Runtime-owned auto-convergence for lifted video — see u_lift_autoconv.h.
 * @ingroup aux_util
 */

#include "util/u_lift_autoconv.h"

#include <math.h>
#include <stddef.h>


/*
 *
 * Helpers.
 *
 */

static float
clampf(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static bool
finitef(float v)
{
	return v == v && v - v == 0.0f; // not NaN, not +-inf
}

static char
lower_ascii(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool
eq_nocase(const char *a, const char *b)
{
	while (*a != '\0' && *b != '\0') {
		if (lower_ascii(*a) != lower_ascii(*b)) {
			return false;
		}
		a++;
		b++;
	}
	return *a == '\0' && *b == '\0';
}

static float
median3(const float *v, uint32_t n)
{
	if (n == 1) {
		return v[0];
	}
	if (n == 2) {
		return 0.5f * (v[0] + v[1]);
	}
	const float a = v[0], b = v[1], c = v[2];
	if ((a <= b && b <= c) || (c <= b && b <= a)) {
		return b;
	}
	if ((b <= a && a <= c) || (c <= a && a <= b)) {
		return a;
	}
	return c;
}


/*
 *
 * Source switch.
 *
 */

enum u_lift_autoconv_source
u_lift_autoconv_source_parse(const char *value)
{
	if (value == NULL) {
		return U_LIFT_AUTOCONV_SOURCE_RUNTIME;
	}
	if (eq_nocase(value, "module") || eq_nocase(value, "off") || eq_nocase(value, "0")) {
		return U_LIFT_AUTOCONV_SOURCE_MODULE;
	}
	return U_LIFT_AUTOCONV_SOURCE_RUNTIME;
}


/*
 *
 * Grid.
 *
 */

static bool
decode_inverse(float sample, bool encoding_inverse, float scale, float offset, float *out_q)
{
	if (!finitef(sample)) {
		return false;
	}
	const float d = scale * sample + offset;
	if (!finitef(d)) {
		return false;
	}
	if (encoding_inverse) {
		if (d < 0.0f) {
			return false;
		}
		*out_q = d; // 0 = the far end, valid
		return true;
	}
	if (!(d > 0.0f)) {
		return false;
	}
	*out_q = 1.0f / d;
	return finitef(*out_q);
}

bool
u_lift_autoconv_grid_from_samples(const float *samples,
                                  uint32_t sw,
                                  uint32_t sh,
                                  uint32_t stride_floats,
                                  bool encoding_inverse,
                                  float value_scale,
                                  float value_offset,
                                  uint32_t max_w,
                                  struct u_lift_autoconv_grid *out)
{
	if (samples == NULL || out == NULL || sw == 0 || sh == 0 || stride_floats < sw) {
		return false;
	}
	const float scale = (value_scale != 0.0f && finitef(value_scale)) ? value_scale : 1.0f;
	const float offset = finitef(value_offset) ? value_offset : 0.0f;

	uint32_t gw = max_w > 0 ? max_w : U_LIFT_AUTOCONV_GRID_MAX_W;
	if (gw > U_LIFT_AUTOCONV_GRID_MAX_W) {
		gw = U_LIFT_AUTOCONV_GRID_MAX_W;
	}
	if (gw > sw) {
		gw = sw;
	}
	uint32_t gh = (uint32_t)((double)sh * (double)gw / (double)sw + 0.5);
	if (gh < 1) {
		gh = 1;
	}
	if (gh > U_LIFT_AUTOCONV_GRID_MAX_H) {
		gh = U_LIFT_AUTOCONV_GRID_MAX_H;
	}
	if (gh > sh) {
		gh = sh;
	}
	out->w = gw;
	out->h = gh;

	// Box average of inverse depth per cell, one grid row at a time (a row of
	// accumulators instead of a whole grid's worth on the stack).
	float qmin = INFINITY, qmax = -INFINITY;
	uint32_t valid_cells = 0;
	for (uint32_t gy = 0; gy < gh; gy++) {
		double sum[U_LIFT_AUTOCONV_GRID_MAX_W];
		uint32_t cnt[U_LIFT_AUTOCONV_GRID_MAX_W];
		for (uint32_t gx = 0; gx < gw; gx++) {
			sum[gx] = 0.0;
			cnt[gx] = 0;
		}
		const uint32_t y0 = (uint32_t)((uint64_t)gy * sh / gh);
		const uint32_t y1 = (uint32_t)((uint64_t)(gy + 1) * sh / gh);
		for (uint32_t y = y0; y < y1; y++) {
			const float *row = samples + (size_t)y * stride_floats;
			for (uint32_t x = 0; x < sw; x++) {
				float q;
				if (!decode_inverse(row[x], encoding_inverse, scale, offset, &q)) {
					continue;
				}
				const uint32_t gx = (uint32_t)((uint64_t)x * gw / sw);
				sum[gx] += q;
				cnt[gx]++;
				if (q < qmin) {
					qmin = q;
				}
				if (q > qmax) {
					qmax = q;
				}
			}
		}
		for (uint32_t gx = 0; gx < gw; gx++) {
			out->v[gy * gw + gx] = cnt[gx] > 0 ? (float)(sum[gx] / cnt[gx]) : NAN;
			valid_cells += cnt[gx] > 0 ? 1u : 0u;
		}
	}
	const float range = qmax - qmin;
	if (valid_cells == 0 || !finitef(range) || !(range > 1e-6f * (fabsf(qmax) > 1.0f ? fabsf(qmax) : 1.0f))) {
		return false; // nothing decodes, or a flat frame: no depth to pick from
	}
	const float inv = 1.0f / range;
	for (uint32_t i = 0; i < gw * gh; i++) {
		if (finitef(out->v[i])) {
			out->v[i] = clampf((out->v[i] - qmin) * inv, 0.0f, 1.0f);
		}
	}
	return true;
}


/*
 *
 * Estimator.
 *
 */

//! Weighted quantile q in [0,1] of a 64-bin histogram over [0,1], interpolated in its bin.
static float
weighted_quantile(const double counts[U_LIFT_AUTOCONV_BINS], double wsum, double q)
{
	const double target = q * wsum;
	double acc = 0.0;
	for (uint32_t b = 0; b < U_LIFT_AUTOCONV_BINS; b++) {
		if (acc + counts[b] >= target && counts[b] > 0.0) {
			const double f = (target - acc) / counts[b];
			return (float)(((double)b + f) / U_LIFT_AUTOCONV_BINS);
		}
		acc += counts[b];
	}
	return 1.0f;
}

void
u_lift_autoconv_params_default(struct u_lift_autoconv_params *p)
{
	p->center_sigma = 0.35f;
	p->bottom_damp = 0.5f;
	p->hist_smooth_bins = 1.5f;
	// Fitted to professional stereo (5 3D trailers, 119 shots, FoundationStereo
	// disparity as ground truth, 2026-10-12): stereographers put the screen
	// plane at the centre-weighted ~76th percentile of nearness (about a quarter
	// of the frame in front of it), consistently across titles. The best
	// predictor of that plane from depth alone was the mean of the
	// centre-weighted 75th percentile and the dominant clump with the far
	// background excluded (mean error 0.21 in normalised depth per shot, vs
	// 0.34 for the earlier foreground-biased mode, which latched onto large far
	// backgrounds and pushed most of the frame out of the screen).
	p->fg_bias = 0.0f;
	p->far_exclude = 0.15f;
	p->percentile = 75.0f;
	p->blend = 0.5f;
	p->peak_floor = 0.05f;
	p->min_confidence = 0.06f;
	p->min_samples = 64;
	p->prefer_h = -1.0f;
	p->prefer_window = 0.08f;
	p->switch_ratio = 1.5f;
}

bool
u_lift_autoconv_find_subject(const struct u_lift_autoconv_grid *g,
                             const float active[4],
                             const struct u_lift_autoconv_params *p,
                             struct u_lift_autoconv_estimate *out)
{
	if (out == NULL) {
		return false;
	}
	*out = (struct u_lift_autoconv_estimate){0};
	if (g == NULL || p == NULL || g->w == 0 || g->h == 0) {
		return false;
	}
	float a[4] = {0.0f, 0.0f, 1.0f, 1.0f};
	if (active != NULL) {
		for (int i = 0; i < 4; i++) {
			a[i] = finitef(active[i]) ? clampf(active[i], 0.0f, 1.0f) : a[i];
		}
	}
	// Cells only partly inside the region (a bar's edge) are left out: round
	// inward, outward only if that leaves nothing.
	const float eps = 1e-3f;
	int32_t x0 = (int32_t)ceilf(a[0] * (float)g->w - eps);
	int32_t y0 = (int32_t)ceilf(a[1] * (float)g->h - eps);
	int32_t x1 = (int32_t)floorf(a[2] * (float)g->w + eps);
	int32_t y1 = (int32_t)floorf(a[3] * (float)g->h + eps);
	if (x1 <= x0) {
		x0 = (int32_t)floorf(a[0] * (float)g->w);
		x1 = (int32_t)ceilf(a[2] * (float)g->w);
	}
	if (y1 <= y0) {
		y0 = (int32_t)floorf(a[1] * (float)g->h);
		y1 = (int32_t)ceilf(a[3] * (float)g->h);
	}
	x0 = x0 < 0 ? 0 : x0;
	y0 = y0 < 0 ? 0 : y0;
	if (x1 > (int32_t)g->w) {
		x1 = (int32_t)g->w;
	}
	if (y1 > (int32_t)g->h) {
		y1 = (int32_t)g->h;
	}
	if (x1 <= x0 || y1 <= y0) {
		return false;
	}
	const float rw = (float)(x1 - x0), rh = (float)(y1 - y0);
	const float cx = (float)x0 + 0.5f * rw, cy = (float)y0 + 0.5f * rh;
	const float sx = (p->center_sigma > 0.0f ? p->center_sigma : 0.35f) * rw;
	const float sy = (p->center_sigma > 0.0f ? p->center_sigma : 0.35f) * rh;
	const float damp = clampf(p->bottom_damp, 0.0f, 1.0f);

	double counts[U_LIFT_AUTOCONV_BINS] = {0};
	double wsum = 0.0;
	uint32_t n = 0;
	for (int32_t y = y0; y < y1; y++) {
		const float dy = ((float)y + 0.5f - cy) / sy;
		const float vdamp = 1.0f - (1.0f - damp) * (((float)y + 0.5f - (float)y0) / rh);
		for (int32_t x = x0; x < x1; x++) {
			const float h = g->v[(uint32_t)y * g->w + (uint32_t)x];
			if (!finitef(h)) {
				continue;
			}
			const float hh = clampf(h, 0.0f, 1.0f);
			const float dx = ((float)x + 0.5f - cx) / sx;
			const double w = exp(-0.5 * (double)(dx * dx + dy * dy)) * (double)vdamp;
			uint32_t b = (uint32_t)(hh * (float)U_LIFT_AUTOCONV_BINS);
			if (b >= U_LIFT_AUTOCONV_BINS) {
				b = U_LIFT_AUTOCONV_BINS - 1;
			}
			counts[b] += w;
			wsum += w;
			uint32_t sb = (uint32_t)(hh * (float)U_LIFT_AUTOCONV_SIG_BINS);
			if (sb >= U_LIFT_AUTOCONV_SIG_BINS) {
				sb = U_LIFT_AUTOCONV_SIG_BINS - 1;
			}
			out->sig[sb] += 1.0f;
			n++;
		}
	}
	out->samples = n;
	if (n == 0 || !(wsum > 0.0)) {
		return false;
	}
	for (uint32_t i = 0; i < U_LIFT_AUTOCONV_SIG_BINS; i++) {
		out->sig[i] /= (float)n;
	}

	// Weighted median and the screen-plane percentile (interpolated in their bins).
	out->median_h = weighted_quantile(counts, wsum, 0.5);
	const float pct = clampf(p->percentile, 0.0f, 100.0f);
	out->pct_h = weighted_quantile(counts, wsum, (double)pct / 100.0);

	// Gaussian-smoothed histogram.
	double sm[U_LIFT_AUTOCONV_BINS];
	const double sig = p->hist_smooth_bins > 0.0f ? p->hist_smooth_bins : 1.5;
	const int rad = (int)ceil(3.0 * sig);
	double smax = 0.0;
	for (int b = 0; b < U_LIFT_AUTOCONV_BINS; b++) {
		double acc = 0.0, norm = 0.0;
		for (int j = -rad; j <= rad; j++) {
			const int bb = b + j;
			if (bb < 0 || bb >= U_LIFT_AUTOCONV_BINS) {
				continue;
			}
			const double gw = exp(-(double)(j * j) / (2.0 * sig * sig));
			acc += counts[bb] * gw;
			norm += gw;
		}
		sm[b] = acc / norm;
		if (sm[b] > smax) {
			smax = sm[b];
		}
	}

	// Peaks (the ends of the range included: with a per-frame normalised
	// range, a close-up subject IS the nearest content), scored by mass ×
	// mild foreground bias.
	int best = -1;
	double best_score = -1.0;
	int kept = -1; // the best peak near the subject currently followed
	double kept_score = -1.0;
	const bool prefer = p->prefer_h >= 0.0f && p->prefer_h <= 1.0f;
	for (int b = 0; b < U_LIFT_AUTOCONV_BINS; b++) {
		const double l = b > 0 ? sm[b - 1] : -1.0;
		const double r = b < U_LIFT_AUTOCONV_BINS - 1 ? sm[b + 1] : -1.0;
		if (!(sm[b] >= l && sm[b] >= r && sm[b] > (double)p->peak_floor * smax && sm[b] > 0.0)) {
			continue;
		}
		const double c = ((double)b + 0.5) / U_LIFT_AUTOCONV_BINS;
		if (c < (double)p->far_exclude) {
			continue; // the far background is never the subject
		}
		double f = 1.0 + (double)p->fg_bias * (c - (double)out->median_h);
		if (f < 0.0) {
			f = 0.0;
		}
		const double score = sm[b] * f;
		if (score > best_score) {
			best_score = score;
			best = b;
		}
		if (prefer && fabs(c - (double)p->prefer_h) <= (double)p->prefer_window && score > kept_score) {
			kept_score = score;
			kept = b;
		}
	}
	const float blend = clampf(p->blend, 0.0f, 1.0f);
	if (best < 0) {
		// Only the far background has a peak (or there is no peak at all): the
		// percentile alone still places the screen plane sensibly.
		if (blend <= 0.0f || n < p->min_samples) {
			return false;
		}
		out->peak_h = -1.0f;
		out->mode_h = out->pct_h;
		out->convergence = 1.0f - out->mode_h;
		out->confidence = p->min_confidence > 0.0f ? p->min_confidence : 0.06f; // weak: the follower slows
		out->valid = true;
		return true;
	}
	// Hysteresis: stay on the followed subject unless another peak clearly
	// beats it. Without this, two comparable clusters (a face and the wall
	// behind it) trade places from one sample to the next and the screen
	// plane oscillates inside a shot.
	if (kept >= 0 && kept != best && best_score < (double)p->switch_ratio * kept_score) {
		best = kept;
	}

	// Sub-bin refinement (parabola through the peak and its neighbours):
	// keeps the measurement from stepping in whole bins.
	double off = 0.0;
	if (best > 0 && best < U_LIFT_AUTOCONV_BINS - 1) {
		const double den = sm[best - 1] - 2.0 * sm[best] + sm[best + 1];
		if (den < 0.0) {
			off = 0.5 * (sm[best - 1] - sm[best + 1]) / den;
			off = off < -0.5 ? -0.5 : (off > 0.5 ? 0.5 : off);
		}
	}
	out->peak_h = clampf((float)(((double)best + 0.5 + off) / U_LIFT_AUTOCONV_BINS), 0.0f, 1.0f);
	// The screen plane: blend of the percentile and the dominant clump.
	out->mode_h = clampf(blend * out->pct_h + (1.0f - blend) * out->peak_h, 0.0f, 1.0f);
	out->convergence = 1.0f - out->mode_h;

	double near_mass = 0.0;
	for (int j = -2; j <= 2; j++) {
		const int bb = best + j;
		if (bb >= 0 && bb < U_LIFT_AUTOCONV_BINS) {
			near_mass += counts[bb];
		}
	}
	out->confidence = (float)(near_mass / wsum);
	out->valid = n >= p->min_samples && out->confidence >= p->min_confidence;
	return out->valid;
}


/*
 *
 * Temporal filter.
 *
 */

void
u_lift_autoconv_filter_params_default(struct u_lift_autoconv_filter_params *p)
{
	p->tau_s = 1.0f;      // 90 % of a step in ~3.9 s: slow drift inside a shot (panel A/B: 0.6 pumped)
	p->tau_cut_s = 0.05f; // 90 % in ~0.2 s: a few converted frames, never one
	p->deadband = 0.04f;
	p->cut_jump = 0.25f;
	p->cut_hist = 0.6f;
	p->cut_agree = 0.1f;
	p->cut_max_s = 0.4f;
	p->cut_confirms = 2;
	p->cut_refractory_s = 1.5f;
	p->conf_full = 0.4f;
}

static void
med_push(struct u_lift_autoconv_filter *f, float c)
{
	f->med[f->med_next] = c;
	f->med_next = (f->med_next + 1) % 3;
	if (f->n_med < 3) {
		f->n_med++;
	}
}

static void
med_fill(struct u_lift_autoconv_filter *f, float c)
{
	f->med[0] = f->med[1] = f->med[2] = c;
	f->n_med = 3;
	f->med_next = 0;
}

void
u_lift_autoconv_filter_measure(struct u_lift_autoconv_filter *f,
                               const struct u_lift_autoconv_filter_params *p,
                               const struct u_lift_autoconv_estimate *e,
                               uint64_t now_ns)
{
	if (f == NULL || p == NULL || e == NULL) {
		return;
	}
	f->measurements++;
	if (!e->valid || !finitef(e->convergence)) {
		f->held++;
		f->cut_pending = false;
		return; // hold
	}
	const float c = clampf(e->convergence, 0.0f, 1.0f);
	f->conf = e->confidence;

	float hist_l1 = 0.0f;
	for (uint32_t i = 0; i < U_LIFT_AUTOCONV_SIG_BINS; i++) {
		hist_l1 += fabsf(e->sig[i] - f->sig[i]);
		f->sig[i] = e->sig[i];
	}

	if (!f->have) {
		f->have = true;
		f->x = f->target = c;
		f->v = 0.0f;
		f->last_ns = now_ns;
		med_fill(f, c);
		return;
	}

	// A cut is confirmed by the next cut_confirms measurements agreeing with
	// the candidate: an outlier, or a brief flip between two clusters, never
	// moves anything fast.
	const uint32_t need = p->cut_confirms > 0 ? p->cut_confirms : 1;
	if (f->cut_pending) {
		if (fabsf(c - f->cut_value) <= p->cut_agree) {
			if (++f->cut_agreed >= need) {
				f->cut_pending = false;
				f->target = c;
				f->fast = true;
				f->fast_since_ns = now_ns;
				f->last_cut_ns = now_ns;
				f->cuts++;
				med_fill(f, c);
				return;
			}
		} else {
			f->cut_pending = false;
		}
	}
	// A cut changes BOTH where the subject is and the scene's depth layout.
	// Either alone happens inside a shot (a subject turning, the per-frame
	// re-normalisation reshuffling the histogram) and is followed slowly.
	const float jump = fabsf(c - f->target);
	const bool refractory = f->last_cut_ns != 0 && p->cut_refractory_s > 0.0f &&
	                        (double)(now_ns - f->last_cut_ns) * 1e-9 < (double)p->cut_refractory_s;
	if (!f->cut_pending && !refractory && jump > p->cut_jump && hist_l1 > p->cut_hist) {
		f->cut_pending = true;
		f->cut_value = c;
		f->cut_agreed = 0;
	}

	med_push(f, c);
	const float m = median3(f->med, f->n_med);
	if (fabsf(m - f->target) > p->deadband) {
		f->target = m;
	}
}

bool
u_lift_autoconv_filter_step(struct u_lift_autoconv_filter *f,
                            const struct u_lift_autoconv_filter_params *p,
                            uint64_t now_ns,
                            float *out_value)
{
	if (f == NULL || p == NULL || !f->have) {
		return false;
	}
	double dt = now_ns > f->last_ns ? (double)(now_ns - f->last_ns) * 1e-9 : 0.0;
	if (dt > 0.5) {
		dt = 0.5; // a stalled stream resumes gently
	}
	f->last_ns = now_ns > f->last_ns ? now_ns : f->last_ns;

	double tau = f->fast ? (p->tau_cut_s > 0.0f ? p->tau_cut_s : 0.05) : (p->tau_s > 0.0f ? p->tau_s : 1.0);
	if (!f->fast && p->conf_full > 0.0f && f->conf > 0.0f && f->conf < p->conf_full) {
		double slow = (double)p->conf_full / (double)f->conf;
		tau *= slow > 3.0 ? 3.0 : slow; // an ambiguous mode moves the plane gently
	}
	const double w = 1.0 / tau;
	// Exact critically damped spring (stable for any dt): y = x − target,
	// y(t) = (y0 + (v0 + w y0) t) e^{−wt}, v(t) = (v0 − w (v0 + w y0) t) e^{−wt}.
	const double y0 = (double)f->x - (double)f->target;
	const double v0 = (double)f->v;
	const double k = v0 + w * y0;
	const double e = exp(-w * dt);
	const double y = (y0 + k * dt) * e;
	const double v = (v0 - w * k * dt) * e;
	f->x = clampf((float)((double)f->target + y), 0.0f, 1.0f);
	f->v = (float)v;

	if (f->fast &&
	    (fabsf(f->x - f->target) < 0.01f || (double)(now_ns - f->fast_since_ns) * 1e-9 > (double)p->cut_max_s)) {
		f->fast = false;
	}
	if (out_value != NULL) {
		*out_value = f->x;
	}
	return true;
}
