// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_stereo_camera (ADR-043 Amendment 4): the vendor-neutral UVC
 *         side-by-side stereo camera source (u_stereo_uvc) — every part that
 *         is not the OS capture call, on every OS:
 *
 *  - config: the documented document parses; every mistake fails the whole
 *    document (a typo must never claim a different camera); fake knob;
 *  - matching: VID:PID from Windows symbolic links / "XXXX:YYYY", name
 *    substring (case-insensitive), both, never "match everything"; a device a
 *    plug-in names in its platform_device_hint is never claimed;
 *  - geometry: per-eye output size (full vs half SBS, the 1280 cap, explicit),
 *    the nominal pinhole, and that it is an exact parallel pair the rectifier
 *    maps as (almost) identity;
 *  - calibration files: OpenCV FileStorage YAML and JSON, rotation vectors,
 *    metres, fisheye, rejection of malformed input;
 *  - the SBS split: eye order, full vs half (an exact 2:1 row average), NV12
 *    and YUY2 input, chroma;
 *  - the source end to end on the FAKE backend (enumerate -> calibration ->
 *    open -> wait_frame): the known disparity and vertical offset come back,
 *    in both layouts and both pixel formats, and "rl" swaps the halves;
 *  - CLOSED LOOP as the service runs it for an uncalibrated pair: frames
 *    passed through, the online refinement measures the fake's 2.4 px
 *    vertical misalignment, the corrected maps bring it to <= 0.25 px.
 */

#include "catch_amalgamated.hpp"

#include "util/u_stereo_camera.h"
#include "util/u_stereo_rectify.h"
#include "util/u_stereo_uvc.h"
#include "util/u_stereo_vrefine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr double k_pi = 3.14159265358979323846;

u_stereo_uvc_config
parse_ok(const char *json)
{
	u_stereo_uvc_config cfg;
	char err[256] = {0};
	bool ok = u_stereo_uvc_config_parse(json, &cfg, err, sizeof(err));
	INFO(err);
	REQUIRE(ok);
	return cfg;
}

std::string
parse_err(const char *json)
{
	u_stereo_uvc_config cfg;
	char err[256] = {0};
	CHECK_FALSE(u_stereo_uvc_config_parse(json, &cfg, err, sizeof(err)));
	CHECK(cfg.count == 0);
	return err;
}

u_stereo_uvc_device
device(const char *name, const char *id)
{
	u_stereo_uvc_device d;
	std::memset(&d, 0, sizeof(d));
	std::snprintf(d.name, sizeof(d.name), "%s", name);
	std::snprintf(d.id, sizeof(d.id), "%s", id);
	d.has_vid_pid = u_stereo_uvc_parse_vid_pid(d.id, &d.vid, &d.pid);
	return d;
}

double
median(std::vector<double> v)
{
	if (v.empty()) {
		return 0.0;
	}
	std::sort(v.begin(), v.end());
	return v[v.size() / 2];
}

//! Block offsets (dx, dy) of a GRAY8 / NV12-Y SBS image over a grid of blocks.
void
block_offsets(const uint8_t *y,
              uint32_t pitch,
              uint32_t ew,
              uint32_t eh,
              uint32_t max_d,
              std::vector<double> &dx,
              std::vector<double> &dy)
{
	dx.clear();
	dy.clear();
	const uint32_t b = 32;
	for (uint32_t by = 40; by + b + 40 < eh; by += 48) {
		for (uint32_t bx = max_d + 8; bx + b + 8 < ew; bx += 48) {
			float fx = 0.0f, fy = 0.0f;
			if (u_stereo_camera_estimate_offset(y, pitch, ew, eh, bx, by, b, b, max_d, 8, &fx, &fy)) {
				dx.push_back(fx);
				dy.push_back(fy);
			}
		}
	}
}

const char *k_full_doc = R"({
  "uvc": [{
    "match": "Eyes",
    "vid_pid": "04f2:b71a",
    "name": "Stereo webcam",
    "layout": "sbs-half",
    "eyes": "rl",
    "mode": "3840x2160@60",
    "eye_size": "1281x721",
    "baseline_mm": 60,
    "hfov_deg": 90,
    "calibration": "C:/cal/eyes.yaml"
  }]
})";

} // namespace


TEST_CASE("uvc config: the documented document parses", "[stereo_uvc]")
{
	u_stereo_uvc_config cfg = parse_ok(k_full_doc);
	REQUIRE(cfg.count == 1);
	const u_stereo_uvc_entry &e = cfg.entries[0];
	CHECK(std::string(e.name_contains) == "Eyes");
	CHECK(e.match_vid_pid);
	CHECK(e.vid == 0x04f2);
	CHECK(e.pid == 0xb71a);
	CHECK(std::string(e.display_name) == "Stereo webcam");
	CHECK(e.layout == U_STEREO_UVC_LAYOUT_SBS_HALF);
	CHECK(e.swap_eyes);
	CHECK(e.mode_width == 3840);
	CHECK(e.mode_height == 2160);
	CHECK(e.mode_fps == Catch::Approx(60.0f));
	CHECK(e.eye_width == 1280); // rounded down to even (NV12)
	CHECK(e.eye_height == 720);
	CHECK(e.baseline_mm == Catch::Approx(60.0f));
	CHECK(e.hfov_deg == Catch::Approx(90.0f));
	CHECK(std::string(e.calibration) == "C:/cal/eyes.yaml");
	CHECK_FALSE(cfg.fake);

	// Defaults: full SBS, left lens left, everything auto, nothing configured.
	u_stereo_uvc_config d = parse_ok(R"({"uvc": [{"vid_pid": "VID_1234&PID_ABCD"}]})");
	REQUIRE(d.count == 1);
	CHECK(d.entries[0].layout == U_STEREO_UVC_LAYOUT_SBS_FULL);
	CHECK_FALSE(d.entries[0].swap_eyes);
	CHECK(d.entries[0].mode_width == 0);
	CHECK(d.entries[0].baseline_mm == 0.0f);
	CHECK(d.entries[0].vid == 0x1234);
	CHECK(d.entries[0].pid == 0xabcd);

	// No list at all = nothing claimed (not an error).
	u_stereo_uvc_config none = parse_ok("{}");
	CHECK(none.count == 0);

	// The fake knob, with and without parameters.
	u_stereo_uvc_config f = parse_ok(R"({"fake": {"disparity": 0.05, "dy": 0.004, "pixel": "yuy2", "fps": 90},
	                                     "uvc": [{"match": "synthetic"}]})");
	CHECK(f.fake);
	CHECK(f.fake_params.disparity == Catch::Approx(0.05));
	CHECK(f.fake_params.dy == Catch::Approx(0.004));
	CHECK(f.fake_params.pixel == U_STEREO_UVC_PIXEL_YUY2);
	CHECK(f.fake_params.fps == Catch::Approx(90.0f));
	CHECK(parse_ok(R"({"fake": true})").fake);
}

