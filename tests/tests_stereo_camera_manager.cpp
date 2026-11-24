// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_stereo_camera (ADR-043 Amendment 4) END TO END through the
 *         service's REAL camera manager (ipc_server_stereo_camera.c, its IPC
 *         handlers called directly) with the UVC source on its fake backend —
 *         no Media Foundation, no device, no service process, so it runs on
 *         every CI host:
 *
 *  - enumerate: one camera, source UVC, no CALIBRATED, nominal baseline/HFOV
 *    reported only because the config gives them;
 *  - calibration: RAW refused (a nominal model is no device calibration),
 *    RECTIFIED = the nominal pinhole (pass-through, no zoom);
 *  - a RECTIFIED NV12 stream starts through the consent path (stored Allow),
 *    frames arrive in the read-only ring, the online refinement APPLIES a
 *    correction for the fake's 2.4 px vertical misalignment, the calibration
 *    generation moves, and the delivered frames' rows align (<= 0.3 px);
 *  - RAW streams are delivered RAW-flagged with the misalignment intact.
 *
 * And (#1961) re-enumeration over a fake display plug-in whose camera list the
 * test edits: unchanged cameras keep index + id + persistentId, a new one is
 * appended, a vanished one turns UNAVAILABLE in place (its streams end with
 * SOURCE_LOST, start is BUSY) and comes back with its id, every change queues
 * one CAMERAS_CHANGED to a connection that enumerated, and the client-path
 * rescan is throttled.
 */

#include "catch_amalgamated.hpp"

#include "tests_stereo_camera_manager_glue.h"

#include "server/ipc_server_stereo_camera.h" // the in-process consumer API

#include "os/os_time.h"
#include "util/u_app_share_fake.h"
#include "util/u_camera_consent.h"
#include "util/u_stereo_camera.h"
#include "xrt/xrt_instance.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

double
median(std::vector<double> v)
{
	if (v.empty()) {
		return 0.0;
	}
	std::sort(v.begin(), v.end());
	return v[v.size() / 2];
}

//! Signed median dy (right - left) over a block grid of an NV12/GRAY8 SBS luma plane.
double
row_offset(const uint8_t *y, uint32_t pitch, uint32_t ew, uint32_t eh, size_t *out_blocks)
{
	std::vector<double> dy;
	for (uint32_t by = 40; by + 32 + 40 < eh; by += 48) {
		for (uint32_t bx = 56; bx + 32 + 8 < ew; bx += 48) {
			float fx = 0.0f, fy = 0.0f;
			if (u_stereo_camera_estimate_offset(y, pitch, ew, eh, bx, by, 32, 32, 48, 8, &fx, &fy)) {
				dy.push_back(fy);
			}
		}
	}
	*out_blocks = dy.size();
	return median(dy);
}

u_stereo_uvc_config
fake_config()
{
	u_stereo_uvc_config cfg;
	char err[256] = {0};
	const char *json = R"({"fake": {"disparity": 0.04, "dy": 0.005, "fps": 60},
	    "uvc": [{"match": "synthetic", "name": "Test SBS webcam", "mode": "1280x480@60",
	             "baseline_mm": 60, "hfov_deg": 70}]})";
	bool ok = u_stereo_uvc_config_parse(json, &cfg, err, sizeof(err));
	INFO(err);
	REQUIRE(ok);
	return cfg;
}

} // namespace

TEST_CASE("manager + UVC fake: enumerate and calibration", "[stereo_camera_manager]")
{
	u_stereo_uvc_config cfg = fake_config();
	scm_fixture *f = scm_create(&cfg);
	REQUIRE(f != nullptr);
	REQUIRE(scm_count(f) == 1);
	xrt_stereo_camera_properties p;
	REQUIRE(scm_properties(f, 0, &p) == XRT_SUCCESS);
	CHECK(p.source == XRT_STEREO_CAMERA_SOURCE_UVC);
	CHECK(std::string(p.display_name) == "Test SBS webcam");
	CHECK((p.flags & XRT_PLUGIN_STEREO_CAMERA_CALIBRATED) == 0);
	CHECK((p.flags & XRT_PLUGIN_STEREO_CAMERA_NATIVELY_RECTIFIED) == 0);
	// Spec v7: not CALIBRATED, yet the service rectifies the nominal model, so a
	// RECTIFIED stream delivers rectified frames and the camera says so up front.
	CHECK((p.flags & XRT_PLUGIN_STEREO_CAMERA_RECTIFIABLE) != 0);
	// Spec v7: a desktop SBS webcam claims no frame orientation.
	CHECK(p.frame_orientation_deg == XRT_STEREO_CAMERA_FRAME_ORIENTATION_UNKNOWN);
	CHECK(p.eye_width == 640);
	CHECK(p.eye_height == 480);
	CHECK(p.view_count == 2);
	CHECK(p.baseline_mm == Catch::Approx(60.0f));
	CHECK(p.horizontal_fov_deg == Catch::Approx(70.0f).margin(0.01));
	CHECK(std::string(p.platform_device_hint) == "dxr-fake-uvc");
	CHECK(std::string(p.persistent_id).rfind("dxrcam-", 0) == 0);

	xrt_stereo_camera_calibration c;
	CHECK(scm_calibration(f, p.camera_id, XRT_STEREO_CAMERA_OUTPUT_RAW, &c) == XRT_ERROR_FEATURE_NOT_SUPPORTED);
	REQUIRE(scm_calibration(f, p.camera_id, XRT_STEREO_CAMERA_OUTPUT_RECTIFIED, &c) == XRT_SUCCESS);
	const double f_nom = 320.0 / std::tan(35.0 * 3.14159265358979323846 / 180.0);
	CHECK(c.eye[0].fx == Catch::Approx(f_nom).epsilon(1e-6)); // pass-through: no alpha = 0 zoom
	CHECK(c.eye[1].fy == Catch::Approx(f_nom).epsilon(1e-6));
	CHECK(c.eye[0].cx == Catch::Approx(319.5));
	CHECK(c.eye[0].cy == Catch::Approx(239.5));
	CHECK(c.eye[0].model == XRT_PLUGIN_STEREO_CAMERA_DISTORTION_NONE);
	CHECK(c.baseline_mm == Catch::Approx(60.0f));
	CHECK(c.position[0] == Catch::Approx(0.060f).margin(1e-5)); // right camera at +x, metres
	CHECK(c.orientation[3] == 1.0f);
	scm_destroy(f);
}

TEST_CASE("manager + UVC fake: RECTIFIED stream converges, RAW stays raw", "[stereo_camera_manager]")
{
	u_stereo_uvc_config cfg = fake_config();
	scm_fixture *f = scm_create(&cfg);
	REQUIRE(f != nullptr);
	REQUIRE(scm_count(f) == 1);
	xrt_stereo_camera_properties p;
	REQUIRE(scm_properties(f, 0, &p) == XRT_SUCCESS);

	SECTION("RECTIFIED")
	{
		xrt_stereo_camera_stream_request req = {p.camera_id, XRT_STEREO_CAMERA_OUTPUT_RECTIFIED,
		                                        XRT_STEREO_CAMERA_FORMAT_NV12,
		                                        XRT_STEREO_CAMERA_TRANSPORT_SHARED_MEMORY, 0.0f};
		uint64_t id = 0;
		REQUIRE(scm_stream_create(f, &req, &id) == XRT_SUCCESS);
		REQUIRE(scm_stream_start(f, id) == XRT_SUCCESS);
		xrt_stereo_camera_stream_layout lay;
		REQUIRE(scm_stream_map(f, id, &lay) == XRT_SUCCESS);
		CHECK(lay.width == 1280);
		CHECK(lay.height == 480);
		CHECK(lay.output == XRT_STEREO_CAMERA_OUTPUT_RECTIFIED);

		// Run until the refinement applied a correction AND a frame of the new
		// generation arrived (fast phase: a measurement every 250 ms), max 10 s.
		const int64_t t_end = os_monotonic_get_ns() + 10ll * 1000 * 1000 * 1000;
		uint32_t frames = 0, first_gen = UINT32_MAX, last_gen = 0;
		double first_dy = 0.0, last_dy = 99.0;
		size_t blocks = 0;
		xrt_stereo_camera_stream_stats st;
		std::memset(&st, 0, sizeof(st));
		while (os_monotonic_get_ns() < t_end) {
			bool ready = false;
			xrt_stereo_camera_frame_info fi;
			const uint8_t *slot = nullptr;
			REQUIRE(scm_acquire(f, id, &ready, &fi, &slot) == XRT_SUCCESS);
			if (!ready) {
				os_nanosleep(5 * 1000 * 1000);
				continue;
			}
			REQUIRE(slot != nullptr);
			CHECK(fi.output == XRT_STEREO_CAMERA_OUTPUT_RECTIFIED);
			CHECK(fi.format == XRT_STEREO_CAMERA_FORMAT_NV12);
			frames++;
			if (first_gen == UINT32_MAX) {
				first_gen = fi.calibration_generation;
				first_dy = row_offset(slot + fi.plane_offset[0], fi.row_pitch[0], 640, 480, &blocks);
			}
			last_gen = fi.calibration_generation;
			REQUIRE(scm_stats(f, id, &st) == XRT_SUCCESS);
			if (st.refine_state == XRT_STEREO_CAMERA_REFINE_APPLIED && last_gen != first_gen) {
				last_dy = row_offset(slot + fi.plane_offset[0], fi.row_pitch[0], 640, 480, &blocks);
				if (std::fabs(last_dy) <= 0.3) {
					break;
				}
			}
		}
		std::printf(
		    "[manager uvc] %u frames; refine state %u, %u update(s), applied a %+.3f px b %+.3f/100px; "
		    "generation %u -> %u; delivered dy %+.3f -> %+.3f px (%zu blocks); source %.1f Hz\n",
		    frames, st.refine_state, st.refine_updates, st.refine_offset_px, st.refine_slope_per_100px,
		    first_gen, last_gen, first_dy, last_dy, blocks, st.source_frame_rate);
		CHECK(frames > 10);
		CHECK(first_dy == Catch::Approx(2.4).margin(0.35)); // pass-through: the device's own misalignment
		CHECK(st.refine_state == XRT_STEREO_CAMERA_REFINE_APPLIED);
		CHECK(last_gen != first_gen);
		CHECK(std::fabs(last_dy) <= 0.3);

		// The rectified calibration now describes the corrected maps.
		xrt_stereo_camera_calibration c;
		REQUIRE(scm_calibration(f, p.camera_id, XRT_STEREO_CAMERA_OUTPUT_RECTIFIED, &c) == XRT_SUCCESS);
		CHECK(c.eye[0].fx > 0.0f);
		CHECK(c.eye[0].fx == c.eye[1].fx);
		CHECK(c.baseline_mm == Catch::Approx(60.0f).margin(0.5));
		CHECK(scm_stream_destroy(f, id) == XRT_SUCCESS);
	}
	SECTION("RAW")
	{
		xrt_stereo_camera_stream_request req = {p.camera_id, XRT_STEREO_CAMERA_OUTPUT_RAW,
		                                        XRT_STEREO_CAMERA_FORMAT_GRAY8,
		                                        XRT_STEREO_CAMERA_TRANSPORT_SHARED_MEMORY, 0.0f};
		uint64_t id = 0;
		REQUIRE(scm_stream_create(f, &req, &id) == XRT_SUCCESS);
		REQUIRE(scm_stream_start(f, id) == XRT_SUCCESS);
		xrt_stereo_camera_stream_layout lay;
		REQUIRE(scm_stream_map(f, id, &lay) == XRT_SUCCESS);
		const int64_t t_end = os_monotonic_get_ns() + 5ll * 1000 * 1000 * 1000;
		bool got = false;
		while (!got && os_monotonic_get_ns() < t_end) {
			bool ready = false;
			xrt_stereo_camera_frame_info fi;
			const uint8_t *slot = nullptr;
			REQUIRE(scm_acquire(f, id, &ready, &fi, &slot) == XRT_SUCCESS);
			if (!ready) {
				os_nanosleep(5 * 1000 * 1000);
				continue;
			}
			got = true;
			CHECK(fi.output == XRT_STEREO_CAMERA_OUTPUT_RAW);
			CHECK(fi.format == XRT_STEREO_CAMERA_FORMAT_GRAY8);
			size_t blocks = 0;
			double dy = row_offset(slot + fi.plane_offset[0], fi.row_pitch[0], 640, 480, &blocks);
			CHECK(blocks > 20);
			CHECK(dy == Catch::Approx(2.4).margin(0.35));
		}
		CHECK(got);
		xrt_stereo_camera_stream_stats st;
		REQUIRE(scm_stats(f, id, &st) == XRT_SUCCESS);
		CHECK(st.refine_state == XRT_STEREO_CAMERA_REFINE_NONE);
		CHECK(scm_stream_destroy(f, id) == XRT_SUCCESS);
	}
	scm_destroy(f);
}


namespace {

struct props_t
{
	uint64_t id;
	std::string pid;
	std::string name;
	uint32_t state;
};

props_t
props_at(scm_fixture *f, uint32_t index)
{
	xrt_stereo_camera_properties p;
	REQUIRE(scm_properties(f, index, &p) == XRT_SUCCESS);
	return props_t{p.camera_id, p.persistent_id, p.display_name, p.state};
}

//! Drain every queued event; count those of @p kind.
uint32_t
drain(scm_fixture *f, uint32_t kind)
{
	uint32_t n = 0;
	xrt_stereo_camera_event ev;
	while (scm_poll_event(f, &ev)) {
		n += ev.kind == kind ? 1u : 0u;
	}
	return n;
}

} // namespace

TEST_CASE("tracker-camera denylist: a display plug-in's declared camera is never claimed",
          "[stereo_camera_manager][denylist]")
{
	// ADR-043 Amendment 5: the config names the device EXPLICITLY, yet the
	// active plug-in declares it as its tracker's camera -> refused.
	u_stereo_uvc_config cfg = fake_config();
	const char *claimed[] = {"dxr-fake-uvc"};
	scm_fake_claims_set(claimed, 1);
	scm_fixture *f = scm_create_claiming_plugin(&cfg);
	REQUIRE(f != nullptr);
	CHECK(scm_count(f) == 0);

	// The plug-in stops declaring it (another camera now): the webcam is ours.
	const char *other[] = {"04e8:20d7"};
	scm_fake_claims_set(other, 1);
	CHECK(scm_rescan(f));
	REQUIRE(scm_count(f) == 1);
	xrt_stereo_camera_properties p;
	REQUIRE(scm_properties(f, 0, &p) == XRT_SUCCESS);
	CHECK(p.source == XRT_STEREO_CAMERA_SOURCE_UVC);

	// Declared again: the camera goes away on the next rescan.
	scm_fake_claims_set(claimed, 1);
	CHECK(scm_rescan(f));
	REQUIRE(scm_properties(f, 0, &p) == XRT_SUCCESS);
	CHECK(p.state == XRT_STEREO_CAMERA_STATE_UNAVAILABLE);
	scm_destroy(f);
	scm_fake_claims_set(nullptr, 0);
}

TEST_CASE("re-enumeration: stable ids, appended, vanished in place, event", "[stereo_camera_manager][rescan]")
{
	const char *ids_a[] = {"test:A"};
	const char *names_a[] = {"Camera - Laptop"};
	scm_fake_plugin_set(ids_a, names_a, 1);
	scm_fixture *f = scm_create_fake_plugin();
	REQUIRE(f != nullptr);
	REQUIRE(scm_count(f) == 1); // also subscribes this connection
	const props_t a0 = props_at(f, 0);
	CHECK(a0.name == "Camera - Laptop");
	CHECK(a0.state == XRT_STEREO_CAMERA_STATE_AVAILABLE);
	CHECK(drain(f, XRT_STEREO_CAMERA_EVENT_CAMERAS_CHANGED) == 0);

	// Unchanged: no change, no event.
	CHECK_FALSE(scm_rescan(f));
	CHECK(drain(f, XRT_STEREO_CAMERA_EVENT_CAMERAS_CHANGED) == 0);

	// A second screen's camera appears: appended, A untouched, ONE event.
	const char *ids_ab[] = {"test:A", "test:B"};
	const char *names_ab[] = {"Camera - Laptop", "Camera - DS1"};
	scm_fake_plugin_set(ids_ab, names_ab, 2);
	CHECK(scm_rescan(f));
	CHECK(drain(f, XRT_STEREO_CAMERA_EVENT_CAMERAS_CHANGED) == 1);
	REQUIRE(scm_count(f) == 2);
	const props_t a1 = props_at(f, 0);
	const props_t b1 = props_at(f, 1);
	CHECK(a1.id == a0.id);
	CHECK(a1.pid == a0.pid);
	CHECK(b1.id != a0.id);
	CHECK(b1.pid != a0.pid);
	CHECK(b1.name == "Camera - DS1");

	// A disappears and B moves to the plug-in's index 0: nothing renumbers,
	// A is UNAVAILABLE in place.
	const char *ids_b[] = {"test:B"};
	const char *names_b[] = {"Camera - DS1"};
	scm_fake_plugin_set(ids_b, names_b, 1);
	CHECK(scm_rescan(f));
	CHECK(drain(f, XRT_STEREO_CAMERA_EVENT_CAMERAS_CHANGED) == 1);
	REQUIRE(scm_count(f) == 2);
	const props_t a2 = props_at(f, 0);
	const props_t b2 = props_at(f, 1);
	CHECK(a2.id == a0.id);
	CHECK(a2.state == XRT_STEREO_CAMERA_STATE_UNAVAILABLE);
	CHECK(b2.id == b1.id);
	CHECK(b2.pid == b1.pid);
	CHECK(b2.state == XRT_STEREO_CAMERA_STATE_AVAILABLE);

	// A comes back (now at the plug-in's index 1): same slot, same id.
	const char *ids_ba[] = {"test:B", "test:A"};
	const char *names_ba[] = {"Camera - DS1", "Camera - Laptop (renamed)"};
	scm_fake_plugin_set(ids_ba, names_ba, 2);
	CHECK(scm_rescan(f));
	CHECK(drain(f, XRT_STEREO_CAMERA_EVENT_CAMERAS_CHANGED) == 1);
	REQUIRE(scm_count(f) == 2);
	const props_t a3 = props_at(f, 0);
	CHECK(a3.id == a0.id);
	CHECK(a3.pid == a0.pid);
	CHECK(a3.state == XRT_STEREO_CAMERA_STATE_AVAILABLE);
	CHECK(a3.name == "Camera - Laptop (renamed)"); // labels follow the source
	CHECK(props_at(f, 1).id == b1.id);

	// The sharing toggle changes the enumerated set too.
	REQUIRE(scm_set_sharing(f, false) == XRT_SUCCESS);
	CHECK(scm_count(f) == 0);
	CHECK(drain(f, XRT_STEREO_CAMERA_EVENT_CAMERAS_CHANGED) == 1);
	REQUIRE(scm_set_sharing(f, true) == XRT_SUCCESS);
	CHECK(drain(f, XRT_STEREO_CAMERA_EVENT_CAMERAS_CHANGED) == 1);
	CHECK(scm_count(f) == 2);
	scm_destroy(f);
}

