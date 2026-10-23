// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Camera-pose calibration end to end through the REAL sim_tracking
 *        plug-in: hidden camera pose -> camera-space samples -> solver ->
 *        CAMERA_POSE setting -> the provider's display-space output.
 *
 * The test loads DisplayXR-SimTracking through the tracking loader (POSIX
 * manifest discovery, XRT_PLUGIN_SEARCH_PATH; registered in CMake), with a
 * hidden `CAMERA_TRUE_POSE`. For each alignment it places the scripted eye
 * midpoint ON THE NORMAL of a dot (OFFSET script = nominal + offset), starts
 * the provider, takes the camera-space block it pushes, and stops it. The
 * solver must recover the hidden pose; once the solved pose is committed as
 * `CAMERA_POSE_0`, the provider's reported eyes must equal the scripted ones.
 *
 * When DXR_CALIB_E2E_OUT is set it also writes the collected pairs in the
 * `tracking calibrate solve --in` shape, for the CLI arm.
 */

#include "catch_amalgamated.hpp"

#include "math/m_mirror_calib.h"
#include "target_tracking_plugin_loader.h"
#include "xrt/xrt_results.h"
#include "xrt/xrt_tracking_plugin.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

// The hidden truth: 35 mm right of centre, 183 mm up, clipped on with 9 deg of downward pitch.
constexpr double kTrueT[3] = {0.035, 0.183, 0.0};
constexpr double kTruePitchDeg = 9.0;
constexpr float kNominalZ = 0.6f;

xrt_tracking_viewer_sample g_last;
int g_pushes = 0;

void
capture_push(const xrt_tracking_sink *, const xrt_tracking_viewer_sample *s)
{
	g_last = *s;
	g_pushes++;
}

void
set_env(const char *key, const std::string &value)
{
	std::string name = std::string("DXR_TRACKING_SIM_TRACKING_") + key;
#ifdef _WIN32
	_putenv_s(name.c_str(), value.c_str());
#else
	setenv(name.c_str(), value.c_str(), 1);
#endif
}

void
unset_env(const char *key)
{
	std::string name = std::string("DXR_TRACKING_SIM_TRACKING_") + key;
#ifdef _WIN32
	_putenv_s(name.c_str(), "");
#else
	unsetenv(name.c_str());
#endif
}

std::string
num(double v)
{
	char b[64];
	std::snprintf(b, sizeof(b), "%.9g", v);
	return b;
}

//! Start the provider with the eye midpoint at (x, y, z), take its first sample, stop it.
xrt_tracking_viewer_sample
sample_at(const xrt_tracking_plugin_iface *iface, xrt_tracking_plugin_instance *inst, double x, double y, double z)
{
	set_env("OFFSET_X_M", num(x));
	set_env("OFFSET_Y_M", num(y));
	set_env("OFFSET_Z_M", num(z - kNominalZ));
	xrt_tracking_sink sink = {};
	sink.struct_size = sizeof(sink);
	sink.push = capture_push;
	const int before = g_pushes;
	REQUIRE(iface->start(inst, &sink) == XRT_SUCCESS);
	iface->stop(inst);
	REQUIRE(g_pushes > before);
	return g_last;
}

} // namespace