TEST_CASE("uvc config: every mistake fails the whole document", "[stereo_uvc]")
{
	CHECK(parse_err("not json").find("JSON") != std::string::npos);
	CHECK(parse_err(R"({"uvc": {}})").find("array") != std::string::npos);
	CHECK(parse_err(R"({"uvc": [{"name": "x"}]})").find("match") != std::string::npos); // no selector
	CHECK(parse_err(R"({"uvc": [{"match": ""}]})").find("match") != std::string::npos);
	CHECK(parse_err(R"({"uvc": [{"vid_pid": "12:34"}]})").find("vid_pid") != std::string::npos);
	CHECK(parse_err(R"({"uvc": [{"match": "a", "layout": "tab"}]})").find("layout") != std::string::npos);
	CHECK(parse_err(R"({"uvc": [{"match": "a", "eyes": "left"}]})").find("eyes") != std::string::npos);
	CHECK(parse_err(R"({"uvc": [{"match": "a", "mode": "1920"}]})").find("mode") != std::string::npos);
	CHECK(parse_err(R"({"uvc": [{"match": "a", "mode": "1922x1080"}]})").find("mode") != std::string::npos);
	CHECK(parse_err(R"({"uvc": [{"match": "a", "baseline_mm": -3}]})").find("baseline") != std::string::npos);
	CHECK(parse_err(R"({"uvc": [{"match": "a", "hfov_deg": 200}]})").find("hfov") != std::string::npos);
	CHECK(parse_err(R"({"uvc": [{"match": "a", "calibration": 3}]})").find("calibration") != std::string::npos);
	CHECK(parse_err(R"({"fake": 3})").find("fake") != std::string::npos);
	// A bad SECOND entry voids the first too.
	CHECK(parse_err(R"({"uvc": [{"match": "a"}, {"match": "b", "eyes": "x"}]})").find("uvc[1]") !=
	      std::string::npos);
	CHECK(parse_err(R"({"uvc": [{"match": "a"}, {"match": "b"}, {"match": "c"}, {"match": "d"}, {"match": "e"}]})")
	          .find("too many") != std::string::npos);
}

TEST_CASE("uvc matching: VID:PID, name, both; never everything", "[stereo_uvc]")
{
	uint16_t v = 0, p = 0;
	CHECK(u_stereo_uvc_parse_vid_pid(
	    "\\\\?\\usb#vid_04f2&pid_b70c&mi_00#6&18588fc8&0&0000#{e5323777-f976-4f5b-9b55-b94699c46e44}\\global", &v,
	    &p));
	CHECK(v == 0x04f2);
	CHECK(p == 0xb70c);
	CHECK(u_stereo_uvc_parse_vid_pid("USB\\VID_1D6B&PID_0102&MI_00\\x", &v, &p));
	CHECK(v == 0x1d6b);
	CHECK(p == 0x0102);
	CHECK(u_stereo_uvc_parse_vid_pid("0x0bda:0x5830", &v, &p));
	CHECK(v == 0x0bda);
	CHECK(p == 0x5830);
	CHECK_FALSE(u_stereo_uvc_parse_vid_pid("/dev/video0", &v, &p));
	CHECK_FALSE(u_stereo_uvc_parse_vid_pid("vid_04f2", &v, &p));
	CHECK_FALSE(u_stereo_uvc_parse_vid_pid("04f2:b70", &v, &p));

	u_stereo_uvc_device tracker =
	    device("Display Tracking Camera", "\\\\?\\usb#vid_04f2&pid_b70c&mi_00#6&1#{e5323777}\\global");
	u_stereo_uvc_device eyes =
	    device("Stereo Eyes Cam", "\\\\?\\usb#vid_2bdf&pid_0281&mi_00#7&2#{e5323777}\\global");
	u_stereo_uvc_device builtin = device("HD WebCam", "\\\\?\\usb#vid_04f2&pid_b5c5&mi_00#6&3#{e5323777}\\global");

	u_stereo_uvc_entry e;
	u_stereo_uvc_entry_defaults(&e);
	CHECK_FALSE(u_stereo_uvc_entry_matches(&e, &eyes)); // no selector = nothing

	std::snprintf(e.name_contains, sizeof(e.name_contains), "eyes");
	CHECK(u_stereo_uvc_entry_matches(&e, &eyes)); // case-insensitive
	CHECK_FALSE(u_stereo_uvc_entry_matches(&e, &tracker));
	CHECK_FALSE(u_stereo_uvc_entry_matches(&e, &builtin));

	u_stereo_uvc_entry_defaults(&e);
	e.match_vid_pid = true;
	e.vid = 0x04f2;
	e.pid = 0xb5c5;
	CHECK(u_stereo_uvc_entry_matches(&e, &builtin));
	CHECK_FALSE(u_stereo_uvc_entry_matches(&e, &tracker)); // same VID, other PID

	// Both given: both must hold.
	std::snprintf(e.name_contains, sizeof(e.name_contains), "Eyes");
	CHECK_FALSE(u_stereo_uvc_entry_matches(&e, &builtin));
	e.pid = 0x0281;
	e.vid = 0x2bdf;
	CHECK(u_stereo_uvc_entry_matches(&e, &eyes));
}

