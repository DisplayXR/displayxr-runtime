// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_display webcam eye tracking (#1855) — everything but the face
 *         estimator and the real camera:
 *
 *  - geometry: hand-computed numbers for a known pinhole (90 deg over 640 px
 *    = f 320 px), the round trip through @ref sim_webcam_project for an
 *    off-axis head, mirroring, the mount offset, and rejection of unusable
 *    landmarks;
 *  - the one-euro filter: first sample passes, constant input is a fixed
 *    point, a step is approached monotonically, a larger beta follows faster,
 *    a stale timestamp changes nothing;
 *  - the timeout + MANAGED blend: tracking up to 300 ms after the last face,
 *    then not; collapse to the nominal viewer; revival from it; the edge
 *    count the per-screen status publishes;
 *  - device + mode selection and the offset parser;
 *  - the WORKER end to end on the synthetic UVC backend with a fake
 *    estimator: frames flow at the camera rate, the known head comes back,
 *    "no face" never tracks, an unknown device never tracks, and destroy
 *    returns promptly. No real camera is ever opened.
 */

#include "catch_amalgamated.hpp"

#include "sim_display_face_estimator.h"
#include "sim_display_webcam_tracker.h"
#include "sim_display_webcam_tracking.h"

#include "os/os_time.h"
#include "util/u_stereo_uvc.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <thread>

using Catch::Approx;

static const float kScreenH = 0.194f; // the sim default panel height
static const uint64_t kMs = 1000000ull;

static sim_face_landmarks
project_pair(const sim_webcam_config &cfg, uint32_t w, uint32_t h, const xrt_vec3 eyes[2])
{
	sim_face_landmarks lm{};
	lm.found = true;
	lm.confidence = 1.0f;
	for (int i = 0; i < 2; i++) {
		REQUIRE(sim_webcam_project(&cfg, kScreenH, w, h, &eyes[i], lm.eye_px[i]));
	}
	return lm;
}

TEST_CASE("webcam geometry: a known pinhole, by hand", "[sim_webcam]")
{
	sim_webcam_config cfg;
	sim_webcam_config_defaults(&cfg);
	cfg.hfov_deg = 90.0f; // f = 320 px over 640 px

	// Pupils 32 px apart, centred: Z = 320 * 0.063 / 32 = 0.63 m, straight out
	// from the camera, which sits at the top-centre of the panel.
	sim_face_landmarks lm{};
	lm.found = true;
	lm.eye_px[0][0] = 304.0f;
	lm.eye_px[0][1] = 240.0f;
	lm.eye_px[1][0] = 336.0f;
	lm.eye_px[1][1] = 240.0f;
	xrt_vec3 e[2];
	REQUIRE(sim_webcam_landmarks_to_eyes(&cfg, kScreenH, 640, 480, &lm, e));
	CHECK(e[0].x == Approx(-0.0315f).margin(1e-6));
	CHECK(e[1].x == Approx(0.0315f).margin(1e-6));
	CHECK(e[0].y == Approx(kScreenH / 2).margin(1e-6));
	CHECK(e[0].z == Approx(0.63f).margin(1e-6));
	CHECK(e[1].z == Approx(0.63f).margin(1e-6));

	// The face 64 px to the image's RIGHT is the viewer's display LEFT (an
	// unmirrored webcam faces the viewer); 48 px down is lower on the panel.
	for (int i = 0; i < 2; i++) {
		lm.eye_px[i][0] += 64.0f;
		lm.eye_px[i][1] += 48.0f;
	}
	REQUIRE(sim_webcam_landmarks_to_eyes(&cfg, kScreenH, 640, 480, &lm, e));
	const float mid_x = (e[0].x + e[1].x) / 2;
	CHECK(mid_x == Approx(-64.0f * 0.63f / 320.0f).margin(1e-6));
	CHECK(e[0].y == Approx(kScreenH / 2 - 48.0f * 0.63f / 320.0f).margin(1e-6));
	CHECK(e[0].x < e[1].x); // [0] is always the viewer's left

	// Mirrored source: the same pixels are the other side.
	cfg.mirrored = true;
	REQUIRE(sim_webcam_landmarks_to_eyes(&cfg, kScreenH, 640, 480, &lm, e));
	CHECK((e[0].x + e[1].x) / 2 == Approx(64.0f * 0.63f / 320.0f).margin(1e-6));
	cfg.mirrored = false;

	// The mount offset moves the camera, and so every eye, by the same amount.
	cfg.cam_offset_m[0] = 0.010f;
	cfg.cam_offset_m[1] = 0.015f;
	cfg.cam_offset_m[2] = -0.005f;
	xrt_vec3 o[2];
	REQUIRE(sim_webcam_landmarks_to_eyes(&cfg, kScreenH, 640, 480, &lm, o));
	sim_webcam_config cfg0 = cfg;
	cfg0.cam_offset_m[0] = cfg0.cam_offset_m[1] = cfg0.cam_offset_m[2] = 0.0f;
	REQUIRE(sim_webcam_landmarks_to_eyes(&cfg0, kScreenH, 640, 480, &lm, e));
	CHECK(o[0].x - e[0].x == Approx(0.010f).margin(1e-6));
	CHECK(o[0].y - e[0].y == Approx(0.015f).margin(1e-6));
	CHECK(o[0].z - e[0].z == Approx(-0.005f).margin(1e-6));
}

