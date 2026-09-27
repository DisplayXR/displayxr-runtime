// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_stereo_camera (ADR-043) R2: the online vertical-alignment
 *         refinement (u_stereo_vrefine + the correction folded into
 *         u_stereo_rectify).
 *
 *  - the correction math: map <-> inverse round trip, and a pair displaced by
 *    a known (A, B) lands on the same row once the correction is folded in;
 *  - the measurement recovers a known UNCALIBRATED offset + slope injected into
 *    the sim_display distorted fake;
 *  - CLOSED LOOP on the fake (render -> rectify -> measure -> fit -> rebuild):
 *    converges to <= 0.2 px residual, judged by the probe's own NCC matcher,
 *    not by the refiner's;
 *  - degenerate input (flat, uncorrelated noise, vertical stripes) never
 *    applies anything, and the clamps hold for any input;
 *  - REAL SR hardware: one raw frame from a Leia SR laptop + its calibration
 *    (fixtures/stereo_vrefine_sr_*): the refinement, run on the runtime's own
 *    rectification of that frame, brings the signed dy median of OpenCV SIFT
 *    matches (an independent reference) from ~+1.7 px to <= 0.5 px;
 *  - [.perf] (hidden): cost of one measurement.
 */

#include "catch_amalgamated.hpp"

#include "util/u_stereo_camera.h"
#include "util/u_stereo_rectify.h"
#include "util/u_stereo_vrefine.h"

#include "sim_display_stereo_camera_pattern.h"
#include "stereo_vrefine_sr_fixture.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#include "stb_image.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr double k_pi = 3.14159265358979323846;
const double k_ideal_fx_640 = 320.0 / std::tan(34.0 * k_pi / 180.0);
const uint32_t W = 640, H = 480;

double
median(std::vector<double> v)
{
	if (v.empty()) {
		return 0.0;
	}
	std::sort(v.begin(), v.end());
	size_t n = v.size();
	return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

u_stereo_rectify_input
input_from_truth(const sim_stereo_camera_truth &t)
{
	u_stereo_rectify_input in;
	std::memset(&in, 0, sizeof(in));
	in.width = t.eye_width;
	in.height = t.eye_height;
	for (int e = 0; e < 2; e++) {
		in.eye[e].fx = t.fx[e];
		in.eye[e].fy = t.fy[e];
		in.eye[e].cx = t.cx[e];
		in.eye[e].cy = t.cy[e];
		in.eye[e].model = U_STEREO_RECTIFY_MODEL_RADTAN5;
		std::memcpy(in.eye[e].d, t.dist[e], sizeof(t.dist[e]));
	}
	std::memcpy(in.R, t.R, sizeof(in.R));
	std::memcpy(in.T, t.T_mm, sizeof(in.T));
	return in;
}

//! Rectify @p raw with the correction (a px, b) — the way the service rebuilds.
struct rectifier
{
	u_stereo_rectify_input in;
	u_stereo_rectify_result geo;
	u_stereo_rectify_lut lut;
	bool have_lut = false;

	bool
	build(double a_px, double b)
	{
		if (have_lut) {
			u_stereo_rectify_lut_fini(&lut);
			have_lut = false;
		}
		// a is in pixels of the current output; the normalized offset uses its f.
		double f = geo.f > 0.0 ? geo.f : 1.0;
		in.v_offset = geo.f > 0.0 ? a_px / f : 0.0;
		in.v_slope = b;
		if (!u_stereo_rectify_compute(&in, &geo)) {
			return false;
		}
		have_lut = u_stereo_rectify_lut_init(&lut, &geo, 1);
		return have_lut;
	}
	void
	apply(const uint8_t *raw, uint8_t *out) const
	{
		u_stereo_rectify_lut_apply(&lut, 1, raw, 2 * geo.width, out, 2 * geo.width);
	}
	~rectifier()
	{
		if (have_lut) {
			u_stereo_rectify_lut_fini(&lut);
		}
	}
};

//! The probe's matcher (u_stereo_camera_match_block) on a grid: signed dy of
//! every confident, unclamped block — an independent judge of the rows.
std::vector<double>
probe_dy(const uint8_t *img, uint32_t eye_w, uint32_t h, uint32_t max_d)
{
	std::vector<double> out;
	const uint32_t B = 32;
	for (uint32_t y = 48; y + B + 16 <= h; y += 24) {
		for (uint32_t x = 96; x + B <= eye_w; x += 24) {
			u_stereo_camera_block_match m;
			if (!u_stereo_camera_match_block(img, 2 * eye_w, eye_w, h, x, y, B, B, max_d, 12, &m) ||
			    m.dx_at_edge || m.dy_at_edge || m.ncc < 0.93f || m.dx < 0.5f) {
				continue;
			}
			out.push_back(m.dy);
		}
	}
	return out;
}

struct fake
{
	sim_stereo_camera_distorted d;
	fake(double dy_px, double slope)
	{
		REQUIRE(sim_stereo_camera_distorted_init(&d, W, H, k_ideal_fx_640, 50.0, 2.0, 0.6));
		sim_stereo_camera_distorted_set_vmisalign(&d, dy_px, slope);
	}
	~fake()
	{
		sim_stereo_camera_distorted_fini(&d);
	}
};

} // namespace


