// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Mirror-alignment camera-pose calibration solver (pure C11).
 * @ingroup aux_math
 */

#include "math/m_mirror_calib.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define MC_MAX_PARAMS 6
#define MC_MAX_RES (2 * M_MIRROR_CALIB_MAX_SAMPLES)
#define MC_PI 3.14159265358979323846
#define MC_RAD2DEG (180.0 / MC_PI)
//! Above this (column-scaled) condition number a solved parameter is barely constrained.
#define MC_ILL_CONDITIONED 1e6
//! Eigenvalues below max * this count as zero (rank).
#define MC_RANK_EPS 1e-10

/*
 *
 * Small linear algebra.
 *
 */

static double
dot3(const double a[3], const double b[3])
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static bool
normalize3(const double in[3], double out[3])
{
	const double n = sqrt(dot3(in, in));
	if (!(n > 1e-12) || !isfinite(n)) {
		return false;
	}
	out[0] = in[0] / n;
	out[1] = in[1] / n;
	out[2] = in[2] / n;
	return true;
}

static void
mat3_vec(const double m[9], const double v[3], double out[3])
{
	out[0] = m[0] * v[0] + m[1] * v[1] + m[2] * v[2];
	out[1] = m[3] * v[0] + m[4] * v[1] + m[5] * v[2];
	out[2] = m[6] * v[0] + m[7] * v[1] + m[8] * v[2];
}

//! Rotation matrix (row-major) of rotation vector @p w (Rodrigues).
static void
rotvec_to_mat(const double w[3], double r[9])
{
	const double th = sqrt(dot3(w, w));
	if (th < 1e-12) {
		r[0] = 1.0;
		r[1] = -w[2];
		r[2] = w[1];
		r[3] = w[2];
		r[4] = 1.0;
		r[5] = -w[0];
		r[6] = -w[1];
		r[7] = w[0];
		r[8] = 1.0;
		return;
	}
	const double kx = w[0] / th, ky = w[1] / th, kz = w[2] / th;
	const double c = cos(th), s = sin(th), v = 1.0 - c;
	r[0] = c + kx * kx * v;
	r[1] = kx * ky * v - kz * s;
	r[2] = kx * kz * v + ky * s;
	r[3] = ky * kx * v + kz * s;
	r[4] = c + ky * ky * v;
	r[5] = ky * kz * v - kx * s;
	r[6] = kz * kx * v - ky * s;
	r[7] = kz * ky * v + kx * s;
	r[8] = c + kz * kz * v;
}

static void
mat_to_quat(const double m[9], double q[4])
{
	const double tr = m[0] + m[4] + m[8];
	double x, y, z, w;
	if (tr > 0.0) {
		const double s = sqrt(tr + 1.0) * 2.0;
		w = 0.25 * s;
		x = (m[7] - m[5]) / s;
		y = (m[2] - m[6]) / s;
		z = (m[3] - m[1]) / s;
	} else if (m[0] > m[4] && m[0] > m[8]) {
		const double s = sqrt(1.0 + m[0] - m[4] - m[8]) * 2.0;
		w = (m[7] - m[5]) / s;
		x = 0.25 * s;
		y = (m[1] + m[3]) / s;
		z = (m[2] + m[6]) / s;
	} else if (m[4] > m[8]) {
		const double s = sqrt(1.0 + m[4] - m[0] - m[8]) * 2.0;
		w = (m[2] - m[6]) / s;
		x = (m[1] + m[3]) / s;
		y = 0.25 * s;
		z = (m[5] + m[7]) / s;
	} else {
		const double s = sqrt(1.0 + m[8] - m[0] - m[4]) * 2.0;
		w = (m[3] - m[1]) / s;
		x = (m[2] + m[6]) / s;
		y = (m[5] + m[7]) / s;
		z = 0.25 * s;
	}
	if (w < 0.0) { // canonical: w >= 0
		x = -x;
		y = -y;
		z = -z;
		w = -w;
	}
	const double n = sqrt(x * x + y * y + z * z + w * w);
	q[0] = x / n;
	q[1] = y / n;
	q[2] = z / n;
	q[3] = w / n;
}