TEST_CASE("webcam geometry: round trip for an off-axis head, any FOV", "[sim_webcam]")
{
	const float fovs[] = {50.0f, 70.0f, 95.0f};
	const uint32_t sizes[][2] = {{640, 480}, {1280, 720}};
	for (float fov : fovs) {
		for (auto &sz : sizes) {
			sim_webcam_config cfg;
			sim_webcam_config_defaults(&cfg);
			cfg.hfov_deg = fov;
			// A 63 mm pair, 12 cm right, 3 cm below centre, 55 cm out.
			const xrt_vec3 truth[2] = {{0.12f - 0.0315f, -0.03f, 0.55f}, {0.12f + 0.0315f, -0.03f, 0.55f}};
			sim_face_landmarks lm = project_pair(cfg, sz[0], sz[1], truth);
			xrt_vec3 e[2];
			REQUIRE(sim_webcam_landmarks_to_eyes(&cfg, kScreenH, sz[0], sz[1], &lm, e));
			for (int i = 0; i < 2; i++) {
				CHECK(e[i].x == Approx(truth[i].x).margin(1e-4));
				CHECK(e[i].y == Approx(truth[i].y).margin(1e-4));
				CHECK(e[i].z == Approx(truth[i].z).margin(1e-4));
			}
		}
	}
}

TEST_CASE("webcam geometry: unusable landmarks are no face", "[sim_webcam]")
{
	sim_webcam_config cfg;
	sim_webcam_config_defaults(&cfg);
	xrt_vec3 e[2];
	sim_face_landmarks lm{};
	CHECK_FALSE(sim_webcam_landmarks_to_eyes(&cfg, kScreenH, 640, 480, &lm, e)); // not found
	lm.found = true;
	lm.eye_px[0][0] = lm.eye_px[1][0] = 320.0f; // coincident pupils
	lm.eye_px[0][1] = lm.eye_px[1][1] = 240.0f;
	CHECK_FALSE(sim_webcam_landmarks_to_eyes(&cfg, kScreenH, 640, 480, &lm, e));
	lm.eye_px[1][0] = 322.0f; // 2 px apart: ~14 m away -> beyond 5 m
	CHECK_FALSE(sim_webcam_landmarks_to_eyes(&cfg, kScreenH, 640, 480, &lm, e));
	lm.eye_px[0][0] = 10.0f; // 620 px apart: ~4.6 cm -> closer than 10 cm
	lm.eye_px[1][0] = 630.0f;
	CHECK_FALSE(sim_webcam_landmarks_to_eyes(&cfg, kScreenH, 640, 480, &lm, e));
	CHECK_FALSE(sim_webcam_landmarks_to_eyes(&cfg, kScreenH, 0, 0, &lm, e)); // no image
}