TEST_CASE("re-enumeration: a vanished camera ends its streams", "[stereo_camera_manager][rescan]")
{
	const char *ids_a[] = {"test:A"};
	const char *names_a[] = {"Camera A"};
	scm_fake_plugin_set(ids_a, names_a, 1);
	scm_fixture *f = scm_create_fake_plugin();
	REQUIRE(f != nullptr);
	REQUIRE(scm_count(f) == 1);
	const props_t a = props_at(f, 0);
	xrt_stereo_camera_stream_request req = {a.id, XRT_STEREO_CAMERA_OUTPUT_RAW, XRT_STEREO_CAMERA_FORMAT_GRAY8,
	                                        XRT_STEREO_CAMERA_TRANSPORT_SHARED_MEMORY, 0.0f};
	uint64_t sid = 0;
	REQUIRE(scm_stream_create(f, &req, &sid) == XRT_SUCCESS);
	REQUIRE(scm_stream_start(f, sid) == XRT_SUCCESS);
	(void)drain(f, 0); // whatever the start queued

	scm_fake_plugin_set(nullptr, nullptr, 0);
	CHECK(scm_rescan(f));
	xrt_stereo_camera_event ended = {};
	uint32_t n_ended = 0, n_changed = 0;
	xrt_stereo_camera_event ev;
	while (scm_poll_event(f, &ev)) {
		if (ev.kind == XRT_STEREO_CAMERA_EVENT_STREAM_ENDED) {
			n_ended++;
			ended = ev;
		} else if (ev.kind == XRT_STEREO_CAMERA_EVENT_CAMERAS_CHANGED) {
			n_changed++;
		}
	}
	CHECK(n_ended == 1);
	CHECK(ended.stream_id == sid);
	CHECK(ended.value == XRT_STEREO_CAMERA_END_SOURCE_LOST);
	CHECK(n_changed == 1);
	bool ready = true;
	xrt_stereo_camera_frame_info fi;
	const uint8_t *slot = nullptr;
	CHECK(scm_acquire(f, sid, &ready, &fi, &slot) == XRT_ERROR_STEREO_CAMERA_STREAM_ENDED);
	CHECK(scm_stream_destroy(f, sid) == XRT_SUCCESS);

	// Still enumerated (UNAVAILABLE); a new stream on it is BUSY until it is back.
	REQUIRE(scm_count(f) == 1);
	CHECK(props_at(f, 0).state == XRT_STEREO_CAMERA_STATE_UNAVAILABLE);
	REQUIRE(scm_stream_create(f, &req, &sid) == XRT_SUCCESS);
	CHECK(scm_stream_start(f, sid) == XRT_ERROR_STEREO_CAMERA_BUSY);
	scm_fake_plugin_set(ids_a, names_a, 1);
	CHECK(scm_rescan(f));
	CHECK(scm_stream_start(f, sid) == XRT_SUCCESS);
	CHECK(scm_stream_destroy(f, sid) == XRT_SUCCESS);
	scm_destroy(f);
}

TEST_CASE("A2 front-camera rule: no live weave is BUSY, with or without the developer override",
          "[stereo_camera_manager][android]")
{
	// The fake plug-in's cameras are USER_FACING. Decision 2026-10-31: the
	// developer override (DXR_STEREO_CAMERA_DEV_ALLOW) admits a debug-signed
	// browser but no longer lifts this rule.
	const char *ids_a[] = {"test:front"};
	const char *names_a[] = {"Front 3D camera"};
	for (int32_t dev : {1, -1}) {
		DYNAMIC_SECTION("dev override " << (dev > 0 ? "on" : "off"))
		{
			scm_fake_plugin_set(ids_a, names_a, 1);
			scm_set_front_rule(true);
			scm_fixture *f = scm_create_fake_plugin_ex(dev);
			REQUIRE(f != nullptr);
			REQUIRE(scm_count(f) == 1);
			const props_t a = props_at(f, 0);
			xrt_stereo_camera_stream_request req = {a.id, XRT_STEREO_CAMERA_OUTPUT_RAW,
			                                        XRT_STEREO_CAMERA_FORMAT_GRAY8,
			                                        XRT_STEREO_CAMERA_TRANSPORT_SHARED_MEMORY, 0.0f};
			uint64_t sid = 0;
			REQUIRE(scm_stream_create(f, &req, &sid) == XRT_SUCCESS);
			// No connection of this app weaves: the front camera is BUSY.
			scm_set_live_weave(f, false);
			CHECK(scm_stream_start(f, sid) == XRT_ERROR_STEREO_CAMERA_BUSY);
			// The same app presents woven 3D (a submit within the last second): it starts.
			scm_set_live_weave(f, true);
			CHECK(scm_stream_start(f, sid) == XRT_SUCCESS);
			CHECK(scm_stream_destroy(f, sid) == XRT_SUCCESS);
			scm_destroy(f);
			scm_set_front_rule(false);
		}
	}
	// Without the platform rule (desktop) a user-facing camera has no such gate.
	scm_fake_plugin_set(ids_a, names_a, 1);
	scm_fixture *f = scm_create_fake_plugin_ex(-1);
	REQUIRE(f != nullptr);
	REQUIRE(scm_count(f) == 1);
	xrt_stereo_camera_stream_request req = {props_at(f, 0).id, XRT_STEREO_CAMERA_OUTPUT_RAW,
	                                        XRT_STEREO_CAMERA_FORMAT_GRAY8,
	                                        XRT_STEREO_CAMERA_TRANSPORT_SHARED_MEMORY, 0.0f};
	uint64_t sid = 0;
	REQUIRE(scm_stream_create(f, &req, &sid) == XRT_SUCCESS);
	CHECK(scm_stream_start(f, sid) == XRT_SUCCESS);
	CHECK(scm_stream_destroy(f, sid) == XRT_SUCCESS);
	scm_destroy(f);
}

TEST_CASE("A2 front-camera rule: a weave that starts just after the request lets the start through",
          "[stereo_camera_manager][android]")
{
	// A browser asks for the front camera while its page is still loading (or
	// right as it returns from the background), ~100 ms before its first weave
	// submit. The start waits a short grace for that weave instead of refusing.
	const char *ids_a[] = {"test:front"};
	const char *names_a[] = {"Front 3D camera"};
	scm_fake_plugin_set(ids_a, names_a, 1);
	scm_set_front_rule(true);
	scm_fixture *f = scm_create_fake_plugin_ex(-1);
	REQUIRE(f != nullptr);
	REQUIRE(scm_count(f) == 1);
	xrt_stereo_camera_stream_request req = {props_at(f, 0).id, XRT_STEREO_CAMERA_OUTPUT_RAW,
	                                        XRT_STEREO_CAMERA_FORMAT_GRAY8,
	                                        XRT_STEREO_CAMERA_TRANSPORT_SHARED_MEMORY, 0.0f};
	uint64_t sid = 0;
	REQUIRE(scm_stream_create(f, &req, &sid) == XRT_SUCCESS);
	scm_set_live_weave(f, false);
	std::thread weaver([f] {
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		scm_set_live_weave(f, true);
	});
	CHECK(scm_stream_start(f, sid) == XRT_SUCCESS);
	weaver.join();
	CHECK(scm_stream_destroy(f, sid) == XRT_SUCCESS);
	scm_destroy(f);
	scm_set_front_rule(false);
}

TEST_CASE("A2 front-camera rule: a weave present-owner on top starts before its first weave",
          "[stereo_camera_manager][android]")
{
	// A call page's only 3D content is the camera preview, so the browser
	// cannot submit a weave before it has frames. Its present-owner connection
	// counts while the app is on top; not on top, it stays BUSY.
	const char *ids_a[] = {"test:front"};
	const char *names_a[] = {"Front 3D camera"};
	scm_fake_plugin_set(ids_a, names_a, 1);
	scm_set_front_rule(true);
	scm_fixture *f = scm_create_fake_plugin_ex(-1);
	REQUIRE(f != nullptr);
	REQUIRE(scm_count(f) == 1);
	xrt_stereo_camera_stream_request req = {props_at(f, 0).id, XRT_STEREO_CAMERA_OUTPUT_RAW,
	                                        XRT_STEREO_CAMERA_FORMAT_GRAY8,
	                                        XRT_STEREO_CAMERA_TRANSPORT_SHARED_MEMORY, 0.0f};
	uint64_t sid = 0;
	REQUIRE(scm_stream_create(f, &req, &sid) == XRT_SUCCESS);
	scm_set_present_owner(f, true, false);
	CHECK(scm_stream_start(f, sid) == XRT_ERROR_STEREO_CAMERA_BUSY);
	scm_set_present_owner(f, false, true);
	CHECK(scm_stream_start(f, sid) == XRT_ERROR_STEREO_CAMERA_BUSY);
	scm_set_present_owner(f, true, true);
	CHECK(scm_stream_start(f, sid) == XRT_SUCCESS);
	CHECK(scm_stream_destroy(f, sid) == XRT_SUCCESS);
	scm_destroy(f);
	scm_set_front_rule(false);
}

TEST_CASE("re-enumeration: each camera reports its screen, and a move is a change", "[stereo_camera_manager][rescan]")
{
	const char *ids_ab[] = {"test:A", "test:B"};
	const char *names_ab[] = {"Camera - Laptop", "Camera - DS1"};
	scm_fake_plugin_set(ids_ab, names_ab, 2);
	const uint64_t mon_ab[] = {0x1111000000000001ull, 0x2222000000000002ull};
	scm_fake_plugin_set_monitors(mon_ab, 2);
	scm_fixture *f = scm_create_fake_plugin();
	REQUIRE(f != nullptr);
	REQUIRE(scm_count(f) == 2);
	xrt_stereo_camera_properties p;
	REQUIRE(scm_properties(f, 0, &p) == XRT_SUCCESS);
	CHECK(p.display_id == mon_ab[0]);
	REQUIRE(scm_properties(f, 1, &p) == XRT_SUCCESS);
	CHECK(p.display_id == mon_ab[1]);
	CHECK(drain(f, XRT_STEREO_CAMERA_EVENT_CAMERAS_CHANGED) == 0);

	// Unchanged screens: no change.
	CHECK_FALSE(scm_rescan(f));

	// B's screen is re-keyed (display moved port): same camera, new screen, ONE event.
	const uint64_t mon_ab2[] = {mon_ab[0], 0x3333000000000003ull};
	scm_fake_plugin_set_monitors(mon_ab2, 2);
	CHECK(scm_rescan(f));
	CHECK(drain(f, XRT_STEREO_CAMERA_EVENT_CAMERAS_CHANGED) == 1);
	REQUIRE(scm_properties(f, 1, &p) == XRT_SUCCESS);
	CHECK(p.display_id == mon_ab2[1]);
	CHECK(std::string(p.display_name) == "Camera - DS1");

	// A plug-in that does not know the screen reports 0.
	scm_fake_plugin_set(ids_ab, names_ab, 2);
	CHECK(scm_rescan(f));
	REQUIRE(scm_properties(f, 0, &p) == XRT_SUCCESS);
	CHECK(p.display_id == 0);
	scm_destroy(f);
}

TEST_CASE("spec v7: properties carry the frame orientation, unknown when the plug-in is silent",
          "[stereo_camera_manager][orientation]")
{
	const char *ids_ab[] = {"test:front", "test:rear"};
	const char *names_ab[] = {"3D Camera", "3D Camera, rear"};
	scm_fake_plugin_set(ids_ab, names_ab, 2);

	// A plug-in that does not write the field (or predates it): UNKNOWN, never 0,
	// so a consumer keeps its own fallback.
	scm_fixture *f = scm_create_fake_plugin();
	REQUIRE(f != nullptr);
	REQUIRE(scm_count(f) == 2);
	xrt_stereo_camera_properties p;
	REQUIRE(scm_properties(f, 0, &p) == XRT_SUCCESS);
	CHECK(p.frame_orientation_deg == XRT_STEREO_CAMERA_FRAME_ORIENTATION_UNKNOWN);
	REQUIRE(scm_properties(f, 1, &p) == XRT_SUCCESS);
	CHECK(p.frame_orientation_deg == XRT_STEREO_CAMERA_FRAME_ORIENTATION_UNKNOWN);
	scm_destroy(f);

	// A portrait-natural tablet: unturned frames, front sensor 270, rear 90.
	const uint32_t tablet[] = {XRT_PLUGIN_STEREO_CAMERA_FRAME_ORIENTATION_270,
	                           XRT_PLUGIN_STEREO_CAMERA_FRAME_ORIENTATION_90};
	scm_fake_plugin_set(ids_ab, names_ab, 2);
	scm_fake_plugin_set_orientations(tablet, 2);
	f = scm_create_fake_plugin();
	REQUIRE(f != nullptr);
	REQUIRE(scm_count(f) == 2); // enumerated: this instance now hears CAMERAS_CHANGED
	REQUIRE(scm_properties(f, 0, &p) == XRT_SUCCESS);
	CHECK(p.frame_orientation_deg == 270);
	REQUIRE(scm_properties(f, 1, &p) == XRT_SUCCESS);
	CHECK(p.frame_orientation_deg == 90);
	CHECK(drain(f, XRT_STEREO_CAMERA_EVENT_CAMERAS_CHANGED) == 0);
	CHECK_FALSE(scm_rescan(f));

	// The plug-in learns / changes it (a pair it now turns upright itself): 0 is
	// a real value, and the change is reported like a label change.
	const uint32_t turned[] = {XRT_PLUGIN_STEREO_CAMERA_FRAME_ORIENTATION_0,
	                           XRT_PLUGIN_STEREO_CAMERA_FRAME_ORIENTATION_90};
	scm_fake_plugin_set_orientations(turned, 2);
	CHECK(scm_rescan(f));
	CHECK(drain(f, XRT_STEREO_CAMERA_EVENT_CAMERAS_CHANGED) == 1);
	REQUIRE(scm_properties(f, 0, &p) == XRT_SUCCESS);
	CHECK(p.frame_orientation_deg == 0);

	// A value outside the enum is a plug-in bug: unknown, never a turn.
	const uint32_t bogus[] = {7u, XRT_PLUGIN_STEREO_CAMERA_FRAME_ORIENTATION_180};
	scm_fake_plugin_set_orientations(bogus, 2);
	CHECK(scm_rescan(f));
	REQUIRE(scm_properties(f, 0, &p) == XRT_SUCCESS);
	CHECK(p.frame_orientation_deg == XRT_STEREO_CAMERA_FRAME_ORIENTATION_UNKNOWN);
	REQUIRE(scm_properties(f, 1, &p) == XRT_SUCCESS);
	CHECK(p.frame_orientation_deg == 180);
	scm_destroy(f);
}

TEST_CASE("re-enumeration: the enumerate path is throttled", "[stereo_camera_manager][rescan]")
{
	const char *ids_a[] = {"test:A"};
	const char *names_a[] = {"Camera A"};
	scm_fake_plugin_set(ids_a, names_a, 1);
	scm_fixture *f = scm_create_fake_plugin();
	REQUIRE(f != nullptr);
	const uint32_t calls = scm_fake_plugin_enumerate_calls();
	// Within a second of the create-time enumeration, enumerate does not re-query.
	for (int i = 0; i < 5; i++) {
		CHECK(scm_count(f) == 1);
	}
	CHECK(scm_fake_plugin_enumerate_calls() == calls);
	// After the window it does, and sees the new camera.
	const char *ids_ab[] = {"test:A", "test:B"};
	const char *names_ab[] = {"Camera A", "Camera B"};
	scm_fake_plugin_set(ids_ab, names_ab, 2);
	os_nanosleep(1100ll * 1000 * 1000);
	CHECK(scm_count(f) == 2);
	CHECK(scm_fake_plugin_enumerate_calls() == calls + 1);
	scm_destroy(f);
}

