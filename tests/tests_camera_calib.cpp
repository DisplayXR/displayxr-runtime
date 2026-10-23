// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief The runtime-owned camera calibration store and the `get_camera_pose`
 *        answer both host ifaces give (tracking-provider-discovery.md §6):
 *        the default prior from the panel size, placement-hint geometry per
 *        edge, the full precedence order, the store round trip (directory
 *        creation, user over machine, reset, iris scale), and the sharing
 *        guarantee — the input host iface answers exactly what the
 *        viewer-tracking host iface answers for the same camera and screen.
 */

#include "catch_amalgamated.hpp"

#include "util/u_camera_calib.h"
#include "xrt/xrt_camera_pose.h"
#include "xrt/xrt_input_plugin.h"
#include "xrt/xrt_tracking_plugin.h"

#include "target_camera_pose.h"
#include "target_input_plugin_loader.h"
#include "target_tracking_plugin_loader.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace {

constexpr double kMargin = XRT_CAMERA_POSE_BEZEL_MARGIN_M;

//! A fresh, NOT yet existing nested directory (the store must create it).
struct TempStore
{
	fs::path root;
	std::string user, machine, viewer;

	TempStore()
	{
		const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
		root = fs::temp_directory_path() / ("dxr_camera_calib_" + std::to_string(tick));
		user = (root / "user" / "nested" / "displayxr" / "camera-calibration.json").string();
		machine = (root / "machine" / "camera-calibration.json").string();
		viewer = (root / "user" / "nested" / "displayxr" / "viewer.json").string();
		u_camera_calib_store_set_test_paths(user.c_str(), machine.c_str(), viewer.c_str());
	}
	~TempStore()
	{
		u_camera_calib_store_set_test_paths(nullptr, nullptr, nullptr);
		std::error_code ec;
		fs::remove_all(root, ec);
	}
};

u_camera_calib_record
calibration(const char *camera, const char *screen, double tx, double ty, double rms)
{
	u_camera_calib_record r = {};
	std::snprintf(r.camera_key, sizeof(r.camera_key), "%s", camera);
	std::snprintf(r.screen_key, sizeof(r.screen_key), "%s", screen);
	r.source = XRT_CAMERA_POSE_SOURCE_USER_CALIBRATION;
	r.t_m[0] = tx;
	r.t_m[1] = ty;
	r.q[0] = 0.0784591; // 9 deg pitch
	r.q[3] = 0.9969173;
	r.focal_scale = 1.01;
	r.rms_mm = rms;
	r.sample_count = 10;
	std::snprintf(r.solver_mode, sizeof(r.solver_mode), "b");
	return r;
}

u_camera_calib_record
placement(const char *camera, const char *screen, uint32_t edge, double offset_mm, double margin_mm)
{
	u_camera_calib_record r = {};
	std::snprintf(r.camera_key, sizeof(r.camera_key), "%s", camera);
	std::snprintf(r.screen_key, sizeof(r.screen_key), "%s", screen);
	r.source = XRT_CAMERA_POSE_SOURCE_PLACEMENT_HINT;
	r.edge = edge;
	r.offset_mm = offset_mm;
	r.margin_mm = margin_mm;
	return r;
}

bool g_declare = false;

bool
declared_hook(const char *camera, const char *screen, u_camera_calib_record *out)
{
	if (!g_declare) {
		return false;
	}
	*out = {};
	std::snprintf(out->camera_key, sizeof(out->camera_key), "%s", camera);
	std::snprintf(out->screen_key, sizeof(out->screen_key), "%s", screen != nullptr ? screen : "");
	out->t_m[1] = 0.111;
	out->q[3] = 1.0;
	out->focal_scale = 1.0;
	return true;
}

} // namespace

