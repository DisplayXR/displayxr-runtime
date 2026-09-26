// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_stereo_camera (ADR-043) R2: vendor-neutral stereo
 *         rectification. See u_stereo_rectify.h.
 *
 * The geometry is Bouguet's algorithm exactly as OpenCV's cvStereoRectify
 * implements it (horizontal pair, CALIB_ZERO_DISPARITY, alpha = 0), so the
 * numbers can be golden-tested against OpenCV without depending on it:
 *
 *  1. split the relative rotation in half (each camera turns by -om/2 / +om/2
 *     so both look the same way): r_r = rodrigues(-rodrigues_inv(R) / 2);
 *  2. rotate both about the axis t x e_x so the baseline becomes the x axis:
 *     R1 = wR * r_r^T, R2 = wR * r_r;
 *  3. common focal = the smaller fy (reduced for barrel lenses, as OpenCV);
 *     common principal point = the mean of both eyes' rectified corner
 *     centroids (one cy for row alignment, one cx for zero disparity at
 *     infinity);
 *  4. alpha = 0: zoom about the principal point until the inner rectangle of
 *     every eye's rectified raw-image border covers the whole output — then
 *     verified on every border pixel of the real maps and nudged up until no
 *     output pixel samples outside its raw image (no black corners).
 *
 * @ingroup aux_util
 */

#include "util/u_stereo_rectify.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>


/*
 *
 * Small 3x3 helpers.
 *
 */

/*
 * Flat row-major 3x3 (m[i * 3 + j]): a `double (*)[3]` -> `const double (*)[3]`
 * argument is a -Wpedantic warning before C23 (GCC/MinGW), a flat pointer is not.
 */
#define M3(m) (&(m)[0][0])

static void
mat3_mul(const double *a, const double *b, double *out)
{
	double r[9];
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			r[i * 3 + j] =
			    a[i * 3 + 0] * b[0 * 3 + j] + a[i * 3 + 1] * b[1 * 3 + j] + a[i * 3 + 2] * b[2 * 3 + j];
		}
	}
	memcpy(out, r, sizeof(r));
}

static void
mat3_transpose(const double *a, double *out)
{
	double r[9];
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			r[i * 3 + j] = a[j * 3 + i];
		}
	}
	memcpy(out, r, sizeof(r));
}

static void
mat3_vec(const double *a, const double v[3], double out[3])
{
	double r[3];
	for (int i = 0; i < 3; i++) {
		r[i] = a[i * 3 + 0] * v[0] + a[i * 3 + 1] * v[1] + a[i * 3 + 2] * v[2];
	}
	memcpy(out, r, sizeof(r));
}

//! a^T * v
static void
mat3t_vec(const double *a, const double v[3], double out[3])
{
	double r[3];
	for (int i = 0; i < 3; i++) {
		r[i] = a[0 * 3 + i] * v[0] + a[1 * 3 + i] * v[1] + a[2 * 3 + i] * v[2];
	}
	memcpy(out, r, sizeof(r));
}

void
u_stereo_rectify_rodrigues(const double rvec[3], double out[3][3])
{
	double th = sqrt(rvec[0] * rvec[0] + rvec[1] * rvec[1] + rvec[2] * rvec[2]);
	if (th < 1e-15) {
		memset(out, 0, sizeof(double) * 9);
		out[0][0] = out[1][1] = out[2][2] = 1.0;
		return;
	}
	double x = rvec[0] / th, y = rvec[1] / th, z = rvec[2] / th;
	double c = cos(th), s = sin(th), C = 1.0 - c;
	out[0][0] = c + x * x * C;
	out[0][1] = x * y * C - z * s;
	out[0][2] = x * z * C + y * s;
	out[1][0] = y * x * C + z * s;
	out[1][1] = c + y * y * C;
	out[1][2] = y * z * C - x * s;
	out[2][0] = z * x * C - y * s;
	out[2][1] = z * y * C + x * s;
	out[2][2] = c + z * z * C;
}

