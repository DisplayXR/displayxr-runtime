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

#include "os/os_time.h"
#include "util/u_stereo_camera.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
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