TEST_CASE("uvc assignment: one device per entry; a loose name claims nothing", "[stereo_uvc]")
{
	// A display's tracking camera and a stereo webcam from the same vendor
	// share a name prefix — the case the ambiguity rule exists for.
	u_stereo_uvc_device devs[] = {
	    device("Acme Tracking Camera", "\\\\?\\usb#vid_04f2&pid_b70c&mi_00#1#{x}\\global"),
	    device("Acme Eyes", "\\\\?\\usb#vid_2bdf&pid_0281&mi_00#2#{x}\\global"),
	    device("Acme Eyes", "\\\\?\\usb#vid_2bdf&pid_0281&mi_00#3#{x}\\global"),
	    device("HD WebCam", "\\\\?\\usb#vid_04f2&pid_b5c5&mi_00#4#{x}\\global"),
	};
	int32_t a[U_STEREO_UVC_MAX_ENTRIES];

	u_stereo_uvc_config cfg = parse_ok(R"({"uvc": [{"match": "acme"}]})");
	u_stereo_uvc_assign(&cfg, devs, 4, nullptr, a);
	CHECK(a[0] == U_STEREO_UVC_ASSIGN_AMBIGUOUS);
	CHECK(a[1] == U_STEREO_UVC_ASSIGN_NONE);

	// Specific enough by name, but two identical webcams: still ambiguous by name...
	cfg = parse_ok(R"({"uvc": [{"match": "eyes"}]})");
	u_stereo_uvc_assign(&cfg, devs, 4, nullptr, a);
	CHECK(a[0] == U_STEREO_UVC_ASSIGN_AMBIGUOUS);
	// ... with one of them unavailable (a plug-in's), it is unique.
	bool unavailable[4] = {false, true, false, false};
	u_stereo_uvc_assign(&cfg, devs, 4, unavailable, a);
	CHECK(a[0] == 2);

	// VID:PID: identical devices go to successive entries in order.
	cfg = parse_ok(R"({"uvc": [{"vid_pid": "2bdf:0281"}, {"vid_pid": "2bdf:0281"}, {"vid_pid": "2bdf:0281"},
	                          {"match": "tracking"}]})");
	u_stereo_uvc_assign(&cfg, devs, 4, nullptr, a);
	CHECK(a[0] == 1);
	CHECK(a[1] == 2);
	CHECK(a[2] == U_STEREO_UVC_ASSIGN_NONE);
	CHECK(a[3] == 0); // unique by name

	// A plug-in's camera is never assigned, whatever the entry says.
	bool tracker_out[4] = {true, false, false, false};
	cfg = parse_ok(R"({"uvc": [{"vid_pid": "04f2:b70c"}]})");
	u_stereo_uvc_assign(&cfg, devs, 4, tracker_out, a);
	CHECK(a[0] == U_STEREO_UVC_ASSIGN_NONE);
}

TEST_CASE("uvc matching: a plug-in's camera is never claimed", "[stereo_uvc]")
{
	u_stereo_uvc_device tracker =
	    device("Display Tracking Camera", "\\\\?\\usb#vid_04f2&pid_b70c&mi_00#6&1#{e5323777}\\global");
	// The same OS id, any case.
	CHECK(
	    u_stereo_uvc_device_claimed_by_hint(&tracker, "\\\\?\\USB#VID_04F2&PID_B70C&MI_00#6&1#{E5323777}\\GLOBAL"));
	// A hint that is only a PnP instance id still names the VID:PID.
	CHECK(u_stereo_uvc_device_claimed_by_hint(&tracker, "USB\\VID_04F2&PID_B70C&MI_00\\6&18588FC8&0&0000"));
	// The form a display plug-in reports for its tracker camera: USB "vvvv:pppp".
	CHECK(u_stereo_uvc_device_claimed_by_hint(&tracker, "04f2:b70c"));
	CHECK(u_stereo_uvc_device_claimed_by_hint(&tracker, "04F2:B70C"));
	CHECK_FALSE(u_stereo_uvc_device_claimed_by_hint(&tracker, "USB\\VID_04F2&PID_B5C5&MI_00\\x"));
	CHECK_FALSE(u_stereo_uvc_device_claimed_by_hint(&tracker, ""));
	CHECK_FALSE(u_stereo_uvc_device_claimed_by_hint(&tracker, nullptr));

	// Through the source: an entry that WOULD match the tracker claims nothing
	// once a plug-in names it (fake backend; its device id is "dxr-fake-uvc").
	u_stereo_uvc_config cfg = parse_ok(R"({"fake": true, "uvc": [{"match": "synthetic"}]})");
	const char *hints[] = {"dxr-fake-uvc"};
	u_stereo_uvc *u = u_stereo_uvc_create(&cfg, nullptr, hints, 1);
	CHECK(u_stereo_uvc_enumerate(u, 0, nullptr) == 0);
	u_stereo_uvc_destroy(&u);
	u = u_stereo_uvc_create(&cfg, nullptr, nullptr, 0);
	CHECK(u_stereo_uvc_enumerate(u, 0, nullptr) == 1);
	u_stereo_uvc_destroy(&u);
	CHECK(u == nullptr);
}

TEST_CASE("uvc geometry: per-eye output size", "[stereo_uvc]")
{
	uint32_t w = 0, h = 0;
	// Full SBS: each half is an eye.
	REQUIRE(u_stereo_uvc_eye_size(U_STEREO_UVC_LAYOUT_SBS_FULL, 1280, 480, 0, 0, &w, &h));
	CHECK(w == 640);
	CHECK(h == 480);
	REQUIRE(u_stereo_uvc_eye_size(U_STEREO_UVC_LAYOUT_SBS_FULL, 2560, 720, 0, 0, &w, &h));
	CHECK(w == 1280);
	CHECK(h == 720);
	// Half SBS: a 2:1-squeezed half has W/2 x H/2 square-pixel information.
	REQUIRE(u_stereo_uvc_eye_size(U_STEREO_UVC_LAYOUT_SBS_HALF, 1920, 1080, 0, 0, &w, &h));
	CHECK(w == 960);
	CHECK(h == 540);
	// ... capped at 1280 wide, aspect kept (3840x2160 half -> 1920x1080 -> 1280x720).
	REQUIRE(u_stereo_uvc_eye_size(U_STEREO_UVC_LAYOUT_SBS_HALF, 3840, 2160, 0, 0, &w, &h));
	CHECK(w == 1280);
	CHECK(h == 720);
	REQUIRE(u_stereo_uvc_eye_size(U_STEREO_UVC_LAYOUT_SBS_FULL, 3840, 1080, 0, 0, &w, &h));
	CHECK(w == 1280);
	CHECK(h == 720);
	// Explicit, rounded down to even.
	REQUIRE(u_stereo_uvc_eye_size(U_STEREO_UVC_LAYOUT_SBS_HALF, 3840, 2160, 1921, 1081, &w, &h));
	CHECK(w == 1920);
	CHECK(h == 1080);
	// Cannot split.
	CHECK_FALSE(u_stereo_uvc_eye_size(U_STEREO_UVC_LAYOUT_SBS_FULL, 1282, 480, 0, 0, &w, &h));
	CHECK_FALSE(u_stereo_uvc_eye_size(U_STEREO_UVC_LAYOUT_SBS_FULL, 0, 480, 0, 0, &w, &h));
}