TEST_CASE("camera calib: default prior from the panel size", "[camera_calib]")
{
	for (double h : {0.194, 0.336}) {
		double t[3] = {9, 9, 9}, q[4] = {9, 9, 9, 9};
		REQUIRE(u_camera_calib_default_prior(h, t, q));
		CHECK(t[0] == 0.0);
		CHECK(t[1] == Catch::Approx(h / 2.0 + 0.008));
		CHECK(t[2] == 0.0);
		CHECK(q[0] == 0.0);
		CHECK(q[1] == 0.0);
		CHECK(q[2] == 0.0);
		CHECK(q[3] == 1.0);

		u_camera_calib_record out;
		REQUIRE(u_camera_calib_resolve(nullptr, nullptr, nullptr, "cam", "scr", 0.3, h, &out));
		CHECK(out.source == XRT_CAMERA_POSE_SOURCE_DEFAULT_PRIOR);
		CHECK(out.t_m[1] == Catch::Approx(h / 2.0 + kMargin));
		CHECK(std::string(u_camera_calib_source_str(out.source)) == "default-prior");
	}
	double t[3], q[4];
	CHECK_FALSE(u_camera_calib_default_prior(0.0, t, q));
	u_camera_calib_record out;
	CHECK_FALSE(u_camera_calib_resolve(nullptr, nullptr, nullptr, "cam", "scr", 0.0, 0.0, &out));
}

TEST_CASE("camera calib: placement-hint geometry for each edge", "[camera_calib]")
{
	const double W = 0.600, H = 0.340, off = 25.0, extra = 4.0;
	const double m = kMargin + extra * 1e-3;
	struct
	{
		uint32_t edge;
		double x, y;
	} cases[] = {
	    {XRT_CAMERA_PLACEMENT_EDGE_TOP, off * 1e-3, H / 2 + m},
	    {XRT_CAMERA_PLACEMENT_EDGE_BOTTOM, off * 1e-3, -(H / 2 + m)},
	    {XRT_CAMERA_PLACEMENT_EDGE_LEFT, -(W / 2 + m), off * 1e-3},
	    {XRT_CAMERA_PLACEMENT_EDGE_RIGHT, W / 2 + m, off * 1e-3},
	};
	for (const auto &c : cases) {
		CAPTURE(u_camera_calib_edge_str(c.edge));
		double t[3], q[4];
		REQUIRE(u_camera_calib_placement_pose(c.edge, off, extra, W, H, t, q));
		CHECK(t[0] == Catch::Approx(c.x));
		CHECK(t[1] == Catch::Approx(c.y));
		CHECK(t[2] == 0.0);
		CHECK(q[3] == 1.0);
		uint32_t parsed = 99;
		REQUIRE(u_camera_calib_edge_parse(u_camera_calib_edge_str(c.edge), &parsed));
		CHECK(parsed == c.edge);
	}
	// Bottom with no offset / margin = the mirror image of the default prior.
	double t[3], q[4], tp[3], qp[4];
	REQUIRE(u_camera_calib_placement_pose(XRT_CAMERA_PLACEMENT_EDGE_BOTTOM, 0, 0, W, H, t, q));
	REQUIRE(u_camera_calib_default_prior(H, tp, qp));
	CHECK(t[1] == Catch::Approx(-tp[1]));
	// The dimension an edge needs must be known.
	CHECK_FALSE(u_camera_calib_placement_pose(XRT_CAMERA_PLACEMENT_EDGE_LEFT, 0, 0, 0.0, H, t, q));
	CHECK_FALSE(u_camera_calib_placement_pose(XRT_CAMERA_PLACEMENT_EDGE_TOP, 0, 0, W, 0.0, t, q));
	CHECK_FALSE(u_camera_calib_placement_pose(7, 0, 0, W, H, t, q));
}