TEST_CASE(
    "manager + UVC fake: a source that cannot start ends the stream with SOURCE_BUSY, start is BUSY, and "
    "it recovers (#2221)",
    "[stereo_camera_manager][busy]")
{
	// Three shapes of "the device is not really ours": open fails; open succeeds and the
	// first read errors (Media Foundation on a device another process holds); open
	// succeeds and nothing ever arrives (the UVC first-frame deadline, 5 s).
	struct fault_t
	{
		uint32_t fault;
		const char *name;
		double max_wait_s;
	};
	const fault_t faults[] = {
	    {U_STEREO_UVC_FAKE_FAULT_READ_ERROR, "read-error", 4.0},
	    {U_STEREO_UVC_FAKE_FAULT_OPEN_FAILS, "open-fails", 4.0},
	    {U_STEREO_UVC_FAKE_FAULT_NEVER_DELIVERS, "never-delivers", 9.0},
	};
	for (const fault_t &fl : faults) {
		DYNAMIC_SECTION("fault " << fl.name)
		{
			u_stereo_uvc_fake_set_fault(fl.fault);
			u_stereo_uvc_config cfg = fake_config();
			scm_fixture *f = scm_create(&cfg);
			REQUIRE(f != nullptr);
			REQUIRE(scm_count(f) == 1);
			const props_t a = props_at(f, 0);
			xrt_stereo_camera_stream_request req = {a.id, XRT_STEREO_CAMERA_OUTPUT_RAW,
			                                        XRT_STEREO_CAMERA_FORMAT_GRAY8,
			                                        XRT_STEREO_CAMERA_TRANSPORT_SHARED_MEMORY, 0.0f};
			uint64_t sid = 0;
			REQUIRE(scm_stream_create(f, &req, &sid) == XRT_SUCCESS);
			(void)drain(f, 0);
			const xrt_result_t first = scm_stream_start(f, sid);
			if (first == XRT_SUCCESS) {
				// Started before anything tried the device: the camera thread opens, fails,
				// and must END the stream — never leave it started at 0 fps.
				bool ended = false;
				uint32_t reason = 0;
				const int64_t deadline = os_monotonic_get_ns() + (int64_t)(fl.max_wait_s * 1e9);
				while (!ended && os_monotonic_get_ns() < deadline) {
					xrt_stereo_camera_event ev;
					while (scm_poll_event(f, &ev)) {
						if (ev.kind == XRT_STEREO_CAMERA_EVENT_STREAM_ENDED &&
						    ev.stream_id == sid) {
							ended = true;
							reason = ev.value;
						}
					}
					if (!ended) {
						os_nanosleep(20 * 1000 * 1000);
					}
				}
				CHECK(ended);
				CHECK(reason == XRT_STEREO_CAMERA_END_SOURCE_BUSY);
				bool ready = true;
				xrt_stereo_camera_frame_info fi;
				const uint8_t *slot = nullptr;
				CHECK(scm_acquire(f, sid, &ready, &fi, &slot) == XRT_ERROR_STEREO_CAMERA_STREAM_ENDED);
			} else {
				// The manager already tried the device (rate measurement at create): refused outright.
				CHECK(first == XRT_ERROR_STEREO_CAMERA_BUSY);
			}
			CHECK(props_at(f, 0).state == XRT_STEREO_CAMERA_STATE_UNAVAILABLE);
			CHECK(scm_stream_destroy(f, sid) == XRT_SUCCESS);

			// While the retry is pending a new start is BUSY — distinct from "started, 0 fps".
			REQUIRE(scm_stream_create(f, &req, &sid) == XRT_SUCCESS);
			CHECK(scm_stream_start(f, sid) == XRT_ERROR_STEREO_CAMERA_BUSY);

			// The device is freed: the background retry succeeds and a start flows again.
			u_stereo_uvc_fake_set_fault(U_STEREO_UVC_FAKE_FAULT_NONE);
			xrt_result_t sr = XRT_ERROR_STEREO_CAMERA_BUSY;
			const int64_t deadline2 = os_monotonic_get_ns() + 6000ll * 1000 * 1000;
			while (sr == XRT_ERROR_STEREO_CAMERA_BUSY && os_monotonic_get_ns() < deadline2) {
				os_nanosleep(100 * 1000 * 1000);
				sr = scm_stream_start(f, sid);
			}
			CHECK(sr == XRT_SUCCESS);
			bool got = false;
			const int64_t deadline3 = os_monotonic_get_ns() + 6000ll * 1000 * 1000;
			while (!got && os_monotonic_get_ns() < deadline3) {
				bool ready = false;
				xrt_stereo_camera_frame_info fi;
				const uint8_t *slot = nullptr;
				if (scm_acquire(f, sid, &ready, &fi, &slot) == XRT_SUCCESS && ready) {
					got = true;
				} else {
					os_nanosleep(20 * 1000 * 1000);
				}
			}
			CHECK(got);
			CHECK(scm_stream_destroy(f, sid) == XRT_SUCCESS);
			scm_destroy(f);
			u_stereo_uvc_fake_set_fault(U_STEREO_UVC_FAKE_FAULT_UNSET);
		}
	}
}


/*
 *
 * In-process consumers (the input host iface's shared-camera slot): a
 * CAMERA_CONSUMER stream with no connection, on the same ring / layout /
 * wake as the IPC form, counted in the status and the quota.
 *
 */

namespace {

ipc_server_stereo_camera_internal_request
internal_request(const char *id = "sim-input", bool trusted = true)
{
	ipc_server_stereo_camera_internal_request rq{};
	rq.provider_id = id;
	rq.display_name = "DisplayXR Sim Input";
	rq.component_trusted = trusted;
	rq.camera_id = 0; // the active display's camera (here: the only one)
	rq.output = XRT_STEREO_CAMERA_OUTPUT_RECTIFIED;
	rq.max_frame_rate = 0.0f;
	return rq;
}

//! Acquire until a frame arrives or @p budget_ms passes.
xrt_result_t
acquire_one(ipc_server_stereo_camera_internal *h,
            ipc_server_stereo_camera_internal_frame *fr,
            uint32_t *state,
            int budget_ms = 3000)
{
	xrt_result_t r = XRT_TIMEOUT;
	for (int waited = 0; waited < budget_ms && r == XRT_TIMEOUT; waited += 100) {
		r = ipc_server_stereo_camera_internal_acquire(h, 100, fr, state);
	}
	return r;
}

} // namespace

TEST_CASE("manager + UVC fake: an in-process consumer gets the IPC stream's layout and frames",
          "[stereo_camera_manager][internal]")
{
	u_stereo_uvc_config cfg = fake_config();
	scm_fixture *f = scm_create(&cfg);
	REQUIRE(f != nullptr);
	REQUIRE(scm_count(f) == 1);

	ipc_server_stereo_camera_internal_request rq = internal_request();
	ipc_server_stereo_camera_internal_info info;
	ipc_server_stereo_camera_internal *h = nullptr;
	REQUIRE(ipc_server_stereo_camera_internal_open(&rq, &info, &h) == XRT_SUCCESS);
	REQUIRE(h != nullptr);
	CHECK(ipc_server_stereo_camera_internal_consumer_count() == 1);

	// The same values a CAMERA_CONSUMER connection gets from get_properties /
	// get_calibration / get_section.
	CHECK(info.props.source == XRT_STEREO_CAMERA_SOURCE_UVC);
	CHECK(info.props.eye_width == 640);
	CHECK(info.calib.output == XRT_STEREO_CAMERA_OUTPUT_RECTIFIED);
	CHECK(info.calib.baseline_mm == Catch::Approx(60.0f));
	CHECK(info.layout.width == 1280);
	CHECK(info.layout.height == 480);
	CHECK(info.layout.format == XRT_STEREO_CAMERA_FORMAT_GRAY8);
	CHECK(info.layout.output == XRT_STEREO_CAMERA_OUTPUT_RECTIFIED);
	CHECK(info.layout.transport == XRT_STEREO_CAMERA_TRANSPORT_SHARED_MEMORY);
	CHECK(info.layout.slot_count == U_STEREO_CAMERA_RING_SLOTS);
	CHECK(info.layout.slot_stride >= 1280ull * 480ull);
	CHECK(info.layout.slot_stride % 4096 == 0);
	CHECK(info.layout.section_size == info.layout.slot_stride * U_STEREO_CAMERA_RING_SLOTS);
	REQUIRE(info.map != nullptr);

	// The tray counts it and names it by the provider's display name.
	ipc_server_stereo_camera_status st;
	REQUIRE(ipc_server_stereo_camera_get_status(&st));
	CHECK(st.started_streams == 1);
	CHECK(st.component_streams == 1);
	CHECK(std::string(st.consumers).find("DisplayXR Sim Input") != std::string::npos);

	ipc_server_stereo_camera_internal_frame fr;
	uint32_t state = 0;
	REQUIRE(acquire_one(h, &fr, &state) == XRT_SUCCESS);
	CHECK(state == XRT_STEREO_CAMERA_STATE_AVAILABLE);
	CHECK(fr.info.width == 1280);
	CHECK(fr.info.height == 480);
	CHECK(fr.info.format == XRT_STEREO_CAMERA_FORMAT_GRAY8);
	CHECK(fr.info.output == XRT_STEREO_CAMERA_OUTPUT_RECTIFIED);
	CHECK(fr.info.frame_index > 0);
	CHECK(fr.info.row_pitch[0] >= 1280);
	CHECK(fr.info.slot < U_STEREO_CAMERA_RING_SLOTS);
	// Planes at slot * slot_stride + plane_offset — exactly the IPC consumer's arithmetic.
	CHECK(fr.plane[0] == info.map + fr.info.slot * info.layout.slot_stride + fr.info.plane_offset[0]);
	CHECK(fr.plane[1] == nullptr);
	ipc_server_stereo_camera_internal_release(h, fr.info.frame_index);
	// A second frame is a newer one.
	ipc_server_stereo_camera_internal_frame fr2;
	REQUIRE(acquire_one(h, &fr2, &state) == XRT_SUCCESS);
	CHECK(fr2.info.frame_index > fr.info.frame_index);

	SECTION("session lock: no frames, state SUSPENDED; unlock resumes")
	{
		ipc_server_stereo_camera_set_session_locked(true, "test");
		xrt_result_t r = XRT_SUCCESS;
		for (int i = 0; i < 10 && r == XRT_SUCCESS; i++) { // drain anything published before the lock
			r = ipc_server_stereo_camera_internal_acquire(h, 200, &fr, &state);
		}
		CHECK(r == XRT_TIMEOUT);
		CHECK(state == XRT_STEREO_CAMERA_STATE_SUSPENDED);
		ipc_server_stereo_camera_set_session_locked(false, "test");
		CHECK(acquire_one(h, &fr, &state) == XRT_SUCCESS);
	}
	SECTION("sharing off ends the stream; a new open is DISABLED")
	{
		REQUIRE(ipc_server_stereo_camera_set_sharing(false, "test"));
		CHECK(ipc_server_stereo_camera_internal_acquire(h, 100, &fr, &state) ==
		      XRT_ERROR_STEREO_CAMERA_STREAM_ENDED);
		ipc_server_stereo_camera_internal *h2 = nullptr;
		CHECK(ipc_server_stereo_camera_internal_open(&rq, &info, &h2) == XRT_ERROR_STEREO_CAMERA_DISABLED);
		CHECK(h2 == nullptr);
		CHECK(ipc_server_stereo_camera_internal_consumer_count() == 1); // the ended one is still open
		REQUIRE(ipc_server_stereo_camera_set_sharing(true, "test"));
	}
	SECTION("the CAMERA_CONSUMER quota (4) holds, RAW is refused")
	{
		ipc_server_stereo_camera_internal *more[3] = {};
		for (auto &m : more) {
			ipc_server_stereo_camera_internal_info i2;
			REQUIRE(ipc_server_stereo_camera_internal_open(&rq, &i2, &m) == XRT_SUCCESS);
		}
		ipc_server_stereo_camera_internal_info i5;
		ipc_server_stereo_camera_internal *fifth = nullptr;
		CHECK(ipc_server_stereo_camera_internal_open(&rq, &i5, &fifth) == XRT_ERROR_CLIENT_LIMIT_REACHED);
		for (auto &m : more) {
			ipc_server_stereo_camera_internal_close(m);
		}
		ipc_server_stereo_camera_internal_request raw = internal_request();
		raw.output = XRT_STEREO_CAMERA_OUTPUT_RAW;
		CHECK(ipc_server_stereo_camera_internal_open(&raw, &i5, &fifth) == XRT_ERROR_NOT_AUTHORIZED);
		CHECK(ipc_server_stereo_camera_internal_consumer_count() == 1);
	}

	ipc_server_stereo_camera_internal_close(h);
	CHECK(ipc_server_stereo_camera_internal_consumer_count() == 0);
	REQUIRE(ipc_server_stereo_camera_get_status(&st));
	CHECK(st.started_streams == 0);
	scm_destroy(f);
}

TEST_CASE("manager: an in-process consumer outlives the manager safely", "[stereo_camera_manager][internal]")
{
	u_stereo_uvc_config cfg = fake_config();
	scm_fixture *f = scm_create(&cfg);
	REQUIRE(f != nullptr);
	ipc_server_stereo_camera_internal_request rq = internal_request();
	ipc_server_stereo_camera_internal_info info;
	ipc_server_stereo_camera_internal *h = nullptr;
	REQUIRE(ipc_server_stereo_camera_internal_open(&rq, &info, &h) == XRT_SUCCESS);

	// Service shutdown destroys the manager before the input providers' devices.
	scm_destroy(f);
	CHECK(ipc_server_stereo_camera_internal_consumer_count() == 0);
	ipc_server_stereo_camera_internal_frame fr;
	uint32_t state = 0;
	CHECK(ipc_server_stereo_camera_internal_acquire(h, 50, &fr, &state) == XRT_ERROR_STEREO_CAMERA_STREAM_ENDED);
	CHECK(state == XRT_STEREO_CAMERA_STATE_UNAVAILABLE);
	ipc_server_stereo_camera_internal_release(h, 1);
	ipc_server_stereo_camera_internal_close(h); // frees the handle, touches no manager

	// And with no manager at all an open is "unavailable", not a crash.
	ipc_server_stereo_camera_internal *h2 = nullptr;
	CHECK(ipc_server_stereo_camera_internal_open(&rq, &info, &h2) == XRT_ERROR_FEATURE_NOT_SUPPORTED);
}


/*
 *
 * ADR-061: the app-share source kind — app entries in their own table after
 * every camera, the visibility gate, rescan exclusions, the exited-app linger,
 * UNAVAILABLE reasons, the share consent at start, and frames + metadata from
 * the service's synthetic source and from a (fake) platform share tap.
 *
 */

namespace {

constexpr long kAppPid = 4000001; // never a real process: its exe resolves to nothing ("pid-4000001")

xrt_stereo_camera_stream_request
share_req(uint64_t id)
{
	return {id, XRT_STEREO_CAMERA_OUTPUT_RECTIFIED, XRT_STEREO_CAMERA_FORMAT_NV12,
	        XRT_STEREO_CAMERA_TRANSPORT_SHARED_MEMORY, 0.0f};
}

//! All properties the consumer sees, in enumeration order.
std::vector<xrt_stereo_camera_properties>
listing(scm_fixture *f)
{
	std::vector<xrt_stereo_camera_properties> v;
	uint32_t n = scm_count(f);
	for (uint32_t i = 0; i < n; i++) {
		xrt_stereo_camera_properties p;
		if (scm_properties(f, i, &p) == XRT_SUCCESS) {
			v.push_back(p);
		}
	}
	return v;
}

bool
is_app(const xrt_stereo_camera_properties &p)
{
	return (p.flags & XRT_STEREO_CAMERA_APP_CONTENT) != 0;
}

//! Signed SAD block disparity (right x - left x) of one row band of an NV12/GRAY8 SBS luma plane.
int
band_disparity(const uint8_t *y, uint32_t pitch, uint32_t ew, uint32_t y0, uint32_t h, int range)
{
	int best = 0;
	uint64_t best_sad = UINT64_MAX;
	for (int d = -range; d <= range; d++) {
		uint64_t sad = 0;
		for (uint32_t r = y0; r < y0 + h; r++) {
			for (uint32_t x = (uint32_t)range + 8; x + (uint32_t)range + 8 < ew; x++) {
				int a = y[(size_t)r * pitch + x];
				int b = y[(size_t)r * pitch + ew + (uint32_t)((int)x + d)];
				sad += (uint64_t)(a > b ? a - b : b - a);
			}
		}
		if (sad < best_sad) {
			best_sad = sad;
			best = d;
		}
	}
	return best;
}

} // namespace

