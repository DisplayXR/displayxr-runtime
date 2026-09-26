// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_stereo_camera (ADR-043) R2: the vendor-neutral rectifier.
 *
 *  - GOLDEN: u_stereo_rectify_compute against OpenCV 4.10's
 *    stereoRectify(CALIB_ZERO_DISPARITY, alpha = 0) on three calibrations
 *    (the sim_display distorted fake, an SR-like tracker pair with a
 *    non-axial baseline, a RADTAN8 pair calibrated at 2x the frame size), and
 *    the per-pixel maps against initUndistortRectifyMap samples
 *    (fixtures/stereo_rectify_fixtures.h, generated offline);
 *  - the lens models invert (RADTAN5/8, KB4) and Rodrigues round-trips;
 *  - alpha = 0 really holds: every output pixel of both eyes samples inside its
 *    raw image (no black corners);
 *  - END TO END on the sim_display DISTORTED fake: the raw pair is visibly
 *    misaligned; after the CPU LUT the vertical disparity on matched blocks is
 *    <= 0.5 px and the horizontal disparity equals f_rect * B / Z within
 *    0.5 px, for the background and the bar;
 *  - the NV12 / BGRA8 plane paths agree with GRAY8;
 *  - [.perf] (hidden): CPU cost per 1280x480 frame.
 */

#include "catch_amalgamated.hpp"

#include "util/u_stereo_camera.h"
#include "util/u_stereo_rectify.h"

#include "sim_display_stereo_camera_pattern.h"
#include "stereo_rectify_fixtures.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

const stereo_rectify_fixture *const k_fixtures[] = {
    &k_fixture_sim_distorted,
    &k_fixture_sr_like,
    &k_fixture_radtan8_scaled,
};

u_stereo_rectify_input
input_from(const stereo_rectify_fixture &f)
{
	u_stereo_rectify_input in;
	std::memset(&in, 0, sizeof(in));
	in.width = (uint32_t)f.width;
	in.height = (uint32_t)f.height;
	in.calib_width = (uint32_t)f.calib_width;
	in.calib_height = (uint32_t)f.calib_height;
	for (int e = 0; e < 2; e++) {
		in.eye[e].fx = f.K[e][0];
		in.eye[e].fy = f.K[e][4];
		in.eye[e].cx = f.K[e][2];
		in.eye[e].cy = f.K[e][5];
		in.eye[e].model = (uint32_t)f.model;
		for (int k = 0; k < 8; k++) {
			in.eye[e].d[k] = f.D[e][k];
		}
	}
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			in.R[i][j] = f.R[i * 3 + j];
		}
		in.T[i] = f.T[i];
	}
	return in;
}

const double k_ideal_fx_640 = 320.0 / std::tan(34.0 * M_PI / 180.0);

} // namespace


TEST_CASE("stereo rectify: Rodrigues and lens models invert", "[stereo_rectify]")
{
	const double rv[3] = {0.012, -0.031, 0.007};
	double m[3][3], back[3];
	u_stereo_rectify_rodrigues(rv, m);
	u_stereo_rectify_rodrigues_inv(m, back);
	for (int i = 0; i < 3; i++) {
		CHECK(back[i] == Catch::Approx(rv[i]).margin(1e-14));
	}
	const double big[3] = {1.1, -0.4, 2.2};
	u_stereo_rectify_rodrigues(big, m);
	u_stereo_rectify_rodrigues_inv(m, back);
	for (int i = 0; i < 3; i++) {
		CHECK(back[i] == Catch::Approx(big[i]).margin(1e-12));
	}

	u_stereo_rectify_lens lenses[3];
	std::memset(lenses, 0, sizeof(lenses));
	const double r5[8] = {-0.21, 0.06, 0.0008, -0.0006, -0.005, 0, 0, 0};
	const double r8[8] = {0.31, -0.12, 0.0006, 0.0003, 0.02, 0.61, -0.05, 0.03};
	const double kb[8] = {0.021, -0.013, 0.004, -0.001, 0, 0, 0, 0};
	const uint32_t models[3] = {U_STEREO_RECTIFY_MODEL_RADTAN5, U_STEREO_RECTIFY_MODEL_RADTAN8,
	                            U_STEREO_RECTIFY_MODEL_KB4};
	const double *coeffs[3] = {r5, r8, kb};
	for (int l = 0; l < 3; l++) {
		lenses[l].fx = 480.0;
		lenses[l].fy = 478.0;
		lenses[l].cx = 325.0;
		lenses[l].cy = 236.0;
		lenses[l].model = models[l];
		std::memcpy(lenses[l].d, coeffs[l], sizeof(r5));
		for (double y = -0.45; y <= 0.45; y += 0.15) {
			for (double x = -0.6; x <= 0.6; x += 0.15) {
				double xd, yd, xu, yu;
				u_stereo_rectify_distort(&lenses[l], x, y, &xd, &yd);
				u_stereo_rectify_undistort_pixel(&lenses[l], lenses[l].fx * xd + lenses[l].cx,
				                                 lenses[l].fy * yd + lenses[l].cy, &xu, &yu);
				CHECK(xu == Catch::Approx(x).margin(1e-9));
				CHECK(yu == Catch::Approx(y).margin(1e-9));
			}
		}
	}
}

