// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief The legacy camera rig's on-screen disparity does not depend on the
 *        display's physical size (#2451).
 *
 * The legacy camera profile seeds the qwerty camera rig, and xrLocateViews
 * computes it with the shared core's camera3d math (oxr_session.c, the
 * `rig_camera || qwerty_camera` branch). In that math the eye offset is
 * multiplied by metersToVirtual and sheared by convergenceDiopters over the
 * base frustum tan(verticalFov/2): the screen height and the virtual display
 * height never enter. For the same profile and the same eyes, the disparity
 * as a fraction of the canvas width is therefore the same on any panel of the
 * same aspect, and equals the closed form
 *
 *     ipdFactor * metersToVirtual * (convergenceDiopters - 1/Z) * eyeSep
 *         / (2 * tan(verticalFov/2) * aspect)
 *
 * for a point Z world units in front of the camera (Z = infinity at the far
 * end). These cases pin that, so a display-size dependence cannot creep in.
 */

#include "displayxr_math_xrt.h"

#include "catch_amalgamated.hpp"

#include <cmath>

using Catch::Approx;

namespace {

struct Panel
{
	const char *name;
	float width_m, height_m, nominal_z_m;
};

// The sim-display Windows defaults (sim_display_device.c) and two Leia SR panels.
constexpr Panel kSim = {"sim-display", 0.344f, 0.194f, 0.60f};
constexpr Panel kLeia156 = {"15.6in 16:9 panel", 0.3442f, 0.1936f, 0.60f};
constexpr Panel kLeia1610 = {"16:10 panel", 0.3444f, 0.2153f, 0.60f};

//! Disparity of a point straight ahead of the camera, `depth` world units in
//! front of it (<= 0 = at infinity), as a signed fraction of the canvas width
//! (right-eye minus left-eye image x; positive = behind the glass).
double
disparity_fraction(const Panel &p, const dxr_camera3d_tunables &t, float eye_sep_m, float depth)
{
	const xrt_vec3 eyes[2] = {{-eye_sep_m / 2, 0.1f, p.nominal_z_m}, {eye_sep_m / 2, 0.1f, p.nominal_z_m}};
	// Canvas-rebased eyes are relative to the canvas centre; the core takes
	// the nominal viewer for z (the 10 cm eye height is an eye offset).
	const xrt_vec3 nominal = {0, 0.1f, p.nominal_z_m};
	const dxr_screen scr = {p.width_m, p.height_m};
	const xrt_pose pose = {{0, 0, 0, 1}, {0, 0, 0}};
	dxr_xrt_view v[2];
	dxr_xrt_camera3d_compute_views(eyes, 2, &nominal, &scr, &t, &pose, v);
	double x[2];
	for (int i = 0; i < 2; i++) {
		const double tl = std::tan((double)v[i].fov.angle_left); // negative
		const double tr = std::tan((double)v[i].fov.angle_right);
		// Direction to the point from this eye (camera looks down -Z).
		const double dir = depth > 0 ? (0.0 - v[i].eye_world.x) / (depth + v[i].eye_world.z) : 0.0;
		x[i] = (dir - tl) / (tr - tl); // 0 = left canvas edge, 1 = right
	}
	return x[1] - x[0];
}

double
closed_form(const Panel &p, const dxr_camera3d_tunables &t, float eye_sep_m, float depth)
{
	const double invz = depth > 0 ? 1.0 / depth : 0.0;
	const double aspect = (double)p.width_m / p.height_m;
	return (double)t.ipd_factor * t.m2v * ((double)t.inv_convergence_distance - invz) * eye_sep_m /
	       (2.0 * t.half_tan_vfov * aspect);
}

dxr_camera3d_tunables
profile(float ipd, float conv_m, float vfov_rad, float m2v)
{
	dxr_camera3d_tunables t = dxr_camera3d_default_tunables();
	t.ipd_factor = ipd;
	t.parallax_factor = 1;
	t.inv_convergence_distance = 1.0f / conv_m;
	t.half_tan_vfov = std::tan(vfov_rad / 2);
	t.m2v = m2v;
	return t;
}

} // namespace

TEST_CASE("Camera-rig disparity matches the closed form on every panel", "[camera-rig][2451]")
{
	for (const Panel &p : {kSim, kLeia156, kLeia1610}) {
		for (const auto &t :
		     {profile(0.7f, 0.8f, 1.05f, 1), profile(0.05f, 0.8f, 1.05f, 1), profile(1, 2.0f, 0.6283f, 1),
		      profile(1, 1 / 1.6667f, 0.355f, 1), profile(0.7f, 0.8f, 1.05f, 3)}) {
			for (float depth : {0.0f, 0.5f, 0.8f, 3.0f}) {
				CAPTURE(p.name, t.ipd_factor, t.inv_convergence_distance, t.half_tan_vfov, t.m2v,
				        depth);
				CHECK(disparity_fraction(p, t, 0.063f, depth) ==
				      Approx(closed_form(p, t, 0.063f, depth)).margin(1e-6));
			}
		}
	}
}

TEST_CASE("Same profile and eyes give the same canvas-relative disparity whatever the panel size", "[camera-rig][2451]")
{
	// Same aspect, panel height x1 and x2, nominal distance x1 and x1.5: the
	// physical size and the nominal viewer distance do not enter the gain
	// (the nominal distance only enters the comfort clamp, applied at seed).
	const auto t = profile(0.7f, 0.8f, 1.05f, 1);
	const Panel small = {"small", 0.344f, 0.194f, 0.60f};
	const Panel large = {"large", 0.688f, 0.388f, 0.90f};
	for (float depth : {0.0f, 0.5f, 3.0f}) {
		CAPTURE(depth);
		CHECK(disparity_fraction(small, t, 0.063f, depth) ==
		      Approx(disparity_fraction(large, t, 0.063f, depth)).margin(1e-7));
	}
	// sim-display (synthetic 60 mm eyes) vs a 16:9 Leia panel (a tracked
	// 63 mm viewer): the only differences are the eye separation and the
	// canvas aspect, i.e. a few percent, never a factor of 7-10.
	const double sim = disparity_fraction(kSim, t, 0.060f, 0.0f);
	const double leia = disparity_fraction(kLeia156, t, 0.063f, 0.0f);
	CHECK(leia / sim ==
	      Approx(1.05 * ((double)kSim.width_m / kSim.height_m) / ((double)kLeia156.width_m / kLeia156.height_m))
	          .epsilon(1e-4));
	CHECK(leia / sim < 1.1);
	CHECK(leia / sim > 0.9);
}

TEST_CASE("ipdFactor scales the camera-rig disparity linearly", "[camera-rig][2451]")
{
	// 0.7 -> 0.05 is a 14x change on any panel; it does not compensate a
	// panel-dependent gain because there is none.
	const double a = disparity_fraction(kLeia1610, profile(0.7f, 0.8f, 1.05f, 1), 0.063f, 3.0f);
	const double b = disparity_fraction(kLeia1610, profile(0.05f, 0.8f, 1.05f, 1), 0.063f, 3.0f);
	CHECK(a / b == Approx(14.0).epsilon(1e-4));
}