TEST_CASE("uvc geometry: the nominal pinhole is an exact parallel pair", "[stereo_uvc]")
{
	xrt_plugin_stereo_camera_calibration c;
	std::memset(&c, 0, sizeof(c));
	c.struct_size = sizeof(c);
	u_stereo_uvc_nominal_calibration(1280, 720, 90.0, 60.0, &c);
	CHECK(c.image_width == 1280);
	CHECK(c.image_height == 720);
	CHECK(c.k[0][0] == Catch::Approx(640.0)); // (w/2) / tan(45 deg)
	CHECK(c.k[0][1] == c.k[0][0]);
	CHECK(c.k[0][2] == Catch::Approx(639.5));
	CHECK(c.k[0][3] == Catch::Approx(359.5));
	for (int i = 0; i < 4; i++) {
		CHECK(c.k[1][i] == c.k[0][i]);
	}
	CHECK(c.distortion_model == XRT_PLUGIN_STEREO_CAMERA_DISTORTION_NONE);
	CHECK(c.translation_right_from_left_mm[0] == Catch::Approx(-60.0));
	CHECK(c.translation_right_from_left_mm[1] == 0.0);
	CHECK(c.rotation_right_from_left[1][1] == 1.0);
	CHECK(c.rotation_right_from_left[0][1] == 0.0);

	// What the service's rectifier makes of it: the same pinhole, baseline,
	// principal row — the maps are the identity up to the alpha = 0 border
	// check's hair of zoom, which the service's pass-through skips entirely.
	u_stereo_rectify_input in;
	std::memset(&in, 0, sizeof(in));
	in.width = 1280;
	in.height = 720;
	for (int e = 0; e < 2; e++) {
		in.eye[e].fx = c.k[e][0];
		in.eye[e].fy = c.k[e][1];
		in.eye[e].cx = c.k[e][2];
		in.eye[e].cy = c.k[e][3];
		in.eye[e].model = c.distortion_model;
	}
	std::memcpy(in.R, c.rotation_right_from_left, sizeof(in.R));
	std::memcpy(in.T, c.translation_right_from_left_mm, sizeof(in.T));
	u_stereo_rectify_result g;
	REQUIRE(u_stereo_rectify_compute(&in, &g));
	CHECK(g.baseline == Catch::Approx(60.0));
	CHECK(g.cy == Catch::Approx(359.5).margin(1e-6));
	CHECK(g.crop_scale < 1.0025);
	CHECK(g.f / c.k[0][0] == Catch::Approx(g.crop_scale).epsilon(1e-9));
	CHECK(g.t_rect[0] == Catch::Approx(-60.0));
}

TEST_CASE("uvc calibration file: OpenCV YAML and JSON", "[stereo_uvc]")
{
	const char *yaml = R"(%YAML:1.0
---
image_width: 1920
image_height: 2160
M1: !!opencv-matrix
   rows: 3
   cols: 3
   dt: d
   data: [ 9.5e+02, 0., 9.6e+02, 0., 1.9e+03,
       1.08e+03, 0., 0., 1. ]
D1: !!opencv-matrix
   rows: 1
   cols: 5
   dt: d
   data: [ -1.2e-01, 3.0e-02, 1.0e-04, -2.0e-04, 0. ]
M2: !!opencv-matrix
   rows: 3
   cols: 3
   dt: d
   data: [ 9.4e+02, 0., 9.55e+02, 0., 1.89e+03, 1.07e+03, 0., 0., 1. ]
D2: !!opencv-matrix
   rows: 1
   cols: 5
   dt: d
   data: [ -1.1e-01, 2.0e-02, 0., 0., 0. ]
R: !!opencv-matrix
   rows: 3
   cols: 3
   dt: d
   data: [ 1., 0., 0., 0., 1., 0., 0., 0., 1. ]
T: !!opencv-matrix
   rows: 3
   cols: 1
   dt: d
   data: [ -6.1e+01, 2.0e-01, -1.0e-01 ]
)";
	xrt_plugin_stereo_camera_calibration c;
	std::memset(&c, 0, sizeof(c));
	c.struct_size = sizeof(c);
	char err[160] = {0};
	REQUIRE(u_stereo_uvc_calibration_parse(yaml, 1, 1, &c, err, sizeof(err)));
	CHECK(c.image_width == 1920);
	CHECK(c.image_height == 2160);
	CHECK(c.k[0][0] == Catch::Approx(950.0));
	CHECK(c.k[0][1] == Catch::Approx(1900.0)); // a half-SBS calibration: anisotropic, fine
	CHECK(c.k[0][3] == Catch::Approx(1080.0));
	CHECK(c.k[1][2] == Catch::Approx(955.0));
	CHECK(c.distortion_model == XRT_PLUGIN_STEREO_CAMERA_DISTORTION_RADTAN5);
	CHECK(c.distortion[0][0] == Catch::Approx(-0.12));
	CHECK(c.distortion[1][1] == Catch::Approx(0.02));
	CHECK(c.translation_right_from_left_mm[0] == Catch::Approx(-61.0));
	CHECK(c.translation_right_from_left_mm[2] == Catch::Approx(-0.1));
	CHECK(c.rotation_right_from_left[2][2] == 1.0);

	// JSON, nested matrices, a rotation VECTOR, T in metres, no size (default).
	const char *json = R"({"K1": [[700, 0, 320], [0, 700, 240], [0, 0, 1]], "D1": [0.1, 0.0, 0.0, 0.0],
	  "K2": [700, 0, 321, 0, 701, 239, 0, 0, 1], "D2": [0.1, 0.0, 0.0, 0.0],
	  "R": [0.0, 0.01, 0.0], "T": [-0.05, 0.0, 0.0], "distortion_model": "fisheye"})";
	REQUIRE(u_stereo_uvc_calibration_parse(json, 640, 480, &c, err, sizeof(err)));
	CHECK(c.image_width == 640);
	CHECK(c.image_height == 480);
	CHECK(c.k[1][1] == Catch::Approx(701.0));
	CHECK(c.distortion_model == XRT_PLUGIN_STEREO_CAMERA_DISTORTION_KB4);
	CHECK(c.translation_right_from_left_mm[0] == Catch::Approx(-50.0)); // metres -> mm
	CHECK(c.rotation_right_from_left[0][0] == Catch::Approx(std::cos(0.01)));
	CHECK(c.rotation_right_from_left[0][2] == Catch::Approx(std::sin(0.01)));

	// Rejections.
	CHECK_FALSE(u_stereo_uvc_calibration_parse(R"({"K1": [1, 2, 3]})", 640, 480, &c, err, sizeof(err)));
	CHECK(std::string(err).find("K1") != std::string::npos);
	CHECK_FALSE(u_stereo_uvc_calibration_parse(
	    R"({"K1": [700,0,320,0,700,240,0,0,1], "K2": [700,0,320,0,700,240,0,0,1], "D1": [0,0,0,0,0],
	        "D2": [0,0,0,0], "R": [1,0,0,0,1,0,0,0,1], "T": [-50,0,0]})",
	    640, 480, &c, err, sizeof(err)));
	CHECK_FALSE(u_stereo_uvc_calibration_parse(
	    R"({"K1": [700,0,320,0,700,240,0,0,1], "K2": [700,0,320,0,700,240,0,0,1], "D1": [0,0,0,0,0],
	        "D2": [0,0,0,0,0], "R": [1,0,0,0,1,0,0,0,1], "T": [0,0,0]})",
	    640, 480, &c, err, sizeof(err)));
	CHECK_FALSE(u_stereo_uvc_calibration_parse(nullptr, 640, 480, &c, err, sizeof(err)));
}