TEST_CASE("stereo rectify: GOLDEN geometry vs OpenCV stereoRectify", "[stereo_rectify]")
{
	for (const stereo_rectify_fixture *fx : k_fixtures) {
		const stereo_rectify_fixture &f = *fx;
		INFO("fixture " << f.name);
		u_stereo_rectify_input in = input_from(f);
		u_stereo_rectify_result r;
		REQUIRE(u_stereo_rectify_compute(&in, &r));

		// R1 / R2: identical algorithm, so identical to double precision.
		for (int i = 0; i < 3; i++) {
			for (int j = 0; j < 3; j++) {
				CHECK(r.rot[0][i][j] == Catch::Approx(f.R1[i * 3 + j]).margin(1e-12));
				CHECK(r.rot[1][i][j] == Catch::Approx(f.R2[i * 3 + j]).margin(1e-12));
			}
		}
		// Principal point: OpenCV's corner-centroid rule, one cx for both eyes
		// (zero disparity at infinity). cvStereoRectify runs undistortPoints for
		// only 5 iterations; we converge, so we match the CONVERGED reference
		// exactly and OpenCV's own P1 to a fraction of a pixel.
		CHECK(r.cx == Catch::Approx(f.converged[1]).margin(1e-4));
		CHECK(r.cy == Catch::Approx(f.converged[2]).margin(1e-4));
		CHECK(r.cx == Catch::Approx(f.P1[2]).margin(0.75));
		CHECK(r.cy == Catch::Approx(f.P1[6]).margin(0.75));
		CHECK(f.P2[2] == Catch::Approx(f.P1[2]).margin(1e-9));
		// Focal before the crop zoom: OpenCV's min-fy rule, exactly.
		CHECK(r.f / r.crop_scale == Catch::Approx(f.converged[0]).epsilon(1e-9));
		// After the alpha = 0 zoom: OpenCV's 9x9 inner rectangle, plus our
		// border verification, which may only zoom further (OpenCV itself leaves
		// a few black pixels on strong lenses). Within 1 % of OpenCV.
		double f_cv = f.P1[0];
		CHECK(r.f == Catch::Approx(f_cv).epsilon(0.01));
		std::printf(
		    "[golden] %-15s f %.4f (OpenCV %.4f, %+.3f%%)  cx %.4f (OpenCV %.4f)  cy %.4f (OpenCV %.4f)  crop "
		    "x%.4f  baseline %.3f\n",
		    f.name, r.f, f_cv, 100.0 * (r.f / f_cv - 1.0), r.cx, f.P1[2], r.cy, f.P1[6], r.crop_scale,
		    r.baseline);
		// Rectified translation: OpenCV P2[0][3] = f * Tx.
		CHECK(r.P[1][0][3] / r.f == Catch::Approx(f.P2[3] / f.P2[0]).margin(1e-9));
		CHECK(std::fabs(r.t_rect[1]) < 1e-7 * r.baseline); // baseline on the x axis
		CHECK(std::fabs(r.t_rect[2]) < 1e-7 * r.baseline);
		CHECK(std::fabs(r.t_rect[0]) == Catch::Approx(r.baseline).epsilon(1e-12));
	}
}