void
u_stereo_rectify_rodrigues_inv(const double m[3][3], double out_rvec[3])
{
	double rx = m[2][1] - m[1][2];
	double ry = m[0][2] - m[2][0];
	double rz = m[1][0] - m[0][1];
	double s = sqrt(rx * rx + ry * ry + rz * rz) * 0.5;
	double c = (m[0][0] + m[1][1] + m[2][2] - 1.0) * 0.5;
	c = c > 1.0 ? 1.0 : c < -1.0 ? -1.0 : c;
	double th = atan2(s, c);
	if (s < 1e-5) {
		if (c > 0.0) {
			// Near identity: theta ~ s, vec = r / 2 (first order, exact to O(th^3)).
			out_rvec[0] = rx * 0.5;
			out_rvec[1] = ry * 0.5;
			out_rvec[2] = rz * 0.5;
			return;
		}
		// Near pi: the axis from the diagonal.
		double t[3] = {(m[0][0] + 1.0) * 0.5, (m[1][1] + 1.0) * 0.5, (m[2][2] + 1.0) * 0.5};
		double ax = sqrt(t[0] > 0 ? t[0] : 0), ay = sqrt(t[1] > 0 ? t[1] : 0), az = sqrt(t[2] > 0 ? t[2] : 0);
		if (m[0][1] < 0) {
			ay = -ay;
		}
		if (m[0][2] < 0) {
			az = -az;
		}
		if (fabs(ax) < fabs(ay) && fabs(ax) < fabs(az) && (m[1][2] > 0) != (ay * az > 0)) {
			az = -az;
		}
		double n = sqrt(ax * ax + ay * ay + az * az);
		n = n > 0 ? th / n : 0;
		out_rvec[0] = ax * n;
		out_rvec[1] = ay * n;
		out_rvec[2] = az * n;
		return;
	}
	double k = th / (2.0 * s);
	out_rvec[0] = rx * k;
	out_rvec[1] = ry * k;
	out_rvec[2] = rz * k;
}


/*
 *
 * Lens models.
 *
 */

void
u_stereo_rectify_distort(const struct u_stereo_rectify_lens *l, double x, double y, double *out_x, double *out_y)
{
	switch (l->model) {
	case U_STEREO_RECTIFY_MODEL_RADTAN5:
	case U_STEREO_RECTIFY_MODEL_RADTAN8: {
		const double *d = l->d;
		double k4 = 0, k5 = 0, k6 = 0;
		if (l->model == U_STEREO_RECTIFY_MODEL_RADTAN8) {
			k4 = d[5];
			k5 = d[6];
			k6 = d[7];
		}
		double r2 = x * x + y * y, r4 = r2 * r2, r6 = r4 * r2;
		double cdist = 1.0 + d[0] * r2 + d[1] * r4 + d[4] * r6;
		double icdist = 1.0 / (1.0 + k4 * r2 + k5 * r4 + k6 * r6);
		double a1 = 2.0 * x * y, a2 = r2 + 2.0 * x * x, a3 = r2 + 2.0 * y * y;
		*out_x = x * cdist * icdist + d[2] * a1 + d[3] * a2;
		*out_y = y * cdist * icdist + d[2] * a3 + d[3] * a1;
		return;
	}
	case U_STEREO_RECTIFY_MODEL_KB4: {
		double r = sqrt(x * x + y * y);
		if (r < 1e-12) {
			*out_x = x;
			*out_y = y;
			return;
		}
		double th = atan(r), t2 = th * th, t4 = t2 * t2, t6 = t4 * t2, t8 = t4 * t4;
		double thd = th * (1.0 + l->d[0] * t2 + l->d[1] * t4 + l->d[2] * t6 + l->d[3] * t8);
		double s = thd / r;
		*out_x = x * s;
		*out_y = y * s;
		return;
	}
	default:
		*out_x = x;
		*out_y = y;
		return;
	}
}