TEST_CASE("uvc split: eye order, full vs half, NV12 and YUY2", "[stereo_uvc]")
{
	// A 16x8 NV12 SBS frame: left half luma = 40 + row, right half = 200 - row;
	// chroma U/V differ per half too.
	const uint32_t W = 16, H = 8;
	std::vector<uint8_t> nv12(W * H * 3 / 2);
	for (uint32_t y = 0; y < H; y++) {
		for (uint32_t x = 0; x < W; x++) {
			nv12[y * W + x] = (uint8_t)(x < W / 2 ? 40 + y : 200 - y);
		}
	}
	for (uint32_t y = 0; y < H / 2; y++) {
		for (uint32_t x = 0; x < W / 2; x++) {
			uint8_t *uv = &nv12[W * H + y * W + 2 * x];
			uv[0] = (uint8_t)(x < W / 4 ? 90 : 100);  // U
			uv[1] = (uint8_t)(x < W / 4 ? 160 : 170); // V
		}
	}
	u_stereo_uvc_raw_frame in;
	std::memset(&in, 0, sizeof(in));
	in.pixel = U_STEREO_UVC_PIXEL_NV12;
	in.width = W;
	in.height = H;
	in.planes[0] = nv12.data();
	in.planes[1] = nv12.data() + W * H;
	in.pitches[0] = in.pitches[1] = W;

	SECTION("full SBS, lr: a straight copy")
	{
		u_stereo_camera_planes lay;
		REQUIRE(u_stereo_camera_layout(XRT_PLUGIN_STEREO_CAMERA_FORMAT_NV12, 16, 8, &lay));
		std::vector<uint8_t> out(lay.size);
		REQUIRE(u_stereo_uvc_split(&in, false, 8, 8, out.data(), &lay));
		for (uint32_t y = 0; y < 8; y++) {
			CHECK(out[lay.offset[0] + y * lay.pitch[0] + 0] == 40 + y);
			CHECK(out[lay.offset[0] + y * lay.pitch[0] + 15] == 200 - y);
		}
		CHECK(out[lay.offset[1] + 0] == 90);
		CHECK(out[lay.offset[1] + 1] == 160);
		CHECK(out[lay.offset[1] + 8] == 100);
		CHECK(out[lay.offset[1] + 9] == 170);
	}
	SECTION("full SBS, rl: the halves swap")
	{
		u_stereo_camera_planes lay;
		REQUIRE(u_stereo_camera_layout(XRT_PLUGIN_STEREO_CAMERA_FORMAT_NV12, 16, 8, &lay));
		std::vector<uint8_t> out(lay.size);
		REQUIRE(u_stereo_uvc_split(&in, true, 8, 8, out.data(), &lay));
		CHECK(out[lay.offset[0] + 3 * lay.pitch[0] + 0] == 197);
		CHECK(out[lay.offset[0] + 3 * lay.pitch[0] + 15] == 43);
		CHECK(out[lay.offset[1] + 0] == 100);
		CHECK(out[lay.offset[1] + 8] == 90);
	}
	SECTION("half SBS: 2:1 vertical is an exact two-row average")
	{
		// 8x8 halves -> 8x4 eyes.
		u_stereo_camera_planes lay;
		REQUIRE(u_stereo_camera_layout(XRT_PLUGIN_STEREO_CAMERA_FORMAT_NV12, 16, 4, &lay));
		std::vector<uint8_t> out(lay.size);
		REQUIRE(u_stereo_uvc_split(&in, false, 8, 4, out.data(), &lay));
		for (uint32_t y = 0; y < 4; y++) {
			// rows 2y, 2y+1: (40+2y + 40+2y+1) / 2 = 40 + 2y + 0.5 -> rounds up
			CHECK(out[lay.offset[0] + y * lay.pitch[0] + 2] == 41 + 2 * y);
			CHECK(out[lay.offset[0] + y * lay.pitch[0] + 12] == 200 - 2 * y);
		}
	}
	SECTION("YUY2 in, NV12 out")
	{
		std::vector<uint8_t> yuy2(W * H * 2);
		for (uint32_t y = 0; y < H; y++) {
			for (uint32_t x = 0; x < W; x += 2) {
				uint8_t *p = &yuy2[y * W * 2 + x * 2];
				bool left = x < W / 2;
				p[0] = (uint8_t)(left ? 40 + y : 200 - y);
				p[1] = (uint8_t)(left ? 90 : 100);
				p[2] = p[0];
				p[3] = (uint8_t)(left ? 160 : 170);
			}
		}
		u_stereo_uvc_raw_frame yin = in;
		yin.pixel = U_STEREO_UVC_PIXEL_YUY2;
		yin.planes[0] = yuy2.data();
		yin.planes[1] = nullptr;
		yin.pitches[0] = W * 2;
		u_stereo_camera_planes lay;
		REQUIRE(u_stereo_camera_layout(XRT_PLUGIN_STEREO_CAMERA_FORMAT_NV12, 16, 8, &lay));
		std::vector<uint8_t> out(lay.size);
		REQUIRE(u_stereo_uvc_split(&yin, false, 8, 8, out.data(), &lay));
		CHECK(out[lay.offset[0] + 5 * lay.pitch[0] + 1] == 45);
		CHECK(out[lay.offset[0] + 5 * lay.pitch[0] + 9] == 195);
		CHECK(out[lay.offset[1] + 2 * lay.pitch[1] + 0] == 90);
		CHECK(out[lay.offset[1] + 2 * lay.pitch[1] + 1] == 160);
		CHECK(out[lay.offset[1] + 2 * lay.pitch[1] + 10] == 100);
		CHECK(out[lay.offset[1] + 2 * lay.pitch[1] + 11] == 170);
	}
	SECTION("bad input")
	{
		u_stereo_camera_planes lay;
		REQUIRE(u_stereo_camera_layout(XRT_PLUGIN_STEREO_CAMERA_FORMAT_NV12, 16, 8, &lay));
		std::vector<uint8_t> out(lay.size);
		u_stereo_uvc_raw_frame bad = in;
		bad.width = 18; // not a multiple of 4
		CHECK_FALSE(u_stereo_uvc_split(&bad, false, 8, 8, out.data(), &lay));
		CHECK_FALSE(u_stereo_uvc_split(&in, false, 7, 8, out.data(), &lay));
		bad = in;
		bad.pixel = 99;
		CHECK_FALSE(u_stereo_uvc_split(&bad, false, 8, 8, out.data(), &lay));
	}
}