static void
quat_to_mat(const double qin[4], double m[9])
{
	double n = sqrt(qin[0] * qin[0] + qin[1] * qin[1] + qin[2] * qin[2] + qin[3] * qin[3]);
	if (!(n > 1e-12)) {
		n = 1.0;
	}
	const double x = qin[0] / n, y = qin[1] / n, z = qin[2] / n, w = qin[3] / n;
	m[0] = 1 - 2 * (y * y + z * z);
	m[1] = 2 * (x * y - z * w);
	m[2] = 2 * (x * z + y * w);
	m[3] = 2 * (x * y + z * w);
	m[4] = 1 - 2 * (x * x + z * z);
	m[5] = 2 * (y * z - x * w);
	m[6] = 2 * (x * z - y * w);
	m[7] = 2 * (y * z + x * w);
	m[8] = 1 - 2 * (x * x + y * y);
}

//! Solve the n x n system A x = b (Gaussian elimination, partial pivoting). A, b are clobbered.
static bool
solve_linear(int n, double a[MC_MAX_PARAMS][MC_MAX_PARAMS], double b[MC_MAX_PARAMS], double x[MC_MAX_PARAMS])
{
	for (int col = 0; col < n; col++) {
		int piv = col;
		for (int r = col + 1; r < n; r++) {
			if (fabs(a[r][col]) > fabs(a[piv][col])) {
				piv = r;
			}
		}
		if (!(fabs(a[piv][col]) > 1e-300)) {
			return false;
		}
		if (piv != col) {
			for (int k = 0; k < n; k++) {
				const double t = a[col][k];
				a[col][k] = a[piv][k];
				a[piv][k] = t;
			}
			const double t = b[col];
			b[col] = b[piv];
			b[piv] = t;
		}
		for (int r = col + 1; r < n; r++) {
			const double f = a[r][col] / a[col][col];
			for (int k = col; k < n; k++) {
				a[r][k] -= f * a[col][k];
			}
			b[r] -= f * b[col];
		}
	}
	for (int r = n - 1; r >= 0; r--) {
		double s = b[r];
		for (int k = r + 1; k < n; k++) {
			s -= a[r][k] * x[k];
		}
		x[r] = s / a[r][r];
		if (!isfinite(x[r])) {
			return false;
		}
	}
	return true;
}

//! Eigenvalues of the symmetric n x n matrix @p a (cyclic Jacobi). @p a is clobbered.
static void
sym_eigenvalues(int n, double a[MC_MAX_PARAMS][MC_MAX_PARAMS], double ev[MC_MAX_PARAMS])
{
	for (int sweep = 0; sweep < 100; sweep++) {
		double off = 0.0;
		for (int i = 0; i < n; i++) {
			for (int j = i + 1; j < n; j++) {
				off += a[i][j] * a[i][j];
			}
		}
		if (off < 1e-30) {
			break;
		}
		for (int p = 0; p < n; p++) {
			for (int q = p + 1; q < n; q++) {
				if (fabs(a[p][q]) < 1e-300) {
					continue;
				}
				const double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
				const double t = (theta >= 0 ? 1.0 : -1.0) / (fabs(theta) + sqrt(theta * theta + 1.0));
				const double c = 1.0 / sqrt(t * t + 1.0), s = t * c;
				for (int k = 0; k < n; k++) { // A' = J^T A J
					const double akp = a[k][p], akq = a[k][q];
					a[k][p] = c * akp - s * akq;
					a[k][q] = s * akp + c * akq;
				}
				for (int k = 0; k < n; k++) {
					const double apk = a[p][k], aqk = a[q][k];
					a[p][k] = c * apk - s * aqk;
					a[q][k] = s * apk + c * aqk;
				}
			}
		}
	}
	for (int i = 0; i < n; i++) {
		ev[i] = a[i][i];
	}
}


/*
 *
 * The model.
 *
 */

struct mc_problem
{
	const struct m_mirror_calib_sample *samples;
	int count;
	const bool *used;
	double dir[M_MIRROR_CALIB_MAX_SAMPLES][3]; //!< normalised input directions
	enum m_mirror_calib_mode mode;
	bool focal;
};

