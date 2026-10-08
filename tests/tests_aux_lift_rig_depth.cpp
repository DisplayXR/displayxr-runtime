// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_lift v3 (ADR-048 Addendum A): the app-rig -> lift viewpoints
 *         mapping (u_lift_rig) against the shared xrLocateViews rig core, and
 *         the auxiliary-depth display transform (u_lift_depth).
 */

#include "util/u_lift_depth.h"
#include "util/u_lift_rig.h"
#include "util/u_lift_viewpoint.h"

#include "dxr_view_math.h"

#include "catch_amalgamated.hpp"

#include <cmath>
#include <cstring>

using Catch::Approx;

namespace {

constexpr float kEps = 1e-4f;

void
require_vec(const float *a, const float *b, float eps = kEps)
{
	CHECK(a[0] == Approx(b[0]).margin(eps));
	CHECK(a[1] == Approx(b[1]).margin(eps));
	CHECK(a[2] == Approx(b[2]).margin(eps));
}

// Rect-relative tracked eyes: viewer 4 cm right, 2 cm up, 0.6 m away, IPD 63 mm.
const float kEyes[6] = {0.04f - 0.0315f, 0.02f, 0.60f, 0.04f + 0.0315f, 0.02f, 0.60f};
constexpr float kRectW = 0.30f;
constexpr float kRectH = 0.18f;
constexpr float kNominalZ = 0.5f;

} // namespace

TEST_CASE("lift_rig: NONE passes the eyes through", "[lift_rig]")
{
	u_lift_rig rig = {};
	rig.type = U_LIFT_RIG_NONE;
	float out[6];
	REQUIRE(u_lift_rig_apply(&rig, kNominalZ, kRectW, kRectH, kEyes, 2, out));
	require_vec(&out[0], &kEyes[0]);
	require_vec(&out[3], &kEyes[3]);
}

TEST_CASE("lift_rig: unknown rect or too many eyes is refused", "[lift_rig]")
{
	u_lift_rig rig = {U_LIFT_RIG_DISPLAY, 1.0f, 1.0f, 1.0f, 0.0f, 0.5f, 1.0f};
	float out[6];
	CHECK_FALSE(u_lift_rig_apply(&rig, kNominalZ, 0.0f, kRectH, kEyes, 2, out));
	CHECK_FALSE(u_lift_rig_apply(&rig, kNominalZ, kRectW, kRectH, kEyes, 0, out));
	CHECK_FALSE(u_lift_rig_apply(&rig, kNominalZ, kRectW, kRectH, kEyes, 9, out));
}