TEST_CASE("app share: own table after every camera, visible only with the extension", "[stereo_camera_manager][app_share]")
{
	const char *ids[] = {"cam-a", "cam-b"};
	const char *names[] = {"Front", "Rear"};
	scm_fake_plugin_set(ids, names, 2);
	scm_app_options o = {};
	o.dev = -1;
	o.fake = -1;
	scm_fixture *f = scm_create_apps(&o);
	REQUIRE(f != nullptr);
	scm_app_connect(f, 1, 11, kAppPid, "Model Viewer", XRT_CLIENT_CLASS_APP, false);
	scm_rescan_apps(f);

	// A plain camera consumer never sees an app.
	CHECK(scm_count(f) == 2);

	scm_set_consumer_sees_apps(f, true);
	auto v = listing(f);
	REQUIRE(v.size() == 3);
	CHECK(!is_app(v[0]));
	CHECK(!is_app(v[1]));
	CHECK(std::string(v[0].display_name) == "Front"); // cameras keep their indices
	const auto &a = v[2];
	CHECK(is_app(a));
	CHECK(a.source == XRT_STEREO_CAMERA_SOURCE_APP);
	CHECK(std::string(a.display_name) == "Model Viewer");
	CHECK((a.flags & XRT_PLUGIN_STEREO_CAMERA_NATIVELY_RECTIFIED) != 0);
	CHECK((a.flags & XRT_PLUGIN_STEREO_CAMERA_RECTIFIABLE) != 0);
	CHECK((a.flags & (XRT_PLUGIN_STEREO_CAMERA_CALIBRATED | XRT_PLUGIN_STEREO_CAMERA_USER_FACING)) == 0);
	CHECK(a.baseline_mm == 0.0f);
	CHECK(a.horizontal_fov_deg == 0.0f);
	CHECK(a.eye_width == 1280);
	CHECK(a.eye_height == 720);
	CHECK(a.max_frame_rate == 30.0f);
	CHECK(a.view_count == 2);
	CHECK(a.supported_formats == XRT_STEREO_CAMERA_BIT(XRT_STEREO_CAMERA_FORMAT_NV12));
	CHECK(a.camera_id != v[0].camera_id);
	CHECK(a.camera_id != v[1].camera_id);
	CHECK(std::string(a.persistent_id).rfind("dxrcam-", 0) == 0);

	// No tap on this platform (no hooks): listed UNAVAILABLE with that reason, start is BUSY.
	CHECK(a.state == XRT_STEREO_CAMERA_STATE_UNAVAILABLE);
	xrt_app_share_source_info ai;
	REQUIRE(scm_app_source(f, a.camera_id, &ai) == XRT_SUCCESS);
	CHECK(ai.availability == XRT_APP_SHARE_UNAVAILABLE_NO_TAP);
	CHECK(ai.pid == (uint64_t)kAppPid);
	CHECK(std::string(ai.application_name) == "Model Viewer");
	CHECK(ai.source == XRT_STEREO_CAMERA_SOURCE_APP);
	CHECK(scm_app_source(f, v[0].camera_id, &ai) == XRT_ERROR_INPUT_UNSUPPORTED); // a camera is not an app
	scm_set_share_answer(U_CAMERA_CONSENT_PROMPT_ALLOW);
	uint64_t sid = 0;
	auto req = share_req(a.camera_id);
	REQUIRE(scm_stream_create(f, &req, &sid) == XRT_SUCCESS);
	CHECK(scm_stream_start(f, sid) == XRT_ERROR_STEREO_CAMERA_BUSY);
	CHECK(scm_share_prompts() == 0); // never prompted for something that cannot be shared

	// Spec §9: RAW / another format / calibration are unsupported on an app entry.
	auto raw = share_req(a.camera_id);
	raw.output = XRT_STEREO_CAMERA_OUTPUT_RAW;
	uint64_t bad = 0;
	CHECK(scm_stream_create(f, &raw, &bad) == XRT_ERROR_FEATURE_NOT_SUPPORTED);
	auto gray = share_req(a.camera_id);
	gray.format = XRT_STEREO_CAMERA_FORMAT_GRAY8;
	CHECK(scm_stream_create(f, &gray, &bad) == XRT_ERROR_FEATURE_NOT_SUPPORTED);
	xrt_stereo_camera_calibration c;
	CHECK(scm_calibration(f, a.camera_id, XRT_STEREO_CAMERA_OUTPUT_RECTIFIED, &c) == XRT_ERROR_FEATURE_NOT_SUPPORTED);

	// A consumer that does not see apps cannot address one by id either.
	scm_set_consumer_sees_apps(f, false);
	CHECK(scm_stream_create(f, &req, &bad) == XRT_ERROR_INPUT_UNSUPPORTED);
	scm_destroy(f);
	scm_fake_plugin_set(nullptr, nullptr, 0);
}

TEST_CASE("app share: rescan exclusions", "[stereo_camera_manager][app_share]")
{
	scm_fake_plugin_set(nullptr, nullptr, 0);
	scm_app_options o = {};
	o.dev = -1;
	o.fake = -1;
	o.consumer_sees_apps = true;
	scm_fixture *f = scm_create_apps(&o);
	REQUIRE(f != nullptr);
	scm_app_connect(f, 1, 1, kAppPid + 1, "Shared App", XRT_CLIENT_CLASS_APP, false);
	scm_app_connect(f, 2, 2, kAppPid + 2, "Shell", XRT_CLIENT_CLASS_CONTROLLER, false);
	scm_app_connect(f, 3, 3, kAppPid + 3, "Diag", XRT_CLIENT_CLASS_DIAG, false);
	scm_app_connect(f, 4, 4, kAppPid + 4, "Capture", XRT_CLIENT_CLASS_CAMERA_CONSUMER, false);
	scm_app_connect(f, 5, 5, kAppPid + 5, "Browser", XRT_CLIENT_CLASS_PRESENT_OWNER, false);
	scm_app_connect(f, 6, 6, kAppPid + 6, "Private", XRT_CLIENT_CLASS_APP, true); // opted out
	scm_app_connect(f, 7, 7, kAppPid + 7, "Headless", XRT_CLIENT_CLASS_APP, false);
	scm_app_disconnect(f, 7);                                                     // no session / compositor
	scm_app_connect(f, 8, 8, 0, "No pid", XRT_CLIENT_CLASS_APP, false); // no OS-derived pid: skipped
	scm_rescan_apps(f);
	auto v = listing(f);
	REQUIRE(v.size() == 1);
	CHECK(std::string(v[0].display_name) == "Shared App");
	scm_destroy(f);
}

TEST_CASE("app share: an exited app lingers UNAVAILABLE, then leaves; its id comes back", "[stereo_camera_manager][app_share]")
{
	scm_fake_plugin_set(nullptr, nullptr, 0);
	scm_set_tap(true);
	scm_app_options o = {};
	o.dev = 1; // no prompt
	o.fake = -1;
	o.linger_ms = 300;
	o.consumer_sees_apps = true;
	scm_fixture *f = scm_create_apps(&o);
	REQUIRE(f != nullptr);
	scm_app_connect(f, 1, 21, kAppPid, "Viewer", XRT_CLIENT_CLASS_APP, false);
	scm_rescan_apps(f);
	auto v = listing(f);
	REQUIRE(v.size() == 1);
	const uint64_t id = v[0].camera_id;
	CHECK(v[0].state == XRT_STEREO_CAMERA_STATE_AVAILABLE); // a tap says it can be shared
	xrt_app_share_source_info ai;
	REQUIRE(scm_app_source(f, id, &ai) == XRT_SUCCESS);
	CHECK(ai.availability == XRT_APP_SHARE_AVAILABLE);
	CHECK(ai.view_count == 4);
	CHECK(ai.active_view_count == 2);
	CHECK(ai.content_eye_width == 1600);
	CHECK(ai.window_width == 1600);

	uint64_t sid = 0;
	auto req = share_req(id);
	REQUIRE(scm_stream_create(f, &req, &sid) == XRT_SUCCESS);
	REQUIRE(scm_stream_start(f, sid) == XRT_SUCCESS);
	xrt_stereo_camera_event ev;
	while (scm_poll_event(f, &ev)) {
	}

	// The session ends: SOURCE_LOST at once, the tap detached before this returns.
	const int64_t t0 = os_monotonic_get_ns();
	while (scm_tap_attached() == 0 && os_monotonic_get_ns() - t0 < 2000ll * 1000 * 1000) {
		os_nanosleep(5 * 1000 * 1000);
	}
	CHECK(scm_tap_attached() == 1);
	scm_app_session_ending(f, 1);
	CHECK(scm_tap_attached() == 0);
	bool lost = false;
	while (scm_poll_event(f, &ev)) {
		lost |= ev.kind == XRT_STEREO_CAMERA_EVENT_STREAM_ENDED && ev.stream_id == sid &&
		        ev.value == XRT_STEREO_CAMERA_END_SOURCE_LOST;
	}
	CHECK(lost);
	bool ready = false;
	xrt_stereo_camera_frame_info fi;
	const uint8_t *slot = nullptr;
	CHECK(scm_acquire(f, sid, &ready, &fi, &slot) == XRT_ERROR_STEREO_CAMERA_STREAM_ENDED);
	scm_app_disconnect(f, 1);
	scm_rescan_apps(f);
	v = listing(f);
	REQUIRE(v.size() == 1); // still listed while it lingers
	CHECK(v[0].camera_id == id);
	CHECK(v[0].state == XRT_STEREO_CAMERA_STATE_UNAVAILABLE);
	REQUIRE(scm_app_source(f, id, &ai) == XRT_SUCCESS);
	CHECK(ai.availability == XRT_APP_SHARE_UNAVAILABLE_EXITED);
	uint64_t sid2 = 0;
	REQUIRE(scm_stream_create(f, &req, &sid2) == XRT_SUCCESS);
	CHECK(scm_stream_start(f, sid2) == XRT_ERROR_STEREO_CAMERA_BUSY);
	scm_stream_destroy(f, sid2);
	scm_stream_destroy(f, sid);

	os_nanosleep(350ll * 1000 * 1000);
	CHECK(scm_rescan_apps(f));
	CHECK(listing(f).empty()); // the linger is over

	// The same executable + instance gets its id back within the service run.
	scm_app_connect(f, 2, 22, kAppPid, "Viewer", XRT_CLIENT_CLASS_APP, false);
	scm_rescan_apps(f);
	v = listing(f);
	REQUIRE(v.size() == 1);
	CHECK(v[0].camera_id == id);
	scm_destroy(f);
	scm_set_tap(false);
}

TEST_CASE("app share: the gate — extension flag vs DXR_APP_SHARE_DEV, and the share prompt", "[stereo_camera_manager][app_share]")
{
	scm_fake_plugin_set(nullptr, nullptr, 0);
	scm_set_tap(true);
	SECTION("DEV: listed to a plain consumer, started without a prompt")
	{
		scm_app_options o = {};
		o.dev = 1;
		o.fake = -1;
		scm_fixture *f = scm_create_apps(&o);
		REQUIRE(f != nullptr);
		scm_app_connect(f, 1, 31, kAppPid, "Viewer", XRT_CLIENT_CLASS_APP, false);
		scm_rescan_apps(f);
		auto v = listing(f); // no XR_DXR_app_share
		REQUIRE(v.size() == 1);
		scm_set_share_answer(U_CAMERA_CONSENT_PROMPT_DENY);
		uint64_t sid = 0;
		auto req = share_req(v[0].camera_id);
		REQUIRE(scm_stream_create(f, &req, &sid) == XRT_SUCCESS);
		CHECK(scm_stream_start(f, sid) == XRT_SUCCESS);
		CHECK(scm_share_prompts() == 0);
		uint32_t shares = 0;
		char names[256] = {0};
		REQUIRE(scm_status(&shares, names, sizeof(names)));
		CHECK(shares == 1);
		CHECK(std::string(names) == "Viewer"); // the tray reads "Sharing Viewer"
		CHECK(scm_stop_all() == 1);            // "Stop camera sharing" stops shares too
		REQUIRE(scm_status(&shares, names, sizeof(names)));
		CHECK(shares == 0);
		scm_destroy(f);
	}
	SECTION("extension: the prompt runs per start; Deny refuses; per-app-run remembers")
	{
		scm_app_options o = {};
		o.dev = -1;
		o.fake = -1;
		o.consumer_sees_apps = true;
		o.prompt_mode = 1; // always
		scm_fixture *f = scm_create_apps(&o);
		REQUIRE(f != nullptr);
		scm_app_connect(f, 1, 41, kAppPid, "Viewer", XRT_CLIENT_CLASS_APP, false);
		scm_rescan_apps(f);
		auto v = listing(f);
		REQUIRE(v.size() == 1);
		auto req = share_req(v[0].camera_id);
		uint64_t sid = 0;
		REQUIRE(scm_stream_create(f, &req, &sid) == XRT_SUCCESS);
		scm_set_share_answer(U_CAMERA_CONSENT_PROMPT_DENY);
		CHECK(scm_stream_start(f, sid) == XRT_ERROR_STEREO_CAMERA_CONSENT_REFUSED);
		scm_set_share_answer(U_CAMERA_CONSENT_PROMPT_ALLOW);
		CHECK(scm_stream_start(f, sid) == XRT_SUCCESS);
		scm_stream_destroy(f, sid);
		REQUIRE(scm_stream_create(f, &req, &sid) == XRT_SUCCESS);
		CHECK(scm_stream_start(f, sid) == XRT_SUCCESS);
		CHECK(scm_share_prompts() == 2); // always: asked again
		scm_destroy(f);
	}
	SECTION("sharing off: zero entries, DISABLED")
	{
		scm_app_options o = {};
		o.dev = 1;
		o.fake = -1;
		scm_fixture *f = scm_create_apps(&o);
		REQUIRE(f != nullptr);
		scm_app_connect(f, 1, 51, kAppPid, "Viewer", XRT_CLIENT_CLASS_APP, false);
		scm_rescan_apps(f);
		auto v = listing(f);
		REQUIRE(v.size() == 1);
		uint64_t sid = 0;
		auto req = share_req(v[0].camera_id);
		REQUIRE(scm_stream_create(f, &req, &sid) == XRT_SUCCESS);
		REQUIRE(scm_set_sharing(f, false) == XRT_SUCCESS);
		CHECK(scm_count(f) == 0);
		CHECK(scm_stream_start(f, sid) == XRT_ERROR_STEREO_CAMERA_DISABLED);
		REQUIRE(scm_set_sharing(f, true) == XRT_SUCCESS);
		scm_destroy(f);
	}
	scm_set_tap(false);
}

TEST_CASE("app share: the synthetic source — frames, metadata, display-referred disparity", "[stereo_camera_manager][app_share]")
{
	scm_fake_plugin_set(nullptr, nullptr, 0);
	scm_app_options o = {};
	o.dev = 1;
	o.fake = 1;
	scm_fixture *f = scm_create_apps(&o);
	REQUIRE(f != nullptr);
	auto v = listing(f);
	REQUIRE(v.size() == 1);
	CHECK(is_app(v[0]));
	CHECK(v[0].state == XRT_STEREO_CAMERA_STATE_AVAILABLE);
	xrt_app_share_source_info ai;
	REQUIRE(scm_app_source(f, v[0].camera_id, &ai) == XRT_SUCCESS);
	CHECK(std::string(ai.executable_name) == "displayxr-app-share-fake");
	CHECK(ai.availability == XRT_APP_SHARE_AVAILABLE);

	uint64_t sid = 0;
	auto req = share_req(v[0].camera_id);
	REQUIRE(scm_stream_create(f, &req, &sid) == XRT_SUCCESS);
	REQUIRE(scm_stream_start(f, sid) == XRT_SUCCESS);
	xrt_stereo_camera_stream_layout lay;
	REQUIRE(scm_stream_map(f, sid, &lay) == XRT_SUCCESS);
	CHECK(lay.width == 2560);
	CHECK(lay.height == 720);
	CHECK(lay.output == XRT_STEREO_CAMERA_OUTPUT_RECTIFIED);

	uint32_t frames = 0;
	uint64_t last_index = 0;
	xrt_stereo_camera_frame_info fi = {};
	const uint8_t *slot = nullptr;
	const int64_t t_end = os_monotonic_get_ns() + 5ll * 1000 * 1000 * 1000;
	while (frames < 10 && os_monotonic_get_ns() < t_end) {
		bool ready = false;
		REQUIRE(scm_acquire(f, sid, &ready, &fi, &slot) == XRT_SUCCESS);
		if (!ready) {
			os_nanosleep(5 * 1000 * 1000);
			continue;
		}
		CHECK(fi.frame_index > last_index);
		last_index = fi.frame_index;
		frames++;
	}
	REQUIRE(frames == 10);
	REQUIRE(slot != nullptr);
	CHECK(fi.format == XRT_STEREO_CAMERA_FORMAT_NV12);
	CHECK(fi.output == XRT_STEREO_CAMERA_OUTPUT_RECTIFIED);
	const xrt_app_share_frame_info app = scm_last_app(f);
	CHECK(app.valid == 1);
	CHECK(app.content_x == 0);
	CHECK(app.content_width == 1280);
	CHECK(app.content_height == 720);
	CHECK(app.mono == 0);
	CHECK(app.content_generation == 1);
	CHECK(app.zero_disparity_shift_frac == 0.0f);
	CHECK(app.commit_time_ns > 0);

	// The zero-disparity band reads 0; the ramp is right-minus-left positive below it, negative above.
	const uint8_t *y = slot + fi.plane_offset[0];
	uint32_t b0 = 0, b1 = 0;
	u_app_share_fake_band(720, &b0, &b1);
	CHECK(band_disparity(y, fi.row_pitch[0], 1280, b0 + 4, 16, 40) == 0);
	const int d_bottom = band_disparity(y, fi.row_pitch[0], 1280, 680, 8, 40);
	const int d_top = band_disparity(y, fi.row_pitch[0], 1280, 32, 8, 40);
	CHECK(d_bottom == Catch::Approx(u_app_share_fake_disparity_px(1280, 720, 684)).margin(1.0));
	CHECK(d_top == Catch::Approx(u_app_share_fake_disparity_px(1280, 720, 36)).margin(1.0));
	CHECK(d_bottom > 10);
	CHECK(d_top < -10);
	std::printf("[manager app share] fake: %u frames, band d = 0, top d = %d px, bottom d = %d px\n", frames, d_top,
	            d_bottom);
	scm_stream_destroy(f, sid);
	scm_destroy(f);
}

