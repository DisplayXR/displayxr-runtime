// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_display FAKE stereo camera: the synthetic SBS scene.
 * @ingroup drv_sim_display
 */

#include "sim_display_stereo_camera_pattern.h"

#include "util/u_stereo_rectify.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

//! 5x7 digits, one byte per row, bit 4 = leftmost column.
static const uint8_t k_digits[10][7] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, // 0
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}, // 1
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, // 2
    {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}, // 3
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, // 4
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}, // 5
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, // 6
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}, // 7
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, // 8
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}, // 9
};

#define COUNTER_DIGITS 8
#define COUNTER_SCALE 3
#define COUNTER_X 8
#define COUNTER_Y 8

static inline uint32_t
hash3(uint32_t x, uint32_t y, uint32_t seed)
{
	uint32_t h = x * 0x8da6b343u ^ y * 0xd8163841u ^ seed * 0xcb1ab31fu;
	h ^= h >> 13;
	h *= 0x5bd1e995u;
	h ^= h >> 15;
	return h;
}

//! Value noise on 2-pixel cells: detail a block matcher locks onto, without
//! single-pixel aliasing.
static inline uint8_t
texel(uint32_t u, uint32_t v, uint32_t seed, uint8_t lo, uint8_t span)
{
	return (uint8_t)(lo + hash3(u >> 1, v >> 1, seed) % span);
}

void
sim_stereo_camera_scene_init(struct sim_stereo_camera_scene *s,
                             uint32_t eye_width,
                             uint32_t eye_height,
                             double fx_px,
                             double baseline_mm,
                             double bg_depth_m,
                             double bar_depth_m)
{
	memset(s, 0, sizeof(*s));
	s->eye_width = eye_width;
	s->eye_height = eye_height;
	s->bg_disparity = (uint32_t)lround(fx_px * (baseline_mm / 1000.0) / bg_depth_m);
	s->bar_disparity = (uint32_t)lround(fx_px * (baseline_mm / 1000.0) / bar_depth_m);
	s->bg_disparity_f = fx_px * (baseline_mm / 1000.0) / bg_depth_m;
	s->bar_disparity_f = fx_px * (baseline_mm / 1000.0) / bar_depth_m;
	s->bar_x0 = eye_width * 3 / 8;
	s->bar_x1 = eye_width * 5 / 8;
	s->bar_y0 = eye_height / 4;
	s->bar_y1 = eye_height * 3 / 4;
}

static void
render_eye(const struct sim_stereo_camera_scene *s, uint8_t *dst, uint32_t pitch, uint32_t x_off, int right)
{
	for (uint32_t y = 0; y < s->eye_height; y++) {
		uint8_t *row = dst + (size_t)y * pitch + x_off;
		bool in_bar_rows = y >= s->bar_y0 && y < s->bar_y1;
		for (uint32_t x = 0; x < s->eye_width; x++) {
			// A feature at scene coordinate u shows at x = u in the left eye and at
			// x = u - d in the right eye, so the right eye samples u = x + d.
			uint32_t ub = right ? x + s->bg_disparity : x;
			uint32_t ubar = right ? x + s->bar_disparity : x;
			if (in_bar_rows && ubar >= s->bar_x0 && ubar < s->bar_x1) {
				row[x] = texel(ubar, y, 0xBA5u, 128, 128);
			} else {
				row[x] = texel(ub, y, 0x0B6u, 0, 112);
			}
		}
	}
}

static void
render_counter(uint8_t *dst, uint32_t pitch, uint32_t x_off, uint32_t eye_w, uint32_t eye_h, uint64_t n)
{
	const uint32_t cw = 6 * COUNTER_SCALE, ch = 9 * COUNTER_SCALE;
	const uint32_t box_w = COUNTER_DIGITS * cw + COUNTER_SCALE, box_h = ch;
	if (COUNTER_X + box_w > eye_w || COUNTER_Y + box_h > eye_h) {
		return;
	}
	for (uint32_t y = 0; y < box_h; y++) {
		memset(dst + (size_t)(COUNTER_Y + y) * pitch + x_off + COUNTER_X, 0, box_w);
	}
	char digits[COUNTER_DIGITS];
	for (int i = COUNTER_DIGITS - 1; i >= 0; i--) {
		digits[i] = (char)(n % 10);
		n /= 10;
	}
	for (uint32_t i = 0; i < COUNTER_DIGITS; i++) {
		const uint8_t *g = k_digits[(int)digits[i]];
		uint32_t gx = COUNTER_X + COUNTER_SCALE + i * cw;
		for (uint32_t r = 0; r < 7; r++) {
			for (uint32_t c = 0; c < 5; c++) {
				if (((g[r] >> (4 - c)) & 1u) == 0) {
					continue;
				}
				for (uint32_t sy = 0; sy < COUNTER_SCALE; sy++) {
					uint8_t *p =
					    dst + (size_t)(COUNTER_Y + COUNTER_SCALE + r * COUNTER_SCALE + sy) * pitch +
					    x_off + gx + c * COUNTER_SCALE;
					memset(p, 255, COUNTER_SCALE);
				}
			}
		}
	}
}