TEST_CASE("lift_rig: sanitize clamps like xrLocateViews", "[lift_rig]")
{
	u_lift_rig rig = {U_LIFT_RIG_CAMERA, -1.0f, 2.0e4f, 50.0f, 30.0f, NAN, 0.0f};
	CHECK(u_lift_rig_sanitize(&rig));
	CHECK(rig.ipd_factor == 0.0f);
	CHECK(rig.parallax_factor == 1.0e4f);
	CHECK(rig.perspective_factor == 10.0f);
	CHECK(rig.inv_convergence_distance == 20.0f);
	CHECK(rig.half_tan_vfov == Approx(std::tan(0.5f)));
	CHECK(rig.m2v == 1.0f); // 0 / unset = identity

	u_lift_rig bad = {77u, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
	u_lift_rig_sanitize(&bad);
	CHECK(bad.type == U_LIFT_RIG_NONE);
}

TEST_CASE("lift_rig: display rig = the xrLocateViews eyes after window rebase", "[lift_rig]")
{
	// xrLocateViews in a window (ADR-012): raw display-space eyes, rebased to
	// the window centre, through the shared display3d core with the window as
	// the screen and the app's virtual display height (any m2v). The lift
	// path with the same rig must land on the same physical eyes.
	const float window_centre[3] = {-0.05f, 0.03f, 0.0f};
	float raw[6];
	for (int i = 0; i < 2; i++) {
		for (int a = 0; a < 3; a++) {
			raw[3 * i + a] = kEyes[3 * i + a] + window_centre[a];
		}
	}

	const float ipd = 0.6f, parallax = 0.7f, persp = 1.3f, vdh = 2.4f; // m2v = vdh / rect_h
	dxr_vec3 rebased[2];
	for (int i = 0; i < 2; i++) {
		rebased[i] = {raw[3 * i] - window_centre[0], raw[3 * i + 1] - window_centre[1],
		              raw[3 * i + 2] - window_centre[2]};
	}
	dxr_display3d_tunables t = dxr_display3d_default_tunables();
	t.ipd_factor = ipd;
	t.parallax_factor = parallax;
	t.perspective_factor = persp;
	t.virtual_display_height = vdh;
	const dxr_vec3 nominal = {0.0f, 0.0f, kNominalZ};
	const dxr_screen screen = {kRectW, kRectH};
	dxr_display3d_view located[2];
	dxr_display3d_compute_views(rebased, 2, &nominal, &screen, &t, nullptr, 0.0f, 0.0f, 0, located);
	const float m2v = vdh / kRectH;

	// Lift: rebase (u_lift_viewpoint_rebase), policy as a pass-through
	// (XYZ, recentering OFF, no clamp), then the rig.
	float vps[6];
	memcpy(vps, raw, sizeof(vps));
	u_lift_viewpoint_rebase(vps, 2, window_centre);
	u_lift_view_control vc;
	u_lift_view_control_default(&vc);
	vc.axis_mode = U_LIFT_AXIS_XYZ;
	vc.recenter_mode = U_LIFT_RECENTER_OFF;
	vc.max_offset_m = 0.0f;
	u_lift_viewpoint_apply(&vc, nullptr, kNominalZ, 0, vps, 2, vps, nullptr, nullptr);
	u_lift_rig rig = {U_LIFT_RIG_DISPLAY, ipd, parallax, persp, 0.0f, 0.5f, 1.0f};
	REQUIRE_FALSE(u_lift_rig_sanitize(&rig));
	REQUIRE(u_lift_rig_apply(&rig, kNominalZ, kRectW, kRectH, vps, 2, vps));

	for (int i = 0; i < 2; i++) {
		const float expect[3] = {located[i].eye_display.x / m2v, located[i].eye_display.y / m2v,
		                         located[i].eye_display.z / m2v};
		require_vec(&vps[3 * i], expect);
	}
	// And the frustum each eye sees onto the rect is the located one.
	for (int i = 0; i < 2; i++) {
		const dxr_fov f =
		    dxr_display3d_compute_fov({vps[3 * i], vps[3 * i + 1], vps[3 * i + 2]}, kRectW, kRectH);
		CHECK(f.angle_left == Approx(located[i].fov.angle_left).margin(kEps));
		CHECK(f.angle_right == Approx(located[i].fov.angle_right).margin(kEps));
		CHECK(f.angle_up == Approx(located[i].fov.angle_up).margin(kEps));
		CHECK(f.angle_down == Approx(located[i].fov.angle_down).margin(kEps));
	}
}

TEST_CASE("lift_rig: camera rig viewpoints see the camera rig's frustum", "[lift_rig]")
{
	const float ipd = 1.5f, parallax = 0.8f, invd = 0.4f, vfov = 0.9f, m2v = 2.0f;
	u_lift_rig rig = {U_LIFT_RIG_CAMERA, ipd, parallax, 1.0f, invd, std::tan(0.5f * vfov), m2v};
	REQUIRE_FALSE(u_lift_rig_sanitize(&rig));
	float vps[6];
	REQUIRE(u_lift_rig_apply(&rig, kNominalZ, kRectW, kRectH, kEyes, 2, vps));

	// The camera rig as xrLocateViews computes it (shared camera3d core).
	dxr_camera3d_tunables t = dxr_camera3d_default_tunables();
	t.ipd_factor = ipd;
	t.parallax_factor = parallax;
	t.inv_convergence_distance = invd;
	t.half_tan_vfov = std::tan(0.5f * vfov);
	t.m2v = m2v;
	const dxr_vec3 eyes[2] = {{kEyes[0], kEyes[1], kEyes[2]}, {kEyes[3], kEyes[4], kEyes[5]}};
	const dxr_vec3 nominal = {0.0f, 0.0f, kNominalZ};
	const dxr_screen screen = {kRectW, kRectH};
	dxr_camera3d_view cam[2];
	dxr_camera3d_compute_views(eyes, 2, &nominal, &screen, &t, nullptr, 0.1f, 100.0f, cam);

	for (int i = 0; i < 2; i++) {
		REQUIRE(vps[3 * i + 2] > 0.0f);
		const dxr_fov f =
		    dxr_display3d_compute_fov({vps[3 * i], vps[3 * i + 1], vps[3 * i + 2]}, kRectW, kRectH);
		CHECK(f.angle_left == Approx(cam[i].fov.angle_left).margin(kEps));
		CHECK(f.angle_right == Approx(cam[i].fov.angle_right).margin(kEps));
		CHECK(f.angle_up == Approx(cam[i].fov.angle_up).margin(kEps));
		CHECK(f.angle_down == Approx(cam[i].fov.angle_down).margin(kEps));
	}
	// The equivalent viewer of the nominal (unperturbed) camera sits at
	// rect_h / (2 tan(vfov / 2)) in front of the rect.
	float nom[3] = {0.0f, 0.0f, kNominalZ};
	float nom_out[3];
	REQUIRE(u_lift_rig_apply(&rig, kNominalZ, kRectW, kRectH, nom, 1, nom_out));
	CHECK(nom_out[2] == Approx(kRectH / (2.0f * std::tan(0.5f * vfov))).margin(kEps));
	CHECK(nom_out[0] == Approx(0.0f).margin(kEps));
}

TEST_CASE("lift_rig: camera rig at infinite convergence collapses the views", "[lift_rig]")
{
	u_lift_rig rig = {U_LIFT_RIG_CAMERA, 1.0f, 1.0f, 1.0f, 0.0f, std::tan(0.4f), 1.0f};
	float vps[6];
	REQUIRE(u_lift_rig_apply(&rig, kNominalZ, kRectW, kRectH, kEyes, 2, vps));
	require_vec(&vps[0], &vps[3]);
	CHECK(vps[2] == Approx(kRectH / (2.0f * std::tan(0.4f))).margin(kEps));
}

TEST_CASE("lift_depth: convergence depth lands on the rect, 0 at the viewpoint", "[lift_depth]")
{
	const float c[3] = {0.03f, -0.01f, 0.55f};   // viewpoint, rect-relative
	const float r[3] = {0.10f, 0.05f, 0.0f};     // rect centre, display space
	const float w = 0.32f, h = 0.20f, dc = 2.5f; // metres, convergence depth
	float m[16];
	REQUIRE(u_lift_depth_to_display(c, r, w, h, dc, m));

	const float uv[5][2] = {{0.5f, 0.5f}, {0.0f, 0.0f}, {1.0f, 0.0f}, {0.25f, 0.75f}, {1.0f, 1.0f}};
	for (const auto &t : uv) {
		float p[3];
		u_lift_depth_transform_point(m, t[0], t[1], dc, p);
		const float on_rect[3] = {r[0] + w * (t[0] - 0.5f), r[1] + h * (0.5f - t[1]), r[2]};
		require_vec(p, on_rect);

		float e[3];
		u_lift_depth_transform_point(m, t[0], t[1], 0.0f, e);
		const float eye[3] = {r[0] + c[0], r[1] + c[1], r[2] + c[2]};
		require_vec(e, eye);

		// Behind the screen (d > dc) stays on the ray eye -> rect point.
		float b[3];
		u_lift_depth_transform_point(m, t[0], t[1], 2.0f * dc, b);
		const float expect[3] = {eye[0] + 2.0f * (on_rect[0] - eye[0]), eye[1] + 2.0f * (on_rect[1] - eye[1]),
		                         eye[2] + 2.0f * (on_rect[2] - eye[2])};
		require_vec(b, expect);
		CHECK(b[2] < r[2]); // behind the screen plane
	}
	CHECK(m[3] == 0.0f);
	CHECK(m[7] == 0.0f);
	CHECK(m[11] == 0.0f);
	CHECK(m[15] == 1.0f); // affine output, w = 1
}

TEST_CASE("lift_depth: invalid inputs report no transform", "[lift_depth]")
{
	const float c[3] = {0.0f, 0.0f, 0.5f};
	const float bad_c[3] = {0.0f, 0.0f, 0.0f};
	const float r[3] = {0.0f, 0.0f, 0.0f};
	float m[16];
	CHECK_FALSE(u_lift_depth_to_display(c, r, 0.0f, 0.2f, 1.0f, m));
	CHECK_FALSE(u_lift_depth_to_display(c, r, 0.3f, 0.2f, 0.0f, m));
	CHECK_FALSE(u_lift_depth_to_display(c, r, 0.3f, 0.2f, NAN, m));
	CHECK_FALSE(u_lift_depth_to_display(bad_c, r, 0.3f, 0.2f, 1.0f, m));
	CHECK(m[0] == 1.0f); // identity on failure
	CHECK(m[5] == 1.0f);
	CHECK(m[12] == 0.0f);
}

TEST_CASE("lift_depth: decode linear and inverse samples", "[lift_depth]")
{
	CHECK(u_lift_depth_decode(0.5f, 1.0f, 1.0f, false) == Approx(1.5f));
	CHECK(u_lift_depth_decode(0.5f, 0.0f, 0.0f, false) == Approx(0.5f)); // scale 0 read as 1
	CHECK(u_lift_depth_decode(0.25f, 2.0f, 0.0f, true) == Approx(2.0f)); // 1 / (2 * 0.25)
	CHECK(u_lift_depth_decode(0.0f, 1.0f, 0.0f, true) == 0.0f);
}
