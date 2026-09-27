// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_stereo_camera (ADR-043) R2: online vertical-alignment
 *         refinement. See u_stereo_vrefine.h.
 * @ingroup aux_util
 */

#include "util/u_stereo_vrefine.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>


/*
 *
 * Measurement.
 *
 */

#define PR_HALF 5  //!< half-res patch radius (11x11)
#define PR_FULL 10 //!< full-res patch radius (21x21)
#define FINE_R 2   //!< full-res search radius around the doubled coarse peak
#define MIN_EIG 12.0
#define MIN_CURV 0.004

void
u_stereo_vrefine_measure_defaults(struct u_stereo_vrefine_measure_params *p, uint32_t max_disparity)
{
	p->max_disparity = max_disparity;
	p->max_dy = 10;
	p->min_ncc = 0.90f;
	p->max_samples = 160;
}

//! A left patch, copied contiguous, with its sums (integer NCC).
struct lpatch
{
	uint8_t px[(2 * PR_FULL + 1) * (2 * PR_FULL + 1)];
	int32_t side;
	double n, sum, var_n; //!< var_n = n * sum(l^2) - sum(l)^2
};

static bool
lpatch_init(struct lpatch *lp, const uint8_t *img, uint32_t pitch, int32_t x, int32_t y, int32_t rad)
{
	const int32_t side = 2 * rad + 1;
	uint32_t s = 0, s2 = 0;
	for (int32_t j = 0; j < side; j++) {
		const uint8_t *row = img + (size_t)(y - rad + j) * pitch + (x - rad);
		uint8_t *o = lp->px + j * side;
		for (int32_t i = 0; i < side; i++) {
			o[i] = row[i];
			s += row[i];
			s2 += (uint32_t)row[i] * row[i];
		}
	}
	lp->side = side;
	lp->n = (double)(side * side);
	lp->sum = s;
	lp->var_n = lp->n * (double)s2 - (double)s * (double)s;
	return lp->var_n > 1e-6;
}

//! sum(l * r) over the right patch centred at (x, y). Integer, vectorizable.
static uint32_t
dot_at(const struct lpatch *lp, const uint8_t *img, uint32_t pitch, int32_t x, int32_t y)
{
	const int32_t side = lp->side, rad = side / 2;
	uint32_t acc = 0;
	for (int32_t j = 0; j < side; j++) {
		const uint8_t *row = img + (size_t)(y - rad + j) * pitch + (x - rad);
		const uint8_t *l = lp->px + j * side;
		uint32_t a = 0;
		for (int32_t i = 0; i < side; i++) {
			a += (uint32_t)l[i] * row[i];
		}
		acc += a;
	}
	return acc;
}

static double
ncc_from(const struct lpatch *lp, double dot, double sr, double sr2)
{
	double var_r = lp->n * sr2 - sr * sr;
	if (!(var_r > 1e-6)) {
		return -1.0;
	}
	return (lp->n * dot - lp->sum * sr) / sqrt(lp->var_n * var_r);
}

//! NCC against the right patch at (x, y), right sums computed directly.
static double
ncc_direct(const struct lpatch *lp, const uint8_t *img, uint32_t pitch, int32_t x, int32_t y)
{
	const int32_t side = lp->side, rad = side / 2;
	uint32_t s = 0, s2 = 0;
	for (int32_t j = 0; j < side; j++) {
		const uint8_t *row = img + (size_t)(y - rad + j) * pitch + (x - rad);
		for (int32_t i = 0; i < side; i++) {
			s += row[i];
			s2 += (uint32_t)row[i] * row[i];
		}
	}
	return ncc_from(lp, (double)dot_at(lp, img, pitch, x, y), (double)s, (double)s2);
}

static float
parabola(double m, double c, double p)
{
	double den = m - 2.0 * c + p;
	if (!(fabs(den) > 1e-12)) {
		return 0.0f;
	}
	double o = 0.5 * (m - p) / den;
	return (float)(o > 0.5 ? 0.5 : o < -0.5 ? -0.5 : o);
}