TEST_CASE("tracking calibrate: sim_tracking hidden pose -> solve -> CAMERA_POSE -> display output", "[calibrate_e2e]")
{
	const double half = kTruePitchDeg * 3.14159265358979323846 / 360.0;
	const std::string true_pose = num(kTrueT[0]) + "," + num(kTrueT[1]) + "," + num(kTrueT[2]) + "," +
	                              num(std::sin(half)) + ",0,0," + num(std::cos(half));
	set_env("ENABLE", "1");
	set_env("CAMERA", "1");
	set_env("SCRIPT", "offset");
	set_env("CAMERA_TRUE_POSE", true_pose);
	for (int n = 0; n < 16; n++) {
		unset_env(("CAMERA_POSE_" + std::to_string(n)).c_str());
		unset_env(("CAMERA_POSE_" + std::to_string(n) + "_CAMERA").c_str());
	}

	// A 0.60 x 0.34 m panel, nominal viewer 0.6 m out on the normal.
	xrt_tracking_host_display_geometry geo = {};
	geo.struct_size = sizeof(geo);
	geo.display_width_m = 0.60f;
	geo.display_height_m = 0.34f;
	geo.nominal_viewer_z_m = kNominalZ;
	target_tracking_plugin_set_display_geometry(&geo);

	REQUIRE(target_tracking_plugin_get_count() >= 1);
	REQUIRE(std::string(target_tracking_plugin_get_row_id(0)) == "sim-tracking");
	const xrt_tracking_plugin_iface *iface = target_tracking_plugin_get_iface(0);
	xrt_tracking_plugin_instance *inst = target_tracking_plugin_get_instance(0);
	REQUIRE(iface != nullptr);

	xrt_tracking_provider_info info = {};
	info.struct_size = sizeof(info);
	REQUIRE(iface->describe(inst, &info) == XRT_SUCCESS);
	CHECK((info.tracks & XRT_TRACKING_TRACKS_CAMERA_SPACE) != 0);
	const std::string key = info.camera_key;
	CHECK(key == "sim-tracking:webcam0");

	// Five dots (centre, then left / right / top / bottom at 30 % inset), two head distances.
	const double dots[5][2] = {{0, 0}, {-0.12, 0}, {0.12, 0}, {0, 0.068}, {0, -0.068}};
	std::vector<m_mirror_calib_sample> pairs;
	std::string json =
	    "{\"mode\":\"b\",\"eye\":\"mid\",\"camera_key\":\"" + key + "\",\"screen\":\"E2E-1\",\"samples\":[";
	for (double z : {0.50, 0.75}) {
		for (const auto &d : dots) {
			const xrt_tracking_viewer_sample s = sample_at(iface, inst, d[0], d[1], z);
			REQUIRE((s.flags & XRT_TRACKING_SAMPLE_CAMERA_VALID) != 0);
			m_mirror_calib_sample m = {};
			m.dot_m[0] = d[0];
			m.dot_m[1] = d[1];
			REQUIRE(m_mirror_calib_eye_point(M_MIRROR_CALIB_EYE_MID, s.camera.left_dir,
			                                 s.camera.left_distance_m, s.camera.right_dir,
			                                 s.camera.right_distance_m, m.dir, &m.est_distance_m));
			pairs.push_back(m);
			char line[512];
			std::snprintf(
			    line, sizeof(line),
			    "%s{\"dot_m\":[%.9g,%.9g],\"sample\":{\"camera\":{\"key\":\"%s\",\"left_dir\":[%.9g,%.9g,%."
			    "9g],"
			    "\"left_distance_m\":%.9g,\"right_dir\":[%.9g,%.9g,%.9g],\"right_distance_m\":%.9g}}}",
			    pairs.size() > 1 ? "," : "", d[0], d[1], key.c_str(), s.camera.left_dir[0],
			    s.camera.left_dir[1], s.camera.left_dir[2], s.camera.left_distance_m, s.camera.right_dir[0],
			    s.camera.right_dir[1], s.camera.right_dir[2], s.camera.right_distance_m);
			json += line;
		}
	}
	json += "]}\n";
	if (const char *out = std::getenv("DXR_CALIB_E2E_OUT")) {
		FILE *f = std::fopen(out, "wb");
		REQUIRE(f != nullptr);
		std::fputs(json.c_str(), f);
		std::fclose(f);
	}

	// Before calibration the provider reports through its bezel prior: off.
	const xrt_tracking_viewer_sample before = sample_at(iface, inst, 0.05, -0.03, 0.65);
	const double before_err = std::hypot(0.5 * (before.left_eye[0] + before.right_eye[0]) - 0.05,
	                                     0.5 * (before.left_eye[1] + before.right_eye[1]) + 0.03);

	m_mirror_calib_options opt;
	m_mirror_calib_options_init(&opt);
	opt.mode = M_MIRROR_CALIB_MODE_PITCH;
	m_mirror_calib_result r;
	REQUIRE(m_mirror_calib_solve(pairs.data(), (int)pairs.size(), &opt, &r));
	std::printf("[calibrate_e2e] hidden T = (%.2f, %.2f, %.2f) mm, pitch %.3f deg\n", kTrueT[0] * 1e3,
	            kTrueT[1] * 1e3, kTrueT[2] * 1e3, kTruePitchDeg);
	std::printf(
	    "[calibrate_e2e] solved T = (%.3f, %.3f, %.3f) mm, pitch %.4f deg, yaw %.4f, roll %.4f; rms %.4f mm, "
	    "rank %d/%d, cond %.3g, iris scale %.5f\n",
	    r.t_m[0] * 1e3, r.t_m[1] * 1e3, r.t_m[2] * 1e3, r.pitch_deg, r.yaw_deg, r.roll_deg, r.rms_m * 1e3, r.rank,
	    r.params, r.condition, r.iris_scale);
	CHECK(std::fabs(r.t_m[0] - kTrueT[0]) < 1e-4);
	CHECK(std::fabs(r.t_m[1] - kTrueT[1]) < 1e-4);
	CHECK(r.pitch_deg == Catch::Approx(kTruePitchDeg).margin(0.02));
	CHECK(r.rms_m < 1e-4);
	CHECK(r.iris_scale == Catch::Approx(1.0).margin(1e-3));

	// Mode (c) on the same ten samples also finds it (yaw / roll ~ 0).
	m_mirror_calib_options optc = opt;
	optc.mode = M_MIRROR_CALIB_MODE_FULL;
	m_mirror_calib_result rc;
	REQUIRE(m_mirror_calib_solve(pairs.data(), (int)pairs.size(), &optc, &rc));
	CHECK(rc.pitch_deg == Catch::Approx(kTruePitchDeg).margin(0.05));
	CHECK(std::fabs(rc.yaw_deg) < 0.05);
	CHECK(std::fabs(rc.roll_deg) < 0.05);

	// Commit as CAMERA_POSE_0 (the environment tier of the same store `calibrate apply` writes).
	char setting[160];
	std::snprintf(setting, sizeof(setting), "%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g", r.t_m[0], r.t_m[1], r.t_m[2],
	              r.q[0], r.q[1], r.q[2], r.q[3]);
	set_env("CAMERA_POSE_0", setting);
	set_env("CAMERA_POSE_0_CAMERA", key);
	set_env("CAMERA_POSE_0_SCREEN", "E2E-1");
	const xrt_tracking_viewer_sample after = sample_at(iface, inst, 0.05, -0.03, 0.65);
	const double ax = 0.5 * (after.left_eye[0] + after.right_eye[0]);
	const double ay = 0.5 * (after.left_eye[1] + after.right_eye[1]);
	const double az = 0.5 * (after.left_eye[2] + after.right_eye[2]);
	const double after_err =
	    std::sqrt((ax - 0.05) * (ax - 0.05) + (ay + 0.03) * (ay + 0.03) + (az - 0.65) * (az - 0.65));
	std::printf(
	    "[calibrate_e2e] reported eye midpoint vs scripted truth: %.2f mm through the prior, %.4f mm after "
	    "CAMERA_POSE_0\n",
	    before_err * 1e3, after_err * 1e3);
	CHECK(before_err > 0.01);
	CHECK(after_err < 2e-4);
}