TEST_CASE("stereo rectify: GOLDEN maps vs OpenCV initUndistortRectifyMap", "[stereo_rectify]")
{
	for (const stereo_rectify_fixture *fx : k_fixtures) {
		const stereo_rectify_fixture &f = *fx;
		INFO("fixture " << f.name);
		u_stereo_rectify_input in = input_from(f);
		u_stereo_rectify_result r;
		REQUIRE(u_stereo_rectify_compute(&in, &r));
		// Feed OpenCV's own R/P so the map math is compared in isolation.
		for (int i = 0; i < 3; i++) {
			for (int j = 0; j < 3; j++) {
				r.rot[0][i][j] = f.R1[i * 3 + j];
				r.rot[1][i][j] = f.R2[i * 3 + j];
			}
		}
		r.f = f.P1[0];
		r.cx = f.P1[2];
		r.cy = f.P1[6];
		double worst = 0.0;
		for (int s = 0; s < STEREO_RECTIFY_FIXTURE_SAMPLES; s++) {
			const double *smp = &f.samples[s * 5];
			double x, y;
			REQUIRE(u_stereo_rectify_map_point(&r, (uint32_t)smp[0], smp[1], smp[2], &x, &y));
			worst = std::max(worst, std::max(std::fabs(x - smp[3]), std::fabs(y - smp[4])));
		}
		// OpenCV stores float32 maps: agreement to its precision.
		CHECK(worst < 2e-3);
		std::printf("[golden] %-15s map max |ours - OpenCV| = %.2e px over %d samples\n", f.name, worst,
		            STEREO_RECTIFY_FIXTURE_SAMPLES);
	}
}

TEST_CASE("stereo rectify: alpha = 0 leaves no black pixel", "[stereo_rectify]")
{
	for (const stereo_rectify_fixture *fx : k_fixtures) {
		INFO("fixture " << fx->name);
		u_stereo_rectify_input in = input_from(*fx);
		u_stereo_rectify_result r;
		REQUIRE(u_stereo_rectify_compute(&in, &r));
		for (uint32_t sub = 1; sub <= 2; sub++) {
			u_stereo_rectify_lut lut;
			REQUIRE(u_stereo_rectify_lut_init(&lut, &r, sub));
			CHECK(lut.width == 2 * r.width / sub);
			CHECK(lut.valid == lut.width * lut.height);
			u_stereo_rectify_lut_fini(&lut);
		}
	}
}

TEST_CASE("stereo rectify: the C fake's truth equals the fixture's inputs", "[stereo_rectify]")
{
	sim_stereo_camera_truth t;
	sim_stereo_camera_truth_init(&t, 640, 480, k_ideal_fx_640, 50.0, 2.0, 0.6);
	const stereo_rectify_fixture &f = k_fixture_sim_distorted;
	for (int e = 0; e < 2; e++) {
		CHECK(t.fx[e] == Catch::Approx(f.K[e][0]).margin(1e-9));
		CHECK(t.fy[e] == Catch::Approx(f.K[e][4]).margin(1e-9));
		CHECK(t.cx[e] == Catch::Approx(f.K[e][2]).margin(1e-9));
		CHECK(t.cy[e] == Catch::Approx(f.K[e][5]).margin(1e-9));
		for (int k = 0; k < 5; k++) {
			CHECK(t.dist[e][k] == Catch::Approx(f.D[e][k]).margin(1e-15));
		}
	}
	for (int i = 0; i < 9; i++) {
		CHECK(t.R[i / 3][i % 3] == Catch::Approx(f.R[i]).margin(1e-12));
	}
	for (int i = 0; i < 3; i++) {
		CHECK(t.T_mm[i] == Catch::Approx(f.T[i]).margin(1e-9));
	}
}