static int
param_count(enum m_mirror_calib_mode mode, bool focal)
{
	switch (mode) {
	case M_MIRROR_CALIB_MODE_TRANSLATION: return 2;
	case M_MIRROR_CALIB_MODE_PITCH: return 3;
	default: return focal ? 6 : 5;
	}
}

//! Params -> (R, T_xy, k).
static void
build_pose(const struct mc_problem *pr, const double *p, double r[9], double t[2], double *k)
{
	t[0] = p[0];
	t[1] = p[1];
	*k = 1.0;
	double w[3] = {0, 0, 0};
	if (pr->mode == M_MIRROR_CALIB_MODE_PITCH) {
		w[0] = p[2];
	} else if (pr->mode == M_MIRROR_CALIB_MODE_FULL) {
		w[0] = p[2];
		w[1] = p[3];
		w[2] = p[4];
		if (pr->focal) {
			*k = p[5];
		}
	}
	rotvec_to_mat(w, r);
}

/*!
 * Sample @p i under pose (r, t, k): the in-plane direction a = xy(R u'), the
 * eliminated s, and the 2-D residual xy(T + s R u') - D.
 */
static void
sample_eval(
    const struct mc_problem *pr, int i, const double r[9], const double t[2], double k, double *out_s, double res[2])
{
	const double *u = pr->dir[i];
	double up[3] = {u[0], u[1], k * u[2]};
	double un[3];
	if (!normalize3(up, un)) {
		un[0] = u[0];
		un[1] = u[1];
		un[2] = u[2];
	}
	double v[3];
	mat3_vec(r, un, v);
	const double d[2] = {pr->samples[i].dot_m[0] - t[0], pr->samples[i].dot_m[1] - t[1]};
	const double aa = v[0] * v[0] + v[1] * v[1];
	double s = 0.0;
	if (aa > 1e-18) {
		s = (v[0] * d[0] + v[1] * d[1]) / aa;
	}
	*out_s = s;
	res[0] = t[0] + s * v[0] - pr->samples[i].dot_m[0];
	res[1] = t[1] + s * v[1] - pr->samples[i].dot_m[1];
}

//! Residual vector over the used samples; returns its length (2 per used sample).
static int
residuals(const struct mc_problem *pr, const double *p, double *out)
{
	double r[9], t[2], k;
	build_pose(pr, p, r, t, &k);
	int m = 0;
	for (int i = 0; i < pr->count; i++) {
		if (!pr->used[i]) {
			continue;
		}
		double s;
		sample_eval(pr, i, r, t, k, &s, &out[m]);
		m += 2;
	}
	return m;
}

static double
cost_of(const double *res, int m)
{
	double c = 0.0;
	for (int i = 0; i < m; i++) {
		c += res[i] * res[i];
	}
	return c;
}

//! Numeric (central-difference) Jacobian, m x n, column-major into jac[col][row].
static void
jacobian(const struct mc_problem *pr, const double *p, int n, double jac[MC_MAX_PARAMS][MC_MAX_RES])
{
	double pp[MC_MAX_PARAMS];
	double rp[MC_MAX_RES], rm[MC_MAX_RES];
	for (int j = 0; j < n; j++) {
		memcpy(pp, p, sizeof(double) * (size_t)n);
		const double h = 1e-6 * (fabs(p[j]) > 1.0 ? fabs(p[j]) : 1.0);
		pp[j] = p[j] + h;
		const int m = residuals(pr, pp, rp);
		pp[j] = p[j] - h;
		(void)residuals(pr, pp, rm);
		for (int i = 0; i < m; i++) {
			jac[j][i] = (rp[i] - rm[i]) / (2.0 * h);
		}
	}
}