TEST_CASE("camera calib: precedence display-declared > calibration > placement > prior", "[camera_calib]")
{
	const auto cal = calibration("cam", "scr", 0.035, 0.183, 2.1);
	const auto hint = placement("cam", "scr", XRT_CAMERA_PLACEMENT_EDGE_BOTTOM, 0, 0);
	u_camera_calib_record declared = {};
	declared.t_m[1] = 0.111;
	declared.q[3] = 1.0;
	u_camera_calib_record out;

	REQUIRE(u_camera_calib_resolve(&declared, &cal, &hint, "cam", "scr", 0.6, 0.34, &out));
	CHECK(out.source == XRT_CAMERA_POSE_SOURCE_DISPLAY_DECLARED);
	CHECK(out.t_m[1] == Catch::Approx(0.111));

	REQUIRE(u_camera_calib_resolve(nullptr, &cal, &hint, "cam", "scr", 0.6, 0.34, &out));
	CHECK(out.source == XRT_CAMERA_POSE_SOURCE_USER_CALIBRATION);
	CHECK(out.t_m[0] == Catch::Approx(0.035));
	CHECK(out.rms_mm == Catch::Approx(2.1));

	REQUIRE(u_camera_calib_resolve(nullptr, nullptr, &hint, "cam", "scr", 0.6, 0.34, &out));
	CHECK(out.source == XRT_CAMERA_POSE_SOURCE_PLACEMENT_HINT);
	CHECK(out.t_m[1] == Catch::Approx(-(0.17 + kMargin)));

	REQUIRE(u_camera_calib_resolve(nullptr, nullptr, nullptr, "cam", "scr", 0.6, 0.34, &out));
	CHECK(out.source == XRT_CAMERA_POSE_SOURCE_DEFAULT_PRIOR);

	// A hint whose edge needs an unknown width falls through to the prior.
	const auto side = placement("cam", "scr", XRT_CAMERA_PLACEMENT_EDGE_LEFT, 0, 0);
	REQUIRE(u_camera_calib_resolve(nullptr, nullptr, &side, "cam", "scr", 0.0, 0.34, &out));
	CHECK(out.source == XRT_CAMERA_POSE_SOURCE_DEFAULT_PRIOR);

	// The source order is numeric: a higher value always wins.
	CHECK(XRT_CAMERA_POSE_SOURCE_DISPLAY_DECLARED > XRT_CAMERA_POSE_SOURCE_USER_CALIBRATION);
	CHECK(XRT_CAMERA_POSE_SOURCE_USER_CALIBRATION > XRT_CAMERA_POSE_SOURCE_PLACEMENT_HINT);
	CHECK(XRT_CAMERA_POSE_SOURCE_PLACEMENT_HINT > XRT_CAMERA_POSE_SOURCE_DEFAULT_PRIOR);
}

TEST_CASE("camera calib: store round trip, directory creation, user over machine", "[camera_calib]")
{
	TempStore ts;
	REQUIRE_FALSE(fs::exists(fs::path(ts.user).parent_path()));

	u_camera_calib_record found;
	CHECK_FALSE(u_camera_calib_store_find("cam", "scr", XRT_CAMERA_POSE_SOURCE_USER_CALIBRATION, &found));

	// First write creates the missing directories (the macOS `ok:false` of #2303).
	const auto cal = calibration("cam", "scr", 0.035, 0.183, 2.1);
	REQUIRE(u_camera_calib_store_put(&cal, false) == U_CAMERA_CALIB_STORE_OK);
	REQUIRE(fs::exists(ts.user));

	REQUIRE(u_camera_calib_store_find("cam", "scr", XRT_CAMERA_POSE_SOURCE_USER_CALIBRATION, &found));
	CHECK(found.t_m[0] == Catch::Approx(0.035));
	CHECK(found.t_m[1] == Catch::Approx(0.183));
	CHECK(found.q[0] == Catch::Approx(0.0784591).margin(1e-6));
	CHECK(found.focal_scale == Catch::Approx(1.01));
	CHECK(found.rms_mm == Catch::Approx(2.1));
	CHECK(found.sample_count == 10);
	CHECK(std::string(found.solver_mode) == "b");
	CHECK(found.timestamp_unix > 0);
	CHECK_FALSE(found.machine);

	// Replacing keeps one record per (camera, screen, kind).
	auto cal2 = calibration("cam", "scr", 0.040, 0.180, 1.5);
	REQUIRE(u_camera_calib_store_put(&cal2, false) == U_CAMERA_CALIB_STORE_OK);
	u_camera_calib_record list[U_CAMERA_CALIB_MAX_RECORDS];
	CHECK(u_camera_calib_store_list(list, U_CAMERA_CALIB_MAX_RECORDS) == 1);

	// The machine layer: the user record still wins for the same identity.
	auto mach = calibration("cam", "scr", 0.0, 0.2, 9.0);
	REQUIRE(u_camera_calib_store_put(&mach, true) == U_CAMERA_CALIB_STORE_OK);
	REQUIRE(u_camera_calib_store_find("cam", "scr", XRT_CAMERA_POSE_SOURCE_USER_CALIBRATION, &found));
	CHECK(found.t_m[0] == Catch::Approx(0.040));
	CHECK_FALSE(found.machine);
	// ...and serves alone once the user record is reset.
	CHECK(u_camera_calib_store_remove("cam", "scr", 0, false, nullptr) == 1);
	REQUIRE(u_camera_calib_store_find("cam", "scr", XRT_CAMERA_POSE_SOURCE_USER_CALIBRATION, &found));
	CHECK(found.machine);
	CHECK(found.rms_mm == Catch::Approx(9.0));

	// A placement hint is a separate record on the same identity.
	const auto hint = placement("cam", "scr", XRT_CAMERA_PLACEMENT_EDGE_RIGHT, -12.5, 3.0);
	REQUIRE(u_camera_calib_store_put(&hint, false) == U_CAMERA_CALIB_STORE_OK);
	REQUIRE(u_camera_calib_store_find("cam", "scr", XRT_CAMERA_POSE_SOURCE_PLACEMENT_HINT, &found));
	CHECK(found.edge == XRT_CAMERA_PLACEMENT_EDGE_RIGHT);
	CHECK(found.offset_mm == Catch::Approx(-12.5));
	CHECK(found.margin_mm == Catch::Approx(3.0));

	// Any-screen lookup: the newest record for the camera.
	auto other = calibration("cam", "scr2", 0.01, 0.19, 1.0);
	other.timestamp_unix = 4000000000LL;
	REQUIRE(u_camera_calib_store_put(&other, false) == U_CAMERA_CALIB_STORE_OK);
	REQUIRE(u_camera_calib_store_find("cam", nullptr, XRT_CAMERA_POSE_SOURCE_USER_CALIBRATION, &found));
	CHECK(std::string(found.screen_key) == "scr2");
	CHECK_FALSE(u_camera_calib_store_find("other-cam", nullptr, XRT_CAMERA_POSE_SOURCE_USER_CALIBRATION, &found));

	// Iris scale is per user, stored apart from every camera record.
	double iris = 0;
	CHECK_FALSE(u_camera_calib_viewer_iris_scale(&iris));
	CHECK(iris == 1.0);
	REQUIRE(u_camera_calib_set_viewer_iris_scale(1.07) == U_CAMERA_CALIB_STORE_OK);
	REQUIRE(u_camera_calib_viewer_iris_scale(&iris));
	CHECK(iris == Catch::Approx(1.07));
	CHECK(fs::exists(ts.viewer));
	REQUIRE(u_camera_calib_set_viewer_iris_scale(0.0) == U_CAMERA_CALIB_STORE_OK);
	CHECK_FALSE(u_camera_calib_viewer_iris_scale(&iris));

	// Bad records are refused.
	u_camera_calib_record bad = {};
	CHECK(u_camera_calib_store_put(&bad, false) == U_CAMERA_CALIB_STORE_UNSUPPORTED);
}