void
u_stereo_rectify_undistort_pixel(
    const struct u_stereo_rectify_lens *l, double u, double v, double *out_x, double *out_y)
{
	double x0 = (u - l->cx) / l->fx;
	double y0 = (v - l->cy) / l->fy;
	switch (l->model) {
	case U_STEREO_RECTIFY_MODEL_RADTAN5:
	case U_STEREO_RECTIFY_MODEL_RADTAN8: {
		const double *d = l->d;
		double k4 = 0, k5 = 0, k6 = 0;
		if (l->model == U_STEREO_RECTIFY_MODEL_RADTAN8) {
			k4 = d[5];
			k5 = d[6];
			k6 = d[7];
		}
		// OpenCV's fixed-point iteration, run to convergence.
		double x = x0, y = y0;
		for (int it = 0; it < 100; it++) {
			double r2 = x * x + y * y;
			double icdist =
			    (1.0 + ((k6 * r2 + k5) * r2 + k4) * r2) / (1.0 + ((d[4] * r2 + d[1]) * r2 + d[0]) * r2);
			if (!(icdist > 0.0)) {
				break; // outside the model's invertible range: keep the last estimate
			}
			double dx = 2.0 * d[2] * x * y + d[3] * (r2 + 2.0 * x * x);
			double dy = d[2] * (r2 + 2.0 * y * y) + 2.0 * d[3] * x * y;
			double nx = (x0 - dx) * icdist, ny = (y0 - dy) * icdist;
			double delta = fabs(nx - x) + fabs(ny - y);
			x = nx;
			y = ny;
			if (delta < 1e-14) {
				break;
			}
		}
		*out_x = x;
		*out_y = y;
		return;
	}
	case U_STEREO_RECTIFY_MODEL_KB4: {
		double thd = sqrt(x0 * x0 + y0 * y0);
		if (thd < 1e-12) {
			*out_x = x0;
			*out_y = y0;
			return;
		}
		// Newton on theta(1 + k1 th^2 + ...) = thd.
		double th = thd;
		for (int it = 0; it < 50; it++) {
			double t2 = th * th, t4 = t2 * t2, t6 = t4 * t2, t8 = t4 * t4;
			double f = th * (1.0 + l->d[0] * t2 + l->d[1] * t4 + l->d[2] * t6 + l->d[3] * t8) - thd;
			double df =
			    1.0 + 3.0 * l->d[0] * t2 + 5.0 * l->d[1] * t4 + 7.0 * l->d[2] * t6 + 9.0 * l->d[3] * t8;
			double step = f / df;
			th -= step;
			if (fabs(step) < 1e-15) {
				break;
			}
		}
		double s = tan(th) / thd;
		*out_x = x0 * s;
		*out_y = y0 * s;
		return;
	}
	default:
		*out_x = x0;
		*out_y = y0;
		return;
	}
}


/*
 *
 * Geometry.
 *
 */

//! RAW pixel of eye @p e -> rectified pixel under (f, cx, cy). false = behind.
static bool
raw_to_rect(const struct u_stereo_rectify_result *r, uint32_t e, double u, double v, double *ou, double *ov)
{
	double x, y;
	u_stereo_rectify_undistort_pixel(&r->raw[e], u, v, &x, &y);
	double p[3] = {x, y, 1.0}, q[3];
	mat3_vec(M3(r->rot[e]), p, q);
	if (q[2] <= 1e-12) {
		return false;
	}
	*ou = r->f * q[0] / q[2] + r->cx;
	*ov = r->f * q[1] / q[2] + r->cy;
	return true;
}

bool
u_stereo_rectify_map_point(
    const struct u_stereo_rectify_result *r, uint32_t eye, double u, double v, double *out_x, double *out_y)
{
	double p[3] = {(u - r->cx) / r->f, (v - r->cy) / r->f, 1.0}, q[3];
	mat3t_vec(M3(r->rot[eye]), p, q); // raw ray = R_e^T * rectified ray
	if (q[2] <= 1e-12) {
		return false;
	}
	double xd, yd;
	u_stereo_rectify_distort(&r->raw[eye], q[0] / q[2], q[1] / q[2], &xd, &yd);
	*out_x = r->raw[eye].fx * xd + r->raw[eye].cx;
	*out_y = r->raw[eye].fy * yd + r->raw[eye].cy;
	return true;
}

//! Does every border pixel of both eyes' output sample inside its raw image?
static bool
border_valid(const struct u_stereo_rectify_result *r)
{
	const double wmax = (double)r->width - 1.0, hmax = (double)r->height - 1.0;
	for (uint32_t e = 0; e < 2; e++) {
		for (uint32_t i = 0; i < 2 * (r->width + r->height); i++) {
			double u, v;
			if (i < r->width) {
				u = i, v = 0;
			} else if (i < 2 * r->width) {
				u = i - r->width, v = hmax;
			} else if (i < 2 * r->width + r->height) {
				u = 0, v = i - 2 * r->width;
			} else {
				u = wmax, v = i - 2 * r->width - r->height;
			}
			double x, y;
			if (!u_stereo_rectify_map_point(r, e, u, v, &x, &y) || x < 0.0 || y < 0.0 || x > wmax ||
			    y > hmax) {
				return false;
			}
		}
	}
	return true;
}