void
sim_stereo_camera_render_gray(const struct sim_stereo_camera_scene *s,
                              uint64_t frame_number,
                              uint8_t *dst,
                              uint32_t pitch)
{
	render_eye(s, dst, pitch, 0, 0);
	render_eye(s, dst, pitch, s->eye_width, 1);
	render_counter(dst, pitch, 0, s->eye_width, s->eye_height, frame_number);
	render_counter(dst, pitch, s->eye_width, s->eye_width, s->eye_height, frame_number);
}


/*
 *
 * DISTORTED variant (R2): the same scene through two raw cameras with known
 * ground truth.
 *
 */

#define DEG(x) ((x)*3.14159265358979323846 / 180.0)

//! Bilinear value noise on 2-pixel cells: continuous, so a resampled image is
//! an honest sample of the scene (no pixel-locked texture).
static float
vnoise(double u, double v, uint32_t seed, float lo, float span)
{
	double gu = u * 0.5, gv = v * 0.5;
	double fu = floor(gu), fv = floor(gv);
	int32_t iu = (int32_t)fu, iv = (int32_t)fv;
	float a = (float)(gu - fu), b = (float)(gv - fv);
	uint32_t sp = (uint32_t)span;
	float h00 = (float)(hash3((uint32_t)iu, (uint32_t)iv, seed) % sp);
	float h10 = (float)(hash3((uint32_t)(iu + 1), (uint32_t)iv, seed) % sp);
	float h01 = (float)(hash3((uint32_t)iu, (uint32_t)(iv + 1), seed) % sp);
	float h11 = (float)(hash3((uint32_t)(iu + 1), (uint32_t)(iv + 1), seed) % sp);
	float top = h00 + (h10 - h00) * a, bot = h01 + (h11 - h01) * a;
	return lo + top + (bot - top) * b;
}

float
sim_stereo_camera_scene_sample(const struct sim_stereo_camera_scene *s, int right, double u, double v)
{
	double ub = right ? u + s->bg_disparity_f : u;
	double ubar = right ? u + s->bar_disparity_f : u;
	if (v >= s->bar_y0 && v < s->bar_y1 && ubar >= s->bar_x0 && ubar < s->bar_x1) {
		return vnoise(ubar, v, 0xBA5u, 128.0f, 127.0f);
	}
	return vnoise(ub, v, 0x0B6u, 0.0f, 111.0f);
}

void
sim_stereo_camera_truth_init(struct sim_stereo_camera_truth *t,
                             uint32_t eye_width,
                             uint32_t eye_height,
                             double ideal_fx,
                             double baseline_mm,
                             double bg_depth_m,
                             double bar_depth_m)
{
	memset(t, 0, sizeof(*t));
	const double w = eye_width, h = eye_height;
	t->eye_width = eye_width;
	t->eye_height = eye_height;
	t->ideal_fx = ideal_fx;
	t->ideal_cx = (w - 1.0) * 0.5;
	t->ideal_cy = (h - 1.0) * 0.5;
	t->baseline_mm = baseline_mm;
	t->bg_depth_m = bg_depth_m;
	t->bar_depth_m = bar_depth_m;

	// Two different, off-centre, barrel lenses (scale with the eye size).
	t->fx[0] = ideal_fx * 1.012;
	t->fy[0] = ideal_fx * 1.008;
	t->cx[0] = t->ideal_cx + w * 0.009;
	t->cy[0] = t->ideal_cy - h * 0.008;
	const double d0[5] = {-0.21, 0.06, 0.0008, -0.0006, -0.005};
	t->fx[1] = ideal_fx * 0.991;
	t->fy[1] = ideal_fx * 0.994;
	t->cx[1] = t->ideal_cx - w * 0.007;
	t->cy[1] = t->ideal_cy + h * 0.007;
	const double d1[5] = {-0.17, 0.04, -0.0005, 0.0007, 0.0};
	memcpy(t->dist[0], d0, sizeof(d0));
	memcpy(t->dist[1], d1, sizeof(d1));

	// Each camera turned by half of (pitch 0.35 deg, yaw 0.25 deg, roll 0.45
	// deg), in opposite senses: 0.7 deg of relative pitch is the vertical
	// misalignment, 0.9 deg of relative roll the tilt.
	const double rv[3] = {DEG(0.35), DEG(0.25), DEG(0.45)};
	u_stereo_rectify_rodrigues(rv, t->S);
	// R = S * S (right-from-left rotation); T = -S * C_right, C_right = (B, 0, 0).
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			t->R[i][j] = t->S[i][0] * t->S[0][j] + t->S[i][1] * t->S[1][j] + t->S[i][2] * t->S[2][j];
		}
		t->T_mm[i] = -t->S[i][0] * baseline_mm;
	}
}