TEST_CASE("webcam smoothing: one-euro filter", "[sim_webcam]")
{
	const uint64_t dt = 33 * kMs;

	sim_one_euro f;
	sim_one_euro_reset(&f);
	CHECK(sim_one_euro_filter(&f, 1.0f, 0.0f, 1.0f, 0.5f, 1000 * kMs) == 0.5f); // first sample passes
	for (int i = 1; i <= 10; i++) {
		CHECK(sim_one_euro_filter(&f, 1.0f, 0.0f, 1.0f, 0.5f, 1000 * kMs + i * dt) == Approx(0.5f));
	}

	// A 10 cm step is approached monotonically, never overshot.
	float prev = 0.5f;
	for (int i = 11; i <= 60; i++) {
		const float y = sim_one_euro_filter(&f, 1.0f, 0.0f, 1.0f, 0.6f, 1000 * kMs + i * dt);
		CHECK(y >= prev);
		CHECK(y <= 0.6f + 1e-6f);
		prev = y;
	}
	CHECK(prev == Approx(0.6f).margin(0.002));

	// A stale or repeated timestamp returns the previous output unchanged.
	CHECK(sim_one_euro_filter(&f, 1.0f, 0.0f, 1.0f, 9.0f, 1000 * kMs) == prev);

	// Speed raises the cutoff: with beta the first step response is closer.
	sim_one_euro slow, fast;
	sim_one_euro_reset(&slow);
	sim_one_euro_reset(&fast);
	sim_one_euro_filter(&slow, 1.0f, 0.0f, 1.0f, 0.0f, 0);
	sim_one_euro_filter(&fast, 1.0f, 5.0f, 1.0f, 0.0f, 0);
	const float ys = sim_one_euro_filter(&slow, 1.0f, 0.0f, 1.0f, 0.1f, dt);
	const float yf = sim_one_euro_filter(&fast, 1.0f, 5.0f, 1.0f, 0.1f, dt);
	CHECK(yf > ys);
}

TEST_CASE("webcam tracking: 300 ms timeout, MANAGED collapse + revival, edges", "[sim_webcam]")
{
	sim_webcam_config cfg;
	sim_webcam_config_defaults(&cfg);
	REQUIRE(cfg.timeout_ns == 300 * kMs);
	const xrt_vec3 nominal[2] = {{-0.03f, 0.1f, 0.65f}, {0.03f, 0.1f, 0.65f}};
	const xrt_vec3 head[2] = {{0.07f, 0.0f, 0.5f}, {0.133f, 0.0f, 0.5f}};

	sim_webcam_track_state s;
	sim_webcam_track_reset(&s);
	sim_webcam_eyes out;

	// Never a face: nominal, not tracking, no edges.
	sim_webcam_track_sample(&s, &cfg, nominal, 5000 * kMs, &out);
	CHECK_FALSE(out.is_tracking);
	CHECK(out.eyes[0].x == nominal[0].x);
	CHECK(out.edges == 0);

	// Faces every 33 ms from t0: tracking; revival eases in from nominal.
	const uint64_t t0 = 10000 * kMs;
	uint64_t t = t0;
	for (; t <= t0 + 600 * kMs; t += 33 * kMs) {
		sim_webcam_track_update(&s, &cfg, true, head, t);
	}
	const uint64_t last = t - 33 * kMs;
	sim_webcam_track_sample(&s, &cfg, nominal, t0, &out);
	CHECK(out.is_tracking);
	CHECK(out.weight == Approx(0.0f)); // first face: still at nominal
	CHECK(out.eyes[0].x == Approx(nominal[0].x));
	sim_webcam_track_sample(&s, &cfg, nominal, t0 + 150 * kMs, &out);
	CHECK(out.weight > 0.0f);
	CHECK(out.weight < 1.0f);
	sim_webcam_track_sample(&s, &cfg, nominal, last, &out);
	CHECK(out.is_tracking);
	CHECK(out.weight == Approx(1.0f));
	CHECK(out.eyes[0].x == Approx(head[0].x).margin(1e-5));
	CHECK(out.eyes[1].z == Approx(head[1].z).margin(1e-5));
	CHECK(out.edges == 1);

	// No face for 299 ms: still tracking. 301 ms: lost — but the eyes have
	// not jumped; they collapse to nominal over collapse_ns.
	sim_webcam_track_update(&s, &cfg, false, head, last + 100 * kMs); // no-face frames change nothing
	sim_webcam_track_sample(&s, &cfg, nominal, last + 299 * kMs, &out);
	CHECK(out.is_tracking);
	sim_webcam_track_sample(&s, &cfg, nominal, last + 301 * kMs, &out);
	CHECK_FALSE(out.is_tracking);
	CHECK(out.edges == 2);
	CHECK(out.weight == Approx(1.0f).margin(0.01));
	sim_webcam_track_sample(&s, &cfg, nominal, last + 300 * kMs + cfg.collapse_ns / 2, &out);
	CHECK(out.weight == Approx(0.5f).margin(0.01));
	sim_webcam_track_sample(&s, &cfg, nominal, last + 300 * kMs + cfg.collapse_ns, &out);
	CHECK(out.weight == 0.0f);
	CHECK(out.eyes[0].x == Approx(nominal[0].x));
	CHECK(out.eyes[1].y == Approx(nominal[1].y));

	// Re-acquired after the collapse: a new run (edge 3), revived from 0.
	const uint64_t t1 = last + 2000 * kMs;
	sim_webcam_track_update(&s, &cfg, true, head, t1);
	sim_webcam_track_sample(&s, &cfg, nominal, t1, &out);
	CHECK(out.is_tracking);
	CHECK(out.edges == 3);
	CHECK(out.weight == Approx(0.0f));
	sim_webcam_track_sample(&s, &cfg, nominal, t1 + cfg.revive_ns, &out);
	CHECK(out.weight == Approx(1.0f));

	// Re-acquired MID-collapse: the blend continues from where it was.
	const uint64_t lost = t1 + 300 * kMs;
	const uint64_t t2 = lost + cfg.collapse_ns / 2;
	sim_webcam_eyes before;
	sim_webcam_track_sample(&s, &cfg, nominal, t2, &before);
	sim_webcam_track_update(&s, &cfg, true, head, t2);
	sim_webcam_track_sample(&s, &cfg, nominal, t2, &out);
	CHECK(out.weight == Approx(before.weight).margin(1e-5));
	CHECK(out.edges == 5);
}