TEST_CASE("stereo vrefine: the correction folded into the rectification", "[stereo_vrefine]")
{
	fake fk(0.0, 0.0);
	u_stereo_rectify_input in = input_from_truth(fk.d.truth);
	u_stereo_rectify_result r0, r1;
	REQUIRE(u_stereo_rectify_compute(&in, &r0));
	const double A = 3.1 / r0.f, B = -0.006;
	in.v_offset = A;
	in.v_slope = B;
	REQUIRE(u_stereo_rectify_compute(&in, &r1));

	// Still the rectified-pair contract: one pinhole, a pure +x baseline.
	CHECK(r1.P[0][1][1] == r1.f);
	CHECK(r1.P[1][1][2] == r1.P[0][1][2]);
	CHECK(r1.P[1][0][3] == Catch::Approx(r1.f * r1.t_rect[0]));
	CHECK(r1.v_offset == A);
	CHECK(r1.v_slope == B);

	// map_point and rect_from_raw are inverses, correction included.
	for (uint32_t e = 0; e < 2; e++) {
		for (double v = 20; v < H - 20; v += 55) {
			for (double u = 20; u < W - 20; u += 70) {
				double x, y, u2, v2;
				REQUIRE(u_stereo_rectify_map_point(&r1, e, u, v, &x, &y));
				REQUIRE(u_stereo_rectify_rect_from_raw(&r1, e, x, y, &u2, &v2));
				CHECK(u2 == Catch::Approx(u).margin(1e-6));
				CHECK(v2 == Catch::Approx(v).margin(1e-6));
			}
		}
	}

	// A pair whose UNCORRECTED rows differ by D = f (A + B y_n) lands on one row.
	double worst = 0.0;
	for (double v = 40; v < H - 40; v += 40) {
		for (double u = 60; u < W - 60; u += 80) {
			double yn = (v - r0.cy) / r0.f;
			double vr = v + r0.f * (A + B * yn);
			double lx, ly, rx, ry, ul, vl, ur, vr1;
			REQUIRE(u_stereo_rectify_map_point(&r0, 0, u, v, &lx, &ly));
			REQUIRE(u_stereo_rectify_map_point(&r0, 1, u - 20.0, vr, &rx, &ry));
			REQUIRE(u_stereo_rectify_rect_from_raw(&r1, 0, lx, ly, &ul, &vl));
			REQUIRE(u_stereo_rectify_rect_from_raw(&r1, 1, rx, ry, &ur, &vr1));
			worst = std::max(worst, std::fabs(vr1 - vl));
		}
	}
	std::printf("[math] D = %.2f px + %.4f/px: corrected rows differ by at most %.4f px\n", A * r0.f, B, worst);
	CHECK(worst < 0.02);
}