/*
 *
 * End to end on the distorted fake.
 *
 */

namespace {

struct block_stats
{
	std::vector<float> dy_bg, dy_bar, dx_bg, dx_bar;
};

float
median(std::vector<float> v)
{
	if (v.empty()) {
		return NAN;
	}
	std::sort(v.begin(), v.end());
	return v[v.size() / 2];
}

float
max_abs_dev(const std::vector<float> &v, float ref)
{
	float m = 0.0f;
	for (float x : v) {
		m = std::max(m, std::fabs(x - ref));
	}
	return m;
}

/*!
 * Block-match a GRAY8 SBS pair on a grid. @p to_ideal maps a LEFT-eye pixel of
 * @p img to the virtual parallel frame the scene is defined in, so each block
 * is classified as background or bar (and skipped when it straddles an edge,
 * the occlusion band next to the bar, or the frame counter).
 */
template <typename ToIdeal>
block_stats
match_blocks(
    const uint8_t *img, uint32_t pitch, uint32_t eye_w, uint32_t h, const sim_stereo_camera_scene &sc, ToIdeal to_ideal)
{
	block_stats st;
	const uint32_t B = 32, max_dy = 10;
	for (uint32_t y = 48; y + B + max_dy <= h; y += B / 2) {
		for (uint32_t x = 72; x + B <= eye_w; x += B / 2) {
			double u0, v0, u1, v1;
			to_ideal(x, y, &u0, &v0);
			to_ideal(x + B, y + B, &u1, &v1);
			const double m = 4.0;
			bool rows_in = v0 >= sc.bar_y0 + m && v1 <= sc.bar_y1 - m;
			bool rows_out = v1 < sc.bar_y0 - m || v0 > sc.bar_y1 + m;
			bool cols_in = u0 >= sc.bar_x0 + m && u1 <= sc.bar_x1 - m;
			// Background blocks must also keep clear of where the bar occludes the
			// right eye's view of them (left x in [x0 - d_bar, x1)).
			bool cols_out = u1 < sc.bar_x0 - sc.bar_disparity_f - m || u0 > sc.bar_x1 + m;
			bool is_bar = rows_in && cols_in;
			bool is_bg = rows_out || cols_out;
			if (!is_bar && !is_bg) {
				continue;
			}
			float dx, dy;
			if (!u_stereo_camera_estimate_offset(img, pitch, eye_w, h, x, y, B, B, 64, max_dy, &dx, &dy)) {
				continue;
			}
			(is_bar ? st.dx_bar : st.dx_bg).push_back(dx);
			(is_bar ? st.dy_bar : st.dy_bg).push_back(dy);
		}
	}
	return st;
}

} // namespace