//! Levenberg-Marquardt over the pose params. Returns the iterations run.
static int
levenberg_marquardt(const struct mc_problem *pr, double *p, int n, int max_iter)
{
	double j_local[MC_MAX_PARAMS][MC_MAX_RES];
	double res[MC_MAX_RES], trial_res[MC_MAX_RES];
	int m = residuals(pr, p, res);
	double cost = cost_of(res, m);
	double lambda = 1e-3;
	int it = 0;
	for (; it < max_iter; it++) {
		jacobian(pr, p, n, j_local);
		double a[MC_MAX_PARAMS][MC_MAX_PARAMS], g[MC_MAX_PARAMS];
		for (int r = 0; r < n; r++) {
			g[r] = 0.0;
			for (int i = 0; i < m; i++) {
				g[r] += j_local[r][i] * res[i];
			}
			for (int c = 0; c < n; c++) {
				double s = 0.0;
				for (int i = 0; i < m; i++) {
					s += j_local[r][i] * j_local[c][i];
				}
				a[r][c] = s;
			}
		}
		bool improved = false;
		double step_norm = 0.0;
		while (lambda < 1e12) {
			double aa[MC_MAX_PARAMS][MC_MAX_PARAMS], b[MC_MAX_PARAMS], delta[MC_MAX_PARAMS];
			for (int r = 0; r < n; r++) {
				for (int c = 0; c < n; c++) {
					aa[r][c] = a[r][c];
				}
				aa[r][r] += lambda * (a[r][r] > 1e-12 ? a[r][r] : 1e-12);
				b[r] = -g[r];
			}
			if (!solve_linear(n, aa, b, delta)) {
				lambda *= 10.0;
				continue;
			}
			double trial[MC_MAX_PARAMS];
			step_norm = 0.0;
			for (int j = 0; j < n; j++) {
				trial[j] = p[j] + delta[j];
				step_norm += delta[j] * delta[j];
			}
			(void)residuals(pr, trial, trial_res);
			const double tc = cost_of(trial_res, m);
			if (tc < cost) {
				const double gain = cost - tc;
				memcpy(p, trial, sizeof(double) * (size_t)n);
				memcpy(res, trial_res, sizeof(double) * (size_t)m);
				cost = tc;
				lambda = lambda / 10.0 > 1e-12 ? lambda / 10.0 : 1e-12;
				improved = true;
				if (gain < 1e-20 + 1e-14 * cost) {
					return it + 1;
				}
				break;
			}
			lambda *= 10.0;
		}
		if (!improved || sqrt(step_norm) < 1e-13) {
			return it + 1;
		}
	}
	return it;
}

//! Column-scaled normal matrix at @p p: rank + condition.
static void
conditioning(const struct mc_problem *pr, const double *p, int n, int m, int *out_rank, double *out_cond)
{
	double j_local[MC_MAX_PARAMS][MC_MAX_RES];
	jacobian(pr, p, n, j_local);
	double a[MC_MAX_PARAMS][MC_MAX_PARAMS];
	double scale[MC_MAX_PARAMS];
	for (int r = 0; r < n; r++) {
		double s = 0.0;
		for (int i = 0; i < m; i++) {
			s += j_local[r][i] * j_local[r][i];
		}
		scale[r] = s > 1e-300 ? sqrt(s) : 0.0;
	}
	for (int r = 0; r < n; r++) {
		for (int c = 0; c < n; c++) {
			double s = 0.0;
			for (int i = 0; i < m; i++) {
				s += j_local[r][i] * j_local[c][i];
			}
			a[r][c] = (scale[r] > 0.0 && scale[c] > 0.0) ? s / (scale[r] * scale[c]) : 0.0;
		}
	}
	double ev[MC_MAX_PARAMS];
	sym_eigenvalues(n, a, ev);
	double mx = 0.0, mn = INFINITY;
	for (int i = 0; i < n; i++) {
		mx = ev[i] > mx ? ev[i] : mx;
		mn = ev[i] < mn ? ev[i] : mn;
	}
	int rank = 0;
	for (int i = 0; i < n; i++) {
		rank += (ev[i] > mx * MC_RANK_EPS) ? 1 : 0;
	}
	*out_rank = rank;
	*out_cond = (mn > 0.0) ? mx / mn : INFINITY;
}

static int
count_distinct_dots(const struct m_mirror_calib_sample *s, int count, const bool *used)
{
	double reps[M_MIRROR_CALIB_MAX_SAMPLES][2];
	int nrep = 0;
	for (int i = 0; i < count; i++) {
		if (!used[i]) {
			continue;
		}
		bool found = false;
		for (int j = 0; j < nrep; j++) {
			const double dx = s[i].dot_m[0] - reps[j][0], dy = s[i].dot_m[1] - reps[j][1];
			if (sqrt(dx * dx + dy * dy) < M_MIRROR_CALIB_DISTINCT_DOT_M) {
				found = true;
				break;
			}
		}
		if (!found) {
			reps[nrep][0] = s[i].dot_m[0];
			reps[nrep][1] = s[i].dot_m[1];
			nrep++;
		}
	}
	return nrep;
}