uint32_t
u_stereo_vrefine_measure(const uint8_t *gray,
                         uint32_t pitch,
                         uint32_t eye_width,
                         uint32_t height,
                         const struct u_stereo_vrefine_measure_params *p,
                         struct u_stereo_vrefine_sample *out,
                         uint32_t cap)
{
	if (gray == NULL || p == NULL || out == NULL || cap == 0 || eye_width < 64 || height < 64) {
		return 0;
	}
	const int32_t hw = (int32_t)(eye_width / 2), hh = (int32_t)(height / 2);
	const int32_t max_d = (int32_t)p->max_disparity, max_dy = (int32_t)p->max_dy;
	const int32_t max_d_h = (max_d + 1) / 2, max_dy_h = (max_dy + 1) / 2;

	// 2x-downscaled eyes (box filter) + the structure-tensor integral images.
	uint8_t *half = malloc((size_t)2 * hw * hh);
	double *integ = malloc(sizeof(double) * 5 * (size_t)(hw + 1) * (hh + 1));
	if (half == NULL || integ == NULL) {
		free(half);
		free(integ);
		return 0;
	}
	for (int32_t e = 0; e < 2; e++) {
		uint8_t *dst = half + (size_t)e * hw * hh;
		const uint8_t *src = gray + (size_t)e * eye_width;
		for (int32_t y = 0; y < hh; y++) {
			const uint8_t *r0 = src + (size_t)(2 * y) * pitch, *r1 = r0 + pitch;
			for (int32_t x = 0; x < hw; x++) {
				dst[(size_t)y * hw + x] =
				    (uint8_t)((r0[2 * x] + r0[2 * x + 1] + r1[2 * x] + r1[2 * x + 1] + 2) >> 2);
			}
		}
	}
	const uint8_t *L = half, *R = half + (size_t)hw * hh;
	const size_t iw = (size_t)hw + 1;
	double *Ixx = integ, *Iyy = integ + iw * (hh + 1), *Ixy = integ + 2 * iw * (hh + 1);
	double *Rs = integ + 3 * iw * (hh + 1), *Rs2 = integ + 4 * iw * (hh + 1);
	memset(integ, 0, sizeof(double) * 5 * iw * (hh + 1));
	for (int32_t y = 0; y < hh; y++) {
		double rxx = 0, ryy = 0, rxy = 0;
		for (int32_t x = 0; x < hw; x++) {
			double gx = 0, gy = 0;
			if (x > 0 && x < hw - 1 && y > 0 && y < hh - 1) {
				gx = 0.5 * ((double)L[(size_t)y * hw + x + 1] - L[(size_t)y * hw + x - 1]);
				gy = 0.5 * ((double)L[(size_t)(y + 1) * hw + x] - L[(size_t)(y - 1) * hw + x]);
			}
			rxx += gx * gx;
			ryy += gy * gy;
			rxy += gx * gy;
			size_t o = (size_t)(y + 1) * iw + (x + 1);
			Ixx[o] = Ixx[o - iw] + rxx;
			Iyy[o] = Iyy[o - iw] + ryy;
			Ixy[o] = Ixy[o - iw] + rxy;
		}
	}
	for (int32_t y = 0; y < hh; y++) {
		double rs = 0, rs2 = 0;
		for (int32_t x = 0; x < hw; x++) {
			double v = R[(size_t)y * hw + x];
			rs += v;
			rs2 += v * v;
			size_t o = (size_t)(y + 1) * iw + (x + 1);
			Rs[o] = Rs[o - iw] + rs;
			Rs2[o] = Rs2[o - iw] + rs2;
		}
	}
#define BOX(I, x0, y0, x1, y1)                                                                                         \
	((I)[(size_t)(y1)*iw + (x1)] - (I)[(size_t)(y0)*iw + (x1)] - (I)[(size_t)(y1)*iw + (x0)] +                     \
	 (I)[(size_t)(y0)*iw + (x0)])

	// Grid of cells, one best Shi-Tomasi patch per cell. The patch must leave
	// room for the full-resolution refinement too.
	const int32_t margin = PR_HALF + 2;
	const int32_t ux0 = margin, ux1 = hw - margin, uy0 = margin, uy1 = hh - margin;
	uint32_t want = p->max_samples > 0 ? p->max_samples : 160;
	double area = (double)(ux1 - ux0) * (double)(uy1 - uy0);
	int32_t cs = (int32_t)sqrt(area / (double)want);
	if (cs < 2 * PR_HALF + 1) {
		cs = 2 * PR_HALF + 1;
	}
	const int32_t side_h = 2 * PR_HALF + 1;
	struct lpatch lp_h, lp_f;
	double fine[2 * FINE_R + 3][2 * FINE_R + 3];
	uint32_t n_out = 0;

	for (int32_t cy0 = uy0; cy0 + cs <= uy1 && n_out < cap; cy0 += cs) {
		for (int32_t cx0 = ux0; cx0 + cs <= ux1 && n_out < cap; cx0 += cs) {
			// Best corner in the cell (stride 2).
			double best = MIN_EIG;
			int32_t bx = -1, by = -1;
			for (int32_t y = cy0; y < cy0 + cs; y += 2) {
				for (int32_t x = cx0; x < cx0 + cs; x += 2) {
					int32_t x0 = x - PR_HALF, y0 = y - PR_HALF, x1 = x + PR_HALF + 1,
					        y1 = y + PR_HALF + 1;
					double a = BOX(Ixx, x0, y0, x1, y1) / (side_h * side_h);
					double c = BOX(Iyy, x0, y0, x1, y1) / (side_h * side_h);
					double b = BOX(Ixy, x0, y0, x1, y1) / (side_h * side_h);
					double mn = 0.5 * (a + c) - sqrt(0.25 * (a - c) * (a - c) + b * b);
					if (mn > best) {
						best = mn;
						bx = x;
						by = y;
					}
				}
			}
			if (bx < 0) {
				continue;
			}

			// Coarse: half resolution, every integer (d, dy) in the window.
			if (!lpatch_init(&lp_h, L, (uint32_t)hw, bx, by, PR_HALF)) {
				continue;
			}
			int32_t d_lo = -1, d_hi = max_d_h;
			if (d_hi > bx - PR_HALF) {
				d_hi = bx - PR_HALF; // right patch x = bx - d must stay inside
			}
			if (d_lo < bx + PR_HALF - (hw - 1)) {
				d_lo = bx + PR_HALF - (hw - 1);
			}
			int32_t y_lo = -max_dy_h, y_hi = max_dy_h;
			if (by + y_lo - PR_HALF < 0) {
				y_lo = PR_HALF - by;
			}
			if (by + y_hi + PR_HALF > hh - 1) {
				y_hi = hh - 1 - PR_HALF - by;
			}
			if (d_hi - d_lo < 2 || y_hi - y_lo < 2) {
				continue;
			}
			double cbest = -2.0;
			int32_t cd = 0, cdy = 0;
			for (int32_t dy = y_lo; dy <= y_hi; dy++) {
				for (int32_t d = d_lo; d <= d_hi; d++) {
					const int32_t rx = bx - d, ry = by + dy;
					const int32_t x0 = rx - PR_HALF, y0 = ry - PR_HALF, x1 = rx + PR_HALF + 1,
					              y1 = ry + PR_HALF + 1;
					double c = ncc_from(&lp_h, (double)dot_at(&lp_h, R, (uint32_t)hw, rx, ry),
					                    BOX(Rs, x0, y0, x1, y1), BOX(Rs2, x0, y0, x1, y1));
					if (c > cbest) {
						cbest = c;
						cd = d;
						cdy = dy;
					}
				}
			}
			if (cbest < p->min_ncc - 0.15 || cd == d_lo || cd == d_hi || cdy == y_lo || cdy == y_hi) {
				continue; // weak, or a clamp on the window: not a measurement
			}

			// Fine: full resolution around the doubled peak.
			const int32_t fx = 2 * bx, fy = 2 * by; // full-res left patch centre
			if (fx - PR_FULL < 0 || fx + PR_FULL >= (int32_t)eye_width || fy - PR_FULL < 0 ||
			    fy + PR_FULL >= (int32_t)height) {
				continue;
			}
			const uint8_t *Rf = gray + eye_width;
			if (!lpatch_init(&lp_f, gray, pitch, fx, fy, PR_FULL)) {
				continue;
			}
			int32_t pd = 2 * cd, pdy = 2 * cdy;
			for (int iter = 0; iter < 4; iter++) {
				double fb = -2.0;
				int32_t md = 0, mdy = 0;
				for (int32_t j = -FINE_R - 1; j <= FINE_R + 1; j++) {
					for (int32_t i = -FINE_R - 1; i <= FINE_R + 1; i++) {
						int32_t d = pd + i, dy = pdy + j;
						int32_t rx = fx - d, ry = fy + dy;
						double c = -2.0;
						if (d >= -2 && d <= max_d && dy >= -max_dy && dy <= max_dy &&
						    rx - PR_FULL >= 0 && rx + PR_FULL < (int32_t)eye_width &&
						    ry - PR_FULL >= 0 && ry + PR_FULL < (int32_t)height) {
							c = ncc_direct(&lp_f, Rf, pitch, rx, ry);
						}
						fine[j + FINE_R + 1][i + FINE_R + 1] = c;
						if (abs(i) <= FINE_R && abs(j) <= FINE_R && c > fb) {
							fb = c;
							md = i;
							mdy = j;
						}
					}
				}
				if (abs(md) < FINE_R && abs(mdy) < FINE_R) {
					pd += md;
					pdy += mdy;
					// Re-centre the 3x3 on the peak (still inside the table).
					const int32_t cj = mdy + FINE_R + 1, ci = md + FINE_R + 1;
					double c = fine[cj][ci], up = fine[cj - 1][ci], dn = fine[cj + 1][ci];
					double lf = fine[cj][ci - 1], rt = fine[cj][ci + 1];
					bool nbr_ok = up > -1.5 && dn > -1.5 && lf > -1.5 && rt > -1.5;
					if (!nbr_ok || c < p->min_ncc || (2.0 * c - up - dn) < MIN_CURV ||
					    (2.0 * c - lf - rt) < MIN_CURV) {
						break;
					}
					float sx = parabola(lf, c, rt), sy = parabola(up, c, dn);
					// d = left x - right x: the right patch at x - (pd + i),
					// a higher score at i = +1 means a larger d.
					float d = (float)pd + sx;
					float dyv = (float)pdy + sy;
					if (d > 0.5f) {
						struct u_stereo_vrefine_sample *o = &out[n_out++];
						o->x = (float)fx;
						o->y = (float)fy;
						o->d = d;
						o->dy = dyv;
						o->ncc = (float)c;
					}
					break;
				}
				pd += md;
				pdy += mdy; // peak on the fine window's edge: move and retry
			}
		}
	}
#undef BOX
	free(half);
	free(integ);
	return n_out;
}