bool
sim_stereo_camera_distorted_init(struct sim_stereo_camera_distorted *d,
                                 uint32_t eye_width,
                                 uint32_t eye_height,
                                 double ideal_fx,
                                 double baseline_mm,
                                 double bg_depth_m,
                                 double bar_depth_m)
{
	memset(d, 0, sizeof(*d));
	sim_stereo_camera_scene_init(&d->scene, eye_width, eye_height, ideal_fx, baseline_mm, bg_depth_m, bar_depth_m);
	sim_stereo_camera_truth_init(&d->truth, eye_width, eye_height, ideal_fx, baseline_mm, bg_depth_m, bar_depth_m);
	d->ideal_uv = malloc(sizeof(float) * 4 * (size_t)eye_width * eye_height);
	if (d->ideal_uv == NULL) {
		return false;
	}
	const struct sim_stereo_camera_truth *t = &d->truth;
	for (uint32_t e = 0; e < 2; e++) {
		struct u_stereo_rectify_lens lens;
		memset(&lens, 0, sizeof(lens));
		lens.fx = t->fx[e];
		lens.fy = t->fy[e];
		lens.cx = t->cx[e];
		lens.cy = t->cy[e];
		lens.model = U_STEREO_RECTIFY_MODEL_RADTAN5;
		memcpy(lens.d, t->dist[e], sizeof(t->dist[e]));
		float *uv = d->ideal_uv + (size_t)e * 2 * eye_width * eye_height;
		for (uint32_t y = 0; y < eye_height; y++) {
			for (uint32_t x = 0; x < eye_width; x++) {
				double xn, yn;
				u_stereo_rectify_undistort_pixel(&lens, x, y, &xn, &yn);
				// Raw ray -> virtual parallel frame: left x_rect = S x_raw
				// (x_raw = S^T x_rect), right x_rect = S^T x_raw.
				double r[3] = {xn, yn, 1.0}, q[3];
				for (int i = 0; i < 3; i++) {
					q[i] = e == 0 ? t->S[i][0] * r[0] + t->S[i][1] * r[1] + t->S[i][2] * r[2]
					              : t->S[0][i] * r[0] + t->S[1][i] * r[1] + t->S[2][i] * r[2];
				}
				float *o = uv + 2 * ((size_t)y * eye_width + x);
				if (q[2] <= 1e-9) {
					o[0] = o[1] = -1e9f;
					continue;
				}
				o[0] = (float)(t->ideal_fx * q[0] / q[2] + t->ideal_cx);
				o[1] = (float)(t->ideal_fx * q[1] / q[2] + t->ideal_cy);
			}
		}
	}
	return true;
}

void
sim_stereo_camera_distorted_fini(struct sim_stereo_camera_distorted *d)
{
	free(d->ideal_uv);
	d->ideal_uv = NULL;
}

void
sim_stereo_camera_render_gray_distorted(const struct sim_stereo_camera_distorted *d,
                                        uint64_t frame_number,
                                        uint8_t *dst,
                                        uint32_t pitch)
{
	const uint32_t w = d->scene.eye_width, h = d->scene.eye_height;
	for (uint32_t e = 0; e < 2; e++) {
		const float *uv = d->ideal_uv + (size_t)e * 2 * w * h;
		for (uint32_t y = 0; y < h; y++) {
			uint8_t *row = dst + (size_t)y * pitch + e * w;
			const float *p = uv + 2 * (size_t)y * w;
			for (uint32_t x = 0; x < w; x++, p += 2) {
				if (p[0] < -1e8f) {
					row[x] = 0;
					continue;
				}
				float val = sim_stereo_camera_scene_sample(&d->scene, (int)e, p[0], p[1]);
				row[x] = (uint8_t)(val < 0.0f ? 0 : val > 255.0f ? 255 : (int)(val + 0.5f));
			}
		}
	}
	render_counter(dst, pitch, 0, w, h, frame_number);
	render_counter(dst, pitch, w, w, h, frame_number);
}