TEST_CASE("stereo vrefine: measurement recovers an injected offset + slope", "[stereo_vrefine]")
{
	// The SR laptop's measured model, in the fake's virtual frame.
	const double a_true = 2.6, b_true = -0.0047;
	fake fk(a_true, b_true);
	rectifier rc;
	rc.in = input_from_truth(fk.d.truth);
	std::memset(&rc.geo, 0, sizeof(rc.geo));
	REQUIRE(rc.build(0.0, 0.0));
	std::vector<uint8_t> raw(2 * W * H), rect(2 * W * H);
	sim_stereo_camera_render_gray_distorted(&fk.d, 7, raw.data(), 2 * W);
	rc.apply(raw.data(), rect.data());

	u_stereo_vrefine_measure_params mp;
	u_stereo_vrefine_measure_defaults(&mp, 96);
	std::vector<u_stereo_vrefine_sample> s(512);
	uint32_t n = u_stereo_vrefine_measure(rect.data(), 2 * W, W, H, &mp, s.data(), (uint32_t)s.size());
	u_stereo_vrefine_fit fit;
	REQUIRE(u_stereo_vrefine_fit_samples(s.data(), n, rc.geo.cy, &fit));
	// The misalignment is defined in the virtual frame (ideal_fx); the
	// rectified frame is that frame zoomed by f / ideal_fx about its centre.
	const double k = rc.geo.f / fk.d.truth.ideal_fx;
	const double a_exp = k * a_true, b_exp = b_true;
	std::printf(
	    "[measure] %u matches (%u inliers): a %+.3f px (expect %+.3f), b %+.5f (expect %+.5f), sigma %.3f\n", n,
	    fit.inliers, fit.a, a_exp, fit.b, b_exp, fit.sigma);
	CHECK(n >= 50);
	CHECK(fit.slope_determined);
	CHECK(fit.a == Catch::Approx(a_exp).margin(0.15));
	CHECK(fit.b == Catch::Approx(b_exp).margin(0.0008));
	for (uint32_t i = 0; i < n; i++) {
		CHECK(s[i].d > 0.5f);
		CHECK(s[i].ncc >= 0.9f);
	}
}

TEST_CASE("stereo vrefine: CLOSED LOOP on the fake converges to <= 0.2 px", "[stereo_vrefine]")
{
	const double a_true = 2.6, b_true = -0.0047;
	fake fk(a_true, b_true);
	rectifier rc;
	rc.in = input_from_truth(fk.d.truth);
	std::memset(&rc.geo, 0, sizeof(rc.geo));
	REQUIRE(rc.build(0.0, 0.0));
	std::vector<uint8_t> raw(2 * W * H), rect(2 * W * H);

	u_stereo_vrefine_config cfg;
	u_stereo_vrefine_config_defaults(&cfg, H, rc.geo.cy);
	u_stereo_vrefine ref;
	u_stereo_vrefine_init(&ref, &cfg);
	u_stereo_vrefine_measure_params mp;
	u_stereo_vrefine_measure_defaults(&mp, 96);
	std::vector<u_stereo_vrefine_sample> s(512);

	// Before: the probe sees the injected offset.
	sim_stereo_camera_render_gray_distorted(&fk.d, 1, raw.data(), 2 * W);
	rc.apply(raw.data(), rect.data());
	double before = median(probe_dy(rect.data(), W, H, 96));

	// 60 frames at 30 Hz = 2 s of the settling phase.
	int64_t now = 1000000000;
	uint32_t updates = 0, measured = 0;
	for (uint64_t frame = 0; frame < 60; frame++, now += 33333333) {
		if (!u_stereo_vrefine_due(&ref, now)) {
			continue;
		}
		sim_stereo_camera_render_gray_distorted(&fk.d, frame, raw.data(), 2 * W);
		rc.apply(raw.data(), rect.data());
		uint32_t n = u_stereo_vrefine_measure(rect.data(), 2 * W, W, H, &mp, s.data(), (uint32_t)s.size());
		measured++;
		if (u_stereo_vrefine_push(&ref, now, s.data(), n) == U_STEREO_VREFINE_UPDATED) {
			updates++;
			REQUIRE(rc.build(ref.a, ref.b));
			CHECK(std::fabs(ref.a) <= cfg.max_offset_px);
			CHECK(std::fabs(ref.b) <= cfg.max_slope);
		}
	}
	sim_stereo_camera_render_gray_distorted(&fk.d, 999, raw.data(), 2 * W);
	rc.apply(raw.data(), rect.data());
	std::vector<double> after = probe_dy(rect.data(), W, H, 96);
	std::vector<double> absd;
	for (double v : after) {
		absd.push_back(std::fabs(v));
	}
	double med_after = median(after), abs_after = median(absd);
	std::printf(
	    "[closed loop] %u frames measured, %u map rebuilds: applied a %+.3f px, b %+.5f; probe signed dy median "
	    "%+.3f -> %+.3f px (|dy| median %.3f, %zu blocks); initial window %+.3f px\n",
	    measured, updates, ref.a, ref.b, before, med_after, abs_after, after.size(), ref.initial_dy);
	CHECK(before > 2.0);
	CHECK(updates >= 1);
	CHECK(after.size() > 40);
	CHECK(std::fabs(med_after) <= 0.2);
	CHECK(abs_after <= 0.25);
	// Rows everywhere, not just at the centre: the slope was fitted.
	CHECK(ref.b == Catch::Approx(b_true).margin(0.001));
	// Converged: further windows are STABLE (the deadband), no more rebuilds.
	uint32_t late = 0;
	for (uint64_t frame = 60; frame < 120; frame++, now += 33333333) {
		if (!u_stereo_vrefine_due(&ref, now)) {
			continue;
		}
		sim_stereo_camera_render_gray_distorted(&fk.d, frame, raw.data(), 2 * W);
		rc.apply(raw.data(), rect.data());
		uint32_t n = u_stereo_vrefine_measure(rect.data(), 2 * W, W, H, &mp, s.data(), (uint32_t)s.size());
		if (u_stereo_vrefine_push(&ref, now, s.data(), n) == U_STEREO_VREFINE_UPDATED) {
			late++;
			REQUIRE(rc.build(ref.a, ref.b));
		}
	}
	CHECK(late == 0);
	CHECK(std::fabs(ref.last_residual_dy) <= 0.2);
}