/*
 *
 * Robust fit.
 *
 */

static int
cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y ? 1 : 0;
}

static double
median_of(double *v, uint32_t n)
{
	qsort(v, n, sizeof(double), cmp_double);
	return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

bool
u_stereo_vrefine_fit_samples(const struct u_stereo_vrefine_sample *s,
                             uint32_t n,
                             double cy,
                             struct u_stereo_vrefine_fit *out)
{
	memset(out, 0, sizeof(*out));
	if (s == NULL || n < 8) {
		return false;
	}
	double *tmp = malloc(sizeof(double) * n);
	double *w = malloc(sizeof(double) * n);
	if (tmp == NULL || w == NULL) {
		free(tmp);
		free(w);
		return false;
	}
	for (uint32_t i = 0; i < n; i++) {
		tmp[i] = s[i].dy;
	}
	out->median_dy = median_of(tmp, n);

	double ym = 0.0, yv = 0.0;
	for (uint32_t i = 0; i < n; i++) {
		ym += s[i].y;
	}
	ym /= n;
	for (uint32_t i = 0; i < n; i++) {
		yv += (s[i].y - ym) * (s[i].y - ym);
	}
	out->slope_determined = sqrt(yv / n) >= 40.0;

	double a = out->median_dy, b = 0.0, sigma = 0.1;
	for (int it = 0; it < 10; it++) {
		for (uint32_t i = 0; i < n; i++) {
			tmp[i] = fabs(s[i].dy - (a + b * (s[i].y - cy)));
		}
		sigma = 1.4826 * median_of(tmp, n);
		if (sigma < 0.1) {
			sigma = 0.1;
		}
		const double c = 4.685 * sigma;
		double sw = 0, swy = 0, swyy = 0, swd = 0, swyd = 0;
		for (uint32_t i = 0; i < n; i++) {
			double y = s[i].y - cy;
			double r = s[i].dy - (a + b * y);
			double u = r / c;
			w[i] = fabs(u) < 1.0 ? (1.0 - u * u) * (1.0 - u * u) : 0.0;
			sw += w[i];
			swy += w[i] * y;
			swyy += w[i] * y * y;
			swd += w[i] * s[i].dy;
			swyd += w[i] * y * s[i].dy;
		}
		if (!(sw > 0.0)) {
			break;
		}
		double na, nb;
		double det = sw * swyy - swy * swy;
		if (out->slope_determined && det > 1e-9 * sw * sw) {
			nb = (sw * swyd - swy * swd) / det;
			na = (swd - nb * swy) / sw;
		} else {
			nb = 0.0;
			na = swd / sw;
		}
		bool conv = fabs(na - a) < 1e-4 && fabs(nb - b) < 1e-7;
		a = na;
		b = nb;
		if (conv) {
			break;
		}
	}
	uint32_t inl = 0;
	const double c = 4.685 * sigma;
	for (uint32_t i = 0; i < n; i++) {
		if (fabs(s[i].dy - (a + b * (s[i].y - cy))) < c) {
			inl++;
		}
	}
	out->a = a;
	out->b = b;
	out->sigma = sigma;
	out->inliers = inl;
	free(tmp);
	free(w);
	return true;
}


/*
 *
 * Controller.
 *
 */

void
u_stereo_vrefine_config_defaults(struct u_stereo_vrefine_config *c, uint32_t height, double cy)
{
	memset(c, 0, sizeof(*c));
	c->height = height;
	c->cy = cy;
	c->min_matches = 50;
	c->min_frames = 3;
	c->max_frames = 12;
	c->deadband_px = 0.2;
	c->max_offset_px = 6.0;
	c->max_slope = 0.02;
	c->fast_period_ns = 250ll * 1000 * 1000;
	c->fast_phase_ns = 5000ll * 1000 * 1000;
	c->slow_period_ns = 30000ll * 1000 * 1000;
}

void
u_stereo_vrefine_init(struct u_stereo_vrefine *r, const struct u_stereo_vrefine_config *cfg)
{
	memset(r, 0, sizeof(*r));
	r->cfg = *cfg;
}

void
u_stereo_vrefine_reset(struct u_stereo_vrefine *r)
{
	struct u_stereo_vrefine_config cfg = r->cfg;
	u_stereo_vrefine_init(r, &cfg);
}

void
u_stereo_vrefine_restart(struct u_stereo_vrefine *r, int64_t now_ns)
{
	r->win_n = 0;
	r->win_frames = 0;
	r->started = true;
	r->phase_start_ns = now_ns;
	r->next_due_ns = now_ns;
}

bool
u_stereo_vrefine_due(struct u_stereo_vrefine *r, int64_t now_ns)
{
	if (!r->started) {
		u_stereo_vrefine_restart(r, now_ns);
	}
	return now_ns >= r->next_due_ns;
}

static double
clampd(double v, double lim)
{
	return v > lim ? lim : v < -lim ? -lim : v;
}

static void
schedule_after_window(struct u_stereo_vrefine *r, int64_t now_ns)
{
	bool fast = now_ns - r->phase_start_ns < r->cfg.fast_phase_ns;
	r->next_due_ns = now_ns + (fast ? r->cfg.fast_period_ns : r->cfg.slow_period_ns);
	r->win_n = 0;
	r->win_frames = 0;
}

enum u_stereo_vrefine_result
u_stereo_vrefine_push(struct u_stereo_vrefine *r, int64_t now_ns, const struct u_stereo_vrefine_sample *s, uint32_t n)
{
	if (!r->started) {
		u_stereo_vrefine_restart(r, now_ns);
	}
	for (uint32_t i = 0; i < n && r->win_n < U_STEREO_VREFINE_WINDOW_CAP; i++) {
		r->win[r->win_n++] = s[i];
	}
	r->win_frames++;

	if (r->win_n < r->cfg.min_matches || r->win_frames < r->cfg.min_frames) {
		if (r->win_frames >= r->cfg.max_frames) {
			schedule_after_window(r, now_ns);
			return U_STEREO_VREFINE_DROPPED;
		}
		r->next_due_ns = now_ns + r->cfg.fast_period_ns;
		return U_STEREO_VREFINE_ACCUMULATING;
	}

	struct u_stereo_vrefine_fit fit;
	if (!u_stereo_vrefine_fit_samples(r->win, r->win_n, r->cfg.cy, &fit) || fit.inliers < r->cfg.min_matches ||
	    !isfinite(fit.a) || !isfinite(fit.b)) {
		schedule_after_window(r, now_ns);
		return U_STEREO_VREFINE_DROPPED;
	}
	r->windows++;
	r->last_residual_dy = fit.median_dy;
	r->last_matches = fit.inliers;
	if (!r->have_initial) {
		r->have_initial = true;
		// The first window after a reset is measured without any correction.
		r->initial_dy = fit.median_dy;
		r->initial_a = fit.a;
		r->initial_b = fit.b;
	}

	double na = clampd(r->a + fit.a, r->cfg.max_offset_px);
	double nb = clampd(r->b + fit.b, r->cfg.max_slope);
	double extent =
	    r->cfg.cy > (double)r->cfg.height - 1.0 - r->cfg.cy ? r->cfg.cy : (double)r->cfg.height - 1.0 - r->cfg.cy;
	double effect = fabs(na - r->a) + fabs(nb - r->b) * extent;
	enum u_stereo_vrefine_result res = U_STEREO_VREFINE_STABLE;
	if (effect > r->cfg.deadband_px) {
		r->prev_a = r->a;
		r->prev_b = r->b;
		r->a = na;
		r->b = nb;
		r->applied = true;
		r->updates++;
		r->phase_start_ns = now_ns; // unstable: back to the settling cadence
		res = U_STEREO_VREFINE_UPDATED;
	}
	schedule_after_window(r, now_ns);
	return res;
}

void
u_stereo_vrefine_revert(struct u_stereo_vrefine *r)
{
	r->a = r->prev_a;
	r->b = r->prev_b;
	if (r->updates > 0) {
		r->updates--;
	}
	r->applied = r->updates > 0 || r->a != 0.0 || r->b != 0.0;
}