TEST_CASE("uvc split: cost of a half-SBS 3840x2160 NV12 frame to 2 x 1280x720", "[stereo_uvc][.perf]")
{
	const uint32_t W = 3840, H = 2160;
	std::vector<uint8_t> nv12((size_t)W * H * 3 / 2);
	for (size_t i = 0; i < nv12.size(); i++) {
		nv12[i] = (uint8_t)(i * 2654435761u >> 24);
	}
	u_stereo_uvc_raw_frame in;
	std::memset(&in, 0, sizeof(in));
	in.pixel = U_STEREO_UVC_PIXEL_NV12;
	in.width = W;
	in.height = H;
	in.planes[0] = nv12.data();
	in.planes[1] = nv12.data() + (size_t)W * H;
	in.pitches[0] = in.pitches[1] = W;
	u_stereo_camera_planes lay;
	REQUIRE(u_stereo_camera_layout(XRT_PLUGIN_STEREO_CAMERA_FORMAT_NV12, 2560, 720, &lay));
	std::vector<uint8_t> out(lay.size);
	for (uint32_t eye_w : {1280u, 2560u / 2}) {
		auto t0 = std::chrono::steady_clock::now();
		const int n = 20;
		for (int i = 0; i < n; i++) {
			REQUIRE(u_stereo_uvc_split(&in, false, eye_w, 720, out.data(), &lay));
		}
		double ms =
		    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / n;
		std::printf("[uvc split perf] 3840x2160 NV12 half-SBS -> 2 x %ux720: %.2f ms/frame\n", eye_w, ms);
	}
	// The GPU path hands over 2560x720 already: a straight copy.
	in.width = 2560;
	in.height = 720;
	in.pitches[0] = in.pitches[1] = 2560;
	in.planes[1] = nv12.data() + (size_t)2560 * 720;
	auto t0 = std::chrono::steady_clock::now();
	for (int i = 0; i < 50; i++) {
		REQUIRE(u_stereo_uvc_split(&in, false, 1280, 720, out.data(), &lay));
	}
	double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 50;
	std::printf("[uvc split perf] 2560x720 (GPU-scaled) -> 2 x 1280x720: %.3f ms/frame\n", ms);
}

namespace {

struct source_run
{
	uint32_t eye_w = 0, eye_h = 0;
	uint32_t flags = 0;
	std::vector<uint8_t> y; //!< the last frame's luma, tight 2*eye_w pitch
	uint32_t frames = 0;
};

//! Enumerate -> open -> read @p n frames through the source on the fake backend.
source_run
run_source(const char *json, uint32_t n)
{
	source_run r;
	u_stereo_uvc_config cfg = parse_ok(json);
	u_stereo_uvc *u = u_stereo_uvc_create(&cfg, nullptr, nullptr, 0);
	REQUIRE(u_stereo_uvc_enumerate(u, 0, nullptr) == 1);
	xrt_plugin_stereo_camera_info info;
	std::memset(&info, 0, sizeof(info));
	info.struct_size = sizeof(info);
	REQUIRE(u_stereo_uvc_enumerate(u, 1, &info) == 1);
	CHECK(info.struct_size == sizeof(info));
	CHECK(info.native_format == XRT_PLUGIN_STEREO_CAMERA_FORMAT_NV12);
	CHECK(std::string(info.platform_device_hint) == "dxr-fake-uvc");
	CHECK(std::string(info.device_identity) == "uvc:dxr-fake-uvc");
	r.eye_w = info.eye_width;
	r.eye_h = info.eye_height;
	r.flags = info.flags;

	u_stereo_uvc_stream *s = nullptr;
	REQUIRE(u_stereo_uvc_open(u, 0, &s) == XRT_SUCCESS);
	uint64_t last_seq = 0;
	for (uint32_t i = 0; i < n; i++) {
		xrt_plugin_stereo_camera_frame f;
		uint32_t w = u_stereo_uvc_wait_frame(s, 500ll * 1000 * 1000, &f);
		REQUIRE(w == XRT_PLUGIN_STEREO_CAMERA_WAIT_OK);
		CHECK(f.sequence == last_seq + 1);
		last_seq = f.sequence;
		CHECK(f.width == 2 * r.eye_w);
		CHECK(f.height == r.eye_h);
		CHECK(f.format == XRT_PLUGIN_STEREO_CAMERA_FORMAT_NV12);
		CHECK(f.time_ns > 0);
		r.y.resize((size_t)f.width * f.height);
		for (uint32_t row = 0; row < f.height; row++) {
			std::memcpy(&r.y[(size_t)row * f.width], f.planes[0] + (size_t)row * f.pitches[0], f.width);
		}
		CHECK(f.planes[1][0] == 128); // the fake is grey: neutral chroma survives the split
		u_stereo_uvc_release_frame(s);
		r.frames++;
	}
	u_stereo_uvc_close(s);
	u_stereo_uvc_destroy(&u);
	return r;
}

} // namespace