TEST_CASE("camera calib: the input and tracking host ifaces answer identically", "[camera_calib]")
{
	TempStore ts;
	const xrt_tracking_plugin_host_iface *th = target_tracking_plugin_get_host_iface();
	const xrt_input_plugin_host_iface *ih = target_input_plugin_get_host_iface();
	REQUIRE(XRT_TRACKING_PLUGIN_HOST_HAS_CAMERA_POSE(th));
	REQUIRE(ih->struct_size >= offsetof(xrt_input_plugin_host_iface, get_camera_pose) + sizeof(void *));
	REQUIRE(ih->get_camera_pose != nullptr);
	CHECK(XRT_INPUT_PLUGIN_HOST_HAS_GET_CAMERA_POSE == 1);

	// The panel size, as the system builder publishes it to the input class.
	xrt_input_host_display_geometry geo = {};
	geo.struct_size = sizeof(geo);
	geo.display_width_m = 0.60f;
	geo.display_height_m = 0.34f;
	geo.nominal_viewer_z_m = 0.6f;
	target_input_plugin_set_display_geometry(&geo);

	auto ask = [&](bool input, const char *screen) {
		xrt_camera_pose a = {};
		a.struct_size = sizeof(a);
		const xrt_result_t r = input ? ih->get_camera_pose(ih, "webcam0", screen, &a)
		                             : th->get_camera_pose(th, "webcam0", screen, &a);
		REQUIRE(r == XRT_SUCCESS);
		return a;
	};
	auto same = [](const xrt_camera_pose &a, const xrt_camera_pose &b) {
		CHECK(a.source == b.source);
		for (int i = 0; i < 3; i++) {
			CHECK(a.position_m[i] == b.position_m[i]);
		}
		for (int i = 0; i < 4; i++) {
			CHECK(a.orientation[i] == b.orientation[i]);
		}
		CHECK(a.focal_scale == b.focal_scale);
		CHECK(a.rms_mm == b.rms_mm);
		CHECK(a.viewer_iris_scale == b.viewer_iris_scale);
		CHECK(a.generation == b.generation);
	};

	// Nothing stored: both get the default prior (never "not found"), hands included.
	auto t0 = ask(false, "SCR-1"), i0 = ask(true, "SCR-1");
	CHECK(t0.source == XRT_CAMERA_POSE_SOURCE_DEFAULT_PRIOR);
	CHECK(t0.position_m[1] == Catch::Approx(0.17 + kMargin));
	same(t0, i0);

	// A placement hint.
	const auto hint = placement("webcam0", "SCR-1", XRT_CAMERA_PLACEMENT_EDGE_BOTTOM, 10.0, 0.0);
	REQUIRE(u_camera_calib_store_put(&hint, false) == U_CAMERA_CALIB_STORE_OK);
	auto t1 = ask(false, "SCR-1"), i1 = ask(true, "SCR-1");
	CHECK(t1.source == XRT_CAMERA_POSE_SOURCE_PLACEMENT_HINT);
	CHECK(t1.position_m[0] == Catch::Approx(0.010));
	CHECK(t1.position_m[1] == Catch::Approx(-(0.17 + kMargin)));
	CHECK(t1.generation != t0.generation);
	same(t1, i1);

	// A calibration done "for head tracking" serves the hands, iris scale included.
	const auto cal = calibration("webcam0", "SCR-1", 0.035, 0.183, 2.1);
	REQUIRE(u_camera_calib_store_put(&cal, false) == U_CAMERA_CALIB_STORE_OK);
	REQUIRE(u_camera_calib_set_viewer_iris_scale(0.96) == U_CAMERA_CALIB_STORE_OK);
	auto t2 = ask(false, "SCR-1"), i2 = ask(true, "SCR-1");
	CHECK(t2.source == XRT_CAMERA_POSE_SOURCE_USER_CALIBRATION);
	CHECK(t2.position_m[0] == Catch::Approx(0.035));
	CHECK(t2.rms_mm == Catch::Approx(2.1));
	CHECK(t2.sample_count == 10);
	CHECK(t2.viewer_iris_scale == Catch::Approx(0.96));
	CHECK(std::string(t2.solver_mode) == "b");
	same(t2, i2);
	// The served screen (NULL) matches any stored screen for the camera.
	same(ask(false, nullptr), t2);
	same(ask(true, ""), t2);
	// Another screen has nothing stored: the prior.
	CHECK(ask(true, "SCR-2").source == XRT_CAMERA_POSE_SOURCE_DEFAULT_PRIOR);

	// Display-declared wins over everything (the #2296 hook).
	g_declare = true;
	target_camera_pose_set_declared_hook(declared_hook);
	auto t3 = ask(false, "SCR-1"), i3 = ask(true, "SCR-1");
	CHECK(t3.source == XRT_CAMERA_POSE_SOURCE_DISPLAY_DECLARED);
	CHECK(t3.position_m[1] == Catch::Approx(0.111));
	same(t3, i3);
	target_camera_pose_set_declared_hook(nullptr);
	g_declare = false;

	// Prefix semantics: an older provider's shorter struct gets only what it knows.
	struct
	{
		xrt_camera_pose head;
		uint32_t canary;
	} small = {};
	small.head.struct_size = (uint32_t)offsetof(xrt_camera_pose, focal_scale);
	small.canary = 0xC0FFEEu;
	REQUIRE(th->get_camera_pose(th, "webcam0", "SCR-1", &small.head) == XRT_SUCCESS);
	CHECK(small.head.struct_size == offsetof(xrt_camera_pose, focal_scale));
	CHECK(small.head.source == XRT_CAMERA_POSE_SOURCE_USER_CALIBRATION);
	CHECK(small.head.focal_scale == 0.0f); // past its struct_size: untouched
	CHECK(small.canary == 0xC0FFEEu);

	// Bad arguments.
	xrt_camera_pose a = {};
	a.struct_size = sizeof(a);
	CHECK(th->get_camera_pose(th, "", "SCR-1", &a) == XRT_ERROR_INPUT_UNSUPPORTED);
	CHECK(ih->get_camera_pose(ih, "webcam0", "SCR-1", nullptr) == XRT_ERROR_INPUT_UNSUPPORTED);
}