static void
mode_needs(enum m_mirror_calib_mode mode, int *min_samples, int *min_dots)
{
	switch (mode) {
	case M_MIRROR_CALIB_MODE_TRANSLATION:
		*min_samples = 2;
		*min_dots = 2;
		break;
	case M_MIRROR_CALIB_MODE_PITCH:
		*min_samples = 3;
		*min_dots = 2;
		break;
	default:
		*min_samples = 7;
		*min_dots = 3;
		break;
	}
}

//! Mode (a) closed form: T = argmin sum |P_i (T - D_i)|^2, P_i = I - a a^T / |a|^2.
static bool
closed_form_translation(const struct mc_problem *pr, double t[2])
{
	double m00 = 0, m01 = 0, m11 = 0, b0 = 0, b1 = 0;
	for (int i = 0; i < pr->count; i++) {
		if (!pr->used[i]) {
			continue;
		}
		const double ax = pr->dir[i][0], ay = pr->dir[i][1];
		const double aa = ax * ax + ay * ay;
		if (aa < 1e-18) {
			continue; // looking straight down the normal: no constraint on T from this line's direction
		}
		const double p00 = 1.0 - ax * ax / aa, p01 = -ax * ay / aa, p11 = 1.0 - ay * ay / aa;
		const double dx = pr->samples[i].dot_m[0], dy = pr->samples[i].dot_m[1];
		m00 += p00;
		m01 += p01;
		m11 += p11;
		b0 += p00 * dx + p01 * dy;
		b1 += p01 * dx + p11 * dy;
	}
	const double det = m00 * m11 - m01 * m01;
	const double tr = m00 + m11;
	if (!(fabs(det) > 1e-12 * (tr * tr + 1e-300))) {
		return false;
	}
	t[0] = (m11 * b0 - m01 * b1) / det;
	t[1] = (m00 * b1 - m01 * b0) / det;
	return true;
}

static int
cmp_double(const void *a, const void *b)
{
	const double x = *(const double *)a, y = *(const double *)b;
	return (x > y) - (x < y);
}

