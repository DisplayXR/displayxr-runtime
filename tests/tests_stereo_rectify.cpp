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

// Not M_PI: MSVC only defines it with _USE_MATH_DEFINES before the first <cmath>,
// which Catch may already have included.
constexpr double k_pi = 3.14159265358979323846;
const double k_ideal_fx_640 = 320.0 / std::tan(34.0 * k_pi / 180.0);

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
match_blocks(const uint8_t *img,
             uint32_t pitch,
             uint32_t eye_w,
             uint32_t h,
             const sim_stereo_camera_scene &sc,
             ToIdeal to_ideal,
             uint32_t max_d = 64,
             bool ncc = false)
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
			if (ncc) {
				// The probe's matcher: flagged / weak blocks are excluded, never clamped.
				u_stereo_camera_block_match bm;
				if (!u_stereo_camera_match_block(img, pitch, eye_w, h, x, y, B, B, max_d, max_dy,
				                                 &bm) ||
				    bm.dx_at_edge || bm.dy_at_edge || bm.ncc < 0.9f) {
					continue;
				}
				dx = bm.dx;
				dy = bm.dy;
			} else if (!u_stereo_camera_estimate_offset(img, pitch, eye_w, h, x, y, B, B, max_d, max_dy,
			                                            &dx, &dy)) {
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

namespace {

//! A parallel pair of the scene, the right eye shifted DOWN by @p dy_px rows
//! (a feature at left row v is at right row v + dy) and given a gain/offset.
std::vector<uint8_t>
render_parallel(const sim_stereo_camera_scene &sc, double dy_px, double gain = 1.0, double offset = 0.0)
{
	const uint32_t W = sc.eye_width, H = sc.eye_height;
	std::vector<uint8_t> img(2 * W * H);
	for (uint32_t y = 0; y < H; y++) {
		for (uint32_t x = 0; x < W; x++) {
			img[y * 2 * W + x] = (uint8_t)std::lround(sim_stereo_camera_scene_sample(&sc, 0, x, y));
			double v = gain * sim_stereo_camera_scene_sample(&sc, 1, x, y - dy_px) + offset;
			img[y * 2 * W + W + x] = (uint8_t)std::lround(std::min(255.0, std::max(0.0, v)));
		}
	}
	return img;
}

} // namespace

TEST_CASE("probe matcher: Leia-SR-sized disparities, SIGNED dy, bound hits flagged", "[stereo_rectify][probe]")
{
	// 120 mm at f 474 px: the bar at 0.6 m is ~95 px (the field case: a face at
	// 0.6 m on the SR tracking camera, which the old fixed 0..64 search clamped).
	const uint32_t W = 640, H = 480;
	sim_stereo_camera_scene sc;
	sim_stereo_camera_scene_init(&sc, W, H, k_ideal_fx_640, 120.0, 2.0, 0.6);
	REQUIRE(sc.bar_disparity_f > 90.0);
	const double dy0 = 1.68; // the field's signed residual
	std::vector<uint8_t> img = render_parallel(sc, dy0);
	const uint32_t max_d = (uint32_t)std::ceil(k_ideal_fx_640 * 0.120 / 0.4); // what the probe computes
	const uint32_t bx = 280, by = 200;                                        // inside the bar

	u_stereo_camera_block_match m;
	REQUIRE(u_stereo_camera_match_block(img.data(), 2 * W, W, H, bx, by, 32, 32, max_d, 24, &m));
	std::printf("[probe] bar block: dx %.3f (GT %.3f), dy %+.3f (GT %+.3f), ncc %.4f, window d %d..%d dy %d..%d\n",
	            m.dx, sc.bar_disparity_f, m.dy, dy0, m.ncc, m.d_lo, m.d_hi, m.dy_lo, m.dy_hi);
	CHECK_FALSE(m.dx_at_edge);
	CHECK_FALSE(m.dy_at_edge);
	CHECK(m.ncc > 0.93f);
	CHECK(m.dx == Catch::Approx(sc.bar_disparity_f).margin(0.25));
	CHECK(m.dy == Catch::Approx(dy0).margin(0.15)); // SIGNED, sub-pixel

	// Background block (28.5 px), same signed dy.
	REQUIRE(u_stereo_camera_match_block(img.data(), 2 * W, W, H, 520, 40, 32, 32, max_d, 24, &m));
	CHECK(m.dx == Catch::Approx(sc.bg_disparity_f).margin(0.25));
	CHECK(m.dy == Catch::Approx(dy0).margin(0.15));

	// The field failure, reproduced: with the old 64 px window the true match is
	// out of reach. The block must then be REJECTED — a peak on the bound, or a
	// correlation under the probe's 0.90 — never reported as a disparity.
	REQUIRE(u_stereo_camera_match_block(img.data(), 2 * W, W, H, bx, by, 32, 32, 64, 24, &m));
	std::printf("[probe] same block, 0..64 window: dx %.2f, ncc %.3f, at_edge %d\n", m.dx, m.ncc,
	            (int)m.dx_at_edge);
	CHECK((m.dx_at_edge || m.ncc < 0.90f));
	// Likewise a dy window narrower than the true offset.
	std::vector<uint8_t> img3 = render_parallel(sc, 3.0);
	REQUIRE(u_stereo_camera_match_block(img3.data(), 2 * W, W, H, bx, by, 32, 32, max_d, 1, &m));
	CHECK(m.dy_at_edge);
	REQUIRE(u_stereo_camera_match_block(img3.data(), 2 * W, W, H, bx, by, 32, 32, max_d, 24, &m));
	CHECK_FALSE(m.dy_at_edge);
	CHECK(m.dy == Catch::Approx(3.0).margin(0.15));

	// NCC is blind to a gain/offset mismatch between the two sensors.
	std::vector<uint8_t> img_g = render_parallel(sc, dy0, 0.75, 30.0);
	REQUIRE(u_stereo_camera_match_block(img_g.data(), 2 * W, W, H, bx, by, 32, 32, max_d, 24, &m));
	CHECK(m.ncc > 0.93f);
	CHECK(m.dx == Catch::Approx(sc.bar_disparity_f).margin(0.25));
	CHECK(m.dy == Catch::Approx(dy0).margin(0.15));

	// A flat block is refused, not matched anywhere.
	std::vector<uint8_t> flat(2 * W * H, 128);
	CHECK_FALSE(u_stereo_camera_match_block(flat.data(), 2 * W, W, H, bx, by, 32, 32, max_d, 24, &m));
}

TEST_CASE("stereo rectify: END TO END at the Leia SR baseline (120 mm)", "[stereo_rectify][probe]")
{
	const uint32_t W = 640, H = 480;
	const double B_mm = 120.0;
	sim_stereo_camera_distorted d;
	REQUIRE(sim_stereo_camera_distorted_init(&d, W, H, k_ideal_fx_640, B_mm, 2.0, 0.6));
	std::vector<uint8_t> raw(2 * W * H), rect(2 * W * H);
	sim_stereo_camera_render_gray_distorted(&d, 77, raw.data(), 2 * W);

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

	const double ifx = d.truth.ideal_fx, icx = d.truth.ideal_cx, icy = d.truth.ideal_cy;
	auto to_ideal = [&](uint32_t x, uint32_t y, double *u, double *v) {
		*u = ifx * (x - r.cx) / r.f + icx;
		*v = ifx * (y - r.cy) / r.f + icy;
	};
	const uint32_t max_d = (uint32_t)std::ceil(r.f * (B_mm / 1000.0) / 0.4);
	block_stats st = match_blocks(rect.data(), 2 * W, W, H, d.scene, to_ideal, max_d, true);
	REQUIRE(st.dx_bg.size() > 20);
	REQUIRE(st.dx_bar.size() > 5);
	const float gt_bg = (float)(r.f * (B_mm / 1000.0) / 2.0);
	const float gt_bar = (float)(r.f * (B_mm / 1000.0) / 0.6);
	std::vector<float> dy_all = st.dy_bg;
	dy_all.insert(dy_all.end(), st.dy_bar.begin(), st.dy_bar.end());
	std::printf(
	    "[e2e 120mm] f %.3f: bar GT %.3f px measured median %.3f (%zu blocks); bg GT %.3f median %.3f; "
	    "signed dy median %+.4f px, |dy| max %.3f\n",
	    r.f, gt_bar, median(st.dx_bar), st.dx_bar.size(), gt_bg, median(st.dx_bg), median(dy_all),
	    max_abs_dev(dy_all, 0.0f));
	CHECK(gt_bar > 64.0f); // the case the old fixed window could not measure
	CHECK(max_abs_dev(st.dx_bar, gt_bar) <= 0.5f);
	CHECK(max_abs_dev(st.dx_bg, gt_bg) <= 0.5f);
	CHECK(max_abs_dev(dy_all, 0.0f) <= 0.5f);
	CHECK(std::fabs(median(dy_all)) <= 0.1f); // no constant vertical offset
	u_stereo_rectify_lut_fini(&lut);
	sim_stereo_camera_distorted_fini(&d);
}

/*
 * CONSTANT VERTICAL OFFSET hypotheses (field: +1.7 px signed dy after
 * rectification on the Leia SR laptop). Each case feeds the rectifier a
 * calibration that differs from the truth in ONE specific way and measures the
 * signed dy it leaves, so the field residual can be matched to a cause once the
 * raw frames + calibration dumps arrive. The matched pair is always the
 * distorted fake at the SR-like 120 mm baseline.
 */
namespace {

struct vshift_rig
{
	sim_stereo_camera_distorted d;
	std::vector<uint8_t> raw;
	u_stereo_rectify_input truth_in;
	static constexpr uint32_t W = 640, H = 480;

	vshift_rig()
	{
		REQUIRE(sim_stereo_camera_distorted_init(&d, W, H, k_ideal_fx_640, 120.0, 2.0, 0.6));
		raw.resize(2 * W * H);
		sim_stereo_camera_render_gray_distorted(&d, 5, raw.data(), 2 * W);
		std::memset(&truth_in, 0, sizeof(truth_in));
		truth_in.width = W;
		truth_in.height = H;
		for (int e = 0; e < 2; e++) {
			truth_in.eye[e].fx = d.truth.fx[e];
			truth_in.eye[e].fy = d.truth.fy[e];
			truth_in.eye[e].cx = d.truth.cx[e];
			truth_in.eye[e].cy = d.truth.cy[e];
			truth_in.eye[e].model = U_STEREO_RECTIFY_MODEL_RADTAN5;
			std::memcpy(truth_in.eye[e].d, d.truth.dist[e], sizeof(d.truth.dist[e]));
		}
		std::memcpy(truth_in.R, d.truth.R, sizeof(truth_in.R));
		std::memcpy(truth_in.T, d.truth.T_mm, sizeof(truth_in.T));
	}
	~vshift_rig()
	{
		sim_stereo_camera_distorted_fini(&d);
	}

	//! Rectify @p img with @p in; signed dy median + spread over NCC-accepted blocks.
	void
	measure(const u_stereo_rectify_input &in,
	        const std::vector<uint8_t> &img,
	        float *dy_median,
	        float *dy_spread,
	        float *dx_median,
	        double *f_rect = nullptr)
	{
		u_stereo_rectify_result r;
		REQUIRE(u_stereo_rectify_compute(&in, &r));
		u_stereo_rectify_lut lut;
		REQUIRE(u_stereo_rectify_lut_init(&lut, &r, 1));
		std::vector<uint8_t> rect(2 * W * H);
		u_stereo_rectify_lut_apply(&lut, 1, img.data(), 2 * W, rect.data(), 2 * W);
		u_stereo_rectify_lut_fini(&lut);
		std::vector<float> dys, dxs;
		const uint32_t max_d = (uint32_t)std::ceil(r.f * 0.120 / 0.4);
		for (uint32_t y = 40; y + 72 <= H; y += 32) {
			for (uint32_t x = 40; x + 40 <= W; x += 32) {
				u_stereo_camera_block_match m;
				if (u_stereo_camera_match_block(rect.data(), 2 * W, W, H, x, y, 32, 32, max_d, 24,
				                                &m) &&
				    !m.dx_at_edge && !m.dy_at_edge && m.ncc >= 0.9f) {
					dys.push_back(m.dy);
					dxs.push_back(m.dx);
				}
			}
		}
		REQUIRE(dys.size() > 30);
		*dy_median = median(dys);
		std::vector<float> dev;
		for (float v : dys) {
			dev.push_back(std::fabs(v - *dy_median));
		}
		std::sort(dev.begin(), dev.end());
		*dy_spread = dev[(dev.size() * 9) / 10]; // p90 |dy - median|
		*dx_median = median(dxs);
		if (f_rect != nullptr) {
			*f_rect = r.f;
		}
	}
};

} // namespace

TEST_CASE("vertical offset H0: the true calibration leaves no constant dy", "[stereo_rectify][vshift]")
{
	vshift_rig rig;
	float med, spread, dx;
	rig.measure(rig.truth_in, rig.raw, &med, &spread, &dx);
	std::printf("[vshift] H0 truth: signed dy median %+.3f px, p90 spread %.3f\n", med, spread);
	CHECK(std::fabs(med) <= 0.1f);
}

TEST_CASE("vertical offset H1: a per-eye cy error is a CONSTANT dy of (f_rect/fy) * dcy", "[stereo_rectify][vshift]")
{
	// Candidate field causes that all reduce to "one eye's cy is off": a flipped
	// (bottom-up) row convention (dcy = H-1-2cy per eye), a crop instead of a
	// scale between calibration and frame size, a +-0.5 px centre convention
	// applied to one eye only, an integer-rounded cy on one eye.
	vshift_rig rig;
	for (double dcy : {0.5, 1.0, -1.5}) {
		u_stereo_rectify_input in = rig.truth_in;
		in.eye[1].cy += dcy; // the calibration says the RIGHT principal point is dcy lower
		float med, spread, dx;
		double f;
		rig.measure(in, rig.raw, &med, &spread, &dx, &f);
		double pred = -f / rig.d.truth.fy[1] * dcy; // first order; barrel magnification adds a few %
		std::printf(
		    "[vshift] H1 right cy %+.1f px: signed dy median %+.3f px (predicted %+.3f), p90 spread %.3f\n",
		    dcy, med, pred, spread);
		CHECK(med == Catch::Approx(pred).epsilon(0.1).margin(0.05));
		CHECK(spread <= 0.35f); // constant across the frame: an offset, not a rotation
	}
}

TEST_CASE("vertical offset H2: R given in the wrong direction (R^T) is a large, roll-shaped dy",
          "[stereo_rectify][vshift]")
{
	// If the vendor's R were x_left = R x_right (camera-to-camera the other way)
	// the rectifier would double the relative rotation instead of cancelling it.
	// Signature: a dy offset ~ f * 2 * relative pitch PLUS a roll gradient (large
	// spread) — so a pure constant +1.7 px does NOT look like this.
	vshift_rig rig;
	u_stereo_rectify_input in = rig.truth_in;
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			in.R[i][j] = rig.truth_in.R[j][i];
		}
	}
	float med, spread, dx;
	rig.measure(in, rig.raw, &med, &spread, &dx);
	std::printf("[vshift] H2 R^T: signed dy median %+.3f px, p90 spread %.3f\n", med, spread);
	CHECK((std::fabs(med) > 2.0f || spread > 1.0f));
}