TEST_CASE("webcam selection: device by index or name, mode near 640 wide", "[sim_webcam]")
{
	u_stereo_uvc_device d[3]{};
	std::strcpy(d[0].name, "Integrated Camera");
	std::strcpy(d[1].name, "Logitech BRIO");
	std::strcpy(d[2].name, "USB2.0 HD UVC WebCam");
	CHECK(sim_webcam_select_device(d, 3, nullptr) == 0);
	CHECK(sim_webcam_select_device(d, 3, "") == 0);
	CHECK(sim_webcam_select_device(d, 3, "2") == 2);
	CHECK(sim_webcam_select_device(d, 3, "3") == -1);
	CHECK(sim_webcam_select_device(d, 3, "brio") == 1);
	CHECK(sim_webcam_select_device(d, 3, "WEBCAM") == 2);
	CHECK(sim_webcam_select_device(d, 3, "nope") == -1);
	CHECK(sim_webcam_select_device(d, 0, nullptr) == -1);

	u_stereo_uvc_mode m[] = {{1920, 1080, 30}, {640, 480, 30}, {640, 480, 60}, {640, 360, 60}, {320, 240, 120}};
	u_stereo_uvc_mode out{};
	REQUIRE(sim_webcam_select_mode(m, 5, &out));
	CHECK(out.width == 640);
	CHECK(out.height == 360); // 60 fps ties; the smaller frame is cheaper
	CHECK(out.fps == Approx(60.0f));
	CHECK_FALSE(sim_webcam_select_mode(m, 0, &out));

	float off[3] = {9, 9, 9};
	CHECK(sim_webcam_parse_offset_mm(" 10, -5.5 ,2", off));
	CHECK(off[0] == Approx(0.010f));
	CHECK(off[1] == Approx(-0.0055f));
	CHECK(off[2] == Approx(0.002f));
	CHECK_FALSE(sim_webcam_parse_offset_mm("10,5", off));
	CHECK_FALSE(sim_webcam_parse_offset_mm("10,5,2,1", off));
	CHECK(off[0] == Approx(0.010f)); // untouched on failure
}


/*
 * The worker, end to end, on the synthetic UVC backend.
 */

struct fake_estimator
{
	sim_face_estimator base;
	sim_webcam_config cfg;
	xrt_vec3 head[2];
	std::atomic<bool> face;
	std::atomic<int> calls;
};

static bool
fake_estimate(sim_face_estimator *est, const u_stereo_uvc_raw_frame *frame, sim_face_landmarks *out)
{
	fake_estimator *f = reinterpret_cast<fake_estimator *>(est);
	f->calls++;
	if (!f->face) {
		out->found = false;
		return true;
	}
	out->found = true;
	for (int i = 0; i < 2; i++) {
		sim_webcam_project(&f->cfg, kScreenH, frame->width, frame->height, &f->head[i], out->eye_px[i]);
	}
	return true;
}

static void
fake_destroy(sim_face_estimator *)
{}

