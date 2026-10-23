// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_tracking `SOURCE=webcam` (#1855 moved into the viewer-tracking
 *         provider, #2144 stack part 3), driven through the provider ABI
 *         exactly as the runtime drives it: negotiate with a fake host iface
 *         (settings + display geometry), probe, describe, start with a fake
 *         sink, stop. The camera is the synthetic UVC backend and the face
 *         estimator a fake that reports a known head, both injected through
 *         the provider's test seam — no real camera is ever opened.
 */

#include "catch_amalgamated.hpp"

#include "sim_face_estimator.h"
#include "sim_tracking_webcam.h"
#include "sim_webcam_tracking.h"

#include "xrt/xrt_tracking_plugin.h"
#include "os/os_time.h"
#include "util/u_stereo_uvc.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Catch::Approx;

extern "C" xrt_result_t
xrtTrackingPluginNegotiate(uint32_t runtime_api_version,
                           const struct xrt_tracking_plugin_host_iface *host,
                           struct xrt_tracking_plugin_iface **out_iface,
                           uint32_t *out_plugin_api_version);

namespace {

const float kScreenH = 0.2f;

std::map<std::string, std::string> g_settings;

bool
host_get_setting(const char *id, const char *key, char *buf, uint32_t cap)
{
	buf[0] = '\0';
	if (std::string(id) != "sim-tracking") {
		return false;
	}
	auto it = g_settings.find(key);
	if (it == g_settings.end()) {
		return false;
	}
	snprintf(buf, cap, "%s", it->second.c_str());
	return true;
}

xrt_result_t
host_geometry(xrt_tracking_host_display_geometry *g)
{
	g->display_width_m = 0.34f;
	g->display_height_m = kScreenH;
	g->nominal_viewer_x_m = 0.0f;
	g->nominal_viewer_y_m = 0.0f;
	g->nominal_viewer_z_m = 0.6f;
	return XRT_SUCCESS;
}

struct fake_estimator
{
	sim_face_estimator base;
	sim_webcam_config cfg;
	xrt_vec3 head[2];
	std::atomic<bool> face;
};

bool
fake_estimate(sim_face_estimator *est, const u_stereo_uvc_raw_frame *frame, sim_face_landmarks *out)
{
	fake_estimator *f = reinterpret_cast<fake_estimator *>(est);
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

void
fake_destroy(sim_face_estimator *)
{}

struct recorder
{
	std::mutex lock;
	std::vector<xrt_tracking_viewer_sample> samples;
};

recorder g_rec;

void
sink_push(const xrt_tracking_sink *, const xrt_tracking_viewer_sample *s)
{
	std::lock_guard<std::mutex> g(g_rec.lock);
	g_rec.samples.push_back(*s);
}

bool
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

bool
latest(xrt_tracking_viewer_sample *out)
{
	std::lock_guard<std::mutex> g(g_rec.lock);
	if (g_rec.samples.empty()) {
		return false;
	}
	*out = g_rec.samples.back();
	return true;
}

} // namespace

TEST_CASE("sim_tracking SOURCE=webcam: provider ABI end to end on a fake camera", "[sim_tracking_webcam]")
{
	xrt_tracking_plugin_host_iface host{};
	host.struct_size = sizeof(host);
	host.host_api_version = XRT_TRACKING_PLUGIN_API_VERSION_CURRENT;
	host.get_setting = host_get_setting;
	host.get_display_geometry = host_geometry;

	xrt_tracking_plugin_iface *iface = nullptr;
	uint32_t ver = 0;
	REQUIRE(xrtTrackingPluginNegotiate(XRT_TRACKING_PLUGIN_API_VERSION_CURRENT, &host, &iface, &ver) ==
	        XRT_SUCCESS);
	REQUIRE(iface != nullptr);

	u_stereo_uvc_fake_params params{};
	params.pixel = U_STEREO_UVC_PIXEL_NV12;
	params.fps = 120.0f;
	u_stereo_uvc_backend backend;
	u_stereo_uvc_fake_backend_init(&backend, &params);

	g_settings.clear();
	g_settings["ENABLE"] = "1";
	g_settings["SOURCE"] = "webcam";
	g_settings["WEBCAM_DEVICE"] = "synthetic";
	g_settings["RATE_HZ"] = "200";

	SECTION("the default build (no estimator): probe declines, the camera stays shut")
	{
		sim_tracking_webcam_set_test_overrides(&backend, nullptr);
		xrt_tracking_plugin_instance *inst = nullptr;
		CHECK(iface->probe(&inst) == XRT_ERROR_PROBER_NOT_SUPPORTED);
	}

	SECTION("SOURCE unset stays the scripted viewer, whatever the camera")
	{
		g_settings.erase("SOURCE");
		sim_tracking_webcam_set_test_overrides(&backend, nullptr);
		xrt_tracking_plugin_instance *inst = nullptr;
		REQUIRE(iface->probe(&inst) == XRT_SUCCESS);
		xrt_tracking_provider_info info{};
		info.struct_size = sizeof(info);
		REQUIRE(iface->describe(inst, &info) == XRT_SUCCESS);
		CHECK(std::string(info.source).find("scripted") == 0);
		CHECK((info.tracks & XRT_TRACKING_TRACKS_CAMERA_SPACE) == 0);
	}

	SECTION("with an estimator: probe, describe, track the head, report the camera block, lose it, stop")
	{
		sim_webcam_config cfg;
		sim_webcam_config_defaults(&cfg); // the provider reads no WEBCAM_* model knobs here: defaults
		fake_estimator est{};
		est.base.name = "fake";
		est.base.estimate = fake_estimate;
		est.base.destroy = fake_destroy;
		est.cfg = cfg;
		est.head[0] = {-0.03f - 0.0315f, 0.02f, 0.55f};
		est.head[1] = {-0.03f + 0.0315f, 0.02f, 0.55f};
		est.face = true;
		sim_tracking_webcam_set_test_overrides(&backend, &est.base);

		xrt_tracking_plugin_instance *inst = nullptr;
		REQUIRE(iface->probe(&inst) == XRT_SUCCESS);

		xrt_tracking_provider_info info{};
		info.struct_size = sizeof(info);
		REQUIRE(iface->describe(inst, &info) == XRT_SUCCESS);
		CHECK((info.tracks & XRT_TRACKING_TRACKS_EYE_PAIR) != 0);
		CHECK((info.tracks & XRT_TRACKING_TRACKS_CAMERA_SPACE) != 0);
		CHECK(std::string(info.camera_key) == "dxr-fake-uvc");
		CHECK(std::string(info.source).find("webcam") == 0);

		{
			std::lock_guard<std::mutex> g(g_rec.lock);
			g_rec.samples.clear();
		}
		xrt_tracking_sink sink{};
		sink.struct_size = sizeof(sink);
		sink.push = sink_push;
		REQUIRE(iface->start(inst, &sink) == XRT_SUCCESS);

		// start() pushed one sample synchronously: the nominal viewer, not tracking.
		xrt_tracking_viewer_sample s{};
		REQUIRE(latest(&s));
		CHECK((s.flags & XRT_TRACKING_SAMPLE_EYES_VALID) != 0);

		// Tracked, past the revival ramp: the known head, with the camera block.
		REQUIRE(wait_until(
		    [&] {
			    return latest(&s) && (s.flags & XRT_TRACKING_SAMPLE_TRACKING) != 0 &&
			           (s.flags & XRT_TRACKING_SAMPLE_CAMERA_VALID) != 0 && s.confidence > 0.999f;
		    },
		    3000));
		CHECK(iface->get_status(inst) == XRT_TRACKING_PROVIDER_STATUS_RUNNING);
		CHECK(s.struct_size == sizeof(xrt_tracking_viewer_sample));
		CHECK(s.left_eye[0] == Approx(est.head[0].x).margin(2e-3));
		CHECK(s.left_eye[1] == Approx(est.head[0].y).margin(2e-3));
		CHECK(s.left_eye[2] == Approx(est.head[0].z).margin(2e-3));
		CHECK(s.right_eye[0] == Approx(est.head[1].x).margin(2e-3));
		CHECK(s.head_position[0] == Approx((est.head[0].x + est.head[1].x) / 2).margin(2e-3));
		CHECK(s.capture_ns > 0);
		CHECK(s.capture_ns <= (int64_t)os_monotonic_get_ns());

		CHECK(std::string(s.camera.camera_key) == "dxr-fake-uvc");
		const float *dirs[2] = {s.camera.left_dir, s.camera.right_dir};
		const float dist[2] = {s.camera.left_distance_m, s.camera.right_distance_m};
		for (int e = 0; e < 2; e++) {
			const float n =
			    std::sqrt(dirs[e][0] * dirs[e][0] + dirs[e][1] * dirs[e][1] + dirs[e][2] * dirs[e][2]);
			CHECK(n == Approx(1.0f).margin(1e-4));
			// Camera at the panel's top-centre (no offset set): camera + dist * dir = the eye.
			CHECK(dist[e] * dirs[e][0] == Approx(est.head[e].x).margin(2e-3));
			CHECK(kScreenH / 2 + dist[e] * dirs[e][1] == Approx(est.head[e].y).margin(2e-3));
			CHECK(dist[e] * dirs[e][2] == Approx(est.head[e].z).margin(2e-3));
		}

		// The face leaves: tracking drops and the camera block goes away (stale).
		est.face = false;
		REQUIRE(wait_until(
		    [&] {
			    return latest(&s) && (s.flags & XRT_TRACKING_SAMPLE_TRACKING) == 0 &&
			           (s.flags & XRT_TRACKING_SAMPLE_CAMERA_VALID) == 0;
		    },
		    2000));
		CHECK((s.flags & XRT_TRACKING_SAMPLE_EYES_VALID) != 0); // MANAGED: positions stay valid

		const auto t0 = std::chrono::steady_clock::now();
		iface->stop(inst);
		CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(1000));
		CHECK(iface->get_status(inst) == XRT_TRACKING_PROVIDER_STATUS_STOPPED);
		size_t n_after_stop;
		{
			std::lock_guard<std::mutex> g(g_rec.lock);
			n_after_stop = g_rec.samples.size();
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		{
			std::lock_guard<std::mutex> g(g_rec.lock);
			CHECK(g_rec.samples.size() == n_after_stop); // no push after stop() returned
		}
		iface->destroy(inst);
	}

	SECTION("a CAMERA_POSE_<n> for the webcam's key replaces the bezel model: display = R * cam + T")
	{
		sim_webcam_config cfg;
		sim_webcam_config_defaults(&cfg);
		fake_estimator est{};
		est.base.name = "fake";
		est.base.estimate = fake_estimate;
		est.base.destroy = fake_destroy;
		est.cfg = cfg;
		est.head[0] = {0.02f - 0.0315f, -0.01f, 0.6f};
		est.head[1] = {0.02f + 0.0315f, -0.01f, 0.6f};
		est.face = true;
		sim_tracking_webcam_set_test_overrides(&backend, &est.base);

		// The camera-space point the core measures = the head relative to the
		// model's camera (top-centre, no offset): p = head - (0, H/2, 0).
		// The calibrated pose: a 10 deg pitch about +X and a translation.
		const double half = 5.0 * 3.14159265358979323846 / 180.0;
		const float T[3] = {0.05f, 0.30f, 0.01f};
		char pose[160];
		snprintf(pose, sizeof(pose), "%.6f,%.6f,%.6f,%.9f,0,0,%.9f", T[0], T[1], T[2], std::sin(half),
		         std::cos(half));
		g_settings["CAMERA_POSE_0_CAMERA"] = "some-other-camera"; // not ours: skipped
		g_settings["CAMERA_POSE_0"] = "1,1,1,0,0,0,1";
		g_settings["CAMERA_POSE_1_CAMERA"] = "dxr-fake-uvc"; // the synthetic camera's key
		g_settings["CAMERA_POSE_1"] = pose;

		xrt_tracking_plugin_instance *inst = nullptr;
		REQUIRE(iface->probe(&inst) == XRT_SUCCESS);
		{
			std::lock_guard<std::mutex> g(g_rec.lock);
			g_rec.samples.clear();
		}
		xrt_tracking_sink sink{};
		sink.struct_size = sizeof(sink);
		sink.push = sink_push;
		REQUIRE(iface->start(inst, &sink) == XRT_SUCCESS);

		xrt_tracking_viewer_sample s{};
		REQUIRE(wait_until(
		    [&] {
			    return latest(&s) && (s.flags & XRT_TRACKING_SAMPLE_TRACKING) != 0 && s.confidence > 0.999f;
		    },
		    3000));
		const double c = std::cos(2 * half), sn = std::sin(2 * half);
		const float *got[2] = {s.left_eye, s.right_eye};
		for (int e = 0; e < 2; e++) {
			const double p[3] = {est.head[e].x, est.head[e].y - kScreenH / 2, est.head[e].z};
			const double want[3] = {T[0] + p[0], T[1] + c * p[1] - sn * p[2], T[2] + sn * p[1] + c * p[2]};
			CHECK(got[e][0] == Approx(want[0]).margin(2e-3));
			CHECK(got[e][1] == Approx(want[1]).margin(2e-3));
			CHECK(got[e][2] == Approx(want[2]).margin(2e-3));
		}
		// The camera block stays the raw camera-space measurement (the calibration's input).
		CHECK(s.camera.left_distance_m * s.camera.left_dir[2] == Approx(est.head[0].z).margin(2e-3));
		iface->stop(inst);
		iface->destroy(inst);
	}

	SECTION("an unknown camera: probe declines even with an estimator")
	{
		fake_estimator est{};
		est.base.name = "fake";
		est.base.estimate = fake_estimate;
		est.base.destroy = fake_destroy;
		sim_tracking_webcam_set_test_overrides(&backend, &est.base);
		g_settings["WEBCAM_DEVICE"] = "no such camera";
		xrt_tracking_plugin_instance *inst = nullptr;
		CHECK(iface->probe(&inst) == XRT_ERROR_PROBER_NOT_SUPPORTED);
	}

	sim_tracking_webcam_set_test_overrides(nullptr, nullptr);
}