bool
u_stereo_rectify_compute(const struct u_stereo_rectify_input *in, struct u_stereo_rectify_result *out)
{
	memset(out, 0, sizeof(*out));
	if (in->width < 8 || in->height < 8) {
		return false;
	}
	const double nx = in->width, ny = in->height;
	out->width = in->width;
	out->height = in->height;

	// 0. Intrinsics at the frame size (pixel centres: u' = (u + 0.5) * s - 0.5).
	double sx = in->calib_width ? nx / (double)in->calib_width : 1.0;
	double sy = in->calib_height ? ny / (double)in->calib_height : 1.0;
	for (int e = 0; e < 2; e++) {
		struct u_stereo_rectify_lens *l = &out->raw[e];
		*l = in->eye[e];
		l->fx = in->eye[e].fx * sx;
		l->fy = in->eye[e].fy * sy;
		l->cx = (in->eye[e].cx + 0.5) * sx - 0.5;
		l->cy = (in->eye[e].cy + 0.5) * sy - 0.5;
		if (!(l->fx > 0.0) || !(l->fy > 0.0) || l->model > U_STEREO_RECTIFY_MODEL_KB4) {
			return false;
		}
	}
	double nt = sqrt(in->T[0] * in->T[0] + in->T[1] * in->T[1] + in->T[2] * in->T[2]);
	if (!(nt > 0.0)) {
		return false;
	}
	out->baseline = nt;

	// 1. Half rotation each.
	double om[3], r_r[3][3];
	u_stereo_rectify_rodrigues_inv(in->R, om);
	for (int i = 0; i < 3; i++) {
		om[i] *= -0.5;
	}
	u_stereo_rectify_rodrigues(om, r_r);
	double t[3];
	mat3_vec(M3(r_r), in->T, t);
	if (fabs(t[0]) < fabs(t[1])) {
		return false; // a vertical pair: not a side-by-side camera
	}

	// 2. Baseline onto the x axis.
	double c = t[0];
	double uu[3] = {c > 0.0 ? 1.0 : -1.0, 0.0, 0.0};
	double ww[3] = {t[1] * uu[2] - t[2] * uu[1], t[2] * uu[0] - t[0] * uu[2], t[0] * uu[1] - t[1] * uu[0]};
	double nw = sqrt(ww[0] * ww[0] + ww[1] * ww[1] + ww[2] * ww[2]);
	if (nw > 0.0) {
		double k = acos(fabs(c) / nt) / nw;
		for (int i = 0; i < 3; i++) {
			ww[i] *= k;
		}
	}
	double wR[3][3], r_rt[3][3];
	u_stereo_rectify_rodrigues(ww, wR);
	mat3_transpose(M3(r_r), M3(r_rt));
	mat3_mul(M3(wR), M3(r_rt), M3(out->rot[0]));
	mat3_mul(M3(wR), M3(r_r), M3(out->rot[1]));
	mat3_vec(M3(out->rot[1]), in->T, out->t_rect);

	// 3. Common focal: the smaller fy, shrunk for barrel lenses (OpenCV).
	double fc = DBL_MAX;
	for (int e = 0; e < 2; e++) {
		double f = out->raw[e].fy;
		double dk1 = (out->raw[e].model == U_STEREO_RECTIFY_MODEL_RADTAN5 ||
		              out->raw[e].model == U_STEREO_RECTIFY_MODEL_RADTAN8)
		                 ? out->raw[e].d[0]
		                 : 0.0;
		if (dk1 < 0.0) {
			f *= 1.0 + dk1 * (nx * nx + ny * ny) / (4.0 * f * f);
		}
		fc = f < fc ? f : fc;
	}
	if (!(fc > 0.0)) {
		return false;
	}

	//    Common principal point: each eye's rectified corner centroid, averaged.
	double ccx[2], ccy[2];
	for (uint32_t e = 0; e < 2; e++) {
		const double corners[4][2] = {{0, 0}, {nx - 1, 0}, {0, ny - 1}, {nx - 1, ny - 1}};
		double ax = 0, ay = 0;
		for (int k = 0; k < 4; k++) {
			double x, y;
			u_stereo_rectify_undistort_pixel(&out->raw[e], corners[k][0], corners[k][1], &x, &y);
			double p[3] = {x, y, 1.0}, q[3];
			mat3_vec(M3(out->rot[e]), p, q);
			ax += q[0] / q[2];
			ay += q[1] / q[2];
		}
		ccx[e] = (nx - 1) * 0.5 - fc * ax * 0.25;
		ccy[e] = (ny - 1) * 0.5 - fc * ay * 0.25;
	}
	out->cx = (ccx[0] + ccx[1]) * 0.5;
	out->cy = (ccy[0] + ccy[1]) * 0.5;
	out->f = fc;

	// 4. alpha = 0: the inner rectangle of each eye's rectified border (the 9x9
	//    grid OpenCV's icvGetRectangles uses) must cover the whole output.
	enum
	{
		N = 9
	};
	double s0 = 0.0;
	for (uint32_t e = 0; e < 2; e++) {
		double ix0 = -DBL_MAX, ix1 = DBL_MAX, iy0 = -DBL_MAX, iy1 = DBL_MAX;
		for (int i = 0; i < N; i++) {
			for (int j = 0; j < N; j++) {
				double u, v;
				if (!raw_to_rect(out, e, j * (nx - 1) / (N - 1), i * (ny - 1) / (N - 1), &u, &v)) {
					return false;
				}
				if (j == 0 && u > ix0) {
					ix0 = u;
				}
				if (j == N - 1 && u < ix1) {
					ix1 = u;
				}
				if (i == 0 && v > iy0) {
					iy0 = v;
				}
				if (i == N - 1 && v < iy1) {
					iy1 = v;
				}
			}
		}
		if (!(ix0 < out->cx && out->cx < ix1 && iy0 < out->cy && out->cy < iy1)) {
			return false; // the principal point is outside the valid region
		}
		double s = out->cx / (out->cx - ix0);
		double s2 = out->cy / (out->cy - iy0);
		double s3 = (nx - 1 - out->cx) / (ix1 - out->cx);
		double s4 = (ny - 1 - out->cy) / (iy1 - out->cy);
		s = s2 > s ? s2 : s;
		s = s3 > s ? s3 : s;
		s = s4 > s ? s4 : s;
		s0 = s > s0 ? s : s0;
	}
	out->crop_scale = s0;
	out->f = fc * s0;

	//    The 9-point inner rectangle is an approximation of the valid region's
	//    inscribed rectangle: verify on every border pixel of the real maps
	//    and zoom a little further until no output pixel samples outside.
	for (int it = 0; it < 200 && !border_valid(out); it++) {
		out->crop_scale *= 1.001;
		out->f = fc * out->crop_scale;
	}
	if (!border_valid(out)) {
		return false;
	}

	memset(out->P, 0, sizeof(out->P));
	for (int e = 0; e < 2; e++) {
		out->P[e][0][0] = out->f;
		out->P[e][0][2] = out->cx;
		out->P[e][1][1] = out->f;
		out->P[e][1][2] = out->cy;
		out->P[e][2][2] = 1.0;
	}
	out->P[1][0][3] = out->f * out->t_rect[0];
	return true;
}