TEST_CASE("stereo vrefine: degenerate frames never apply; clamps always hold", "[stereo_vrefine]")
{
	u_stereo_vrefine_config cfg;
	u_stereo_vrefine_config_defaults(&cfg, H, 239.5);
	u_stereo_vrefine_measure_params mp;
	u_stereo_vrefine_measure_defaults(&mp, 96);
	std::vector<u_stereo_vrefine_sample> s(512);
	std::vector<uint8_t> img(2 * W * H);

	uint32_t lcg = 12345;
	auto rnd = [&]() {
		lcg = lcg * 1664525u + 1013904223u;
		return (uint8_t)(lcg >> 24);
	};
	for (int kind = 0; kind < 3; kind++) {
		for (uint32_t y = 0; y < H; y++) {
			for (uint32_t x = 0; x < 2 * W; x++) {
				uint8_t v = 128;
				if (kind == 1) {
					v = rnd(); // uncorrelated noise between the eyes
				} else if (kind == 2) {
					v = ((x % W) / 7) % 2 ? 200 : 40; // vertical stripes: dy unobservable
				}
				img[y * 2 * W + x] = v;
			}
		}
		u_stereo_vrefine ref;
		u_stereo_vrefine_init(&ref, &cfg);
		uint32_t total = 0;
		int64_t now = 0;
		for (int f = 0; f < 60; f++, now += 250000000) {
			uint32_t n =
			    u_stereo_vrefine_measure(img.data(), 2 * W, W, H, &mp, s.data(), (uint32_t)s.size());
			total += n;
			CHECK(u_stereo_vrefine_push(&ref, now, s.data(), n) != U_STEREO_VREFINE_UPDATED);
		}
		std::printf("[degenerate] kind %d: %u matches over 60 frames, applied %d\n", kind, total, ref.applied);
		CHECK_FALSE(ref.applied);
		CHECK(ref.a == 0.0);
		CHECK(ref.b == 0.0);
	}

	// Clamps: an absurd consistent residual saturates at the limits, never beyond.
	u_stereo_vrefine ref;
	u_stereo_vrefine_init(&ref, &cfg);
	std::vector<u_stereo_vrefine_sample> big(60);
	for (size_t i = 0; i < big.size(); i++) {
		big[i].x = 320;
		big[i].y = (float)(20 + 7 * i);
		big[i].d = 30;
		big[i].ncc = 0.99f;
		big[i].dy = (float)(15.0 + 0.05 * (big[i].y - 239.5));
	}
	int64_t now = 0;
	for (int w = 0; w < 10; w++) {
		for (uint32_t f = 0; f < cfg.min_frames; f++, now += 250000000) {
			u_stereo_vrefine_push(&ref, now, big.data(), 20);
		}
		CHECK(std::fabs(ref.a) <= cfg.max_offset_px);
		CHECK(std::fabs(ref.b) <= cfg.max_slope);
	}
	CHECK(ref.a == cfg.max_offset_px);
	CHECK(ref.b == cfg.max_slope);

	// Too few frames or matches: nothing applies.
	u_stereo_vrefine_init(&ref, &cfg);
	CHECK(u_stereo_vrefine_push(&ref, 0, big.data(), 60) == U_STEREO_VREFINE_ACCUMULATING); // 1 frame
	CHECK(u_stereo_vrefine_push(&ref, 1, big.data(), 0) == U_STEREO_VREFINE_ACCUMULATING);  // 2 frames
	CHECK(u_stereo_vrefine_push(&ref, 2, big.data(), 0) == U_STEREO_VREFINE_UPDATED);       // 3 frames, 60 matches
	u_stereo_vrefine_init(&ref, &cfg);
	for (int f = 0; f < 11; f++) {
		CHECK(u_stereo_vrefine_push(&ref, f, big.data(), 4) == U_STEREO_VREFINE_ACCUMULATING); // 44 < 50
	}
	// 12th frame: 48 < 50 matches when the window expires -> dropped, not applied.
	CHECK(u_stereo_vrefine_push(&ref, 12, big.data(), 4) == U_STEREO_VREFINE_DROPPED);
	CHECK_FALSE(ref.applied);
}