TEST_CASE("app share: frames + metadata through a platform share tap", "[stereo_camera_manager][app_share]")
{
	scm_fake_plugin_set(nullptr, nullptr, 0);
	scm_set_tap(true);
	scm_app_options o = {};
	o.dev = -1;
	o.fake = -1;
	o.consumer_sees_apps = true;
	o.prompt_mode = 2; // per-app-run
	scm_fixture *f = scm_create_apps(&o);
	REQUIRE(f != nullptr);
	scm_app_connect(f, 1, 61, kAppPid, "Viewer", XRT_CLIENT_CLASS_APP, false);
	scm_rescan_apps(f);
	auto v = listing(f);
	REQUIRE(v.size() == 1);
	scm_set_share_answer(U_CAMERA_CONSENT_PROMPT_ALLOW);
	uint64_t sid = 0;
	auto req = share_req(v[0].camera_id);
	REQUIRE(scm_stream_create(f, &req, &sid) == XRT_SUCCESS);
	REQUIRE(scm_stream_start(f, sid) == XRT_SUCCESS);
	xrt_stereo_camera_stream_layout lay;
	REQUIRE(scm_stream_map(f, sid, &lay) == XRT_SUCCESS);
	bool ready = false;
	xrt_stereo_camera_frame_info fi = {};
	const uint8_t *slot = nullptr;
	const int64_t t_end = os_monotonic_get_ns() + 5ll * 1000 * 1000 * 1000;
	while (!ready && os_monotonic_get_ns() < t_end) {
		REQUIRE(scm_acquire(f, sid, &ready, &fi, &slot) == XRT_SUCCESS);
		if (!ready) {
			os_nanosleep(5 * 1000 * 1000);
		}
	}
	REQUIRE(ready);
	const xrt_app_share_frame_info app = scm_last_app(f);
	CHECK(app.valid == 1);
	CHECK(app.content_y == 8);
	CHECK(app.content_height == 704);
	CHECK(app.mono == 1);
	CHECK(app.content_generation == 3);
	// per-app-run: a second start of the same app by the same consumer is not prompted.
	scm_stream_destroy(f, sid);
	REQUIRE(scm_stream_create(f, &req, &sid) == XRT_SUCCESS);
	REQUIRE(scm_stream_start(f, sid) == XRT_SUCCESS);
	CHECK(scm_share_prompts() == 1);
	scm_stream_destroy(f, sid);
	scm_destroy(f);
	CHECK(scm_tap_attached() == 0); // every attach was detached
	scm_set_tap(false);
}

TEST_CASE("app share: a relaunch INSIDE the linger keeps one row per cameraId", "[stereo_camera_manager][app_share]")
{
	scm_fake_plugin_set(nullptr, nullptr, 0);
	scm_set_tap(true);
	scm_app_options o = {};
	o.dev = 1;
	o.fake = -1;
	o.linger_ms = 10000; // the relaunch happens well inside it
	o.consumer_sees_apps = true;
	scm_fixture *f = scm_create_apps(&o);
	REQUIRE(f != nullptr);
	scm_app_connect(f, 1, 71, kAppPid, "Viewer", XRT_CLIENT_CLASS_APP, false);
	scm_rescan_apps(f);
	auto v = listing(f);
	REQUIRE(v.size() == 1);
	const uint64_t id = v[0].camera_id;

	// Exit, and the same executable comes straight back (a new session / process).
	scm_app_session_ending(f, 1);
	scm_app_disconnect(f, 1);
	scm_rescan_apps(f);
	REQUIRE(listing(f).size() == 1); // lingering
	scm_app_connect(f, 2, 72, kAppPid, "Viewer", XRT_CLIENT_CLASS_APP, false);
	scm_rescan_apps(f);
	v = listing(f);
	REQUIRE(v.size() == 1); // never two rows with one cameraId
	CHECK(v[0].camera_id == id);
	CHECK(v[0].state == XRT_STEREO_CAMERA_STATE_AVAILABLE);
	xrt_app_share_source_info ai;
	REQUIRE(scm_app_source(f, id, &ai) == XRT_SUCCESS);
	CHECK(ai.availability == XRT_APP_SHARE_AVAILABLE); // the live one, not the dead one
	uint64_t sid = 0;
	auto req = share_req(id);
	REQUIRE(scm_stream_create(f, &req, &sid) == XRT_SUCCESS);
	CHECK(scm_stream_start(f, sid) == XRT_SUCCESS); // not BUSY on a live app
	scm_stream_destroy(f, sid);
	scm_destroy(f);
	scm_set_tap(false);
}

TEST_CASE("app share: a per-app-run grant dies with the app", "[stereo_camera_manager][app_share]")
{
	scm_fake_plugin_set(nullptr, nullptr, 0);
	scm_set_tap(true);
	scm_app_options o = {};
	o.dev = -1;
	o.fake = -1;
	o.consumer_sees_apps = true;
	o.prompt_mode = 2; // per-app-run
	scm_fixture *f = scm_create_apps(&o);
	REQUIRE(f != nullptr);
	scm_set_share_answer(U_CAMERA_CONSENT_PROMPT_ALLOW);
	scm_app_connect(f, 1, 81, kAppPid, "Viewer", XRT_CLIENT_CLASS_APP, false);
	scm_rescan_apps(f);
	auto v = listing(f);
	REQUIRE(v.size() == 1);
	auto req = share_req(v[0].camera_id);
	uint64_t sid = 0;
	REQUIRE(scm_stream_create(f, &req, &sid) == XRT_SUCCESS);
	REQUIRE(scm_stream_start(f, sid) == XRT_SUCCESS);
	scm_stream_destroy(f, sid);
	REQUIRE(scm_stream_create(f, &req, &sid) == XRT_SUCCESS);
	REQUIRE(scm_stream_start(f, sid) == XRT_SUCCESS);
	scm_stream_destroy(f, sid);
	CHECK(scm_share_prompts() == 1); // the grant covered the second start

	// The app exits and comes back with the SAME pid (pid reuse) and the same id:
	// the old grant must not silently re-share it.
	scm_app_session_ending(f, 1);
	scm_app_disconnect(f, 1);
	scm_rescan_apps(f);
	scm_app_connect(f, 2, 82, kAppPid, "Viewer", XRT_CLIENT_CLASS_APP, false);
	scm_rescan_apps(f);
	v = listing(f);
	REQUIRE(v.size() == 1);
	CHECK(v[0].camera_id == req.camera_id);
	REQUIRE(scm_stream_create(f, &req, &sid) == XRT_SUCCESS);
	REQUIRE(scm_stream_start(f, sid) == XRT_SUCCESS);
	CHECK(scm_share_prompts() == 2);
	scm_stream_destroy(f, sid);
	scm_destroy(f);
	scm_set_tap(false);
}

TEST_CASE("app share: the tap's release hook runs without the manager lock", "[stereo_camera_manager][app_share]")
{
	scm_fake_plugin_set(nullptr, nullptr, 0);
	scm_set_tap(true);
	scm_app_options o = {};
	o.dev = 1;
	o.fake = -1;
	o.consumer_sees_apps = true;
	scm_fixture *f = scm_create_apps(&o);
	REQUIRE(f != nullptr);
	scm_app_connect(f, 1, 91, kAppPid, "Viewer", XRT_CLIENT_CLASS_APP, false);
	scm_rescan_apps(f);
	auto v = listing(f);
	REQUIRE(v.size() == 1);
	auto req = share_req(v[0].camera_id);
	uint64_t sid = 0;
	REQUIRE(scm_stream_create(f, &req, &sid) == XRT_SUCCESS);
	REQUIRE(scm_stream_start(f, sid) == XRT_SUCCESS);
	const int before = scm_tap_releases();
	const int64_t t_end = os_monotonic_get_ns() + 3ll * 1000 * 1000 * 1000;
	while (scm_tap_releases() < before + 3 && os_monotonic_get_ns() < t_end) {
		os_nanosleep(10 * 1000 * 1000);
	}
	CHECK(scm_tap_releases() >= before + 3); // the published path
	// The in-flight frame of an app that went (the other release site).
	scm_app_session_ending(f, 1);
	scm_stream_destroy(f, sid);
	scm_destroy(f);
	scm_set_tap(false);
}

TEST_CASE("app share: enumerate-driven rescans are throttled, apps_changed is not", "[stereo_camera_manager][app_share]")
{
	scm_fake_plugin_set(nullptr, nullptr, 0);
	scm_app_options o = {};
	o.dev = -1;
	o.fake = -1;
	o.consumer_sees_apps = true;
	scm_fixture *f = scm_create_apps(&o);
	REQUIRE(f != nullptr);
	CHECK(scm_count(f) == 0); // this enumerate rescanned (first time)
	scm_app_connect(f, 1, 101, kAppPid, "Viewer", XRT_CLIENT_CLASS_APP, false);
	CHECK(scm_count(f) == 0); // within a second: no rescan on enumerate
	scm_apps_changed(f);      // a session began: the next enumerate rescans
	CHECK(scm_count(f) == 1);
	scm_destroy(f);
}


/*
 *
 * ADR-061 arm 2: in-process apps over their service links (SOURCE_APP_LINK).
 *
 */

namespace {

constexpr long kLinkPid = 4000101; // never a real process ("pid-4000101")

xrt_share_link_registration
link_reg(const char *name, bool running = true, bool tap = true)
{
	xrt_share_link_registration r = {};
	r.caps = XRT_SHARE_LINK_CAP_SHARE;
	std::snprintf(r.app_name, sizeof(r.app_name), "%s", name);
	r.tier = XRT_SHARE_LINK_TIER_D3D11;
	r.tap_available = tap ? 1u : 0u;
	r.session_running = running ? 1u : 0u;
	return r;
}

//! The app entries the consumer sees, keyed by display name.
const xrt_stereo_camera_properties *
find_named(const std::vector<xrt_stereo_camera_properties> &v, const char *name)
{
	for (const auto &p : v) {
		if (std::string(p.display_name) == name) {
			return &p;
		}
	}
	return nullptr;
}

//! Poll the link until the service asks for @p state (the camera thread opens asynchronously).
bool
link_wait_state(scm_fixture *f, uint32_t slot, uint32_t state, xrt_share_link_poll *out, int64_t ms = 4000)
{
	const int64_t t_end = os_monotonic_get_ns() + ms * 1000 * 1000;
	while (os_monotonic_get_ns() < t_end) {
		REQUIRE(scm_link_poll(f, slot, out) == XRT_SUCCESS);
		if (out->state == state) {
			return true;
		}
		os_nanosleep(5 * 1000 * 1000);
	}
	return false;
}

xrt_share_link_ring_desc
link_ring(uint32_t generation, uint32_t stream_generation, uint32_t content_w, uint32_t content_h)
{
	xrt_share_link_ring_desc d = {};
	d.generation = generation;
	d.stream_generation = stream_generation;
	d.format = 28; // DXGI_FORMAT_R8G8B8A8_UNORM, as the Windows tap would publish
	d.slot_width = 2 * content_w;
	d.slot_height = content_h;
	d.content_width = content_w;
	d.content_height = content_h;
	d.slot_count = XRT_SHARE_LINK_RING_SLOTS;
	return d;
}

/*!
 * Post frame_ready for ring @p generation until @p want frames were acquired
 * (as the app's compositor would, once per frame), or the time runs out.
 */
uint32_t
link_pump(scm_fixture *f, uint32_t slot, uint64_t sid, uint32_t generation, uint32_t content_gen, uint32_t want)
{
	uint32_t got = 0, n = 0;
	const int64_t t_end = os_monotonic_get_ns() + 5ll * 1000 * 1000 * 1000;
	while (got < want && os_monotonic_get_ns() < t_end) {
		xrt_share_link_frame fr = {};
		fr.generation = generation;
		fr.slot = n++ % XRT_SHARE_LINK_RING_SLOTS;
		fr.fence_value = n;
		fr.commit_ns = os_monotonic_get_ns();
		fr.content_generation = content_gen;
		xrt_share_link_poll p = {};
		REQUIRE(scm_link_frame_ready(f, slot, &fr, &p) == XRT_SUCCESS);
		os_nanosleep(4 * 1000 * 1000);
		bool ready = false;
		xrt_stereo_camera_frame_info fi = {};
		const uint8_t *s = nullptr;
		REQUIRE(scm_acquire(f, sid, &ready, &fi, &s) == XRT_SUCCESS);
		got += ready ? 1u : 0u;
	}
	return got;
}

} // namespace

TEST_CASE("app share link: registration, reasons, churn", "[stereo_camera_manager][app_share_link]")
{
	scm_fake_plugin_set(nullptr, nullptr, 0);
	const uint8_t svc_luid[XRT_LUID_SIZE] = {1, 2, 3, 4, 5, 6, 7, 8};
	scm_set_link_importer(true, svc_luid);
	scm_app_options o = {};
	o.dev = -1;
	o.fake = -1;
	o.linger_ms = 300;
	o.consumer_sees_apps = true;
	scm_fixture *f = scm_create_apps(&o);
	REQUIRE(f != nullptr);

	// A: tappable, on the service's adapter -> AVAILABLE.
	scm_link_connect(f, 1, 101, kLinkPid + 1, XRT_CLIENT_CLASS_APP);
	auto a = link_reg("Link A");
	std::memcpy(a.luid, svc_luid, XRT_LUID_SIZE);
	a.luid_valid = 1;
	REQUIRE(scm_link_register(f, 1, &a) == XRT_SUCCESS);
	// B: OpenGL -> UNAVAILABLE (unsupported API), whatever the tap says.
	scm_link_connect(f, 2, 102, kLinkPid + 2, XRT_CLIENT_CLASS_APP);
	auto b = link_reg("Link B");
	b.tier = XRT_SHARE_LINK_TIER_GL;
	REQUIRE(scm_link_register(f, 2, &b) == XRT_SUCCESS);
	// C: opted out -> never listed.
	scm_link_connect(f, 3, 103, kLinkPid + 3, XRT_CLIENT_CLASS_APP);
	auto c = link_reg("Link C");
	c.opt_out = 1;
	REQUIRE(scm_link_register(f, 3, &c) == XRT_SUCCESS);
	// D: session not running -> not listed.
	scm_link_connect(f, 4, 104, kLinkPid + 4, XRT_CLIENT_CLASS_APP);
	auto d = link_reg("Link D", false);
	REQUIRE(scm_link_register(f, 4, &d) == XRT_SUCCESS);
	// E: another adapter -> UNAVAILABLE (other adapter).
	scm_link_connect(f, 5, 105, kLinkPid + 5, XRT_CLIENT_CLASS_APP);
	auto e = link_reg("Link E");
	const uint8_t other[XRT_LUID_SIZE] = {9, 9, 9, 9, 9, 9, 9, 9};
	std::memcpy(e.luid, other, XRT_LUID_SIZE);
	e.luid_valid = 1;
	REQUIRE(scm_link_register(f, 5, &e) == XRT_SUCCESS);
	// F: no tap (macOS / Linux / a Windows session before its tap) -> UNAVAILABLE (no tap).
	scm_link_connect(f, 6, 106, kLinkPid + 6, XRT_CLIENT_CLASS_APP);
	auto ff = link_reg("Link F", true, false);
	REQUIRE(scm_link_register(f, 6, &ff) == XRT_SUCCESS);
	// G: the fill arm declined a texture-class session -> that reason, verbatim.
	scm_link_connect(f, 7, 107, kLinkPid + 7, XRT_CLIENT_CLASS_APP);
	auto g = link_reg("Link G", true, false);
	g.tap_unavailable_reason = XRT_APP_SHARE_UNAVAILABLE_SHARED_TEXTURE;
	REQUIRE(scm_link_register(f, 7, &g) == XRT_SUCCESS);
	// A browser's connection may not register a share link at all.
	scm_link_connect(f, 8, 108, kLinkPid + 8, XRT_CLIENT_CLASS_PRESENT_OWNER);
	auto h = link_reg("Link H");
	CHECK(scm_link_register(f, 8, &h) == XRT_ERROR_FEATURE_NOT_SUPPORTED);
	// A lift-only link registers nothing that is listed.
	scm_link_connect(f, 9, 109, kLinkPid + 9, XRT_CLIENT_CLASS_APP);
	auto l = link_reg("Link L");
	l.caps = XRT_SHARE_LINK_CAP_LIFT;
	REQUIRE(scm_link_register(f, 9, &l) == XRT_SUCCESS);

	scm_rescan_apps(f);
	auto v = listing(f);
	CHECK(v.size() == 5); // A B E F G
	struct expect
	{
		const char *name;
		uint32_t availability;
		uint32_t state;
	} want[] = {
	    {"Link A", XRT_APP_SHARE_AVAILABLE, XRT_STEREO_CAMERA_STATE_AVAILABLE},
	    {"Link B", XRT_APP_SHARE_UNAVAILABLE_UNSUPPORTED_API, XRT_STEREO_CAMERA_STATE_UNAVAILABLE},
	    {"Link E", XRT_APP_SHARE_UNAVAILABLE_OTHER_ADAPTER, XRT_STEREO_CAMERA_STATE_UNAVAILABLE},
	    {"Link F", XRT_APP_SHARE_UNAVAILABLE_NO_TAP, XRT_STEREO_CAMERA_STATE_UNAVAILABLE},
	    {"Link G", XRT_APP_SHARE_UNAVAILABLE_SHARED_TEXTURE, XRT_STEREO_CAMERA_STATE_UNAVAILABLE},
	};
	for (const auto &w : want) {
		INFO(w.name);
		const auto *p = find_named(v, w.name);
		REQUIRE(p != nullptr);
		CHECK(is_app(*p));
		CHECK(p->state == w.state);
		xrt_app_share_source_info ai;
		REQUIRE(scm_app_source(f, p->camera_id, &ai) == XRT_SUCCESS);
		CHECK(ai.availability == w.availability);
		CHECK(ai.source == XRT_STEREO_CAMERA_SOURCE_APP_LINK);
	}
	CHECK(find_named(v, "Link C") == nullptr);
	CHECK(find_named(v, "Link D") == nullptr);
	CHECK(find_named(v, "Link L") == nullptr);

	// Churn: re-registering the same connection never duplicates; a session
	// end lingers UNAVAILABLE; the same exe + instance gets its id back.
	const uint64_t id_a = find_named(v, "Link A")->camera_id;
	REQUIRE(scm_link_register(f, 1, &a) == XRT_SUCCESS);
	scm_rescan_apps(f);
	CHECK(listing(f).size() == 5);
	auto a_end = a;
	a_end.session_running = 0;
	REQUIRE(scm_link_register(f, 1, &a_end) == XRT_SUCCESS);
	scm_rescan_apps(f);
	v = listing(f);
	REQUIRE(find_named(v, "Link A") != nullptr);
	CHECK(find_named(v, "Link A")->state == XRT_STEREO_CAMERA_STATE_UNAVAILABLE);
	REQUIRE(scm_link_register(f, 1, &a) == XRT_SUCCESS);
	scm_rescan_apps(f);
	v = listing(f);
	REQUIRE(find_named(v, "Link A") != nullptr);
	CHECK(find_named(v, "Link A")->camera_id == id_a);
	CHECK(find_named(v, "Link A")->state == XRT_STEREO_CAMERA_STATE_AVAILABLE);

	// The opt-out arriving later delists at once (no linger).
	auto b_out = b;
	b_out.opt_out = 1;
	REQUIRE(scm_link_register(f, 2, &b_out) == XRT_SUCCESS);
	scm_rescan_apps(f);
	CHECK(find_named(listing(f), "Link B") == nullptr);

	// Disconnects: lingered, then gone.
	for (uint32_t s = 1; s <= 9; s++) {
		scm_link_disconnect(f, s);
	}
	scm_rescan_apps(f);
	os_nanosleep(350ll * 1000 * 1000);
	scm_rescan_apps(f);
	CHECK(listing(f).empty());

	// No importer in this service (macOS / Linux): a tappable link is "no tap".
	scm_set_link_importer(false, nullptr);
	scm_link_connect(f, 1, 111, kLinkPid + 11, XRT_CLIENT_CLASS_APP);
	auto m = link_reg("Link M");
	REQUIRE(scm_link_register(f, 1, &m) == XRT_SUCCESS);
	scm_rescan_apps(f);
	v = listing(f);
	REQUIRE(v.size() == 1);
	xrt_app_share_source_info ai;
	REQUIRE(scm_app_source(f, v[0].camera_id, &ai) == XRT_SUCCESS);
	CHECK(ai.availability == XRT_APP_SHARE_UNAVAILABLE_NO_TAP);
	scm_link_disconnect(f, 1);
	scm_destroy(f);
}