uint32_t
u_stereo_rectify_build_map(const struct u_stereo_rectify_result *r, uint32_t eye, uint32_t sub, float *map_xy)
{
	if (sub == 0) {
		sub = 1;
	}
	const uint32_t w = r->width / sub, h = r->height / sub;
	const double half = (sub - 1) * 0.5; // sample centre offset in full-res pixels
	uint32_t valid = 0;
	for (uint32_t y = 0; y < h; y++) {
		for (uint32_t x = 0; x < w; x++) {
			double sxf, syf;
			float *o = map_xy + 2 * ((size_t)y * w + x);
			if (u_stereo_rectify_map_point(r, eye, x * (double)sub + half, y * (double)sub + half, &sxf,
			                               &syf)) {
				sxf = (sxf - half) / sub;
				syf = (syf - half) / sub;
				if (sxf >= 0.0 && syf >= 0.0 && sxf <= w - 1.0 && syf <= h - 1.0) {
					o[0] = (float)sxf;
					o[1] = (float)syf;
					valid++;
					continue;
				}
			}
			o[0] = -1.0f;
			o[1] = -1.0f;
		}
	}
	return valid;
}


/*
 *
 * CPU LUT.
 *
 */

bool
u_stereo_rectify_lut_init(struct u_stereo_rectify_lut *lut, const struct u_stereo_rectify_result *r, uint32_t sub)
{
	memset(lut, 0, sizeof(*lut));
	if (sub == 0) {
		sub = 1;
	}
	const uint32_t ew = r->width / sub, eh = r->height / sub;
	if (ew < 2 || eh < 2 || 2 * ew > 32767 || eh > 32767) {
		return false;
	}
	float *map = malloc(sizeof(float) * 2 * (size_t)ew * eh);
	lut->taps = malloc(sizeof(*lut->taps) * 2 * (size_t)ew * eh);
	if (map == NULL || lut->taps == NULL) {
		free(map);
		free(lut->taps);
		lut->taps = NULL;
		return false;
	}
	lut->width = 2 * ew;
	lut->height = eh;
	lut->sub = sub;
	for (uint32_t e = 0; e < 2; e++) {
		lut->valid += u_stereo_rectify_build_map(r, e, sub, map);
		for (uint32_t y = 0; y < eh; y++) {
			for (uint32_t x = 0; x < ew; x++) {
				const float *m = map + 2 * ((size_t)y * ew + x);
				struct u_stereo_rectify_tap *t = &lut->taps[(size_t)y * lut->width + e * ew + x];
				if (m[0] < 0.0f) {
					t->sx = -1;
					t->sy = -1;
					t->wx = t->wy = 0;
					continue;
				}
				int ix = (int)floorf(m[0]), iy = (int)floorf(m[1]);
				int wx = (int)lrintf((m[0] - (float)ix) * 256.0f);
				int wy = (int)lrintf((m[1] - (float)iy) * 256.0f);
				if (wx >= 256) {
					ix++, wx = 0;
				}
				if (wy >= 256) {
					iy++, wy = 0;
				}
				// Keep both bilinear taps inside this eye's half.
				if (ix >= (int)ew - 1) {
					ix = (int)ew - 2, wx = 255;
				}
				if (iy >= (int)eh - 1) {
					iy = (int)eh - 2, wy = 255;
				}
				t->sx = (int16_t)(ix + (int)(e * ew));
				t->sy = (int16_t)iy;
				t->wx = (uint8_t)wx;
				t->wy = (uint8_t)wy;
			}
		}
	}
	free(map);
	return true;
}