TEST_CASE("stereo vrefine: duty cycle — settle fast, then every 30 s", "[stereo_vrefine]")
{
	u_stereo_vrefine_config cfg;
	u_stereo_vrefine_config_defaults(&cfg, H, 239.5);
	u_stereo_vrefine ref;
	u_stereo_vrefine_init(&ref, &cfg);
	std::vector<u_stereo_vrefine_sample> ok(30);
	for (size_t i = 0; i < ok.size(); i++) {
		ok[i].x = 320;
		ok[i].y = (float)(20 + 15 * i);
		ok[i].d = 30;
		ok[i].ncc = 0.99f;
		ok[i].dy = (float)(0.01 * ((int)(i % 3) - 1)); // aligned rows: STABLE windows
	}
	const int64_t step = 16666667; // 60 Hz
	uint32_t measured_fast = 0, measured_slow = 0;
	for (int64_t t = 0; t < 65ll * 1000000000; t += step) {
		if (u_stereo_vrefine_due(&ref, t)) {
			u_stereo_vrefine_push(&ref, t, ok.data(), (uint32_t)ok.size());
			(t < cfg.fast_phase_ns ? measured_fast : measured_slow)++;
		}
	}
	std::printf("[duty] measured %u frames in the first 5 s, %u in the next 60 s\n", measured_fast, measured_slow);
	CHECK(measured_fast >= 15);
	CHECK(measured_fast <= 21);
	CHECK(measured_slow <= 3 * 3); // two or three windows of min_frames
	CHECK_FALSE(ref.applied);
}

