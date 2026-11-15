// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Automatic 2D and automatic re-convergence for a legacy stereo
 *         session: detector, disparity estimate and controller.
 * @ingroup aux_util
 */

#include "u_stereo_auto.h"
#include "u_lift_autoconv.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static inline float
clampf_(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

void
u_stereo_auto_grid_from_luma(
    const float *luma, uint32_t stride, uint32_t height, uint32_t eye_x0, uint32_t eye_w, float *out)
{
	const uint32_t gx = U_STEREO_AUTO_GRID_X, gy = U_STEREO_AUTO_GRID_Y;
	if (luma == NULL || out == NULL || eye_w == 0 || height == 0) {
		for (uint32_t i = 0; out != NULL && i < U_STEREO_AUTO_CELLS; i++) {
			out[i] = 0.0f;
		}
		return;
	}
	for (uint32_t cy = 0; cy < gy; cy++) {
		uint32_t y0 = (uint32_t)(((uint64_t)cy * height) / gy);
		uint32_t y1 = (uint32_t)(((uint64_t)(cy + 1) * height) / gy);
		if (y1 <= y0) {
			y1 = y0 + 1;
		}
		if (y1 > height) {
			y1 = height;
			y0 = y1 - 1;
		}
		for (uint32_t cx = 0; cx < gx; cx++) {
			uint32_t x0 = (uint32_t)(((uint64_t)cx * eye_w) / gx);
			uint32_t x1 = (uint32_t)(((uint64_t)(cx + 1) * eye_w) / gx);
			if (x1 <= x0) {
				x1 = x0 + 1;
			}
			if (x1 > eye_w) {
				x1 = eye_w;
				x0 = x1 - 1;
			}
			double sum = 0.0;
			for (uint32_t y = y0; y < y1; y++) {
				const float *row = luma + (size_t)y * stride + eye_x0;
				for (uint32_t x = x0; x < x1; x++) {
					sum += row[x];
				}
			}
			out[cy * gx + cx] = (float)(sum / (double)((y1 - y0) * (x1 - x0)));
		}
	}
}


/*
 *
 * Flat-picture detector.
 *
 */

void
u_stereo_flat_params_default(struct u_stereo_flat_params *p)
{
	// Measured on the reference grid: a flat menu at a fixed depth leaves about
	// 1.1 after the best shift (sampling noise); real stereo stays far above 3.
	// The ratio guards low-contrast 3D scenes, where every difference is small.
	p->unshifted_max = 3.0;
	p->residual_max = 3.0;
	p->residual_ratio = 0.3;
	p->max_shift_frac = 0.2;
	// About half a second at ten probes a second to enter, two probes to leave:
	// a scene cut never flickers the lens.
	p->enter_probes = 5;
	p->leave_probes = 2;
}

double
u_stereo_flat_eye_difference(const float *left, const float *right, double shift)
{
	const uint32_t gx = U_STEREO_AUTO_GRID_X;
	double sum = 0.0;
	uint32_t count = 0;
	for (uint32_t y = 0; y < U_STEREO_AUTO_GRID_Y; y++) {
		const float *l = left + (size_t)y * gx;
		const float *r = right + (size_t)y * gx;
		for (uint32_t x = 0; x < gx; x++) {
			const double rx = (double)x - shift;
			if (rx < 0.0 || rx > (double)gx - 1.0) {
				continue;
			}
			const uint32_t x0 = (uint32_t)rx;
			const uint32_t x1 = x0 + 1 < gx ? x0 + 1 : x0;
			const double t = rx - (double)x0;
			const double v = r[x0] * (1.0 - t) + r[x1] * t;
			const double d = l[x] - v;
			sum += d < 0.0 ? -d : d;
			count++;
		}
	}
	return count > 0 ? sum / count : 0.0;
}

void
u_stereo_flat_measure(const float *left,
                      const float *right,
                      const struct u_stereo_flat_params *p,
                      struct u_stereo_flat_measurement *m)
{
	const int max_shift = (int)(U_STEREO_AUTO_GRID_X * p->max_shift_frac);
	m->unshifted = u_stereo_flat_eye_difference(left, right, 0.0);
	m->best = m->unshifted;
	m->best_shift = 0.0;
	// Whole cells first, then 1/8 cells around the best.
	for (int s = -max_shift; s <= max_shift; s++) {
		const double d = u_stereo_flat_eye_difference(left, right, (double)s);
		if (d < m->best) {
			m->best = d;
			m->best_shift = s;
		}
	}
	const double coarse = m->best_shift;
	for (double s = coarse - 1.0; s <= coarse + 1.0; s += 0.125) {
		const double d = u_stereo_flat_eye_difference(left, right, s);
		if (d < m->best) {
			m->best = d;
			m->best_shift = s;
		}
	}
	m->flat = m->unshifted < p->unshifted_max ||
	          (m->best < p->residual_max && m->best < p->residual_ratio * m->unshifted);
}


/*
 *
 * Disparity estimate.
 *
 */

//! Block window: BLOCK_W cells around the block centre, BLOCK_ROWS rows.
#define BLOCK_W 16
#define BLOCK_ROWS (U_STEREO_AUTO_GRID_Y / U_STEREO_AUTO_BLOCKS_Y)
//! A block whose left-eye luma varies less than this (std dev) has nothing to match.
#define BLOCK_MIN_TEXTURE 4.0
//! The best cost must be below this fraction of the best cost away from it.
#define BLOCK_DISTINCT 0.7
//! ... and below this absolute mean difference.
#define BLOCK_MAX_COST 12.0
#define MAX_SEARCH 64

static double
block_cost(const float *left, const float *right, int x0, int x1, uint32_t y0, int s)
{
	const int gx = (int)U_STEREO_AUTO_GRID_X;
	double sum = 0.0;
	int n = 0;
	for (uint32_t y = y0; y < y0 + BLOCK_ROWS; y++) {
		const float *l = left + (size_t)y * (uint32_t)gx;
		const float *r = right + (size_t)y * (uint32_t)gx;
		for (int x = x0; x < x1; x++) {
			const int rx = x - s;
			if (rx < 0 || rx >= gx) {
				continue;
			}
			const double d = (double)l[x] - (double)r[rx];
			sum += d < 0.0 ? -d : d;
			n++;
		}
	}
	// Too little overlap is no evidence at all.
	return n * 2 >= (x1 - x0) * (int)BLOCK_ROWS ? sum / n : 1e30;
}

static int
cmp_float(const void *a, const void *b)
{
	const float x = *(const float *)a, y = *(const float *)b;
	return x < y ? -1 : (x > y ? 1 : 0);
}

//! Linear-interpolated quantile of an ascending array.
static float
quantile_sorted(const float *v, uint32_t n, float q)
{
	if (n == 0) {
		return 0.0f;
	}
	const float pos = clampf_(q, 0.0f, 1.0f) * (float)(n - 1);
	const uint32_t i = (uint32_t)pos;
	const float t = pos - (float)i;
	return i + 1 < n ? v[i] * (1.0f - t) + v[i + 1] * t : v[n - 1];
}

bool
u_stereo_auto_estimate(const float *left,
                       const float *right,
                       double max_shift_frac,
                       struct u_stereo_disparity_estimate *out)
{
	memset(out, 0, sizeof(*out));
	const int gx = (int)U_STEREO_AUTO_GRID_X;
	int max_shift = (int)(gx * max_shift_frac);
	if (max_shift < 1) {
		max_shift = 1;
	}
	if (max_shift > MAX_SEARCH) {
		max_shift = MAX_SEARCH;
	}
	const uint32_t step = U_STEREO_AUTO_GRID_X / U_STEREO_AUTO_BLOCKS_X;
	float sorted[U_STEREO_AUTO_BLOCKS];
	uint32_t n = 0;
	double cost[2 * MAX_SEARCH + 1];
	for (uint32_t by = 0; by < U_STEREO_AUTO_BLOCKS_Y; by++) {
		const uint32_t y0 = by * BLOCK_ROWS;
		for (uint32_t bx = 0; bx < U_STEREO_AUTO_BLOCKS_X; bx++) {
			const uint32_t bi = by * U_STEREO_AUTO_BLOCKS_X + bx;
			out->block_d[bi] = NAN;
			const int c = (int)(bx * step + step / 2);
			int x0 = c - BLOCK_W / 2, x1 = c + BLOCK_W / 2;
			x0 = x0 < 0 ? 0 : x0;
			x1 = x1 > gx ? gx : x1;
			// Texture: a flat patch matches every shift equally well.
			double mean = 0.0, sq = 0.0;
			for (uint32_t y = y0; y < y0 + BLOCK_ROWS; y++) {
				for (int x = x0; x < x1; x++) {
					const double v = left[y * (uint32_t)gx + (uint32_t)x];
					mean += v;
					sq += v * v;
				}
			}
			const double cnt = (double)((x1 - x0) * (int)BLOCK_ROWS);
			mean /= cnt;
			const double var = sq / cnt - mean * mean;
			if (!(var >= BLOCK_MIN_TEXTURE * BLOCK_MIN_TEXTURE)) {
				continue;
			}
			int best = 0;
			for (int s = -max_shift; s <= max_shift; s++) {
				cost[s + max_shift] = block_cost(left, right, x0, x1, y0, s);
				if (cost[s + max_shift] < cost[best + max_shift]) {
					best = s;
				}
			}
			const double cb = cost[best + max_shift];
			double other = 1e30;
			for (int s = -max_shift; s <= max_shift; s++) {
				if (abs(s - best) > 2 && cost[s + max_shift] < other) {
					other = cost[s + max_shift];
				}
			}
			if (!(cb < BLOCK_MAX_COST) || !(cb < BLOCK_DISTINCT * other)) {
				continue; // ambiguous (a repeating pattern) or no match at all
			}
			double s_ref = best;
			if (best > -max_shift && best < max_shift) {
				const double cm = cost[best - 1 + max_shift], cp = cost[best + 1 + max_shift];
				const double den = cm - 2.0 * cb + cp;
				if (den > 1e-9 && cm < 1e29 && cp < 1e29) {
					const double off = 0.5 * (cm - cp) / den;
					s_ref += off < -0.5 ? -0.5 : (off > 0.5 ? 0.5 : off);
				}
			}
			// L[x] matches R[x - s]: the right eye sees it s cells further left.
			const float d = (float)(-s_ref / (double)gx);
			out->block_d[bi] = d;
			sorted[n++] = d;
		}
	}
	out->blocks = n;
	if (n < 12) {
		return false;
	}
	qsort(sorted, n, sizeof(float), cmp_float);
	out->near_d = quantile_sorted(sorted, n, 0.05f);
	out->far_d = quantile_sorted(sorted, n, 0.95f);
	const float dmin = sorted[0], dmax = sorted[n - 1];
	const float range = dmax - dmin;
	if (!(range > 0.5f / (float)gx)) {
		// One depth (within half a cell): it goes on the screen.
		out->screen = quantile_sorted(sorted, n, 0.5f);
		out->valid = true;
		return true;
	}

	// Nearness h = 1 nearest, 0 farthest: the lift rule's input (inverse depth
	// is linear in disparity, so its percentiles and clumps carry over).
	struct u_lift_autoconv_grid *g = (struct u_lift_autoconv_grid *)malloc(sizeof(*g));
	if (g == NULL) {
		return false;
	}
	g->w = U_STEREO_AUTO_BLOCKS_X;
	g->h = U_STEREO_AUTO_BLOCKS_Y;
	for (uint32_t i = 0; i < U_STEREO_AUTO_BLOCKS; i++) {
		const float d = out->block_d[i];
		g->v[i] = isnan(d) ? NAN : (dmax - d) / range;
	}
	struct u_lift_autoconv_params lp;
	u_lift_autoconv_params_default(&lp);
	lp.min_samples = 12;
	struct u_lift_autoconv_estimate le;
	const bool ok = u_lift_autoconv_find_subject(g, NULL, &lp, &le);
	float h;
	if (ok) {
		h = le.mode_h;
	} else {
		// No confident mode: the centre-weighted p78 of nearness alone.
		lp.percentile = 78.0f;
		lp.blend = 1.0f;
		lp.min_confidence = 0.0f;
		h = u_lift_autoconv_find_subject(g, NULL, &lp, &le) ? le.pct_h : 0.5f;
	}
	free(g);
	out->screen = dmax - h * range;
	out->valid = true;
	return true;
}

float
u_stereo_auto_target_shift(const struct u_stereo_disparity_estimate *e, const struct u_legacy_stereo_auto *cfg)
{
	if (e == NULL || !e->valid || cfg == NULL) {
		return 0.0f;
	}
	float shift = -e->screen;
	const float lo = -cfg->max_front_fraction - e->near_d; // near content must stay in front by <= max_front
	const float hi = cfg->max_behind_fraction - e->far_d;  // far content behind by <= max_behind
	if (lo <= hi) {
		shift = clampf_(shift, lo, hi);
	} else {
		shift = 0.5f * (lo + hi); // more depth than the budget: share the excess
	}
	return clampf_(shift, -cfg->max_shift_fraction, cfg->max_shift_fraction);
}


/*
 *
 * Controller.
 *
 */

void
u_stereo_auto_params_default(struct u_stereo_auto_params *p)
{
	u_stereo_flat_params_default(&p->flat);
	p->tau_s = 1.0f;      // the lift follower's: a slow drift inside a shot
	p->deadband = 0.002f; // ~4 px on a 1920-wide eye
	p->cut_luma = 20.0f;  // mean absolute change of the left grid (0..255)
}

void
u_stereo_auto_init(struct u_stereo_auto *a, const struct u_legacy_stereo_auto *cfg)
{
	memset(a, 0, sizeof(*a));
	a->cfg = *cfg;
	u_stereo_auto_params_default(&a->p);
}

void
u_stereo_auto_probe(
    struct u_stereo_auto *a, const float *left, const float *right, uint64_t now_ns, struct u_stereo_auto_result *out)
{
	memset(out, 0, sizeof(*out));
	a->probes++;

	u_stereo_flat_measure(left, right, &a->p.flat, &out->flat);
	const bool flat = out->flat.flat;
	const bool was_2d = a->want_2d;
	if (a->cfg.auto_2d) {
		a->flat_streak = flat ? a->flat_streak + 1 : 0;
		a->depth_streak = flat ? 0 : a->depth_streak + 1;
		a->want_2d = a->want_2d ? (a->depth_streak < a->p.flat.leave_probes)
		                        : (a->flat_streak >= a->p.flat.enter_probes);
	} else {
		a->want_2d = false;
	}
	out->want_2d = a->want_2d;

	// Scene cut: the left eye's picture changed as a whole since the last probe.
	bool cut = false;
	if (a->have_prev) {
		double sum = 0.0;
		for (uint32_t i = 0; i < U_STEREO_AUTO_CELLS; i++) {
			const double d = (double)left[i] - (double)a->prev_left[i];
			sum += d < 0.0 ? -d : d;
		}
		cut = sum / U_STEREO_AUTO_CELLS > a->p.cut_luma;
	}
	memcpy(a->prev_left, left, sizeof(a->prev_left));
	a->have_prev = true;
	// Back from 2D: whatever the shift was, it belonged to another scene. A
	// cut seen while the eyes were flat is owed to the next estimate too.
	if (was_2d && !a->want_2d) {
		cut = true;
	}
	if (cut) {
		a->snap_pending = true;
	}

	const float dt = a->last_ns != 0 && now_ns > a->last_ns ? (float)((double)(now_ns - a->last_ns) * 1e-9) : 0.0f;
	a->last_ns = now_ns;

	if (a->cfg.auto_convergence && !flat && !a->want_2d) {
		out->estimated = true;
		if (u_stereo_auto_estimate(left, right, a->p.flat.max_shift_frac, &out->est)) {
			out->target_shift = u_stereo_auto_target_shift(&out->est, &a->cfg);
			if (!a->have_shift || a->snap_pending) {
				a->shift = out->target_shift;
				if (a->have_shift) {
					a->cuts++;
					out->cut = true;
				}
				a->have_shift = true;
				a->snap_pending = false;
			} else if (fabsf(out->target_shift - a->shift) > a->p.deadband) {
				const float k =
				    1.0f - expf(-clampf_(dt, 0.0f, 1.0f) / (a->p.tau_s > 0.0f ? a->p.tau_s : 1.0f));
				a->shift += (out->target_shift - a->shift) * k;
			}
		}
	}
	if (!a->cfg.auto_convergence) {
		a->shift = 0.0f;
		a->have_shift = false;
	}
	out->shift = a->shift;
}