void
u_stereo_rectify_lut_fini(struct u_stereo_rectify_lut *lut)
{
	free(lut->taps);
	memset(lut, 0, sizeof(*lut));
}

void
u_stereo_rectify_lut_apply_rows(const struct u_stereo_rectify_lut *lut,
                                uint32_t channels,
                                const uint8_t *src,
                                uint32_t src_pitch,
                                uint8_t *dst,
                                uint32_t dst_pitch,
                                uint32_t y0,
                                uint32_t y1)
{
	if (y1 > lut->height) {
		y1 = lut->height;
	}
	const uint32_t W = lut->width;
	for (uint32_t y = y0; y < y1; y++) {
		const struct u_stereo_rectify_tap *t = lut->taps + (size_t)y * W;
		uint8_t *d = dst + (size_t)y * dst_pitch;
		if (channels == 1) {
			for (uint32_t x = 0; x < W; x++, t++) {
				if (t->sx < 0) {
					d[x] = 0;
					continue;
				}
				const uint8_t *p = src + (size_t)t->sy * src_pitch + t->sx;
				uint32_t wx = t->wx, wy = t->wy;
				uint32_t top = p[0] * (256 - wx) + p[1] * wx;
				uint32_t bot = p[src_pitch] * (256 - wx) + p[src_pitch + 1] * wx;
				d[x] = (uint8_t)((top * (256 - wy) + bot * wy + 32768u) >> 16);
			}
			continue;
		}
		for (uint32_t x = 0; x < W; x++, t++) {
			uint8_t *o = d + (size_t)x * channels;
			if (t->sx < 0) {
				for (uint32_t ch = 0; ch < channels; ch++) {
					// black: neutral chroma for NV12 UV, opaque for BGRA
					o[ch] = channels == 2 ? 128 : (channels == 4 && ch == 3) ? 255 : 0;
				}
				continue;
			}
			const uint8_t *p = src + (size_t)t->sy * src_pitch + (size_t)t->sx * channels;
			const uint8_t *q = p + src_pitch;
			uint32_t wx = t->wx, wy = t->wy;
			for (uint32_t ch = 0; ch < channels; ch++) {
				uint32_t top = p[ch] * (256 - wx) + p[ch + channels] * wx;
				uint32_t bot = q[ch] * (256 - wx) + q[ch + channels] * wx;
				o[ch] = (uint8_t)((top * (256 - wy) + bot * wy + 32768u) >> 16);
			}
		}
	}
}

void
u_stereo_rectify_lut_apply(const struct u_stereo_rectify_lut *lut,
                           uint32_t channels,
                           const uint8_t *src,
                           uint32_t src_pitch,
                           uint8_t *dst,
                           uint32_t dst_pitch)
{
	u_stereo_rectify_lut_apply_rows(lut, channels, src, src_pitch, dst, dst_pitch, 0, lut->height);
}