TEST_CASE("uvc source on the FAKE backend: the known disparity and dy come back", "[stereo_uvc]")
{
	struct variant
	{
		const char *json;
		uint32_t eye_w, eye_h;
	} v[] = {
	    // Full SBS, NV12: 1280x480 -> 640x480 per eye.
	    {R"({"fake": {"disparity": 0.04, "dy": 0.005, "fps": 240},
	         "uvc": [{"match": "synthetic", "mode": "1280x480@240"}]})",
	     640, 480},
	    // Half SBS, YUY2: 1280x960 -> 640x480 per eye (2:1 vertical).
	    {R"({"fake": {"disparity": 0.04, "dy": 0.005, "pixel": "yuy2", "fps": 240},
	         "uvc": [{"match": "SYNTHETIC", "layout": "sbs-half", "mode": "1280x960@240"}]})",
	     640, 480},
	};
	for (const variant &t : v) {
		source_run r = run_source(t.json, 3);
		CHECK(r.eye_w == t.eye_w);
		CHECK(r.eye_h == t.eye_h);
		CHECK((r.flags & XRT_PLUGIN_STEREO_CAMERA_CALIBRATED) == 0); // nominal model
		CHECK((r.flags & XRT_PLUGIN_STEREO_CAMERA_SHARED_WITH_EYE_TRACKING) == 0);
		std::vector<double> dx, dy;
		block_offsets(r.y.data(), 2 * r.eye_w, r.eye_w, r.eye_h, 48, dx, dy);
		REQUIRE(dx.size() > 20);
		double mdx = median(dx), mdy = median(dy);
		std::printf(
		    "[uvc fake] eye %ux%u: disparity %.2f px (want %.2f), dy %+.2f px (want %+.2f), %zu blocks\n",
		    r.eye_w, r.eye_h, mdx, 0.04 * r.eye_w, mdy, 0.005 * r.eye_h, dx.size());
		CHECK(mdx == Catch::Approx(0.04 * r.eye_w).margin(0.35));
		CHECK(mdy == Catch::Approx(0.005 * r.eye_h).margin(0.35));
	}

	// "rl": the device's RIGHT lens is in the left half -> the halves swap.
	source_run lr = run_source(R"({"fake": {"fps": 240}, "uvc": [{"match": "synthetic", "mode": "1280x480"}]})", 1);
	source_run rl = run_source(
	    R"({"fake": {"fps": 240}, "uvc": [{"match": "synthetic", "mode": "1280x480", "eyes": "rl"}]})", 1);
	REQUIRE(lr.y.size() == rl.y.size());
	bool swapped = true;
	for (uint32_t row = 0; row < lr.eye_h && swapped; row++) {
		const uint8_t *a = &lr.y[(size_t)row * 2 * lr.eye_w];
		const uint8_t *b = &rl.y[(size_t)row * 2 * rl.eye_w];
		swapped = std::memcmp(a, b + lr.eye_w, lr.eye_w) == 0 && std::memcmp(a + lr.eye_w, b, lr.eye_w) == 0;
	}
	CHECK(swapped);
}

TEST_CASE("uvc source: calibration — nominal by default, the file's when given", "[stereo_uvc]")
{
	u_stereo_uvc_config cfg = parse_ok(
	    R"({"fake": true, "uvc": [{"match": "synthetic", "mode": "1280x480", "baseline_mm": 65, "hfov_deg": 80}]})");
	u_stereo_uvc *u = u_stereo_uvc_create(&cfg, nullptr, nullptr, 0);
	REQUIRE(u_stereo_uvc_enumerate(u, 0, nullptr) == 1);
	xrt_plugin_stereo_camera_calibration c;
	std::memset(&c, 0, sizeof(c));
	c.struct_size = sizeof(c);
	bool nominal = false, bl = false, hf = false;
	REQUIRE(u_stereo_uvc_get_calibration(u, 0, &c, &nominal, &bl, &hf) == XRT_SUCCESS);
	CHECK(nominal);
	CHECK(bl);
	CHECK(hf);
	CHECK(c.k[0][0] == Catch::Approx(320.0 / std::tan(40.0 * k_pi / 180.0)));
	CHECK(c.translation_right_from_left_mm[0] == Catch::Approx(-65.0));
	CHECK(u_stereo_uvc_get_calibration(u, 1, &c, &nominal, &bl, &hf) == XRT_ERROR_FEATURE_NOT_SUPPORTED);
	u_stereo_uvc_mode m;
	uint32_t layout = 0;
	REQUIRE(u_stereo_uvc_get_mode(u, 0, &m, &layout));
	CHECK(m.width == 1280);
	CHECK(layout == U_STEREO_UVC_LAYOUT_SBS_FULL);
	u_stereo_uvc_destroy(&u);

	// Unconfigured baseline / HFOV: defaults drive the model, flagged unknown.
	cfg = parse_ok(R"({"fake": true, "uvc": [{"match": "synthetic"}]})");
	u = u_stereo_uvc_create(&cfg, nullptr, nullptr, 0);
	REQUIRE(u_stereo_uvc_get_calibration(u, 0, &c, &nominal, &bl, &hf) == XRT_SUCCESS);
	CHECK(nominal);
	CHECK_FALSE(bl);
	CHECK_FALSE(hf);
	CHECK(c.translation_right_from_left_mm[0] == Catch::Approx(-U_STEREO_UVC_DEFAULT_BASELINE_MM));
	// Auto mode = the largest the device lists (the fake's 2560x720).
	REQUIRE(u_stereo_uvc_get_mode(u, 0, &m, &layout));
	CHECK(m.width == 2560);
	CHECK(m.height == 720);
	u_stereo_uvc_destroy(&u);

	// A missing calibration file falls back to the nominal model (and says so).
	cfg = parse_ok(R"({"fake": true, "uvc": [{"match": "synthetic", "calibration": "/nonexistent/cal.yaml"}]})");
	u = u_stereo_uvc_create(&cfg, nullptr, nullptr, 0);
	REQUIRE(u_stereo_uvc_get_calibration(u, 0, &c, &nominal, &bl, &hf) == XRT_SUCCESS);
	CHECK(nominal);
	u_stereo_uvc_destroy(&u);

	// No backend at all (a platform without one): configured entries claim nothing.
	cfg = parse_ok(R"({"uvc": [{"match": "x"}]})");
	u = u_stereo_uvc_create(&cfg, nullptr, nullptr, 0);
	CHECK(u_stereo_uvc_enumerate(u, 0, nullptr) == 0);
	u_stereo_uvc *none = nullptr;
	u_stereo_uvc_stream *ss = nullptr;
	CHECK(u_stereo_uvc_open(none, 0, &ss) != XRT_SUCCESS);
	CHECK(ss == nullptr);
	u_stereo_uvc_destroy(&u);
}