TEST_CASE("stereo vrefine: REAL Leia SR frame — residual +1.7 px -> <= 0.5 px", "[stereo_vrefine]")
{
	int w = 0, h = 0, ch = 0;
	uint8_t *raw = stbi_load(STEREO_FIXTURE_DIR "/stereo_vrefine_sr_raw.jpg", &w, &h, &ch, 1);
	REQUIRE(raw != nullptr);
	REQUIRE(w == 1280);
	REQUIRE(h == 480);

	rectifier rc;
	std::memset(&rc.in, 0, sizeof(rc.in));
	std::memset(&rc.geo, 0, sizeof(rc.geo));
	rc.in.width = 640;
	rc.in.height = 480;
	for (int e = 0; e < 2; e++) {
		rc.in.eye[e].fx = k_sr_K[e][0];
		rc.in.eye[e].fy = k_sr_K[e][1];
		rc.in.eye[e].cx = k_sr_K[e][2];
		rc.in.eye[e].cy = k_sr_K[e][3];
		rc.in.eye[e].model = U_STEREO_RECTIFY_MODEL_RADTAN5;
		for (int k = 0; k < 5; k++) {
			rc.in.eye[e].d[k] = k_sr_D[e][k];
		}
	}
	std::memcpy(rc.in.R, k_sr_R, sizeof(rc.in.R));
	std::memcpy(rc.in.T, k_sr_T, sizeof(rc.in.T));
	REQUIRE(rc.build(0.0, 0.0));

	auto sift_dy = [&](std::vector<double> *ys) {
		std::vector<double> dys;
		for (const stereo_vrefine_sr_match &m : k_sr_matches) {
			double ul, vl, ur, vr;
			if (u_stereo_rectify_rect_from_raw(&rc.geo, 0, m.lx, m.ly, &ul, &vl) &&
			    u_stereo_rectify_rect_from_raw(&rc.geo, 1, m.rx, m.ry, &ur, &vr)) {
				dys.push_back(vr - vl);
				if (ys != nullptr) {
					ys->push_back(vl);
				}
			}
		}
		return dys;
	};
	std::vector<double> dy0 = sift_dy(nullptr);
	double before = median(dy0);

	std::vector<uint8_t> rect(1280 * 480);
	u_stereo_vrefine_config cfg;
	u_stereo_vrefine_config_defaults(&cfg, 480, rc.geo.cy);
	u_stereo_vrefine ref;
	u_stereo_vrefine_init(&ref, &cfg);
	u_stereo_vrefine_measure_params mp;
	// A subject at 0.4 m on this 120 mm, f ~ 513 px pair: ~154 px.
	u_stereo_vrefine_measure_defaults(&mp, (uint32_t)(rc.geo.f * rc.geo.baseline / 400.0));
	std::vector<u_stereo_vrefine_sample> s(512);
	uint32_t first_n = 0, updates = 0;
	int64_t now = 0;
	// One recorded frame stands in for the live stream (the service measures
	// consecutive frames; the matcher is deterministic, so a repeated frame
	// only repeats its matches). 30 frames = 7.5 s at the settling cadence.
	for (int f = 0; f < 30; f++, now += cfg.fast_period_ns) {
		rc.apply(raw, rect.data());
		uint32_t n = u_stereo_vrefine_measure(rect.data(), 1280, 640, 480, &mp, s.data(), (uint32_t)s.size());
		if (f == 0) {
			first_n = n;
		}
		if (u_stereo_vrefine_push(&ref, now, s.data(), n) == U_STEREO_VREFINE_UPDATED) {
			updates++;
			REQUIRE(rc.build(ref.a, ref.b));
		}
	}
	std::vector<double> dy1 = sift_dy(nullptr);
	double after = median(dy1);
	std::printf(
	    "[SR] %u refiner matches per frame, %u rebuilds: applied a %+.3f px, b %+.3f px/100 px; initial window "
	    "%+.3f px, last residual %+.3f px (%u inliers)\n"
	    "[SR] OpenCV SIFT reference (%zu matches): signed dy median %+.3f px (OpenCV's own rectification %+.3f) "
	    "-> %+.3f px\n",
	    first_n, updates, ref.a, ref.b * 100.0, ref.initial_dy, ref.last_residual_dy, ref.last_matches, dy1.size(),
	    before, k_sr_ocv_median_dy, after);
	CHECK(before == Catch::Approx(k_sr_ocv_median_dy).margin(0.3)); // the runtime reproduces OpenCV
	CHECK(before > 1.2);
	CHECK(updates >= 1);
	CHECK(std::fabs(after) <= 0.5);
	CHECK(std::fabs(ref.a) <= cfg.max_offset_px);
	CHECK(std::fabs(ref.b) <= cfg.max_slope);
	stbi_image_free(raw);
}

TEST_CASE("stereo vrefine: cost of one measurement", "[stereo_vrefine][.perf]")
{
	fake fk(2.6, -0.0047);
	rectifier rc;
	rc.in = input_from_truth(fk.d.truth);
	std::memset(&rc.geo, 0, sizeof(rc.geo));
	REQUIRE(rc.build(0.0, 0.0));
	std::vector<uint8_t> raw(2 * W * H), rect(2 * W * H);
	sim_stereo_camera_render_gray_distorted(&fk.d, 3, raw.data(), 2 * W);
	rc.apply(raw.data(), rect.data());
	u_stereo_vrefine_measure_params mp;
	u_stereo_vrefine_measure_defaults(&mp, 154);
	std::vector<u_stereo_vrefine_sample> s(512);
	const int N = 20;
	uint32_t n = 0;
	auto t0 = std::chrono::steady_clock::now();
	for (int i = 0; i < N; i++) {
		n = u_stereo_vrefine_measure(rect.data(), 2 * W, W, H, &mp, s.data(), (uint32_t)s.size());
	}
	double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / N;
	auto t1 = std::chrono::steady_clock::now();
	REQUIRE(rc.build(1.0, 0.001));
	double build_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
	std::printf("[perf] measure %.2f ms per 1280x480 frame (%u matches, max disparity 154); map rebuild %.1f ms\n",
	            ms, n, build_ms);
}