static double
median(double *v, int n)
{
	// insertion sort: n <= 64
	for (int i = 1; i < n; i++) {
		const double key = v[i];
		int j = i - 1;
		while (j >= 0 && cmp_double(&v[j], &key) > 0) {
			v[j + 1] = v[j];
			j--;
		}
		v[j + 1] = key;
	}
	if (n == 0) {
		return 0.0;
	}
	return (n % 2) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

static bool
fail(struct m_mirror_calib_result *out, enum m_mirror_calib_status st, const char *fmt, int a, int b)
{
	out->status = st;
	snprintf(out->reason, sizeof(out->reason), fmt, a, b);
	return false;
}


/*
 *
 * Public.
 *
 */

void
m_mirror_calib_options_init(struct m_mirror_calib_options *opt)
{
	memset(opt, 0, sizeof(*opt));
	opt->mode = M_MIRROR_CALIB_MODE_TRANSLATION;
	opt->max_iterations = 100;
}

//! One fit over the currently used samples, into p (sized for the final mode).
static int
fit(struct mc_problem *pr, const struct m_mirror_calib_options *opt, double p[MC_MAX_PARAMS], bool *ok)
{
	const int iters = opt->max_iterations > 0 ? opt->max_iterations : 100;
	double t[2];
	*ok = closed_form_translation(pr, t);
	if (!*ok) {
		return 0;
	}
	memset(p, 0, sizeof(double) * MC_MAX_PARAMS);
	p[0] = t[0];
	p[1] = t[1];
	const enum m_mirror_calib_mode final_mode = pr->mode;
	int it = 0;
	if (final_mode == M_MIRROR_CALIB_MODE_TRANSLATION) {
		// The closed form is exact for (a); a short polish keeps the code path uniform.
		pr->mode = M_MIRROR_CALIB_MODE_TRANSLATION;
		it = levenberg_marquardt(pr, p, 2, iters);
		return it;
	}
	// (a) -> (b)
	pr->mode = M_MIRROR_CALIB_MODE_PITCH;
	pr->focal = false;
	it = levenberg_marquardt(pr, p, 3, iters);
	if (final_mode == M_MIRROR_CALIB_MODE_PITCH) {
		return it;
	}
	// (b) -> (c): rotation vector (pitch, 0, 0), k = 1
	pr->mode = M_MIRROR_CALIB_MODE_FULL;
	pr->focal = opt->solve_focal_scale;
	p[3] = 0.0;
	p[4] = 0.0;
	p[5] = 1.0;
	it = levenberg_marquardt(pr, p, param_count(M_MIRROR_CALIB_MODE_FULL, pr->focal), iters);
	return it;
}

bool
m_mirror_calib_solve(const struct m_mirror_calib_sample *samples,
                     int count,
                     const struct m_mirror_calib_options *opt_in,
                     struct m_mirror_calib_result *out)
{
	if (out == NULL) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	struct m_mirror_calib_options opt;
	if (opt_in != NULL) {
		opt = *opt_in;
	} else {
		m_mirror_calib_options_init(&opt);
	}
	out->mode = opt.mode;
	out->focal_scale = 1.0;
	out->q[3] = 1.0;
	out->r[0] = out->r[4] = out->r[8] = 1.0;
	out->sample_count = count;

	if (samples == NULL || count < 0 || count > M_MIRROR_CALIB_MAX_SAMPLES ||
	    (opt.mode != M_MIRROR_CALIB_MODE_TRANSLATION && opt.mode != M_MIRROR_CALIB_MODE_PITCH &&
	     opt.mode != M_MIRROR_CALIB_MODE_FULL)) {
		return fail(out, M_MIRROR_CALIB_ERROR_INVALID_INPUT, "invalid input (count %d, max %d)", count,
		            M_MIRROR_CALIB_MAX_SAMPLES);
	}

	struct mc_problem pr;
	memset(&pr, 0, sizeof(pr));
	bool used[M_MIRROR_CALIB_MAX_SAMPLES];
	pr.samples = samples;
	pr.count = count;
	pr.used = used;
	pr.mode = opt.mode;
	pr.focal = opt.solve_focal_scale && opt.mode == M_MIRROR_CALIB_MODE_FULL;
	for (int i = 0; i < count; i++) {
		used[i] = true;
		if (!isfinite(samples[i].dot_m[0]) || !isfinite(samples[i].dot_m[1]) ||
		    !normalize3(samples[i].dir, pr.dir[i])) {
			return fail(out, M_MIRROR_CALIB_ERROR_INVALID_INPUT,
			            "sample %d has a non-finite dot or a zero / non-finite direction%.0d", i, 0);
		}
	}

	int min_samples, min_dots;
	mode_needs(opt.mode, &min_samples, &min_dots);
	if (count < min_samples) {
		return fail(out, M_MIRROR_CALIB_ERROR_TOO_FEW_SAMPLES, "mode needs at least %d samples, got %d",
		            min_samples, count);
	}
	const int distinct = count_distinct_dots(samples, count, used);
	if (distinct < min_dots) {
		if (distinct <= 1) {
			return fail(out, M_MIRROR_CALIB_ERROR_DEGENERATE,
			            "all %d samples are at one dot: the distance along the ray is unconstrained (scale "
			            "ambiguity) - align at least %d distinct dots",
			            count, min_dots);
		}
		return fail(out, M_MIRROR_CALIB_ERROR_DEGENERATE,
		            "only %d distinct dots (>= 10 mm apart); this mode needs at least %d", distinct, min_dots);
	}

	const int n = param_count(opt.mode, pr.focal);
	double p[MC_MAX_PARAMS];
	int iterations = 0;
	for (;;) {
		bool ok = false;
		pr.mode = opt.mode;
		iterations = fit(&pr, &opt, p, &ok);
		pr.mode = opt.mode;
		pr.focal = opt.solve_focal_scale && opt.mode == M_MIRROR_CALIB_MODE_FULL;
		if (!ok) {
			return fail(
			    out, M_MIRROR_CALIB_ERROR_DEGENERATE,
			    "the sample rays are parallel in the display plane: the camera position is unconstrained "
			    "(%d samples, %d distinct dots)",
			    count, count_distinct_dots(samples, count, used));
		}
		if (!(opt.reject_threshold_m > 0.0)) {
			break;
		}
		// Drop the worst used sample while it exceeds the threshold and enough remain.
		double r9[9], t2[2], k;
		build_pose(&pr, p, r9, t2, &k);
		int worst = -1;
		double worst_res = 0.0;
		int used_count = 0;
		for (int i = 0; i < count; i++) {
			if (!used[i]) {
				continue;
			}
			used_count++;
			double s, res[2];
			sample_eval(&pr, i, r9, t2, k, &s, res);
			const double e = sqrt(res[0] * res[0] + res[1] * res[1]);
			if (e > worst_res) {
				worst_res = e;
				worst = i;
			}
		}
		if (worst < 0 || worst_res <= opt.reject_threshold_m || used_count - 1 < min_samples) {
			break;
		}
		used[worst] = false;
		if (count_distinct_dots(samples, count, used) < min_dots) {
			used[worst] = true;
			break;
		}
		out->rejected[worst] = true;
	}

	// Report.
	double r9[9], t2[2], k;
	build_pose(&pr, p, r9, t2, &k);
	double sum2 = 0.0, mx = 0.0;
	int used_count = 0;
	double ratios[M_MIRROR_CALIB_MAX_SAMPLES];
	int nratio = 0;
	for (int i = 0; i < count; i++) {
		double s, res[2];
		sample_eval(&pr, i, r9, t2, k, &s, res);
		const double e = sqrt(res[0] * res[0] + res[1] * res[1]);
		out->residual_m[i] = e;
		out->distance_m[i] = s;
		if (!used[i]) {
			continue;
		}
		if (!(s > 0.0)) {
			return fail(
			    out, M_MIRROR_CALIB_ERROR_BEHIND_CAMERA,
			    "the fit puts sample %d behind the camera (solved distance <= 0)%.0d - check the dot "
			    "positions and the camera's handedness",
			    i, 0);
		}
		used_count++;
		sum2 += e * e;
		mx = e > mx ? e : mx;
		if (samples[i].est_distance_m > 0.0) {
			ratios[nratio++] = s / samples[i].est_distance_m;
		}
	}
	out->used_count = used_count;
	out->distinct_dots = count_distinct_dots(samples, count, used);
	out->rms_m = used_count > 0 ? sqrt(sum2 / used_count) : 0.0;
	out->max_m = mx;
	out->iris_scale = nratio > 0 ? median(ratios, nratio) : 0.0;
	out->params = n;
	out->iterations = iterations;
	conditioning(&pr, p, n, 2 * used_count, &out->rank, &out->condition);
	out->ill_conditioned = !(out->condition <= MC_ILL_CONDITIONED);

	memcpy(out->r, r9, sizeof(r9));
	mat_to_quat(r9, out->q);
	out->t_m[0] = t2[0];
	out->t_m[1] = t2[1];
	out->t_m[2] = opt.assumed_tz_m;
	out->focal_scale = k;
	out->pitch_deg = atan2(r9[7], r9[8]) * MC_RAD2DEG;
	const double sy = -r9[6] > 1.0 ? 1.0 : (-r9[6] < -1.0 ? -1.0 : -r9[6]);
	out->yaw_deg = asin(sy) * MC_RAD2DEG;
	out->roll_deg = atan2(r9[3], r9[0]) * MC_RAD2DEG;

	if (out->rank < n) {
		return fail(out, M_MIRROR_CALIB_ERROR_DEGENERATE,
		            "the samples constrain only %d of the %d pose parameters (rank deficient): vary the head "
		            "distance and use more dots",
		            out->rank, n);
	}
	out->status = M_MIRROR_CALIB_OK;
	out->reason[0] = '\0';
	return true;
}

bool
m_mirror_calib_eye_point(enum m_mirror_calib_eye eye,
                         const float left_dir[3],
                         float left_distance_m,
                         const float right_dir[3],
                         float right_distance_m,
                         double out_dir[3],
                         double *out_distance_m)
{
	double l[3] = {left_dir[0], left_dir[1], left_dir[2]};
	double r[3] = {right_dir[0], right_dir[1], right_dir[2]};
	double ln[3], rn[3];
	const bool lok = normalize3(l, ln), rok = normalize3(r, rn);
	*out_distance_m = 0.0;
	if (eye == M_MIRROR_CALIB_EYE_LEFT || eye == M_MIRROR_CALIB_EYE_RIGHT) {
		const bool ok = eye == M_MIRROR_CALIB_EYE_LEFT ? lok : rok;
		if (!ok) {
			return false;
		}
		memcpy(out_dir, eye == M_MIRROR_CALIB_EYE_LEFT ? ln : rn, sizeof(double) * 3);
		const float d = eye == M_MIRROR_CALIB_EYE_LEFT ? left_distance_m : right_distance_m;
		*out_distance_m = d > 0.0f ? (double)d : 0.0;
		return true;
	}
	if (!lok || !rok) {
		return false;
	}
	const bool have_d = left_distance_m > 0.0f && right_distance_m > 0.0f;
	const double dl = have_d ? (double)left_distance_m : 1.0;
	const double dr = have_d ? (double)right_distance_m : 1.0;
	double m[3] = {0.5 * (dl * ln[0] + dr * rn[0]), 0.5 * (dl * ln[1] + dr * rn[1]),
	               0.5 * (dl * ln[2] + dr * rn[2])};
	const double len = sqrt(dot3(m, m));
	if (!normalize3(m, out_dir)) {
		return false;
	}
	*out_distance_m = have_d ? len : 0.0;
	return true;
}

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
m_mirror_calib_parse_mode(const char *s, enum m_mirror_calib_mode *out)
{
	if (s == NULL || out == NULL) {
		return false;
	}
	if (ieq(s, "a") || ieq(s, "translation")) {
		*out = M_MIRROR_CALIB_MODE_TRANSLATION;
	} else if (ieq(s, "b") || ieq(s, "pitch")) {
		*out = M_MIRROR_CALIB_MODE_PITCH;
	} else if (ieq(s, "c") || ieq(s, "full")) {
		*out = M_MIRROR_CALIB_MODE_FULL;
	} else {
		return false;
	}
	return true;
}

bool
m_mirror_calib_parse_eye(const char *s, enum m_mirror_calib_eye *out)
{
	if (s == NULL || out == NULL) {
		return false;
	}
	if (ieq(s, "mid")) {
		*out = M_MIRROR_CALIB_EYE_MID;
	} else if (ieq(s, "left")) {
		*out = M_MIRROR_CALIB_EYE_LEFT;
	} else if (ieq(s, "right")) {
		*out = M_MIRROR_CALIB_EYE_RIGHT;
	} else {
		return false;
	}
	return true;
}

const char *
m_mirror_calib_mode_str(enum m_mirror_calib_mode mode)
{
	switch (mode) {
	case M_MIRROR_CALIB_MODE_TRANSLATION: return "a";
	case M_MIRROR_CALIB_MODE_PITCH: return "b";
	case M_MIRROR_CALIB_MODE_FULL: return "c";
	default: return "?";
	}
}

const char *
m_mirror_calib_status_str(enum m_mirror_calib_status status)
{
	switch (status) {
	case M_MIRROR_CALIB_OK: return "ok";
	case M_MIRROR_CALIB_ERROR_INVALID_INPUT: return "invalid_input";
	case M_MIRROR_CALIB_ERROR_TOO_FEW_SAMPLES: return "too_few_samples";
	case M_MIRROR_CALIB_ERROR_DEGENERATE: return "degenerate";
	case M_MIRROR_CALIB_ERROR_BEHIND_CAMERA: return "behind_camera";
	default: return "unknown";
	}
}

void
m_mirror_calib_apply_pose(const double t[3], const double q[4], const double p[3], double out[3])
{
	double m[9], v[3];
	quat_to_mat(q, m);
	mat3_vec(m, p, v);
	out[0] = t[0] + v[0];
	out[1] = t[1] + v[1];
	out[2] = t[2] + v[2];
}