TEST_CASE("app share link: start -> SHARE_REQUESTED -> ring -> frames; resize; stop -> SHARE_STOPPED",
          "[stereo_camera_manager][app_share_link]")
{
	scm_fake_plugin_set(nullptr, nullptr, 0);
	scm_set_link_importer(true, nullptr);
	scm_app_options o = {};
	o.dev = -1;
	o.fake = -1;
	o.consumer_sees_apps = true;
	o.prompt_mode = 1; // always
	scm_fixture *f = scm_create_apps(&o);
	REQUIRE(f != nullptr);
	scm_link_connect(f, 1, 201, kLinkPid + 21, XRT_CLIENT_CLASS_APP);
	// lift + share on ONE connection: a link that also lifts is the same app entry.
	auto r = link_reg("Shared Viewer");
	r.caps = XRT_SHARE_LINK_CAP_LIFT | XRT_SHARE_LINK_CAP_SHARE;
	REQUIRE(scm_link_register(f, 1, &r) == XRT_SUCCESS);
	scm_rescan_apps(f);
	auto v = listing(f);
	REQUIRE(v.size() == 1);
	const uint64_t id = v[0].camera_id;
	xrt_share_link_poll p = {};
	REQUIRE(scm_link_poll(f, 1, &p) == XRT_SUCCESS);
	CHECK(p.state == XRT_SHARE_LINK_STATE_NONE); // nobody shares it yet

	// The consent arm is the camera's share prompt: a Deny never reaches the app.
	scm_set_share_answer(U_CAMERA_CONSENT_PROMPT_DENY);
	uint64_t sid = 0;
	auto req = share_req(id);
	REQUIRE(scm_stream_create(f, &req, &sid) == XRT_SUCCESS);
	CHECK(scm_stream_start(f, sid) != XRT_SUCCESS);
	REQUIRE(scm_link_poll(f, 1, &p) == XRT_SUCCESS);
	CHECK(p.state == XRT_SHARE_LINK_STATE_NONE);
	scm_stream_destroy(f, sid);

	scm_set_share_answer(U_CAMERA_CONSENT_PROMPT_ALLOW);
	REQUIRE(scm_stream_create(f, &req, &sid) == XRT_SUCCESS);
	REQUIRE(scm_stream_start(f, sid) == XRT_SUCCESS);
	xrt_stereo_camera_stream_layout lay;
	REQUIRE(scm_stream_map(f, sid, &lay) == XRT_SUCCESS);
	REQUIRE(link_wait_state(f, 1, XRT_SHARE_LINK_STATE_SHARE_REQUESTED, &p));
	CHECK(p.eye_w == 1280);
	CHECK(p.eye_h == 720);
	CHECK(p.fps > 0.0f);
	const uint32_t sg = p.stream_generation;
	CHECK(sg >= 1);

	// A ring for an older stream generation is dropped, never imported.
	auto stale = link_ring(7, sg - 1, 1600, 900);
	REQUIRE(scm_link_publish_ring(f, 1, &stale) == XRT_SUCCESS);
	auto ring1 = link_ring(1, sg, 1600, 900);
	REQUIRE(scm_link_publish_ring(f, 1, &ring1) == XRT_SUCCESS);
	CHECK(link_pump(f, 1, sid, 1, 1, 5) == 5);
	CHECK(scm_link_imports() == 1);
	CHECK(scm_link_last_import_generation() == 1);
	CHECK(scm_link_last_import_slot_width() == 3200);
	xrt_app_share_frame_info app = scm_last_app(f);
	CHECK(app.valid == 1);
	CHECK(app.content_width == 1280);
	CHECK(app.content_generation == 1);

	// Resize: the app re-creates its ring (generation 2); frames of ring 1 are
	// dropped, ring 2 is imported and the old one released.
	auto ring2 = link_ring(2, sg, 1920, 1080);
	REQUIRE(scm_link_publish_ring(f, 1, &ring2) == XRT_SUCCESS);
	CHECK(link_pump(f, 1, sid, 2, 2, 3) == 3);
	CHECK(scm_link_imports() == 2);
	CHECK(scm_link_live_imports() == 1);
	CHECK(scm_link_last_import_generation() == 2);
	app = scm_last_app(f);
	CHECK(app.content_generation == 2);

	// Stop: once the source closes (after the camera linger) the link hears
	// SHARE_STOPPED exactly once, then NONE; the import is released.
	scm_stream_destroy(f, sid);
	REQUIRE(link_wait_state(f, 1, XRT_SHARE_LINK_STATE_SHARE_STOPPED, &p, 6000));
	REQUIRE(scm_link_poll(f, 1, &p) == XRT_SUCCESS);
	CHECK(p.state == XRT_SHARE_LINK_STATE_NONE);
	CHECK(scm_link_live_imports() == 0);

	// A new start bumps the stream generation.
	REQUIRE(scm_stream_create(f, &req, &sid) == XRT_SUCCESS);
	REQUIRE(scm_stream_start(f, sid) == XRT_SUCCESS);
	REQUIRE(link_wait_state(f, 1, XRT_SHARE_LINK_STATE_SHARE_REQUESTED, &p));
	CHECK(p.stream_generation > sg);
	scm_stream_destroy(f, sid);
	scm_link_disconnect(f, 1);
	scm_destroy(f);
	CHECK(scm_link_live_imports() == 0);
	scm_set_link_importer(false, nullptr);
}

TEST_CASE("app share link: the app exits mid-stream -> SOURCE_LOST", "[stereo_camera_manager][app_share_link]")
{
	scm_fake_plugin_set(nullptr, nullptr, 0);
	scm_set_link_importer(true, nullptr);
	scm_app_options o = {};
	o.dev = 1; // no prompt
	o.fake = -1;
	o.linger_ms = 300;
	o.consumer_sees_apps = true;
	scm_fixture *f = scm_create_apps(&o);
	REQUIRE(f != nullptr);
	scm_link_connect(f, 1, 301, kLinkPid + 31, XRT_CLIENT_CLASS_APP);
	auto r = link_reg("Leaving App");
	REQUIRE(scm_link_register(f, 1, &r) == XRT_SUCCESS);
	scm_rescan_apps(f);
	auto v = listing(f);
	REQUIRE(v.size() == 1);
	const uint64_t id = v[0].camera_id;
	uint64_t sid = 0;
	auto req = share_req(id);
	REQUIRE(scm_stream_create(f, &req, &sid) == XRT_SUCCESS);
	REQUIRE(scm_stream_start(f, sid) == XRT_SUCCESS);
	xrt_stereo_camera_stream_layout lay;
	REQUIRE(scm_stream_map(f, sid, &lay) == XRT_SUCCESS);
	xrt_share_link_poll p = {};
	REQUIRE(link_wait_state(f, 1, XRT_SHARE_LINK_STATE_SHARE_REQUESTED, &p));
	auto ring = link_ring(1, p.stream_generation, 1280, 720);
	REQUIRE(scm_link_publish_ring(f, 1, &ring) == XRT_SUCCESS);
	REQUIRE(link_pump(f, 1, sid, 1, 1, 3) == 3);
	xrt_stereo_camera_event ev;
	while (scm_poll_event(f, &ev)) {
	}

	// The app process exits: its link connection closes.
	scm_link_disconnect(f, 1);
	bool lost = false;
	while (scm_poll_event(f, &ev)) {
		lost |= ev.kind == XRT_STEREO_CAMERA_EVENT_STREAM_ENDED && ev.stream_id == sid &&
		        ev.value == XRT_STEREO_CAMERA_END_SOURCE_LOST;
	}
	CHECK(lost);
	bool ready = false;
	xrt_stereo_camera_frame_info fi;
	const uint8_t *slot = nullptr;
	CHECK(scm_acquire(f, sid, &ready, &fi, &slot) == XRT_ERROR_STEREO_CAMERA_STREAM_ENDED);
	v = listing(f);
	REQUIRE(v.size() == 1); // lingers UNAVAILABLE
	CHECK(v[0].state == XRT_STEREO_CAMERA_STATE_UNAVAILABLE);
	xrt_app_share_source_info ai;
	REQUIRE(scm_app_source(f, id, &ai) == XRT_SUCCESS);
	CHECK(ai.availability == XRT_APP_SHARE_UNAVAILABLE_EXITED);
	scm_stream_destroy(f, sid);
	const int64_t t_end = os_monotonic_get_ns() + 3ll * 1000 * 1000 * 1000;
	while (scm_link_live_imports() != 0 && os_monotonic_get_ns() < t_end) {
		os_nanosleep(5 * 1000 * 1000);
	}
	CHECK(scm_link_live_imports() == 0); // the camera thread closed the source
	scm_destroy(f);
	scm_set_link_importer(false, nullptr);
}

TEST_CASE("app share link: the DXR_APP_SHARE_FAKE=link registrant end to end",
          "[stereo_camera_manager][app_share_link]")
{
	scm_fake_plugin_set(nullptr, nullptr, 0);
	scm_app_options o = {};
	o.dev = 1;
	o.fake = -1;
	o.fake_link = 1;
	scm_fixture *f = scm_create_apps(&o);
	REQUIRE(f != nullptr);
	// The registrant registers from its own thread; the rescan lists it.
	std::vector<xrt_stereo_camera_properties> v;
	const int64_t t_list = os_monotonic_get_ns() + 3ll * 1000 * 1000 * 1000;
	while (v.empty() && os_monotonic_get_ns() < t_list) {
		scm_rescan_apps(f);
		v = listing(f);
		if (v.empty()) {
			os_nanosleep(10 * 1000 * 1000);
		}
	}
	REQUIRE(v.size() == 1);
	CHECK(is_app(v[0]));
	CHECK(v[0].state == XRT_STEREO_CAMERA_STATE_AVAILABLE);
	xrt_app_share_source_info ai;
	REQUIRE(scm_app_source(f, v[0].camera_id, &ai) == XRT_SUCCESS);
	CHECK(ai.source == XRT_STEREO_CAMERA_SOURCE_APP_LINK);
	CHECK(ai.availability == XRT_APP_SHARE_AVAILABLE);
	CHECK(std::string(ai.executable_name) == "displayxr-app-share-fake-link");

	uint64_t sid = 0;
	auto req = share_req(v[0].camera_id);
	REQUIRE(scm_stream_create(f, &req, &sid) == XRT_SUCCESS);
	REQUIRE(scm_stream_start(f, sid) == XRT_SUCCESS);
	xrt_stereo_camera_stream_layout lay;
	REQUIRE(scm_stream_map(f, sid, &lay) == XRT_SUCCESS);
	CHECK(lay.width == 2560);
	CHECK(lay.height == 720);
	uint32_t frames = 0;
	uint64_t last_index = 0;
	xrt_stereo_camera_frame_info fi = {};
	const uint8_t *slot = nullptr;
	const int64_t t_end = os_monotonic_get_ns() + 8ll * 1000 * 1000 * 1000;
	while (frames < 10 && os_monotonic_get_ns() < t_end) {
		bool ready = false;
		REQUIRE(scm_acquire(f, sid, &ready, &fi, &slot) == XRT_SUCCESS);
		if (!ready) {
			os_nanosleep(5 * 1000 * 1000);
			continue;
		}
		CHECK(fi.frame_index > last_index);
		last_index = fi.frame_index;
		frames++;
	}
	REQUIRE(frames == 10);
	const xrt_app_share_frame_info app = scm_last_app(f);
	CHECK(app.valid == 1);
	CHECK(app.content_width == 1280);
	CHECK(app.content_height == 720);
	CHECK(app.commit_time_ns > 0);
	// The CPU ring carried the synthetic pattern through unchanged: its zero-disparity band reads 0.
	const uint8_t *y = slot + fi.plane_offset[0];
	uint32_t b0 = 0, b1 = 0;
	u_app_share_fake_band(720, &b0, &b1);
	CHECK(band_disparity(y, fi.row_pitch[0], 1280, b0 + 4, 16, 40) == 0);
	const int d_bottom = band_disparity(y, fi.row_pitch[0], 1280, 680, 8, 40);
	CHECK(d_bottom == Catch::Approx(u_app_share_fake_disparity_px(1280, 720, 684)).margin(1.0));
	std::printf("[manager app share link] fake link: %u frames, bottom d = %d px\n", frames, d_bottom);
	scm_stream_destroy(f, sid);
	scm_destroy(f);
}

namespace {

//! One started share of a fresh link in slot 1; returns the stream id and the stream generation.
uint64_t
link_started(scm_fixture *f, uint32_t client_id, const char *name, uint32_t *out_sg)
{
	scm_link_connect(f, 1, client_id, kLinkPid + client_id, XRT_CLIENT_CLASS_APP);
	auto r = link_reg(name);
	REQUIRE(scm_link_register(f, 1, &r) == XRT_SUCCESS);
	scm_rescan_apps(f);
	auto v = listing(f);
	REQUIRE(v.size() == 1);
	uint64_t sid = 0;
	auto req = share_req(v[0].camera_id);
	REQUIRE(scm_stream_create(f, &req, &sid) == XRT_SUCCESS);
	REQUIRE(scm_stream_start(f, sid) == XRT_SUCCESS);
	xrt_stereo_camera_stream_layout lay;
	REQUIRE(scm_stream_map(f, sid, &lay) == XRT_SUCCESS);
	xrt_share_link_poll p = {};
	REQUIRE(link_wait_state(f, 1, XRT_SHARE_LINK_STATE_SHARE_REQUESTED, &p));
	*out_sg = p.stream_generation;
	xrt_stereo_camera_event ev;
	while (scm_poll_event(f, &ev)) {
	}
	return sid;
}

//! The share ended SOURCE_LOST and the app heard SHARE_STOPPED.
void
check_share_failed(scm_fixture *f, uint64_t sid)
{
	xrt_share_link_poll p = {};
	CHECK(link_wait_state(f, 1, XRT_SHARE_LINK_STATE_SHARE_STOPPED, &p, 6000));
	bool lost = false;
	xrt_stereo_camera_event ev;
	while (scm_poll_event(f, &ev)) {
		lost |= ev.kind == XRT_STEREO_CAMERA_EVENT_STREAM_ENDED && ev.stream_id == sid &&
		        ev.value == XRT_STEREO_CAMERA_END_SOURCE_LOST;
	}
	CHECK(lost);
	bool ready = false;
	xrt_stereo_camera_frame_info fi;
	const uint8_t *slot = nullptr;
	CHECK(scm_acquire(f, sid, &ready, &fi, &slot) == XRT_ERROR_STEREO_CAMERA_STREAM_ENDED);
}

scm_fixture *
link_fixture()
{
	scm_fake_plugin_set(nullptr, nullptr, 0);
	scm_app_options o = {};
	o.dev = 1;
	o.fake = -1;
	o.consumer_sees_apps = true;
	return scm_create_apps(&o);
}

} // namespace