TEST_CASE("stereo rectify: END TO END on the distorted sim fake", "[stereo_rectify]")
{
	const uint32_t W = 640, H = 480;
	sim_stereo_camera_distorted d;
	REQUIRE(sim_stereo_camera_distorted_init(&d, W, H, k_ideal_fx_640, 50.0, 2.0, 0.6));
	std::vector<uint8_t> raw(2 * W * H), rect(2 * W * H);
	sim_stereo_camera_render_gray_distorted(&d, 1234, raw.data(), 2 * W);

	// The RAW pair really is misaligned (the thing the rectifier must fix):
	// measured in raw pixels, the rows of the two eyes differ by several px.
	{
		block_stats st =
		    match_blocks(raw.data(), 2 * W, W, H, d.scene, [](uint32_t x, uint32_t y, double *u, double *v) {
			    *u = x; // roughly: classification only needs to be conservative
			    *v = y;
		    });
		std::vector<float> all = st.dy_bg;
		all.insert(all.end(), st.dy_bar.begin(), st.dy_bar.end());
		REQUIRE(all.size() > 20);
		std::vector<float> absdy;
		for (float v : all) {
			absdy.push_back(std::fabs(v));
		}
		float med = median(absdy);
		float mx = *std::max_element(absdy.begin(), absdy.end());
		std::printf("[e2e] RAW pair: |dy| median %.2f px, max %.2f px over %zu blocks\n", med, mx, all.size());
		CHECK(med > 2.0f);
	}

	// Rectify with the calibration the fake's plug-in slot reports.
	u_stereo_rectify_input in;
	std::memset(&in, 0, sizeof(in));
	in.width = W;
	in.height = H;
	for (int e = 0; e < 2; e++) {
		in.eye[e].fx = d.truth.fx[e];
		in.eye[e].fy = d.truth.fy[e];
		in.eye[e].cx = d.truth.cx[e];
		in.eye[e].cy = d.truth.cy[e];
		in.eye[e].model = U_STEREO_RECTIFY_MODEL_RADTAN5;
		std::memcpy(in.eye[e].d, d.truth.dist[e], sizeof(d.truth.dist[e]));
	}
	std::memcpy(in.R, d.truth.R, sizeof(in.R));
	std::memcpy(in.T, d.truth.T_mm, sizeof(in.T));
	u_stereo_rectify_result r;
	REQUIRE(u_stereo_rectify_compute(&in, &r));
	u_stereo_rectify_lut lut;
	REQUIRE(u_stereo_rectify_lut_init(&lut, &r, 1));
	u_stereo_rectify_lut_apply(&lut, 1, raw.data(), 2 * W, rect.data(), 2 * W);

	// The symmetric construction makes the rectified frame the virtual one:
	// rectified (u, v) -> virtual pixel by the focal / principal point only.
	const double ifx = d.truth.ideal_fx, icx = d.truth.ideal_cx, icy = d.truth.ideal_cy;
	auto to_ideal = [&](uint32_t x, uint32_t y, double *u, double *v) {
		*u = ifx * (x - r.cx) / r.f + icx;
		*v = ifx * (y - r.cy) / r.f + icy;
	};
	block_stats st = match_blocks(rect.data(), 2 * W, W, H, d.scene, to_ideal);
	REQUIRE(st.dx_bg.size() > 20);
	REQUIRE(st.dx_bar.size() > 5);

	const double baseline_m = 0.05;
	const float gt_bg = (float)(r.f * baseline_m / 2.0);
	const float gt_bar = (float)(r.f * baseline_m / 0.6);
	std::vector<float> dy_all = st.dy_bg;
	dy_all.insert(dy_all.end(), st.dy_bar.begin(), st.dy_bar.end());
	float dy_max = max_abs_dev(dy_all, 0.0f);
	float bg_med = median(st.dx_bg), bar_med = median(st.dx_bar);
	float bg_dev = max_abs_dev(st.dx_bg, gt_bg), bar_dev = max_abs_dev(st.dx_bar, gt_bar);
	std::printf(
	    "[e2e] RECTIFIED (f %.3f px, crop x%.4f): |dy| max %.3f px over %zu blocks (median %.3f)\n"
	    "[e2e]   background: GT f*B/Z = %.3f px, measured median %.3f, max |err| %.3f px (%zu blocks)\n"
	    "[e2e]   bar:        GT f*B/Z = %.3f px, measured median %.3f, max |err| %.3f px (%zu blocks)\n",
	    r.f, r.crop_scale, dy_max, dy_all.size(), median(dy_all), gt_bg, bg_med, bg_dev, st.dx_bg.size(), gt_bar,
	    bar_med, bar_dev, st.dx_bar.size());
	CHECK(dy_max <= 0.5f);
	CHECK(bg_dev <= 0.5f);
	CHECK(bar_dev <= 0.5f);

	u_stereo_rectify_lut_fini(&lut);
	sim_stereo_camera_distorted_fini(&d);
}