TEST_CASE("vertical offset H3: calibration size vs frame size (pixel-centre rescale)", "[stereo_rectify][vshift]")
{
	// Intrinsics expressed at 2x the frame (u2 = 2u + 0.5) must rectify
	// identically; the WRONG convention (u2 = 2u) shifts both eyes' cy by 0.25 px
	// at frame scale — equal in both eyes, so it must NOT create relative dy.
	vshift_rig rig;
	u_stereo_rectify_input in = rig.truth_in;
	in.calib_width = 2 * rig.W;
	in.calib_height = 2 * rig.H;
	for (int e = 0; e < 2; e++) {
		in.eye[e].fx *= 2.0;
		in.eye[e].fy *= 2.0;
		in.eye[e].cx = (in.eye[e].cx + 0.5) * 2.0 - 0.5;
		in.eye[e].cy = (in.eye[e].cy + 0.5) * 2.0 - 0.5;
	}
	float med, spread, dx, med0, spread0, dx0;
	rig.measure(rig.truth_in, rig.raw, &med0, &spread0, &dx0);
	rig.measure(in, rig.raw, &med, &spread, &dx);
	CHECK(med == Catch::Approx(med0).margin(0.02));
	CHECK(dx == Catch::Approx(dx0).margin(0.02));
	for (int e = 0; e < 2; e++) {
		in.eye[e].cx = rig.truth_in.eye[e].cx * 2.0; // wrong (corner) convention
		in.eye[e].cy = rig.truth_in.eye[e].cy * 2.0;
	}
	rig.measure(in, rig.raw, &med, &spread, &dx);
	std::printf("[vshift] H3 corner-convention rescale: signed dy median %+.3f px (truth %+.3f)\n", med, med0);
	CHECK(std::fabs(med - med0) <= 0.1f); // common-mode: no relative dy
}