TEST_CASE("app share link: a ring needs one texture per slot; a declared fence is awaited",
          "[stereo_camera_manager][app_share_link]")
{
	scm_set_link_importer(true, nullptr);
	scm_fixture *f = link_fixture();
	REQUIRE(f != nullptr);
	uint32_t sg = 0;
	const uint64_t sid = link_started(f, 401, "Fenced App", &sg);

	// No textures for a 3-slot ring from a real app: refused, nothing imported.
	auto bare = link_ring(1, sg, 1280, 720);
	CHECK(scm_link_publish_ring_n(f, 1, &bare, 0) == XRT_ERROR_FEATURE_NOT_SUPPORTED);
	CHECK(scm_link_publish_ring_n(f, 1, &bare, 2) == XRT_ERROR_FEATURE_NOT_SUPPORTED);
	os_nanosleep(150ll * 1000 * 1000);
	CHECK(scm_link_imports() == 0);

	// A ring that declares its fence is imported only with it, and the importer gets it.
	auto fenced = link_ring(2, sg, 1280, 720);
	fenced.has_fence = 1;
	REQUIRE(scm_link_publish_ring(f, 1, &fenced) == XRT_SUCCESS);
	os_nanosleep(200ll * 1000 * 1000);
	CHECK(scm_link_imports() == 0); // still waiting for the fence
	REQUIRE(scm_link_publish_fence(f, 1, 2) == XRT_SUCCESS);
	CHECK(link_pump(f, 1, sid, 2, 1, 3) == 3);
	CHECK(scm_link_imports() == 1);
	CHECK(scm_link_last_import_texture_count() == XRT_SHARE_LINK_RING_SLOTS);
	CHECK(scm_link_last_import_had_fence());

	// A declared fence that never comes: the share ends loudly, not an endless wait.
	auto orphan = link_ring(3, sg, 1280, 720);
	orphan.has_fence = 1;
	REQUIRE(scm_link_publish_ring(f, 1, &orphan) == XRT_SUCCESS);
	check_share_failed(f, sid);
	CHECK(scm_link_imports() == 1);
	scm_stream_destroy(f, sid);
	scm_link_disconnect(f, 1);
	scm_destroy(f);
	CHECK(scm_link_live_imports() == 0);
	scm_set_link_importer(false, nullptr);
}

TEST_CASE("app share link: a failed import ends the share SOURCE_LOST and tells the app",
          "[stereo_camera_manager][app_share_link]")
{
	scm_set_link_importer(true, nullptr);
	scm_set_link_import_fails(true);
	scm_fixture *f = link_fixture();
	REQUIRE(f != nullptr);
	uint32_t sg = 0;
	const uint64_t sid = link_started(f, 402, "Broken Ring", &sg);
	auto ring = link_ring(1, sg, 1280, 720);
	REQUIRE(scm_link_publish_ring(f, 1, &ring) == XRT_SUCCESS);
	check_share_failed(f, sid);
	CHECK(scm_link_imports() == 0);
	CHECK(scm_link_last_import_texture_count() == XRT_SHARE_LINK_RING_SLOTS); // handed over, and closed
	scm_stream_destroy(f, sid);
	scm_link_disconnect(f, 1);
	scm_destroy(f);
	scm_set_link_importer(false, nullptr);
}

TEST_CASE("app share link: logical refusals are never a dead pipe", "[stereo_camera_manager][app_share_link]")
{
	scm_set_link_importer(true, nullptr);
	scm_fixture *f = link_fixture();
	REQUIRE(f != nullptr);
	// A connection that never registered a share link (a lift-only link).
	scm_link_connect(f, 1, 501, kLinkPid + 501, XRT_CLIENT_CLASS_APP);
	xrt_share_link_frame fr = {};
	xrt_share_link_poll p = {};
	CHECK(scm_link_frame_ready(f, 1, &fr, &p) == XRT_ERROR_OUTPUT_REQUEST_FAILURE);
	CHECK(p.state == XRT_SHARE_LINK_STATE_NONE);
	auto ring = link_ring(1, 1, 64, 64);
	CHECK(scm_link_publish_ring(f, 1, &ring) == XRT_ERROR_OUTPUT_REQUEST_FAILURE);
	CHECK(scm_link_publish_fence(f, 1, 1) == XRT_ERROR_OUTPUT_REQUEST_FAILURE);
	REQUIRE(scm_link_poll(f, 1, &p) == XRT_SUCCESS);
	CHECK(p.state == XRT_SHARE_LINK_STATE_NONE);

	// The link table full: a refusal with its own code.
	auto r = link_reg("Many");
	xrt_result_t xret = XRT_SUCCESS;
	uint32_t ok = 0;
	for (uint32_t id = 600; id < 600 + 64 && xret == XRT_SUCCESS; id++) {
		scm_link_register_as(f, 1, id, &r, &xret);
		ok += xret == XRT_SUCCESS ? 1u : 0u;
	}
	CHECK(xret == XRT_ERROR_CLIENT_LIMIT_REACHED);
	CHECK(ok >= 32);
	scm_destroy(f); // frees every registered link
	scm_set_link_importer(false, nullptr);
}

TEST_CASE("app share link: an incomplete ring hook table is refused whole", "[stereo_camera_manager][app_share_link]")
{
	scm_set_link_partial_hooks();
	scm_fixture *f = link_fixture();
	REQUIRE(f != nullptr);
	scm_link_connect(f, 1, 701, kLinkPid + 701, XRT_CLIENT_CLASS_APP);
	auto r = link_reg("Half Hooks");
	REQUIRE(scm_link_register(f, 1, &r) == XRT_SUCCESS);
	scm_rescan_apps(f);
	auto v = listing(f);
	REQUIRE(v.size() == 1);
	xrt_app_share_source_info ai;
	REQUIRE(scm_app_source(f, v[0].camera_id, &ai) == XRT_SUCCESS);
	CHECK(ai.availability == XRT_APP_SHARE_UNAVAILABLE_NO_TAP); // never imported, never leaked
	scm_link_disconnect(f, 1);
	scm_destroy(f);
	scm_set_link_importer(false, nullptr);
}


/*
 *
 * Spec v8: modes, controls, stills — through the real manager over a fake
 * plug-in (tests_stereo_camera_manager_glue.h, scm_create_v8_plugin).
 *
 */