TEST_CASE("stereo rectify: NV12 and BGRA8 planes agree with GRAY8", "[stereo_rectify]")
{
	const uint32_t W = 640, H = 480, SW = 2 * W;
	sim_stereo_camera_distorted d;
	REQUIRE(sim_stereo_camera_distorted_init(&d, W, H, k_ideal_fx_640, 50.0, 2.0, 0.6));
	std::vector<uint8_t> gray(SW * H), gray_r(SW * H);
	sim_stereo_camera_render_gray_distorted(&d, 7, gray.data(), SW);

	u_stereo_rectify_input in = input_from(k_fixture_sim_distorted);
	u_stereo_rectify_result r;
	REQUIRE(u_stereo_rectify_compute(&in, &r));
	u_stereo_rectify_lut full, half;
	REQUIRE(u_stereo_rectify_lut_init(&full, &r, 1));
	REQUIRE(u_stereo_rectify_lut_init(&half, &r, 2));
	u_stereo_rectify_lut_apply(&full, 1, gray.data(), SW, gray_r.data(), SW);

	// BGRA with B = G = R = gray: every colour channel equals the GRAY8 result.
	std::vector<uint8_t> bgra(SW * H * 4), bgra_r(SW * H * 4);
	for (size_t i = 0; i < gray.size(); i++) {
		bgra[4 * i] = bgra[4 * i + 1] = bgra[4 * i + 2] = gray[i];
		bgra[4 * i + 3] = 255;
	}
	u_stereo_rectify_lut_apply(&full, 4, bgra.data(), SW * 4, bgra_r.data(), SW * 4);
	size_t mism = 0;
	for (size_t i = 0; i < gray.size(); i++) {
		mism += bgra_r[4 * i] != gray_r[i] || bgra_r[4 * i + 2] != gray_r[i] || bgra_r[4 * i + 3] != 255;
	}
	CHECK(mism == 0);

	// NV12 chroma: a constant UV plane stays constant (no black pull-in).
	std::vector<uint8_t> uv(SW * H / 2, 0), uv_r(SW * H / 2, 0);
	for (size_t i = 0; i < uv.size(); i += 2) {
		uv[i] = 90;
		uv[i + 1] = 170;
	}
	REQUIRE(half.width == W);
	u_stereo_rectify_lut_apply(&half, 2, uv.data(), SW, uv_r.data(), SW);
	size_t off = 0;
	for (size_t i = 0; i < uv_r.size(); i += 2) {
		off += uv_r[i] != 90 || uv_r[i + 1] != 170;
	}
	CHECK(off == 0);

	u_stereo_rectify_lut_fini(&full);
	u_stereo_rectify_lut_fini(&half);
	sim_stereo_camera_distorted_fini(&d);
}

TEST_CASE("stereo rectify: CPU cost per 1280x480 frame", "[.perf][stereo_rectify]")
{
	const uint32_t W = 640, H = 480, SW = 2 * W;
	u_stereo_rectify_input in = input_from(k_fixture_sim_distorted);
	u_stereo_rectify_result r;
	auto t0 = std::chrono::steady_clock::now();
	REQUIRE(u_stereo_rectify_compute(&in, &r));
	u_stereo_rectify_lut full, half;
	REQUIRE(u_stereo_rectify_lut_init(&full, &r, 1));
	REQUIRE(u_stereo_rectify_lut_init(&half, &r, 2));
	double setup_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

	std::vector<uint8_t> src(SW * H * 4, 77), dst(SW * H * 4);
	for (size_t i = 0; i < src.size(); i++) {
		src[i] = (uint8_t)(i * 2654435761u >> 24);
	}
	const int N = 200;
	auto run = [&](const u_stereo_rectify_lut &lut, uint32_t ch) {
		auto a = std::chrono::steady_clock::now();
		for (int i = 0; i < N; i++) {
			u_stereo_rectify_lut_apply(&lut, ch, src.data(), lut.width * ch, dst.data(), lut.width * ch);
		}
		return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count() / N;
	};
	double g = run(full, 1);
	double y = g;
	double uvp = run(half, 2);
	double b = run(full, 4);
	std::printf(
	    "[perf] setup (geometry + both LUTs) %.1f ms | per 1280x480 frame: GRAY8 %.3f ms, NV12 %.3f ms "
	    "(Y %.3f + UV %.3f), BGRA8 %.3f ms\n",
	    setup_ms, g, y + uvp, y, uvp, b);
	u_stereo_rectify_lut_fini(&full);
	u_stereo_rectify_lut_fini(&half);
}