TEST_CASE("uvc CLOSED LOOP: pass-through + online refinement aligns an uncalibrated pair", "[stereo_uvc]")
{
	// The service's path for an uncalibrated UVC pair, step by step: the
	// nominal model, frames passed through untouched, the refinement measuring
	// the device's own vertical misalignment (2.4 px here) and the corrected
	// maps it rebuilds. Judged by an independent block matcher.
	source_run r = run_source(R"({"fake": {"disparity": 0.04, "dy": 0.005, "fps": 240},
	                              "uvc": [{"match": "synthetic", "mode": "1280x480", "baseline_mm": 60,
	                                       "hfov_deg": 70}]})",
	                          2);
	const uint32_t W = r.eye_w, H = r.eye_h;
	xrt_plugin_stereo_camera_calibration c;
	std::memset(&c, 0, sizeof(c));
	c.struct_size = sizeof(c);
	u_stereo_uvc_nominal_calibration(W, H, 70.0, 60.0, &c);
	u_stereo_rectify_input in;
	std::memset(&in, 0, sizeof(in));
	in.width = W;
	in.height = H;
	in.calib_width = c.image_width;
	in.calib_height = c.image_height;
	for (int e = 0; e < 2; e++) {
		in.eye[e].fx = c.k[e][0];
		in.eye[e].fy = c.k[e][1];
		in.eye[e].cx = c.k[e][2];
		in.eye[e].cy = c.k[e][3];
		in.eye[e].model = c.distortion_model;
	}
	std::memcpy(in.R, c.rotation_right_from_left, sizeof(in.R));
	std::memcpy(in.T, c.translation_right_from_left_mm, sizeof(in.T));
	u_stereo_rectify_result geo;
	REQUIRE(u_stereo_rectify_compute(&in, &geo));

	u_stereo_vrefine_config cfg;
	u_stereo_vrefine_config_defaults(&cfg, H, geo.cy);
	u_stereo_vrefine ref;
	u_stereo_vrefine_init(&ref, &cfg);
	u_stereo_vrefine_measure_params mp;
	double dmax = geo.f * geo.baseline / 300.0;
	dmax = dmax < 24.0 ? 24.0 : dmax > W / 2.0 ? W / 2.0 : dmax;
	u_stereo_vrefine_measure_defaults(&mp, (uint32_t)dmax);
	std::vector<u_stereo_vrefine_sample> s(512);

	const std::vector<uint8_t> &raw = r.y; // the static fake scene
	std::vector<uint8_t> rect = raw;       // pass-through until a correction applies
	u_stereo_rectify_lut lut;
	std::memset(&lut, 0, sizeof(lut));
	bool have_lut = false;

	std::vector<double> dx, dy;
	block_offsets(rect.data(), 2 * W, W, H, 48, dx, dy);
	const double before = median(dy);

	int64_t now = 1000000000;
	uint32_t updates = 0;
	for (uint32_t frame = 0; frame < 90; frame++, now += 33333333) {
		if (!u_stereo_vrefine_due(&ref, now)) {
			continue;
		}
		uint32_t n = u_stereo_vrefine_measure(rect.data(), 2 * W, W, H, &mp, s.data(), (uint32_t)s.size());
		if (u_stereo_vrefine_push(&ref, now, s.data(), n) == U_STEREO_VREFINE_UPDATED) {
			updates++;
			u_stereo_rectify_input ci = in;
			ci.v_offset = ref.a / geo.f;
			ci.v_slope = ref.b;
			u_stereo_rectify_result g;
			REQUIRE(u_stereo_rectify_compute(&ci, &g));
			if (have_lut) {
				u_stereo_rectify_lut_fini(&lut);
			}
			REQUIRE(u_stereo_rectify_lut_init(&lut, &g, 1));
			have_lut = true;
			u_stereo_rectify_lut_apply(&lut, 1, raw.data(), 2 * W, rect.data(), 2 * W);
		}
	}
	block_offsets(rect.data(), 2 * W, W, H, 48, dx, dy);
	const double after = median(dy);
	std::printf(
	    "[uvc closed loop] %u map rebuild(s): correction a %+.3f px b %+.5f; signed dy median %+.3f -> "
	    "%+.3f px (%zu blocks), disparity %.2f px\n",
	    updates, ref.a, ref.b, before, after, dy.size(), median(dx));
	CHECK(before == Catch::Approx(2.4).margin(0.35));
	CHECK(updates >= 1);
	CHECK(ref.applied);
	CHECK(std::fabs(after) <= 0.25);
	CHECK(median(dx) == Catch::Approx(0.04 * W).margin(0.6)); // the correction is vertical only
	if (have_lut) {
		u_stereo_rectify_lut_fini(&lut);
	}
}