namespace {

xrt_stereo_camera_stream_request
v8_req(uint64_t camera_id, uint32_t eye_w, uint32_t eye_h)
{
	xrt_stereo_camera_stream_request r{};
	r.camera_id = camera_id;
	r.output = XRT_STEREO_CAMERA_OUTPUT_RECTIFIED;
	r.format = XRT_STEREO_CAMERA_FORMAT_GRAY8;
	r.transport = XRT_STEREO_CAMERA_TRANSPORT_SHARED_MEMORY;
	r.max_frame_rate = 0.0f;
	r.eye_width = eye_w;
	r.eye_height = eye_h;
	return r;
}

//! Create + start + map a stream; returns its id (0 on failure).
uint64_t
v8_start(scm_fixture *f, uint64_t camera_id, uint32_t eye_w, uint32_t eye_h, xrt_stereo_camera_stream_layout *lay)
{
	xrt_stereo_camera_stream_request r = v8_req(camera_id, eye_w, eye_h);
	uint64_t id = 0;
	if (scm_stream_create(f, &r, &id) != XRT_SUCCESS || scm_stream_start(f, id) != XRT_SUCCESS ||
	    scm_stream_map(f, id, lay) != XRT_SUCCESS) {
		return 0;
	}
	return id;
}

//! Wait (<= 3 s) for a frame on the LAST mapped stream; true with its info + slot.
bool
v8_frame(scm_fixture *f, uint64_t id, xrt_stereo_camera_frame_info *fi, const uint8_t **slot)
{
	for (int i = 0; i < 300; i++) {
		bool ready = false;
		if (scm_acquire(f, id, &ready, fi, slot) == XRT_SUCCESS && ready && *slot != nullptr) {
			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return false;
}

//! Wait (<= 3 s) for an event of @p kind.
bool
v8_event(scm_fixture *f, uint32_t kind, xrt_stereo_camera_event *out)
{
	for (int i = 0; i < 300; i++) {
		while (scm_poll_event(f, out)) {
			if (out->kind == kind) {
				return true;
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return false;
}

} // namespace

TEST_CASE("spec v8: modes are listed, one horizontal field of view, default first", "[stereo_camera_manager][v8]")
{
	scm_fixture *f = scm_create_v8_plugin();
	REQUIRE(f != nullptr);
	REQUIRE(scm_count(f) == 1);
	xrt_stereo_camera_properties p;
	REQUIRE(scm_properties(f, 0, &p) == XRT_SUCCESS);
	CHECK(p.eye_width == 320);
	xrt_stereo_camera_mode_list ml;
	REQUIRE(scm_modes(f, p.camera_id, &ml) == XRT_SUCCESS);
	// 640x360 (a centred 16:9 cut) and 1280x720 are kept; 640x120 (relative height 0.25) is dropped.
	REQUIRE(ml.count == 5);
	CHECK(ml.modes[0].eye_width == 320);
	CHECK(ml.modes[0].eye_height == 240);
	CHECK(ml.modes[0].flags ==
	      (XRT_STEREO_CAMERA_MODE_DEFAULT | XRT_STEREO_CAMERA_MODE_STREAM | XRT_STEREO_CAMERA_MODE_STILL));
	CHECK(ml.modes[1].eye_width == 160);
	CHECK(ml.modes[1].flags == XRT_STEREO_CAMERA_MODE_STREAM);
	CHECK(ml.modes[2].eye_width == 640);
	CHECK(ml.modes[2].flags == (XRT_STEREO_CAMERA_MODE_STREAM | XRT_STEREO_CAMERA_MODE_STILL));
	CHECK(ml.modes[3].eye_width == 640);
	CHECK(ml.modes[3].eye_height == 360);
	CHECK(ml.modes[3].max_frame_rate == 0.0f); // not known until it runs
	CHECK(ml.modes[4].eye_width == 1280);
	CHECK(ml.modes[4].eye_height == 720);
	CHECK(scm_modes(f, 999, &ml) == XRT_ERROR_INPUT_UNSUPPORTED);

	// A mode that is not listed is refused at create.
	xrt_stereo_camera_stream_request bad = v8_req(p.camera_id, 640, 120);
	uint64_t id = 0;
	CHECK(scm_stream_create(f, &bad, &id) == XRT_ERROR_INPUT_UNSUPPORTED);

	// Calibration of a mode: the device calibration rescaled, pixel-centre aware.
	xrt_stereo_camera_calibration c;
	REQUIRE(scm_calibration_mode(f, p.camera_id, XRT_STEREO_CAMERA_OUTPUT_RAW, 640, 480, &c) == XRT_SUCCESS);
	CHECK(c.eye[0].width == 640);
	CHECK(c.eye[0].fx == Catch::Approx(400.0f));
	CHECK(c.eye[0].cx == Catch::Approx(319.5f));
	CHECK(c.eye[1].cy == Catch::Approx(239.5f));
	CHECK(c.baseline_mm == Catch::Approx(50.0f));
	REQUIRE(scm_calibration_mode(f, p.camera_id, XRT_STEREO_CAMERA_OUTPUT_RECTIFIED, 160, 120, &c) == XRT_SUCCESS);
	CHECK(c.eye[0].fx == Catch::Approx(100.0f));
	CHECK(c.eye[0].cx == Catch::Approx(79.5f));
	CHECK(c.eye[0].model == XRT_PLUGIN_STEREO_CAMERA_DISTORTION_NONE);
	// Another vertical extent: scale by the width, keep the principal point about the centre.
	REQUIRE(scm_calibration_mode(f, p.camera_id, XRT_STEREO_CAMERA_OUTPUT_RAW, 640, 360, &c) == XRT_SUCCESS);
	CHECK(c.eye[0].height == 360);
	CHECK(c.eye[0].fx == Catch::Approx(400.0f));
	CHECK(c.eye[0].fy == Catch::Approx(400.0f));
	CHECK(c.eye[0].cx == Catch::Approx(319.5f));
	CHECK(c.eye[0].cy == Catch::Approx(179.5f)); // (119.5 + 0.5 - 120) * 2 + 180 - 0.5
	CHECK(scm_calibration_mode(f, p.camera_id, XRT_STEREO_CAMERA_OUTPUT_RAW, 640, 120, &c) ==
	      XRT_ERROR_INPUT_UNSUPPORTED);
	// v7: no mode = the calibration's own size.
	REQUIRE(scm_calibration(f, p.camera_id, XRT_STEREO_CAMERA_OUTPUT_RAW, &c) == XRT_SUCCESS);
	CHECK(c.eye[0].width == 320);
	CHECK(c.eye[0].fx == Catch::Approx(200.0f));

	// The fixed default focus distance is exposed with the capabilities.
	xrt_stereo_camera_control_caps caps;
	REQUIRE(scm_control_caps(f, p.camera_id, &caps) == XRT_SUCCESS);
	CHECK(caps.default_focus_distance_diopters == Catch::Approx(0.8f));

	// Once the 0-rate mode has run, its MEASURED rate is listed.
	xrt_stereo_camera_stream_layout lay;
	uint64_t s = v8_start(f, p.camera_id, 640, 360, &lay);
	REQUIRE(s != 0);
	bool measured = false;
	for (int i = 0; i < 300 && !measured; i++) {
		REQUIRE(scm_modes(f, p.camera_id, &ml) == XRT_SUCCESS);
		measured = ml.modes[3].max_frame_rate > 0.0f;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	CHECK(measured);
	scm_destroy(f);
}

TEST_CASE("spec v8: the source runs the largest started mode; smaller streams are downscaled",
          "[stereo_camera_manager][v8]")
{
	scm_fixture *f = scm_create_v8_plugin();
	REQUIRE(f != nullptr);
	REQUIRE(scm_count(f) == 1);
	xrt_stereo_camera_properties p;
	REQUIRE(scm_properties(f, 0, &p) == XRT_SUCCESS);

	// Alone, a half-size stream makes the source run the half-size mode.
	xrt_stereo_camera_stream_layout lay;
	uint64_t small = v8_start(f, p.camera_id, 160, 120, &lay);
	REQUIRE(small != 0);
	CHECK(lay.width == 320);
	CHECK(lay.height == 120);
	xrt_stereo_camera_frame_info fi;
	const uint8_t *slot = nullptr;
	REQUIRE(v8_frame(f, small, &fi, &slot));
	CHECK(fi.width == 320);
	scm_v8_state st;
	scm_v8_get_state(&st);
	CHECK(st.last_mode_index == 1);

	// A double-size stream joins: the source switches to it, the default-size
	// stream below is served downscaled, each half from its own half.
	xrt_stereo_camera_stream_layout big_lay;
	uint64_t big = v8_start(f, p.camera_id, 640, 480, &big_lay);
	REQUIRE(big != 0);
	CHECK(big_lay.width == 1280);
	REQUIRE(v8_frame(f, big, &fi, &slot));
	CHECK(fi.width == 1280);
	CHECK(fi.height == 480);
	scm_v8_get_state(&st);
	CHECK(st.last_mode_index == 2);

	xrt_stereo_camera_stream_layout def_lay;
	uint64_t def = v8_start(f, p.camera_id, 0, 0, &def_lay); // no mode = the default, as in v7
	REQUIRE(def != 0);
	CHECK(def_lay.width == 640);
	REQUIRE(v8_frame(f, def, &fi, &slot));
	CHECK(fi.width == 640);
	CHECK(fi.height == 240);
	scm_v8_get_state(&st);
	CHECK(st.last_mode_index == 2); // still the largest
	const uint32_t pitch = fi.row_pitch[0];
	for (uint32_t y : {0u, 120u, 239u}) {
		const uint8_t *row = slot + (size_t)y * pitch;
		// Output row y averages source rows 2y, 2y + 1 of the 480-row source.
		const double want = 20.0 + 100.0 * (2.0 * y + 0.5) / 479.0;
		CHECK(std::abs(row[0] - want) <= 1.0);
		CHECK(row[319] == row[0]); // no bleed across the seam
		CHECK(std::abs(row[320] - (want + 120.0)) <= 1.0);
		CHECK(row[639] == row[320]);
	}

	// The big stream stops: the source goes back down to the largest left (the default).
	REQUIRE(scm_stream_stop(f, big) == XRT_SUCCESS);
	bool down = false;
	for (int i = 0; i < 300 && !down; i++) {
		scm_v8_get_state(&st);
		down = st.last_mode_index == 0;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	CHECK(down);
	scm_destroy(f);
}

TEST_CASE("spec v8: a refused mode switch ends only the streams it cannot serve", "[stereo_camera_manager][v8]")
{
	scm_fixture *f = scm_create_v8_plugin();
	REQUIRE(f != nullptr);
	REQUIRE(scm_count(f) == 1);
	xrt_stereo_camera_properties p;
	REQUIRE(scm_properties(f, 0, &p) == XRT_SUCCESS);
	scm_v8_refuse_modes(true);
	xrt_stereo_camera_stream_layout lay;
	uint64_t def = v8_start(f, p.camera_id, 320, 240, &lay);
	REQUIRE(def != 0);
	xrt_stereo_camera_frame_info fi;
	const uint8_t *slot = nullptr;
	REQUIRE(v8_frame(f, def, &fi, &slot));
	drain(f, 0);
	uint64_t big = v8_start(f, p.camera_id, 640, 480, &lay);
	REQUIRE(big != 0);
	xrt_stereo_camera_event ev;
	REQUIRE(v8_event(f, XRT_STEREO_CAMERA_EVENT_STREAM_ENDED, &ev));
	CHECK(ev.stream_id == big);
	CHECK(ev.value == XRT_STEREO_CAMERA_END_SOURCE_BUSY);
	// The default stream keeps streaming (re-map it: the big stream's map replaced it).
	REQUIRE(scm_stream_map(f, def, &lay) == XRT_SUCCESS);
	REQUIRE(v8_frame(f, def, &fi, &slot));
	CHECK(fi.width == 640);
	scm_v8_refuse_modes(false);
	scm_destroy(f);
}

TEST_CASE("spec v8: controls — one holder per camera, ranges, started streams only, capture state",
          "[stereo_camera_manager][v8]")
{
	scm_fixture *f = scm_create_v8_plugin();
	REQUIRE(f != nullptr);
	REQUIRE(scm_count(f) == 1);
	xrt_stereo_camera_properties p;
	REQUIRE(scm_properties(f, 0, &p) == XRT_SUCCESS);
	xrt_stereo_camera_control_caps caps;
	REQUIRE(scm_control_caps(f, p.camera_id, &caps) == XRT_SUCCESS);
	CHECK(caps.supported_controls == 0xFu);
	CHECK(caps.min_focus_distance_diopters == Catch::Approx(5.0f));
	CHECK(caps.exposure_compensation_step_ev == Catch::Approx(0.5f));

	xrt_stereo_camera_stream_request r = v8_req(p.camera_id, 0, 0);
	uint64_t a = 0;
	REQUIRE(scm_stream_create(f, &r, &a) == XRT_SUCCESS);
	xrt_stereo_camera_controls c{};
	c.valid_controls =
	    XRT_PLUGIN_STEREO_CAMERA_CONTROL_FOCUS_MODE | XRT_PLUGIN_STEREO_CAMERA_CONTROL_FOCUS_DISTANCE;
	c.focus_mode = XRT_PLUGIN_STEREO_CAMERA_FOCUS_MANUAL;
	c.focus_distance_diopters = 2.0f;
	CHECK(scm_set_controls(f, a, &c) == XRT_ERROR_CALL_ORDER_INVALID); // not started
	xrt_stereo_camera_stream_layout lay;
	REQUIRE(scm_stream_start(f, a) == XRT_SUCCESS);
	REQUIRE(scm_stream_map(f, a, &lay) == XRT_SUCCESS);

	xrt_stereo_camera_controls bad = c;
	bad.focus_distance_diopters = 6.0f; // nearer than the lens can focus
	CHECK(scm_set_controls(f, a, &bad) == XRT_ERROR_INPUT_UNSUPPORTED);
	bad = c;
	bad.valid_controls = 0x10; // not a control
	CHECK(scm_set_controls(f, a, &bad) == XRT_ERROR_FEATURE_NOT_SUPPORTED);
	REQUIRE(scm_set_controls(f, a, &c) == XRT_SUCCESS);

	// The source gets the WHOLE state, and the frames carry it back.
	bool seen = false;
	for (int i = 0; i < 300 && !seen; i++) {
		xrt_stereo_camera_frame_info fi;
		const uint8_t *slot = nullptr;
		bool ready = false;
		if (scm_acquire(f, a, &ready, &fi, &slot) == XRT_SUCCESS && ready) {
			xrt_stereo_camera_capture_result res = scm_last_result(f);
			seen = (res.valid_fields & XRT_PLUGIN_STEREO_CAMERA_RESULT_FOCUS_DISTANCE) != 0 &&
			       std::fabs(res.focus_distance_diopters - 2.0f) < 1e-4f;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	CHECK(seen);
	scm_v8_state st;
	scm_v8_get_state(&st);
	CHECK(st.last_controls.valid_controls == c.valid_controls);
	CHECK(st.last_controls.focus_mode == XRT_PLUGIN_STEREO_CAMERA_FOCUS_MANUAL);

	// EV is rounded to the step; a second stream cannot take the controls.
	xrt_stereo_camera_controls ev{};
	ev.valid_controls = XRT_PLUGIN_STEREO_CAMERA_CONTROL_EXPOSURE_COMPENSATION;
	ev.exposure_compensation_ev = 0.7f;
	REQUIRE(scm_set_controls(f, a, &ev) == XRT_SUCCESS);
	uint64_t b = 0;
	REQUIRE(scm_stream_create(f, &r, &b) == XRT_SUCCESS);
	REQUIRE(scm_stream_start(f, b) == XRT_SUCCESS);
	CHECK(scm_set_controls(f, b, &ev) == XRT_ERROR_STEREO_CAMERA_CONTROL_HELD);

	// AUTO_ONCE is edge-triggered; the metering point passes through (natively rectified).
	xrt_stereo_camera_controls once{};
	once.valid_controls =
	    XRT_PLUGIN_STEREO_CAMERA_CONTROL_FOCUS_MODE | XRT_PLUGIN_STEREO_CAMERA_CONTROL_METERING_POINT;
	once.focus_mode = XRT_PLUGIN_STEREO_CAMERA_FOCUS_AUTO_ONCE;
	once.metering_x = 0.25f;
	once.metering_y = 0.75f;
	REQUIRE(scm_set_controls(f, a, &once) == XRT_SUCCESS);
	bool applied = false;
	for (int i = 0; i < 300 && !applied; i++) {
		scm_v8_get_state(&st);
		applied = st.last_controls.focus_mode == XRT_PLUGIN_STEREO_CAMERA_FOCUS_AUTO_ONCE;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	REQUIRE(applied);
	CHECK(st.last_controls.focus_trigger == 1);
	CHECK(st.last_controls.metering_x == Catch::Approx(0.25f));
	CHECK(st.last_controls.exposure_compensation_ev == Catch::Approx(0.5f)); // 0.7 on a 0.5 step
	CHECK((st.last_controls.valid_controls & XRT_PLUGIN_STEREO_CAMERA_CONTROL_FOCUS_DISTANCE) != 0); // merged

	// The holder stops: the source goes back to its defaults and the other stream may take them.
	REQUIRE(scm_stream_stop(f, a) == XRT_SUCCESS);
	bool released = false;
	for (int i = 0; i < 300 && !released; i++) {
		scm_v8_get_state(&st);
		released = st.last_controls.valid_controls == 0;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	CHECK(released);
	CHECK(scm_set_controls(f, b, &ev) == XRT_SUCCESS);
	CHECK(scm_release_controls(f, b) == XRT_SUCCESS);
	scm_destroy(f);
}

TEST_CASE("spec v8: a still is delivered once, in its own section, with its calibration", "[stereo_camera_manager][v8]")
{
	scm_fixture *f = scm_create_v8_plugin();
	REQUIRE(f != nullptr);
	REQUIRE(scm_count(f) == 1);
	xrt_stereo_camera_properties p;
	REQUIRE(scm_properties(f, 0, &p) == XRT_SUCCESS);

	xrt_stereo_camera_stream_request r = v8_req(p.camera_id, 160, 120);
	uint64_t s = 0;
	REQUIRE(scm_stream_create(f, &r, &s) == XRT_SUCCESS);
	xrt_stereo_camera_still_request q{};
	q.output = XRT_STEREO_CAMERA_OUTPUT_RECTIFIED;
	q.format = XRT_STEREO_CAMERA_FORMAT_GRAY8;
	uint64_t rid = 0;
	CHECK(scm_request_still(f, s, &q, &rid) == XRT_ERROR_CALL_ORDER_INVALID); // not started
	xrt_stereo_camera_stream_layout lay;
	REQUIRE(scm_stream_start(f, s) == XRT_SUCCESS);
	REQUIRE(scm_stream_map(f, s, &lay) == XRT_SUCCESS);

	q.eye_width = 160; // a STREAM-only size is not a still size
	q.eye_height = 120;
	CHECK(scm_request_still(f, s, &q, &rid) == XRT_ERROR_INPUT_UNSUPPORTED);
	q.eye_width = 0; // the largest still size: 640 x 480
	q.eye_height = 0;
	REQUIRE(scm_request_still(f, s, &q, &rid) == XRT_SUCCESS);
	CHECK(rid != 0);
	uint64_t again = 0;
	CHECK(scm_request_still(f, s, &q, &again) == XRT_ERROR_CLIENT_LIMIT_REACHED); // one at a time
	xrt_stereo_camera_still_info info;
	std::vector<uint8_t> px((size_t)1280 * 480);
	// Before it is ready, acquire is a call-order error (unless it raced ahead).
	xrt_result_t early = scm_acquire_still(f, s, rid, &info, px.data(), px.size());
	CHECK((early == XRT_ERROR_CALL_ORDER_INVALID || early == XRT_SUCCESS));

	xrt_stereo_camera_event ev;
	REQUIRE(v8_event(f, XRT_STEREO_CAMERA_EVENT_STILL_READY, &ev));
	CHECK(ev.stream_id == s);
	CHECK(ev.request_id == rid);
	scm_v8_state st;
	scm_v8_get_state(&st);
	CHECK(st.last_still_w == 640);
	CHECK(st.last_still_h == 480);

	REQUIRE(scm_acquire_still(f, s, rid, &info, px.data(), px.size()) == XRT_SUCCESS);
	CHECK(info.request_id == rid);
	CHECK(info.width == 1280);
	CHECK(info.height == 480);
	CHECK(info.output == XRT_STEREO_CAMERA_OUTPUT_RECTIFIED); // natively rectified
	CHECK(info.format == XRT_STEREO_CAMERA_FORMAT_GRAY8);
	CHECK((info.result.valid_fields & XRT_PLUGIN_STEREO_CAMERA_RESULT_SENSITIVITY) != 0);
	CHECK(info.result.sensitivity_iso == 400);
	CHECK((info.flags & XRT_PLUGIN_STEREO_CAMERA_STILL_LOSSY_SOURCE) != 0); // lossy source, passed through
	CHECK(info.calibration.eye[0].width == 640);
	CHECK(info.calibration.eye[0].fx == Catch::Approx(400.0f));
	const uint8_t *row = px.data() + info.plane_offset[0] + (size_t)240 * info.row_pitch[0];
	CHECK(row[0] == 60);
	CHECK(row[639] == 60);
	CHECK(row[640] == 180);
	CHECK(row[1279] == 180);
	CHECK(scm_request_still(f, s, &q, &again) == XRT_ERROR_CLIENT_LIMIT_REACHED); // not released yet

	REQUIRE(scm_release_still(f, s, rid) == XRT_SUCCESS);
	CHECK(scm_acquire_still(f, s, rid, &info, px.data(), px.size()) == XRT_ERROR_INPUT_UNSUPPORTED);
	CHECK(scm_release_still(f, s, rid) == XRT_SUCCESS); // idempotent
	REQUIRE(scm_request_still(f, s, &q, &again) == XRT_SUCCESS);
	CHECK(again > rid);

	// The stream stops before the still lands: it fails STREAM_ENDED (or was already ready).
	REQUIRE(scm_stream_stop(f, s) == XRT_SUCCESS);
	xrt_stereo_camera_event e2;
	bool done = false;
	for (int i = 0; i < 300 && !done; i++) {
		while (scm_poll_event(f, &e2)) {
			if (e2.request_id == again && (e2.kind == XRT_STEREO_CAMERA_EVENT_STILL_FAILED ||
			                               e2.kind == XRT_STEREO_CAMERA_EVENT_STILL_READY)) {
				done = true;
				if (e2.kind == XRT_STEREO_CAMERA_EVENT_STILL_FAILED) {
					CHECK(e2.result == XRT_ERROR_STEREO_CAMERA_STREAM_ENDED);
				}
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	CHECK(done);
	scm_destroy(f);
}

TEST_CASE("spec v8: a 16:9 stream is served from a 4:3 source by its centred rows; no common mode = BUSY",
          "[stereo_camera_manager][v8]")
{
	scm_fixture *f = scm_create_v8_plugin();
	REQUIRE(f != nullptr);
	REQUIRE(scm_count(f) == 1);
	xrt_stereo_camera_properties p;
	REQUIRE(scm_properties(f, 0, &p) == XRT_SUCCESS);

	// A 4:3 default stream and a 16:9 640x360 stream: the smallest mode serving both
	// is 640x480 (640 wide, 4:3 tall), and the 16:9 one gets its centred rows 60..419.
	xrt_stereo_camera_stream_layout lay;
	uint64_t def = v8_start(f, p.camera_id, 0, 0, &lay);
	REQUIRE(def != 0);
	uint64_t wide = v8_start(f, p.camera_id, 640, 360, &lay);
	REQUIRE(wide != 0);
	CHECK(lay.width == 1280);
	CHECK(lay.height == 360);
	xrt_stereo_camera_frame_info fi;
	const uint8_t *slot = nullptr;
	bool running = false;
	for (int i = 0; i < 300 && !running; i++) {
		scm_v8_state st;
		scm_v8_get_state(&st);
		running = st.last_mode_index == 2; // 640x480 (640x360 could not serve the 4:3 stream)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	REQUIRE(running);
	std::this_thread::sleep_for(std::chrono::milliseconds(50)); // past any frame of the old mode
	REQUIRE(v8_frame(f, wide, &fi, &slot));
	REQUIRE(v8_frame(f, wide, &fi, &slot));
	CHECK(fi.width == 1280);
	CHECK(fi.height == 360);
	for (uint32_t y : {0u, 180u, 359u}) {
		const uint8_t *row = slot + (size_t)y * fi.row_pitch[0];
		const int want = (int)(20.0 + 100.0 * (y + 60) / 479.0 + 0.5); // source row y + 60, never stretched
		CHECK(row[0] == want);
		CHECK(row[639] == want);
		CHECK(row[640] == want + 120);
	}
	// Its numbers: the default's, scaled by the width, principal point about the centre.
	xrt_stereo_camera_calibration c;
	REQUIRE(scm_calibration_mode(f, p.camera_id, XRT_STEREO_CAMERA_OUTPUT_RECTIFIED, 640, 360, &c) == XRT_SUCCESS);
	CHECK(c.eye[0].cy == Catch::Approx(179.5f));

	// 1280x720 (16:9) cannot share a source with the 4:3 stream: no listed mode is
	// 1280 wide AND 4:3 tall -> BUSY at start; the others keep streaming.
	xrt_stereo_camera_stream_request big = v8_req(p.camera_id, 1280, 720);
	uint64_t b = 0;
	REQUIRE(scm_stream_create(f, &big, &b) == XRT_SUCCESS);
	CHECK(scm_stream_start(f, b) == XRT_ERROR_STEREO_CAMERA_BUSY);
	// Alone it is fine.
	REQUIRE(scm_stream_stop(f, def) == XRT_SUCCESS);
	REQUIRE(scm_stream_stop(f, wide) == XRT_SUCCESS);
	CHECK(scm_stream_start(f, b) == XRT_SUCCESS);
	scm_destroy(f);
}

TEST_CASE("spec v8: the controls hold is lost when the holder's frames are withheld", "[stereo_camera_manager][v8]")
{
	scm_fixture *f = scm_create_v8_plugin();
	REQUIRE(f != nullptr);
	REQUIRE(scm_count(f) == 1);
	xrt_stereo_camera_properties p;
	REQUIRE(scm_properties(f, 0, &p) == XRT_SUCCESS);
	xrt_stereo_camera_stream_layout lay;
	uint64_t a = v8_start(f, p.camera_id, 0, 0, &lay);
	REQUIRE(a != 0);
	xrt_stereo_camera_frame_info fi;
	const uint8_t *slot = nullptr;
	REQUIRE(v8_frame(f, a, &fi, &slot));
	xrt_stereo_camera_controls c{};
	c.valid_controls =
	    XRT_PLUGIN_STEREO_CAMERA_CONTROL_FOCUS_MODE | XRT_PLUGIN_STEREO_CAMERA_CONTROL_FOCUS_DISTANCE;
	c.focus_mode = XRT_PLUGIN_STEREO_CAMERA_FOCUS_MANUAL;
	c.focus_distance_diopters = 3.0f;
	REQUIRE(scm_set_controls(f, a, &c) == XRT_SUCCESS);
	scm_v8_state st;
	bool applied = false;
	for (int i = 0; i < 300 && !applied; i++) {
		scm_v8_get_state(&st);
		applied = st.last_controls.valid_controls == c.valid_controls;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	REQUIRE(applied);
	drain(f, 0);

	// The session locks: frames withheld -> the hold is gone, the source back to its
	// defaults, the client told SUSPENDED; setting controls meanwhile is refused.
	scm_set_session_locked(true);
	bool released = false;
	for (int i = 0; i < 300 && !released; i++) {
		scm_v8_get_state(&st);
		released = st.last_controls.valid_controls == 0;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	CHECK(released);
	xrt_stereo_camera_event ev;
	bool suspended = false;
	while (scm_poll_event(f, &ev)) {
		suspended |=
		    ev.kind == XRT_STEREO_CAMERA_EVENT_CAMERA_STATE && ev.value == XRT_STEREO_CAMERA_STATE_SUSPENDED;
	}
	CHECK(suspended);
	CHECK(scm_set_controls(f, a, &c) == XRT_ERROR_NOT_AUTHORIZED);

	// Unlocked: the stream has no controls until it sets them again.
	scm_set_session_locked(false);
	bool available = false;
	while (scm_poll_event(f, &ev)) {
		available |=
		    ev.kind == XRT_STEREO_CAMERA_EVENT_CAMERA_STATE && ev.value == XRT_STEREO_CAMERA_STATE_AVAILABLE;
	}
	CHECK(available);
	scm_v8_get_state(&st);
	CHECK(st.last_controls.valid_controls == 0);
	CHECK(scm_set_controls(f, a, &c) == XRT_SUCCESS); // re-applied on resume
	scm_destroy(f);
}