TEST_CASE("vertical offset H4: the plug-in's half swap + (R^T, -R^T T) is exact", "[stereo_rectify][vshift]")
{
	// L1 (Leia) swaps the SBS halves and re-expresses the calibration from camera
	// 2's side: K/D swapped, R' = R^T, T' = -R^T T. Model exactly that here.
	vshift_rig rig;
	const uint32_t W = rig.W, H = rig.H;
	std::vector<uint8_t> swapped(2 * W * H);
	for (uint32_t y = 0; y < H; y++) {
		std::memcpy(&swapped[y * 2 * W], &rig.raw[y * 2 * W + W], W);
		std::memcpy(&swapped[y * 2 * W + W], &rig.raw[y * 2 * W], W);
	}
	// The vendor's own calibration of the swapped pair (camera 1 = SBS RIGHT):
	// x_2 = R_v x_1 + T_v with camera 1 the truth's RIGHT eye.
	u_stereo_rectify_input v = rig.truth_in;
	v.eye[0] = rig.truth_in.eye[1];
	v.eye[1] = rig.truth_in.eye[0];
	double Rv[3][3], Tv[3];
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			Rv[i][j] = rig.truth_in.R[j][i];
		}
	}
	for (int i = 0; i < 3; i++) {
		Tv[i] = -(Rv[i][0] * rig.truth_in.T[0] + Rv[i][1] * rig.truth_in.T[1] + Rv[i][2] * rig.truth_in.T[2]);
	}
	REQUIRE(Tv[0] > 0.0); // what makes L1 decide to swap
	// L1's leia_scam_calibration_for_runtime(swap = true):
	u_stereo_rectify_input in = v;
	in.eye[0] = v.eye[1];
	in.eye[1] = v.eye[0];
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			in.R[i][j] = Rv[j][i];
		}
	}
	for (int i = 0; i < 3; i++) {
		in.T[i] = -(in.R[i][0] * Tv[0] + in.R[i][1] * Tv[1] + in.R[i][2] * Tv[2]);
	}
	// ...applied to the halves swapped BACK, which is what L1 delivers.
	std::vector<uint8_t> back(2 * W * H);
	for (uint32_t y = 0; y < H; y++) {
		std::memcpy(&back[y * 2 * W], &swapped[y * 2 * W + W], W);
		std::memcpy(&back[y * 2 * W + W], &swapped[y * 2 * W], W);
	}
	float med, spread, dx;
	rig.measure(in, back, &med, &spread, &dx);
	std::printf("[vshift] H4 swap round trip: signed dy median %+.3f px, dx median %.2f\n", med, dx);
	CHECK(std::fabs(med) <= 0.1f);
	CHECK(dx > 20.0f); // positive disparities
	// Swapping the intrinsics but NOT the distortion (a plausible slip) is visible:
	u_stereo_rectify_input bad = in;
	std::memcpy(bad.eye[0].d, in.eye[1].d, sizeof(bad.eye[0].d));
	std::memcpy(bad.eye[1].d, in.eye[0].d, sizeof(bad.eye[1].d));
	rig.measure(bad, back, &med, &spread, &dx);
	std::printf("[vshift] H4 distortion NOT swapped: signed dy median %+.3f px, p90 spread %.3f\n", med, spread);
}

