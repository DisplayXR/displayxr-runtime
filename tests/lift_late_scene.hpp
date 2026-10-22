// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Synthetic scenes for the ADR-058 late-placement tests
 *         (tests_aux_lift_reproject, tests_lift_late_viewsyn_conformance).
 *
 * Colour ENCODES its own coordinates where a test needs to read positions
 * back (R = source u, G = source v from the bottom, both texel centres) and
 * carries a texture in B (a checker) so a misplaced view is a visible error.
 * @ingroup tests
 */

#pragma once

#include "util/u_conv_viewsyn.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace lift_late {

constexpr float kRectW = 0.32f, kRectH = 0.18f; // the lifted rect, metres
constexpr float kIpd = 0.063f;

//! RGBA floats, row 0 = top: R = u, G = v up, B = checker (8 px cells), A = 1.
inline std::vector<float>
make_color(uint32_t w, uint32_t h)
{
	std::vector<float> c((size_t)w * h * 4);
	for (uint32_t y = 0; y < h; y++) {
		for (uint32_t x = 0; x < w; x++) {
			float *p = &c[((size_t)y * w + x) * 4];
			p[0] = (x + 0.5f) / (float)w;
			p[1] = 1.0f - (y + 0.5f) / (float)h;
			p[2] = (((x / 8) + (y / 8)) & 1) != 0 ? 1.0f : 0.0f;
			p[3] = 1.0f;
		}
	}
	return c;
}

//! Affine disparity (larger = nearer): a near box (1.0) over a ground that
//! rises from 0 at the top to 0.6 at the bottom.
inline std::vector<float>
make_disparity(uint32_t w, uint32_t h)
{
	std::vector<float> d((size_t)w * h);
	for (uint32_t y = 0; y < h; y++) {
		for (uint32_t x = 0; x < w; x++) {
			const bool box = x >= w * 3 / 8 && x < w * 5 / 8 && y >= h / 3 && y < h * 2 / 3;
			d[(size_t)y * w + x] = box ? 1.0f : 0.6f * (y + 0.5f) / (float)h;
		}
	}
	return d;
}

//! Metric depth (metres): a box at @p box_m over a ground from @p far_m at the
//! top to @p near_ground_m at the bottom.
inline std::vector<float>
make_metric(uint32_t w, uint32_t h, float box_m, float near_ground_m, float far_m)
{
	std::vector<float> d((size_t)w * h);
	for (uint32_t y = 0; y < h; y++) {
		for (uint32_t x = 0; x < w; x++) {
			const bool box = x >= w * 3 / 8 && x < w * 5 / 8 && y >= h / 3 && y < h * 2 / 3;
			const float t = (y + 0.5f) / (float)h;
			d[(size_t)y * w + x] = box ? box_m : far_m + (near_ground_m - far_m) * t;
		}
	}
	return d;
}

//! A stereo pair around @p centre (rect-relative metres), eyes along x.
inline void
eye_pair(const float centre[3], float out[2][3])
{
	out[0][0] = centre[0] - 0.5f * kIpd;
	out[1][0] = centre[0] + 0.5f * kIpd;
	for (int i = 0; i < 2; i++) {
		out[i][1] = centre[1];
		out[i][2] = centre[2];
	}
}

//! A stereo pair around @p centre, the head turned by @p yaw_rad about +y
//! (positive = the right eye moves away from the glass).
inline void
eye_pair_yaw(const float centre[3], double yaw_rad, float out[2][3])
{
	const double hx = 0.5 * kIpd * std::cos(yaw_rad), hz = 0.5 * kIpd * std::sin(yaw_rad);
	out[0][0] = (float)(centre[0] - hx);
	out[0][2] = (float)(centre[2] - hz);
	out[1][0] = (float)(centre[0] + hx);
	out[1][2] = (float)(centre[2] + hz);
	out[0][1] = out[1][1] = centre[1];
}

//! The S5 params a composed stream would run for the affine-disparity scene.
inline u_conv_viewsyn_params
composed_params(const float eyes[2][3], float budget)
{
	u_conv_viewsyn_params p;
	u_conv_viewsyn_params_defaults(&p);
	p.view_count = 2;
	for (int i = 0; i < 2; i++) {
		for (int k = 0; k < 3; k++) {
			p.viewpoints_m[i][k] = eyes[i][k];
		}
	}
	p.rect_width_m = kRectW;
	p.rect_height_m = kRectH;
	p.depth.kind = XRT_CONV_DEPTH_AFFINE_DISPARITY;
	p.range_near = 1.0f;
	p.range_far = 0.0f;
	p.convergence = 0.5f;
	p.budget = budget;
	p.upsample = false; // the CPU reference has no upsample / snap passes
	p.snap = false;
	return p;
}

//! Mean / max absolute difference over RGB of two view-major RGBA rasters, in 1/255 units.
struct Err
{
	double mean = 0.0, max = 0.0;
	double over4 = 0.0; //!< share of texels (any of RGB) off by more than 4/255
};

inline Err
diff(const std::vector<float> &a, const std::vector<float> &b)
{
	Err e;
	double sum = 0.0;
	size_t n = 0, texels = 0, bad = 0;
	for (size_t i = 0; i + 3 < a.size(); i += 4) {
		bool off = false;
		for (size_t k = 0; k < 3; k++) {
			const double d = std::fabs((double)a[i + k] - (double)b[i + k]) * 255.0;
			sum += d;
			e.max = d > e.max ? d : e.max;
			off = off || d > 4.0;
			n++;
		}
		texels++;
		bad += off ? 1 : 0;
	}
	e.mean = n > 0 ? sum / (double)n : 0.0;
	e.over4 = texels > 0 ? (double)bad / (double)texels : 0.0;
	return e;
}

} // namespace lift_late