static bool
wait_until(const std::function<bool()> &pred, int ms)
{
	for (int waited = 0; waited < ms; waited += 10) {
		if (pred()) {
			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return pred();
}

TEST_CASE("webcam worker: synthetic camera + fake estimator, end to end", "[sim_webcam]")
{
	u_stereo_uvc_fake_params params{};
	params.pixel = U_STEREO_UVC_PIXEL_NV12;
	params.fps = 120.0f;
	u_stereo_uvc_backend backend;
	u_stereo_uvc_fake_backend_init(&backend, &params);

	sim_webcam_config cfg;
	sim_webcam_config_defaults(&cfg);
	fake_estimator est{};
	est.base.name = "fake";
	est.base.estimate = fake_estimate;
	est.base.destroy = fake_destroy;
	est.cfg = cfg;
	est.head[0] = {-0.05f - 0.0315f, 0.02f, 0.6f};
	est.head[1] = {-0.05f + 0.0315f, 0.02f, 0.6f};
	est.face = true;

	const xrt_vec3 nominal[2] = {{-0.03f, 0.1f, 0.65f}, {0.03f, 0.1f, 0.65f}};

	SECTION("a face is tracked to the known head")
	{
		sim_webcam_tracker *t = sim_webcam_tracker_create(&cfg, &backend, "synthetic", &est.base, kScreenH);
		REQUIRE(t != nullptr);
		REQUIRE(wait_until([&] { return sim_webcam_tracker_frame_count(t) >= 10; }, 3000));
		sim_webcam_eyes out;
		// Past the revival ramp, the reported eyes are the head.
		REQUIRE(wait_until(
		    [&] {
			    sim_webcam_tracker_sample(t, nominal, os_monotonic_get_ns(), &out);
			    return out.is_tracking && out.weight > 0.999f;
		    },
		    3000));
		for (int i = 0; i < 2; i++) {
			CHECK(out.eyes[i].x == Approx(est.head[i].x).margin(1e-3));
			CHECK(out.eyes[i].y == Approx(est.head[i].y).margin(1e-3));
			CHECK(out.eyes[i].z == Approx(est.head[i].z).margin(1e-3));
		}

		// The face leaves: within the timeout + a frame it is lost.
		est.face = false;
		REQUIRE(wait_until(
		    [&] {
			    sim_webcam_tracker_sample(t, nominal, os_monotonic_get_ns(), &out);
			    return !out.is_tracking;
		    },
		    2000));
		CHECK(out.edges == 2);

		const auto start = std::chrono::steady_clock::now();
		sim_webcam_tracker_destroy(&t);
		CHECK(t == nullptr);
		CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(1000));
	}

	SECTION("no face: frames flow, never tracking, nominal eyes")
	{
		est.face = false;
		sim_webcam_tracker *t = sim_webcam_tracker_create(&cfg, &backend, nullptr, &est.base, kScreenH);
		REQUIRE(t != nullptr);
		REQUIRE(wait_until([&] { return sim_webcam_tracker_frame_count(t) >= 5; }, 3000));
		sim_webcam_eyes out;
		sim_webcam_tracker_sample(t, nominal, os_monotonic_get_ns(), &out);
		CHECK_FALSE(out.is_tracking);
		CHECK(out.eyes[0].x == nominal[0].x);
		sim_webcam_tracker_destroy(&t);
	}

	SECTION("an unknown device: never opened, never tracking, clean destroy")
	{
		sim_webcam_tracker *t =
		    sim_webcam_tracker_create(&cfg, &backend, "no such camera", &est.base, kScreenH);
		REQUIRE(t != nullptr);
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		CHECK(sim_webcam_tracker_frame_count(t) == 0);
		CHECK(est.calls == 0);
		sim_webcam_eyes out;
		sim_webcam_tracker_sample(t, nominal, os_monotonic_get_ns(), &out);
		CHECK_FALSE(out.is_tracking);
		sim_webcam_tracker_destroy(&t);
	}

	SECTION("bad arguments are refused")
	{
		CHECK(sim_webcam_tracker_create(&cfg, &backend, nullptr, nullptr, kScreenH) == nullptr);
		u_stereo_uvc_backend empty{};
		CHECK(sim_webcam_tracker_create(&cfg, &empty, nullptr, &est.base, kScreenH) == nullptr);
	}
}

TEST_CASE("webcam estimator: the default build ships none, so the camera stays shut", "[sim_webcam]")
{
	// The open question of #1855: until an estimator lands, the slot reports
	// unavailable, and the glue then never advertises tracking or opens a
	// camera. Compiled against the real stub. A build that adds an estimator
	// flips both of these together.
	CHECK_FALSE(sim_face_estimator_default_available());
	CHECK(sim_face_estimator_create_default() == nullptr);
}