TEST_CASE("vertical offset H5: identity calibration is an identity LUT (no half-pixel shift)",
          "[stereo_rectify][vshift]")
{
	u_stereo_rectify_input in;
	std::memset(&in, 0, sizeof(in));
	in.width = 640;
	in.height = 480;
	for (int e = 0; e < 2; e++) {
		in.eye[e].fx = in.eye[e].fy = 500.0;
		in.eye[e].cx = 319.5;
		in.eye[e].cy = 239.5;
		in.eye[e].model = U_STEREO_RECTIFY_MODEL_RADTAN5;
	}
	in.R[0][0] = in.R[1][1] = in.R[2][2] = 1.0;
	in.T[0] = -120.0;
	u_stereo_rectify_result r;
	REQUIRE(u_stereo_rectify_compute(&in, &r));
	CHECK(r.cy == Catch::Approx(239.5).margin(1e-6));
	// The border check may zoom a hair (x1.001 per step) even here, but the map
	// must then be a PURE scale about (cx, cy) — identical in both eyes, with the
	// principal row fixed: no half-pixel or rounding offset anywhere.
	std::printf("[vshift] H5 identity: f %.4f (raw 500), crop x%.5f, cy %.4f\n", r.f, r.crop_scale, r.cy);
	CHECK(r.crop_scale < 1.0025);
	double worst = 0.0;
	for (uint32_t e = 0; e < 2; e++) {
		for (double v : {0.0, 100.0, 239.5, 479.0}) {
			for (double u : {0.0, 320.0, 639.0}) {
				double x, y;
				REQUIRE(u_stereo_rectify_map_point(&r, e, u, v, &x, &y));
				worst = std::max(worst, std::fabs((y - 239.5) - (v - 239.5) * 500.0 / r.f));
			}
		}
	}
	CHECK(worst < 1e-6);
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
