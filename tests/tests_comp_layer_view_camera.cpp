// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Tests for the shared per-view camera resolver (#1580).
 * @ingroup tests
 *
 * The bug: the D3D11 renderer composed quad / cylinder / equirect / cube
 * layers with a HARDCODED camera (eyes at {+-0.032, 0, 0}, symmetric +-45 deg)
 * while projection layers were an identity-MVP fullscreen blit — i.e. they
 * landed exactly where the app's own off-axis Kooima frustum put them. Same
 * world pose, different display pixels; the Khronos CTS composition set
 * (GradientFormatsLinearVsNonLinear, QuadProjectionQuad, ...) fails on that.
 *
 * Two facts this file pins, because they are what makes branch (a) legal:
 *
 *  1. `data.quad.pose` and `data.proj.v[i].pose` come out of the SAME
 *     `handle_space()` call in oxr_session_frame_end.c — including the
 *     VIEW-reference-space path, which since #1607 goes through the very same
 *     head-device locate (the #1502 eye-centroid offset riding along as that
 *     locate's base offset). Both are therefore already in the head device's
 *     TRACKING-ORIGIN ("root") frame, so reusing a projection layer's per-view
 *     pose as the quad's camera needs NO re-basing — while branch (b), which
 *     starts from the HEAD-relative DP eye, does (#1594).
 *
 *  2. Every CTS quad sets XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT
 *     and NOT XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT, so PREMULTIPLIED
 *     is the variant conformance actually exercises (#1621 — this line said
 *     "straight" until the three backends were compared on one layer).
 *
 * `comp_layer_accum` is a plain aggregate (an array of `struct comp_layer` plus
 * a count), so the fixtures below fill it directly rather than going through
 * `comp_layer_accum_quad()` / `_projection()`, which want live
 * `xrt_swapchain` references this test has no device to create.
 */

#include "catch_amalgamated.hpp"

#include "util/comp_layer_accum.h"
#include "util/comp_layer_view_camera.h"

#include "math/m_api.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>

namespace {

constexpr float kEps = 1e-5f;

void
push_projection(struct comp_layer_accum &accum,
                uint32_t view_count,
                const struct xrt_pose *poses,
                const struct xrt_fov *fovs,
                enum xrt_layer_type type = XRT_LAYER_PROJECTION)
{
	struct comp_layer &layer = accum.layers[accum.layer_count++];
	memset(&layer, 0, sizeof(layer));
	layer.data.type = type;
	layer.data.view_count = view_count;
	for (uint32_t i = 0; i < view_count; i++) {
		layer.data.proj.v[i].pose = poses[i];
		layer.data.proj.v[i].fov = fovs[i];
	}
}

void
push_quad(struct comp_layer_accum &accum,
          const struct xrt_pose &pose,
          enum xrt_layer_eye_visibility visibility = XRT_LAYER_EYE_VISIBILITY_BOTH)
{
	struct comp_layer &layer = accum.layers[accum.layer_count++];
	memset(&layer, 0, sizeof(layer));
	layer.data.type = XRT_LAYER_QUAD;
	layer.data.view_count = 2;
	layer.data.quad.pose = pose;
	layer.data.quad.visibility = visibility;
	layer.data.quad.size = {0.5f, 0.5f};
	layer.data.flags = XRT_LAYER_COMPOSITION_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
}

struct xrt_eye_positions
make_eyes(std::initializer_list<struct xrt_eye_position> list)
{
	struct xrt_eye_positions eyes = {};
	for (const auto &e : list) {
		eyes.eyes[eyes.count++] = e;
	}
	eyes.valid = true;
	return eyes;
}

/*!
 * The located view a DisplayXR session reports for one eye, computed from the
 * closed-form Kooima relations rather than from the code under test.
 *
 * This is the oracle the compositor's camera must match: `xrLocateViews`
 * reports the eye verbatim as the view POSE (oxr_session.c, the eye-override /
 * tracked-eye branches, re-expressed head-relative) with identity orientation,
 * and the off-axis frustum of that eye against the canvas as the FOV
 * (dxr_display3d_compute_fov through dxr_xrt_display3d_compute_views, which
 * reduces to exactly these four atans under the default tunables a session with
 * no chained rig gets: ipd = parallax = perspective = 1, vH = screen height so
 * m2v = 1).
 */
struct comp_layer_view_camera
located_view(const struct xrt_vec3 &eye, float canvas_w_m, float canvas_h_m, const struct xrt_vec3 &canvas_center)
{
	const float ex = eye.x - canvas_center.x;
	const float ey = eye.y - canvas_center.y;
	const float ez = eye.z - canvas_center.z;

	struct comp_layer_view_camera cam = {};
	cam.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
	cam.pose.position = eye;
	cam.fov.angle_left = std::atan((-canvas_w_m / 2 - ex) / ez);
	cam.fov.angle_right = std::atan((canvas_w_m / 2 - ex) / ez);
	cam.fov.angle_up = std::atan((canvas_h_m / 2 - ey) / ez);
	cam.fov.angle_down = std::atan((-canvas_h_m / 2 - ey) / ez);
	cam.source = COMP_LAYER_VIEW_CAMERA_FROM_DISPLAY3D;
	return cam;
}

/*!
 * Put frame cameras on an accum, the way the state tracker does at xrEndFrame
 * (oxr_session_frame_end.c fills xrt_layer_frame_data::cameras from
 * oxr_session_frame_view_cameras()).
 */
void
set_frame_cameras(struct comp_layer_accum &accum,
                  std::initializer_list<struct xrt_frame_view_camera> list)
{
	accum.data.camera_count = 0;
	for (const auto &c : list) {
		accum.data.cameras[accum.data.camera_count++] = c;
	}
	accum.data.cameras_valid = accum.data.camera_count > 0;
}

/*!
 * #1594: T_root_head, the ROOT-frame pose of the head device for this frame,
 * the way the state tracker fills it at xrEndFrame
 * (oxr_session_frame_end.c, `xrt_device_get_tracked_pose(head, ...)`).
 *
 * Deliberately non-trivial — a yaw AND a translation. A translation-only head
 * would let an orientation bug through, and an identity head would let a
 * MISSING lift through, which is the whole point of the follow-up.
 */
struct xrt_pose
make_head_pose()
{
	struct xrt_pose p = {};
	// 30 deg about +Y.
	p.orientation = {0.0f, 0.258819f, 0.0f, 0.9659258f};
	// The measured sim-rig head pose from #1594, moved off-axis in x so a
	// dropped rotation cannot hide.
	p.position = {0.05f, 0.100f, 0.600f};
	return p;
}

void
set_head_pose(struct comp_layer_accum &accum, const struct xrt_pose &head)
{
	accum.data.head_pose = head;
	accum.data.head_pose_valid = true;
}

//! head o head_relative, with math_pose_transform's exact semantics.
struct xrt_pose
lift_to_root(const struct xrt_pose &head, const struct xrt_vec3 &head_relative_pos)
{
	struct xrt_pose in = {};
	in.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
	in.position = head_relative_pos;

	struct xrt_pose out = {};
	math_pose_transform(&head, &in, &out);
	return out;
}

void
check_same_camera(const struct comp_layer_view_camera &got, const struct comp_layer_view_camera &want)
{
	CHECK(got.pose.position.x == Catch::Approx(want.pose.position.x).margin(kEps));
	CHECK(got.pose.position.y == Catch::Approx(want.pose.position.y).margin(kEps));
	CHECK(got.pose.position.z == Catch::Approx(want.pose.position.z).margin(kEps));
	CHECK(got.pose.orientation.x == Catch::Approx(want.pose.orientation.x).margin(kEps));
	CHECK(got.pose.orientation.y == Catch::Approx(want.pose.orientation.y).margin(kEps));
	CHECK(got.pose.orientation.z == Catch::Approx(want.pose.orientation.z).margin(kEps));
	CHECK(got.pose.orientation.w == Catch::Approx(want.pose.orientation.w).margin(kEps));
	CHECK(got.fov.angle_left == Catch::Approx(want.fov.angle_left).margin(kEps));
	CHECK(got.fov.angle_right == Catch::Approx(want.fov.angle_right).margin(kEps));
	CHECK(got.fov.angle_up == Catch::Approx(want.fov.angle_up).margin(kEps));
	CHECK(got.fov.angle_down == Catch::Approx(want.fov.angle_down).margin(kEps));
}

} // namespace


TEST_CASE("comp_layer_view_camera: (a) projection layer present wins")
{
	// The app's real camera: the sim head's off-axis Kooima frustum at an
	// eye 10 cm up and 60 cm out, i.e. nothing like the old +-45 deg
	// placeholder.
	struct xrt_pose poses[2] = {
	    {{0.0f, 0.0f, 0.0f, 1.0f}, {-0.032f, 0.10f, 0.60f}},
	    {{0.0f, 0.0f, 0.0f, 1.0f}, {0.032f, 0.10f, 0.60f}},
	};
	struct xrt_fov fovs[2] = {
	    {-0.2327f, 0.3241f, -0.0055f, -0.3176f},
	    {-0.3241f, 0.2327f, -0.0055f, -0.3176f},
	};

	struct comp_layer_accum accum = {};
	push_projection(accum, 2, poses, fovs);
	push_quad(accum, {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f}});

	for (uint32_t view = 0; view < 2; view++) {
		struct comp_layer_view_camera cam = {};
		// Deliberately ALSO pass a usable eye + canvas: branch (a) must
		// still win, because the app's own camera beats anything the
		// runtime would synthesise.
		struct xrt_vec3 eye = {0.0f, 0.10f, 0.60f};
		REQUIRE(comp_layer_view_camera_select(&accum, view, &eye, 0.60f, 0.34f, &cam));

		CHECK(cam.source == COMP_LAYER_VIEW_CAMERA_FROM_PROJECTION);
		// #1580, the whole point: the quad camera IS the projection
		// layer's camera for that view. Same {pose, fov}, same pixels.
		CHECK(cam.pose.position.x == Catch::Approx(poses[view].position.x).epsilon(kEps));
		CHECK(cam.pose.position.y == Catch::Approx(poses[view].position.y).epsilon(kEps));
		CHECK(cam.pose.position.z == Catch::Approx(poses[view].position.z).epsilon(kEps));
		CHECK(cam.fov.angle_left == Catch::Approx(fovs[view].angle_left).epsilon(kEps));
		CHECK(cam.fov.angle_right == Catch::Approx(fovs[view].angle_right).epsilon(kEps));
		CHECK(cam.fov.angle_up == Catch::Approx(fovs[view].angle_up).epsilon(kEps));
		CHECK(cam.fov.angle_down == Catch::Approx(fovs[view].angle_down).epsilon(kEps));
	}
}

TEST_CASE("comp_layer_view_camera: (a) a 3D zone layer is projection-class too")
{
	struct xrt_pose poses[2] = {
	    {{0.0f, 0.0f, 0.0f, 1.0f}, {-0.03f, 0.05f, 0.55f}},
	    {{0.0f, 0.0f, 0.0f, 1.0f}, {0.03f, 0.05f, 0.55f}},
	};
	struct xrt_fov fovs[2] = {
	    {-0.4f, 0.3f, 0.2f, -0.25f},
	    {-0.3f, 0.4f, 0.2f, -0.25f},
	};

	struct comp_layer_accum accum = {};
	push_projection(accum, 2, poses, fovs, XRT_LAYER_ZONE_3D);

	struct comp_layer_view_camera cam = {};
	REQUIRE(comp_layer_view_camera_select(&accum, 1, nullptr, 0.0f, 0.0f, &cam));
	CHECK(cam.source == COMP_LAYER_VIEW_CAMERA_FROM_PROJECTION);
	CHECK(cam.fov.angle_right == Catch::Approx(0.4f).epsilon(kEps));
}

TEST_CASE("comp_layer_view_camera: (a) is skipped for views the layer does not cover")
{
	// A mono (view_count == 1) projection layer in a 2-view frame: view 1
	// is not covered, so it must NOT read v[1] (zeroes) — it falls through
	// to the synthesised camera.
	struct xrt_pose poses[1] = {{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.6f}}};
	struct xrt_fov fovs[1] = {{-0.4f, 0.4f, 0.3f, -0.3f}};

	struct comp_layer_accum accum = {};
	push_projection(accum, 1, poses, fovs);

	struct xrt_vec3 eye = {0.05f, 0.0f, 0.5f};
	struct comp_layer_view_camera cam = {};
	REQUIRE(comp_layer_view_camera_select(&accum, 1, &eye, 0.60f, 0.34f, &cam));
	CHECK(cam.source == COMP_LAYER_VIEW_CAMERA_FROM_DISPLAY3D);
}

TEST_CASE("comp_layer_view_camera: (b) synthesised from eye + canvas metres")
{
	// Quad-only frame — no projection layer anywhere.
	struct comp_layer_accum accum = {};
	push_quad(accum, {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f}});

	const float w = 0.60f;
	const float h = 0.34f;
	struct xrt_vec3 eye = {0.05f, 0.10f, 0.60f};

	struct comp_layer_view_camera cam = {};
	REQUIRE(comp_layer_view_camera_select(&accum, 0, &eye, w, h, &cam));

	CHECK(cam.source == COMP_LAYER_VIEW_CAMERA_FROM_DISPLAY3D);

	// No head pose on this frame, so the pose is the eye verbatim with
	// identity orientation — the pre-#1594 head-relative result. The lifted
	// case is pinned in "(b) is lifted into the ROOT layer frame" below.
	CHECK(cam.pose.position.x == Catch::Approx(eye.x).epsilon(kEps));
	CHECK(cam.pose.position.y == Catch::Approx(eye.y).epsilon(kEps));
	CHECK(cam.pose.position.z == Catch::Approx(eye.z).epsilon(kEps));
	CHECK(cam.pose.orientation.w == Catch::Approx(1.0f).epsilon(kEps));

	// The FOV is the shared Kooima core's, i.e. asymmetric about the eye.
	CHECK(cam.fov.angle_left == Catch::Approx(std::atan((-w / 2 - eye.x) / eye.z)).epsilon(kEps));
	CHECK(cam.fov.angle_right == Catch::Approx(std::atan((w / 2 - eye.x) / eye.z)).epsilon(kEps));
	CHECK(cam.fov.angle_up == Catch::Approx(std::atan((h / 2 - eye.y) / eye.z)).epsilon(kEps));
	CHECK(cam.fov.angle_down == Catch::Approx(std::atan((-h / 2 - eye.y) / eye.z)).epsilon(kEps));

	// It is NOT the old placeholder.
	CHECK(std::abs(cam.fov.angle_right - 0.785f) > 0.01f);
}

TEST_CASE("comp_layer_view_camera: (b) the canvas centre moves the frustum, not the pose")
{
	struct comp_layer_accum accum = {};
	const float w = 0.40f;
	const float h = 0.24f;

	// Eye on the display axis, window 10 cm to the right of the display
	// centre: relative to the CANVAS the eye is 10 cm to the LEFT, so the
	// frustum must lean right.
	struct xrt_vec3 eye = {0.0f, 0.0f, 0.60f};
	struct xrt_vec3 canvas_center = {0.10f, 0.0f, 0.0f};

	struct comp_layer_view_camera cam = {};
	REQUIRE(comp_layer_view_camera_select_ex(&accum, 0, &eye, &canvas_center, w, h, &cam));

	CHECK(cam.source == COMP_LAYER_VIEW_CAMERA_FROM_DISPLAY3D);
	// Pose stays at the eye (no head pose on this frame) — untouched by the
	// canvas rebase, which is a FOV-only concern.
	CHECK(cam.pose.position.x == Catch::Approx(0.0f).margin(kEps));
	// FOV is rebased.
	CHECK(cam.fov.angle_left == Catch::Approx(std::atan((-w / 2 + 0.10f) / eye.z)).epsilon(kEps));
	CHECK(cam.fov.angle_right == Catch::Approx(std::atan((w / 2 + 0.10f) / eye.z)).epsilon(kEps));

	// A NULL canvas centre is exactly the centred case.
	struct comp_layer_view_camera centred = {};
	REQUIRE(comp_layer_view_camera_select(&accum, 0, &eye, w, h, &centred));
	CHECK(centred.fov.angle_left == Catch::Approx(-centred.fov.angle_right).epsilon(kEps));
}

TEST_CASE("comp_layer_view_camera: (c) fallback when neither is available")
{
	struct comp_layer_accum accum = {};
	push_quad(accum, {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f}});

	// No eye at all.
	struct comp_layer_view_camera cam = {};
	CHECK_FALSE(comp_layer_view_camera_select(&accum, 0, nullptr, 0.60f, 0.34f, &cam));
	CHECK(cam.source == COMP_LAYER_VIEW_CAMERA_FALLBACK);
	// Still fully populated — a false return is a diagnostic, not "skip".
	CHECK(cam.pose.position.x == Catch::Approx(-0.032f).epsilon(kEps));
	CHECK(cam.pose.orientation.w == Catch::Approx(1.0f).epsilon(kEps));
	CHECK(cam.fov.angle_right == Catch::Approx(0.785f).epsilon(kEps));
	CHECK(cam.fov.angle_left == Catch::Approx(-0.785f).epsilon(kEps));

	// Eye present but no canvas metres.
	struct xrt_vec3 eye = {0.0f, 0.1f, 0.6f};
	struct comp_layer_view_camera cam2 = {};
	CHECK_FALSE(comp_layer_view_camera_select(&accum, 1, &eye, 0.0f, 0.0f, &cam2));
	CHECK(cam2.source == COMP_LAYER_VIEW_CAMERA_FALLBACK);
	CHECK(cam2.pose.position.x == Catch::Approx(0.032f).epsilon(kEps));

	// NULL out is the one hard failure.
	CHECK_FALSE(comp_layer_view_camera_select(&accum, 0, &eye, 0.60f, 0.34f, nullptr));
}

TEST_CASE("comp_layer_view_camera: (b') the frame's own cameras beat the DP-eye derivation")
{
	/*
	 * The #1580 case the DP-eye branch provably CANNOT serve. On a
	 * runtime-window session the qwerty debug rig is synthesised into the
	 * locate (oxr_session.c ~2408) and defaults to camera_mode = true
	 * (qwerty_device.c), so the app is handed a CAMERA-centric frustum:
	 * half_tan_vfov 0.3249 (36 deg vFOV) sheared by the convergence, which
	 * does not depend on the nominal viewer distance at all. The
	 * display-centric synthesis from the DP eye + panel metres produces a
	 * completely different frustum (at z = 0.2 m over a 0.194 m panel, an
	 * entirely NEGATIVE vertical range). Only the plumbed camera is right.
	 */
	struct comp_layer_accum accum = {};
	push_quad(accum, {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f}});

	struct xrt_frame_view_camera cam0 = {};
	cam0.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
	cam0.pose.position = {0.0f, 0.1f, 0.0f}; // camera rig: eye_local.z = z - nominal_z = 0
	cam0.fov = {-0.5227f, 0.5227f, 0.2683f, -0.3587f};
	struct xrt_frame_view_camera cam1 = cam0;
	cam1.pose.position = {0.03f, 0.1f, 0.0f};
	set_frame_cameras(accum, {cam0, cam1});

	// Eye + canvas ARE available; the frame cameras must still win.
	const struct xrt_eye_positions eyes = make_eyes({{0.0f, 0.1f, 0.2f}, {0.03f, 0.1f, 0.2f}});

	struct comp_layer_view_camera got = {};
	REQUIRE(comp_layer_view_camera_select_eyes(&accum, 0, &eyes, 2, nullptr, 0.344f, 0.194f, &got));
	CHECK(got.source == COMP_LAYER_VIEW_CAMERA_FROM_FRAME);

	struct comp_layer_view_camera want = {};
	want.pose = cam0.pose;
	want.fov = cam0.fov;
	check_same_camera(got, want);

	// Per view, not a single camera reused.
	struct comp_layer_view_camera got1 = {};
	REQUIRE(comp_layer_view_camera_select_eyes(&accum, 1, &eyes, 2, nullptr, 0.344f, 0.194f, &got1));
	CHECK(got1.pose.position.x == Catch::Approx(0.03f).margin(kEps));

	// And it is emphatically NOT what the DP-eye branch would have said.
	const struct comp_layer_view_camera derived = located_view({0.0f, 0.1f, 0.2f}, 0.344f, 0.194f, {0, 0, 0});
	CHECK(std::abs(got.fov.angle_up - derived.fov.angle_up) > 0.1f);
	CHECK(std::abs(got.pose.position.z - derived.pose.position.z) > 0.1f);
}

TEST_CASE("comp_layer_view_camera: a projection layer still outranks the frame cameras")
{
	/*
	 * Order matters and (a) stays first: render_projection_layer() is an
	 * identity-MVP fullscreen blit, so the view tile IS the app's submitted
	 * frustum. A quad beside it must use that same one even when the frame
	 * also carries cameras, or the two disagree by whatever the app's
	 * submitted pose differs from the runtime's located one.
	 */
	struct comp_layer_accum accum = {};
	struct xrt_pose proj_pose = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.011f, 0.022f, 0.033f}};
	struct xrt_fov proj_fov = {-0.4f, 0.41f, 0.3f, -0.31f};
	struct xrt_pose poses[2] = {proj_pose, proj_pose};
	struct xrt_fov fovs[2] = {proj_fov, proj_fov};
	push_projection(accum, 2, poses, fovs);
	push_quad(accum, {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f}});

	struct xrt_frame_view_camera frame_cam = {};
	frame_cam.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
	frame_cam.pose.position = {0.9f, 0.9f, 0.9f};
	frame_cam.fov = {-1.0f, 1.0f, 1.0f, -1.0f};
	set_frame_cameras(accum, {frame_cam, frame_cam});

	const struct xrt_eye_positions eyes = make_eyes({{0.0f, 0.1f, 0.6f}, {0.03f, 0.1f, 0.6f}});

	struct comp_layer_view_camera got = {};
	REQUIRE(comp_layer_view_camera_select_eyes(&accum, 0, &eyes, 2, nullptr, 0.344f, 0.194f, &got));
	CHECK(got.source == COMP_LAYER_VIEW_CAMERA_FROM_PROJECTION);
	CHECK(got.pose.position.x == Catch::Approx(proj_pose.position.x).margin(kEps));
	CHECK(got.fov.angle_right == Catch::Approx(proj_fov.angle_right).margin(kEps));
}

TEST_CASE("comp_layer_view_camera: the full fallback ladder, in order")
{
	const float w = 0.344f;
	const float h = 0.194f;
	const struct xrt_eye_positions eyes = make_eyes({{0.0f, 0.1f, 0.6f}});

	struct xrt_frame_view_camera frame_cam = {};
	frame_cam.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
	frame_cam.pose.position = {0.5f, 0.5f, 0.5f};
	frame_cam.fov = {-1.0f, 1.0f, 1.0f, -1.0f};

	struct xrt_pose proj_pose = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.1f, 0.2f, 0.3f}};
	struct xrt_fov proj_fov = {-0.4f, 0.4f, 0.3f, -0.3f};
	struct xrt_pose poses[2] = {proj_pose, proj_pose};
	struct xrt_fov fovs[2] = {proj_fov, proj_fov};

	SECTION("projection + frame + eye -> (a)")
	{
		struct comp_layer_accum accum = {};
		push_projection(accum, 2, poses, fovs);
		set_frame_cameras(accum, {frame_cam, frame_cam});
		struct comp_layer_view_camera cam = {};
		REQUIRE(comp_layer_view_camera_select_eyes(&accum, 0, &eyes, 2, nullptr, w, h, &cam));
		CHECK(cam.source == COMP_LAYER_VIEW_CAMERA_FROM_PROJECTION);
	}

	SECTION("frame + eye -> (b')")
	{
		struct comp_layer_accum accum = {};
		push_quad(accum, {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f}});
		set_frame_cameras(accum, {frame_cam, frame_cam});
		struct comp_layer_view_camera cam = {};
		REQUIRE(comp_layer_view_camera_select_eyes(&accum, 0, &eyes, 2, nullptr, w, h, &cam));
		CHECK(cam.source == COMP_LAYER_VIEW_CAMERA_FROM_FRAME);
	}

	SECTION("eye only -> (b)")
	{
		struct comp_layer_accum accum = {};
		push_quad(accum, {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f}});
		struct comp_layer_view_camera cam = {};
		REQUIRE(comp_layer_view_camera_select_eyes(&accum, 0, &eyes, 2, nullptr, w, h, &cam));
		CHECK(cam.source == COMP_LAYER_VIEW_CAMERA_FROM_DISPLAY3D);
	}

	SECTION("nothing -> (c)")
	{
		struct comp_layer_accum accum = {};
		push_quad(accum, {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f}});
		struct comp_layer_view_camera cam = {};
		CHECK_FALSE(comp_layer_view_camera_select_eyes(&accum, 0, nullptr, 2, nullptr, 0.0f, 0.0f, &cam));
		CHECK(cam.source == COMP_LAYER_VIEW_CAMERA_FALLBACK);
	}

	SECTION("a view past camera_count falls THROUGH to (b), never off the end")
	{
		struct comp_layer_accum accum = {};
		push_quad(accum, {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f}});
		set_frame_cameras(accum, {frame_cam}); // one camera, four views
		struct comp_layer_view_camera cam = {};
		REQUIRE(comp_layer_view_camera_select_eyes(&accum, 3, &eyes, 4, nullptr, w, h, &cam));
		CHECK(cam.source == COMP_LAYER_VIEW_CAMERA_FROM_DISPLAY3D);
	}

	SECTION("cameras_valid false is ignored even with a populated array")
	{
		struct comp_layer_accum accum = {};
		push_quad(accum, {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f}});
		set_frame_cameras(accum, {frame_cam, frame_cam});
		accum.data.cameras_valid = false;
		struct comp_layer_view_camera cam = {};
		REQUIRE(comp_layer_view_camera_select_eyes(&accum, 0, &eyes, 2, nullptr, w, h, &cam));
		CHECK(cam.source == COMP_LAYER_VIEW_CAMERA_FROM_DISPLAY3D);
	}
}

TEST_CASE("comp_layer_view_camera: (b) the DP-eye fallback is the display-centric Kooima frustum")
{
	/*
	 * Branch (b) is the LAST-RESORT reconstruction, used only when the frame
	 * carries no cameras (a caller with no xrt_layer_frame_data, or a locate
	 * with no valid pose bits yet). It is a DISPLAY-centric frustum: the DP
	 * eye against the canvas. Pinned here so its inputs and output stay
	 * exactly the shared Kooima core's — and so the contrast with (b') is on
	 * the record. On the sim display with its nominal viewer 10 cm above the
	 * panel centre (sim_display_processor.c nominal_y_m = 0.1f) at
	 * SIM_DISPLAY_NOMINAL_Z_M = 0.2 over the 0.344 x 0.194 m panel, that
	 * frustum is entirely negative vertically — which is NOT what a session
	 * running the (default) qwerty camera rig is handed at xrLocateViews.
	 * That is the whole reason branch (b') exists above it.
	 */
	struct comp_layer_accum accum = {};
	push_quad(accum, {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f}});

	const float w = 0.344f;
	const float h = 0.194f;
	const struct xrt_vec3 origin = {0.0f, 0.0f, 0.0f};
	const struct xrt_eye_positions eyes = make_eyes({{0.0f, 0.1f, 0.2f}});

	struct comp_layer_view_camera cam = {};
	REQUIRE(comp_layer_view_camera_select_eyes(&accum, 0, &eyes, 1, nullptr, w, h, &cam));

	CHECK(cam.source == COMP_LAYER_VIEW_CAMERA_FROM_DISPLAY3D);
	check_same_camera(cam, located_view({0.0f, 0.1f, 0.2f}, w, h, origin));

	// Sanity on the oracle itself: the eye's own horizontal sits ABOVE the
	// panel's top edge here, so BOTH vertical half-angles come out negative.
	CHECK(cam.fov.angle_up < 0.0f);
	CHECK(cam.fov.angle_down < cam.fov.angle_up);
}

TEST_CASE("comp_layer_view_camera: (a) and (b) agree for the same frame inputs")
{
	/*
	 * The invariant in one assertion: adding a projection layer to a frame
	 * must not move the quads. Branch (a) takes the app's submitted
	 * proj.v[0].{pose,fov} — which IS what xrLocateViews handed it — and
	 * branch (b) synthesises from the DP eye + canvas. Same frame, same
	 * camera, or a quad lands on different display pixels depending on
	 * whether the app happened to also submit projection content.
	 */
	const float w = 0.344f;
	const float h = 0.194f;
	const struct xrt_vec3 origin = {0.0f, 0.0f, 0.0f};
	const struct xrt_vec3 eye = {-0.032f, 0.1f, 0.6f};
	const struct xrt_eye_positions eyes = make_eyes({{eye.x, eye.y, eye.z}, {0.032f, 0.1f, 0.6f}});

	// What the app got back from xrLocateViews for view 0, and therefore
	// what it submits in its projection layer.
	const struct comp_layer_view_camera reported = located_view(eye, w, h, origin);

	struct comp_layer_accum with_proj = {};
	struct xrt_pose proj_poses[2] = {reported.pose, reported.pose};
	struct xrt_fov proj_fovs[2] = {reported.fov, reported.fov};
	push_projection(with_proj, 2, proj_poses, proj_fovs);
	push_quad(with_proj, {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f}});

	struct comp_layer_accum quads_only = {};
	push_quad(quads_only, {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f}});

	struct comp_layer_view_camera from_a = {};
	struct comp_layer_view_camera from_b = {};
	REQUIRE(comp_layer_view_camera_select_eyes(&with_proj, 0, &eyes, 2, nullptr, w, h, &from_a));
	REQUIRE(comp_layer_view_camera_select_eyes(&quads_only, 0, &eyes, 2, nullptr, w, h, &from_b));

	CHECK(from_a.source == COMP_LAYER_VIEW_CAMERA_FROM_PROJECTION);
	CHECK(from_b.source == COMP_LAYER_VIEW_CAMERA_FROM_DISPLAY3D);
	check_same_camera(from_b, from_a);
	check_same_camera(from_a, reported);
}

TEST_CASE("comp_layer_view_camera: (b) view i composes through eye i, not a left/right pair")
{
	/*
	 * The sim display's 2x2 Quad mode reports FOUR eyes, the upper pair
	 * 64 mm above the lower (sim_display_processor.c, the vc >= 4 branch).
	 * The left/right pair this resolver used to be handed could only express
	 * two of them, so views 2 and 3 were composed 64 mm below where
	 * xrLocateViews put them — 3.7 deg of vertical error at 1 m.
	 */
	struct comp_layer_accum accum = {};
	push_quad(accum, {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f}});

	const float w = 0.344f;
	const float h = 0.194f;
	const struct xrt_vec3 origin = {0.0f, 0.0f, 0.0f};
	const struct xrt_eye_positions eyes = make_eyes({{-0.03f, 0.068f, 0.6f},
	                                                 {0.03f, 0.068f, 0.6f},
	                                                 {-0.03f, 0.132f, 0.6f},
	                                                 {0.03f, 0.132f, 0.6f}});

	for (uint32_t view = 0; view < 4; view++) {
		struct comp_layer_view_camera cam = {};
		REQUIRE(comp_layer_view_camera_select_eyes(&accum, view, &eyes, 4, nullptr, w, h, &cam));
		const struct xrt_vec3 want = {eyes.eyes[view].x, eyes.eyes[view].y, eyes.eyes[view].z};
		check_same_camera(cam, located_view(want, w, h, origin));
	}

	// Surplus views (a mode wider than the DP's reported set) reuse the LAST
	// eye — the same rule the state tracker's #615 coherence guard applies.
	struct comp_layer_view_camera surplus = {};
	REQUIRE(comp_layer_view_camera_select_eyes(&accum, 6, &eyes, 8, nullptr, w, h, &surplus));
	check_same_camera(surplus, located_view({0.03f, 0.132f, 0.6f}, w, h, origin));
}

TEST_CASE("comp_layer_view_camera: (b) a mono frame composes through the eye CENTROID")
{
	/*
	 * A DP that keeps reporting two eyes while the active mode has ONE view
	 * (the Windows 2D path). xrLocateViews collapses to the centroid for
	 * active_view_count == 1 (oxr_session.c) and so does the IPC server
	 * (ipc_server_handler.c, #521/#575); pairing that centred pose with eye
	 * 0's off-axis frustum is the 2D lateral shift of modelviewer#100.
	 */
	struct comp_layer_accum accum = {};
	push_quad(accum, {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f}});

	const float w = 0.344f;
	const float h = 0.194f;
	const struct xrt_vec3 origin = {0.0f, 0.0f, 0.0f};
	const struct xrt_eye_positions eyes = make_eyes({{-0.032f, 0.1f, 0.6f}, {0.032f, 0.1f, 0.6f}});

	struct comp_layer_view_camera mono = {};
	REQUIRE(comp_layer_view_camera_select_eyes(&accum, 0, &eyes, 1, nullptr, w, h, &mono));
	check_same_camera(mono, located_view({0.0f, 0.1f, 0.6f}, w, h, origin));
	// Centred eye => a horizontally symmetric frustum.
	CHECK(mono.fov.angle_left == Catch::Approx(-mono.fov.angle_right).margin(kEps));

	// The SAME eye set in a 2-view mode must NOT collapse.
	struct comp_layer_view_camera stereo = {};
	REQUIRE(comp_layer_view_camera_select_eyes(&accum, 0, &eyes, 2, nullptr, w, h, &stereo));
	check_same_camera(stereo, located_view({-0.032f, 0.1f, 0.6f}, w, h, origin));
}

TEST_CASE("comp_layer_view_camera: the eye-set entry point degrades like the single-eye one")
{
	struct comp_layer_accum accum = {};
	push_quad(accum, {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f}});

	// No eyes at all, and an empty set, are both branch (c).
	struct comp_layer_view_camera cam = {};
	CHECK_FALSE(comp_layer_view_camera_select_eyes(&accum, 0, nullptr, 2, nullptr, 0.344f, 0.194f, &cam));
	CHECK(cam.source == COMP_LAYER_VIEW_CAMERA_FALLBACK);

	const struct xrt_eye_positions empty = {};
	struct comp_layer_view_camera cam2 = {};
	CHECK_FALSE(comp_layer_view_camera_select_eyes(&accum, 0, &empty, 2, nullptr, 0.344f, 0.194f, &cam2));
	CHECK(cam2.source == COMP_LAYER_VIEW_CAMERA_FALLBACK);

	// A one-eye set with no active count is exactly the single-eye entry
	// point, so the two APIs cannot drift apart.
	const struct xrt_eye_positions one = make_eyes({{0.0f, 0.1f, 0.6f}});
	struct xrt_vec3 eye = {0.0f, 0.1f, 0.6f};
	struct comp_layer_view_camera via_set = {};
	struct comp_layer_view_camera via_single = {};
	REQUIRE(comp_layer_view_camera_select_eyes(&accum, 0, &one, 0, nullptr, 0.344f, 0.194f, &via_set));
	REQUIRE(comp_layer_view_camera_select(&accum, 0, &eye, 0.344f, 0.194f, &via_single));
	check_same_camera(via_set, via_single);

	// The canvas centre still rebases the FRUSTUM only, pose untouched.
	const struct xrt_vec3 canvas_center = {0.10f, 0.0f, 0.0f};
	struct comp_layer_view_camera offset = {};
	REQUIRE(comp_layer_view_camera_select_eyes(&accum, 0, &one, 0, &canvas_center, 0.344f, 0.194f, &offset));
	check_same_camera(offset, located_view(eye, 0.344f, 0.194f, canvas_center));
	CHECK(offset.pose.position.x == Catch::Approx(eye.x).margin(kEps));
}

TEST_CASE("comp_layer_view_camera: (b) is lifted into the ROOT layer frame by head_pose")
{
	/*
	 * #1594 follow-up. Since #1607 EVERY layer pose handle_space() emits is
	 * in the head device's tracking-origin ("root") frame — VIEW-space quads
	 * included, which is the bit that changed. Branch (b) starts from the DP
	 * eye, which is HEAD-relative, so it must compose T_root_head or it
	 * reproduces the #1594 defect with the opposite sign: the VIEW quad would
	 * register and the LOCAL quad would miss by the head pose.
	 */
	const float w = 0.344f;
	const float h = 0.194f;
	const struct xrt_vec3 eye = {-0.032f, 0.0f, 0.600f}; // DP eye, HEAD-relative
	const struct xrt_pose head = make_head_pose();

	struct comp_layer_accum accum = {};
	push_quad(accum, {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f}});
	set_head_pose(accum, head);

	struct comp_layer_view_camera cam = {};
	REQUIRE(comp_layer_view_camera_select(&accum, 0, &eye, w, h, &cam));

	CHECK(cam.source == COMP_LAYER_VIEW_CAMERA_FROM_DISPLAY3D);
	CHECK_FALSE(cam.head_relative_fallback);

	// The pose is head_pose o eye — position AND orientation.
	const struct xrt_pose want = lift_to_root(head, eye);
	CHECK(cam.pose.position.x == Catch::Approx(want.position.x).margin(kEps));
	CHECK(cam.pose.position.y == Catch::Approx(want.position.y).margin(kEps));
	CHECK(cam.pose.position.z == Catch::Approx(want.position.z).margin(kEps));
	CHECK(cam.pose.orientation.x == Catch::Approx(head.orientation.x).margin(kEps));
	CHECK(cam.pose.orientation.y == Catch::Approx(head.orientation.y).margin(kEps));
	CHECK(cam.pose.orientation.z == Catch::Approx(head.orientation.z).margin(kEps));
	CHECK(cam.pose.orientation.w == Catch::Approx(head.orientation.w).margin(kEps));

	// A non-trivial head pose actually moved it: a missing lift cannot pass.
	CHECK(std::abs(cam.pose.position.z - eye.z) > 0.3f);
	CHECK(std::abs(cam.pose.orientation.w - 1.0f) > 0.01f);

	// The FOV is untouched by the lift — it is a function of the eye
	// relative to the CANVAS, both of which stay head-relative.
	const struct comp_layer_view_camera head_relative = located_view(eye, w, h, {0, 0, 0});
	CHECK(cam.fov.angle_left == Catch::Approx(head_relative.fov.angle_left).margin(kEps));
	CHECK(cam.fov.angle_right == Catch::Approx(head_relative.fov.angle_right).margin(kEps));
	CHECK(cam.fov.angle_up == Catch::Approx(head_relative.fov.angle_up).margin(kEps));
	CHECK(cam.fov.angle_down == Catch::Approx(head_relative.fov.angle_down).margin(kEps));
}

TEST_CASE("comp_layer_view_camera: (b) without head_pose falls back head-relative, and SAYS so")
{
	const float w = 0.344f;
	const float h = 0.194f;
	const struct xrt_vec3 eye = {-0.032f, 0.0f, 0.600f};

	struct comp_layer_accum accum = {};
	push_quad(accum, {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f}});
	// head_pose_valid stays false: the state tracker could not locate the head.

	struct comp_layer_view_camera cam = {};
	REQUIRE(comp_layer_view_camera_select(&accum, 0, &eye, w, h, &cam));

	CHECK(cam.source == COMP_LAYER_VIEW_CAMERA_FROM_DISPLAY3D);
	// Never silent: the flag is the in-band half of the report (the other
	// half is the once-per-process U_LOG_W the resolver emits).
	CHECK(cam.head_relative_fallback);
	check_same_camera(cam, located_view(eye, w, h, {0, 0, 0}));

	// And a populated-but-invalid head pose is ignored, not half-applied.
	accum.data.head_pose = make_head_pose();
	accum.data.head_pose_valid = false;
	struct comp_layer_view_camera again = {};
	REQUIRE(comp_layer_view_camera_select(&accum, 0, &eye, w, h, &again));
	CHECK(again.head_relative_fallback);
	check_same_camera(again, cam);
}

TEST_CASE("comp_layer_view_camera: the eye set reaches the same ROOT camera as the single eye")
{
	const float w = 0.344f;
	const float h = 0.194f;
	const struct xrt_pose head = make_head_pose();

	struct comp_layer_accum accum = {};
	push_quad(accum, {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f}});
	set_head_pose(accum, head);

	const struct xrt_eye_positions eyes = make_eyes({{-0.032f, 0.0f, 0.600f}, {0.032f, 0.0f, 0.600f}});

	for (uint32_t view = 0; view < 2; view++) {
		struct comp_layer_view_camera cam = {};
		REQUIRE(comp_layer_view_camera_select_eyes(&accum, view, &eyes, 2, nullptr, w, h, &cam));
		CHECK(cam.source == COMP_LAYER_VIEW_CAMERA_FROM_DISPLAY3D);
		CHECK_FALSE(cam.head_relative_fallback);

		const struct xrt_vec3 e = {eyes.eyes[view].x, eyes.eyes[view].y, eyes.eyes[view].z};
		const struct xrt_pose want = lift_to_root(head, e);
		CHECK(cam.pose.position.x == Catch::Approx(want.position.x).margin(kEps));
		CHECK(cam.pose.position.y == Catch::Approx(want.position.y).margin(kEps));
		CHECK(cam.pose.position.z == Catch::Approx(want.position.z).margin(kEps));
	}

	// The two views are still a real baseline apart after the lift — the
	// head rotation must not have collapsed them onto each other.
	struct comp_layer_view_camera l = {};
	struct comp_layer_view_camera r = {};
	REQUIRE(comp_layer_view_camera_select_eyes(&accum, 0, &eyes, 2, nullptr, w, h, &l));
	REQUIRE(comp_layer_view_camera_select_eyes(&accum, 1, &eyes, 2, nullptr, w, h, &r));
	const float dx = r.pose.position.x - l.pose.position.x;
	const float dz = r.pose.position.z - l.pose.position.z;
	CHECK(std::sqrt(dx * dx + dz * dz) == Catch::Approx(0.064f).margin(1e-4f));
}

TEST_CASE("comp_layer_view_camera: (b') and (a) are already root-framed, head_pose must not move them")
{
	/*
	 * Legs (a) and (b') come from the state tracker, which resolved them
	 * through handle_space() / oxr_session_frame_view_cameras() — already in
	 * the layer frame. Composing head_pose onto them again would double it.
	 */
	const float w = 0.344f;
	const float h = 0.194f;
	const struct xrt_pose head = make_head_pose();
	const struct xrt_eye_positions eyes = make_eyes({{-0.032f, 0.0f, 0.600f}, {0.032f, 0.0f, 0.600f}});

	struct xrt_frame_view_camera frame_cam = {};
	frame_cam.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
	frame_cam.pose.position = {0.011f, 1.722f, 0.567f}; // a ROOT-frame camera
	frame_cam.fov = {-0.5227f, 0.5227f, 0.2683f, -0.3587f};

	SECTION("(b') verbatim")
	{
		struct comp_layer_accum accum = {};
		push_quad(accum, {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f}});
		set_frame_cameras(accum, {frame_cam, frame_cam});
		set_head_pose(accum, head);

		struct comp_layer_view_camera cam = {};
		REQUIRE(comp_layer_view_camera_select_eyes(&accum, 0, &eyes, 2, nullptr, w, h, &cam));
		CHECK(cam.source == COMP_LAYER_VIEW_CAMERA_FROM_FRAME);
		CHECK_FALSE(cam.head_relative_fallback);

		struct comp_layer_view_camera want = {};
		want.pose = frame_cam.pose;
		want.fov = frame_cam.fov;
		check_same_camera(cam, want);
	}

	SECTION("(a) verbatim")
	{
		struct xrt_pose proj_pose = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.011f, 1.742f, 0.878f}};
		struct xrt_fov proj_fov = {-0.4f, 0.41f, 0.3f, -0.31f};
		struct xrt_pose poses[2] = {proj_pose, proj_pose};
		struct xrt_fov fovs[2] = {proj_fov, proj_fov};

		struct comp_layer_accum accum = {};
		push_projection(accum, 2, poses, fovs);
		push_quad(accum, {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f}});
		set_head_pose(accum, head);

		struct comp_layer_view_camera cam = {};
		REQUIRE(comp_layer_view_camera_select_eyes(&accum, 0, &eyes, 2, nullptr, w, h, &cam));
		CHECK(cam.source == COMP_LAYER_VIEW_CAMERA_FROM_PROJECTION);
		CHECK_FALSE(cam.head_relative_fallback);

		struct comp_layer_view_camera want = {};
		want.pose = proj_pose;
		want.fov = proj_fov;
		check_same_camera(cam, want);
	}
}

TEST_CASE("comp_layer_view_camera: a FALSE return is a diagnostic — out is still fully usable")
{
	/*
	 * THE FOOTGUN. Every entry point returns false on branch (c) while
	 * still writing a complete, drawable camera into `out`. An adopter who
	 * reads that as a go/no-go and writes
	 *
	 *     if (!comp_layer_view_camera_select(...)) { continue; }
	 *
	 * draws NO quads at all on a fallback frame — which is how the Metal
	 * port (#1584) lost its layers. The signature is not changing (mac's
	 * #1584 and #1608 already call it), so this case exists to make the
	 * contract executable: on a FALSE return `out` must still be a camera a
	 * renderer can build an MVP from.
	 */
	struct comp_layer_accum accum = {};
	push_quad(accum, {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f}});

	// Poison every field first, so "populated" cannot pass by accident.
	struct comp_layer_view_camera cam = {};
	cam.pose.orientation = {9.0f, 9.0f, 9.0f, 9.0f};
	cam.pose.position = {9.0f, 9.0f, 9.0f};
	cam.fov = {9.0f, 9.0f, 9.0f, 9.0f};
	cam.source = COMP_LAYER_VIEW_CAMERA_FROM_PROJECTION;

	// Branch (c): no eye, no canvas, no frame cameras, no projection layer.
	const bool from_real_data = comp_layer_view_camera_select(&accum, 0, nullptr, 0.0f, 0.0f, &cam);
	CHECK_FALSE(from_real_data); // the diagnostic...

	// ...and yet: a complete, usable camera. Nothing left poisoned.
	CHECK(cam.source == COMP_LAYER_VIEW_CAMERA_FALLBACK);
	CHECK(cam.head_relative_fallback);

	// A unit quaternion (a renderer can invert it to build a view matrix).
	const float qlen = std::sqrt(
	    cam.pose.orientation.x * cam.pose.orientation.x + cam.pose.orientation.y * cam.pose.orientation.y +
	    cam.pose.orientation.z * cam.pose.orientation.z + cam.pose.orientation.w * cam.pose.orientation.w);
	CHECK(qlen == Catch::Approx(1.0f).margin(kEps));

	// A finite position, not the poison.
	CHECK(std::isfinite(cam.pose.position.x));
	CHECK(std::isfinite(cam.pose.position.y));
	CHECK(std::isfinite(cam.pose.position.z));
	CHECK(cam.pose.position.x != Catch::Approx(9.0f).margin(kEps));

	// A non-degenerate frustum: left < right, down < up, all finite.
	CHECK(cam.fov.angle_left < cam.fov.angle_right);
	CHECK(cam.fov.angle_down < cam.fov.angle_up);
	CHECK(std::isfinite(cam.fov.angle_left));
	CHECK(std::isfinite(cam.fov.angle_right));
	CHECK(std::isfinite(cam.fov.angle_up));
	CHECK(std::isfinite(cam.fov.angle_down));
	CHECK(std::abs(cam.fov.angle_right - cam.fov.angle_left) > 0.01f);
	CHECK(std::abs(cam.fov.angle_up - cam.fov.angle_down) > 0.01f);

	// Same promise through the eye-set entry point the D3D11 renderers use.
	struct comp_layer_view_camera via_eyes = {};
	via_eyes.fov = {9.0f, 9.0f, 9.0f, 9.0f};
	CHECK_FALSE(comp_layer_view_camera_select_eyes(&accum, 0, nullptr, 2, nullptr, 0.0f, 0.0f, &via_eyes));
	check_same_camera(via_eyes, cam);
	CHECK(via_eyes.source == COMP_LAYER_VIEW_CAMERA_FALLBACK);
}

TEST_CASE("comp_layer_view_camera: a NULL accum just skips branch (a)")
{
	struct xrt_vec3 eye = {0.0f, 0.0f, 0.5f};
	struct comp_layer_view_camera cam = {};
	REQUIRE(comp_layer_view_camera_select(nullptr, 0, &eye, 0.5f, 0.3f, &cam));
	CHECK(cam.source == COMP_LAYER_VIEW_CAMERA_FROM_DISPLAY3D);
	// ...and, with no accum, no head pose either — so it is head-relative and
	// says so rather than silently claiming the layer frame (#1594).
	CHECK(cam.head_relative_fallback);
}

TEST_CASE("is_layer_view_visible_n: N = 1, 2, 3, 4 truth table")
{
	struct xrt_layer_data left = {};
	left.type = XRT_LAYER_QUAD;
	left.quad.visibility = XRT_LAYER_EYE_VISIBILITY_LEFT_BIT;

	struct xrt_layer_data right = left;
	right.quad.visibility = XRT_LAYER_EYE_VISIBILITY_RIGHT_BIT;

	struct xrt_layer_data both = left;
	both.quad.visibility = XRT_LAYER_EYE_VISIBILITY_BOTH;

	struct xrt_layer_data none = left;
	none.quad.visibility = XRT_LAYER_EYE_VISIBILITY_NONE;

	SECTION("N = 1 and N = 2 are the old parity rule, bit for bit")
	{
		for (uint32_t n = 1; n <= 2; n++) {
			for (uint32_t v = 0; v < n; v++) {
				CHECK(is_layer_view_visible_n(&left, v, n) == (v % 2 == 0));
				CHECK(is_layer_view_visible_n(&right, v, n) == (v % 2 == 1));
			}
		}
	}

	SECTION("N = 3: halves overlap, so the centre view is drawn for BOTH")
	{
		// left  -> v < (3 + 1) / 2 == 2
		CHECK(is_layer_view_visible_n(&left, 0, 3));
		CHECK(is_layer_view_visible_n(&left, 1, 3));
		CHECK_FALSE(is_layer_view_visible_n(&left, 2, 3));
		// right -> v >= 3 / 2 == 1
		CHECK_FALSE(is_layer_view_visible_n(&right, 0, 3));
		CHECK(is_layer_view_visible_n(&right, 1, 3));
		CHECK(is_layer_view_visible_n(&right, 2, 3));
	}

	SECTION("N = 4: a clean split, no view left undrawn")
	{
		CHECK(is_layer_view_visible_n(&left, 0, 4));
		CHECK(is_layer_view_visible_n(&left, 1, 4));
		CHECK_FALSE(is_layer_view_visible_n(&left, 2, 4));
		CHECK_FALSE(is_layer_view_visible_n(&left, 3, 4));

		CHECK_FALSE(is_layer_view_visible_n(&right, 0, 4));
		CHECK_FALSE(is_layer_view_visible_n(&right, 1, 4));
		CHECK(is_layer_view_visible_n(&right, 2, 4));
		CHECK(is_layer_view_visible_n(&right, 3, 4));
	}

	SECTION("BOTH / NONE are view-count independent")
	{
		for (uint32_t n = 1; n <= 4; n++) {
			for (uint32_t v = 0; v < n; v++) {
				CHECK(is_layer_view_visible_n(&both, v, n));
				CHECK_FALSE(is_layer_view_visible_n(&none, v, n));
			}
		}
	}

	SECTION("view_count 0 means stereo, and projection-class is always visible")
	{
		CHECK(is_layer_view_visible_n(&left, 0, 0));
		CHECK_FALSE(is_layer_view_visible_n(&left, 1, 0));

		struct xrt_layer_data proj = {};
		proj.type = XRT_LAYER_PROJECTION;
		for (uint32_t v = 0; v < 4; v++) {
			CHECK(is_layer_view_visible_n(&proj, v, 4));
		}
	}
}

TEST_CASE("CTS quads set BLEND_TEXTURE_SOURCE_ALPHA and NOT the unpremultiplied bit")
{
	// Not a renderer test (no D3D device here) — it pins what the CTS quad
	// fixture actually submits, which is what comp_layer_blend_mode() is fed
	// in conformance. That combination is PREMULTIPLIED (#1621): the spec's
	// straight-alpha switch is UNPREMULTIPLIED_ALPHA_BIT, which the CTS quad
	// does not set. (This case read "so straight alpha is the exercised path"
	// until #1621 — the same inversion the Metal and GL ports carried.)
	struct comp_layer_accum accum = {};
	push_quad(accum, {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f}});

	CHECK((accum.layers[0].data.flags & XRT_LAYER_COMPOSITION_BLEND_TEXTURE_SOURCE_ALPHA_BIT) != 0);
	CHECK((accum.layers[0].data.flags & XRT_LAYER_COMPOSITION_UNPREMULTIPLIED_ALPHA_BIT) == 0);
}

/*
 * #1581 follow-up: no backend may GATE its draw on the resolver's return.
 *
 * The "(c) fallback" case above already pins "a false return still yields a
 * usable camera" — but that is the helper's own side of the deal, and the GL
 * compositor shipped `if (!comp_layer_view_camera_select_eyes(...)) continue;`
 * anyway, so a fallback frame silently drew no quads at all. The behaviour
 * that would catch that directly — "a fallback frame still draws its quads" —
 * lives in each backend's render pass and needs a live GL / Metal / D3D device
 * plus swapchains, which this harness has none of. So the pin is structural
 * instead: every call site in the tree must DISCARD the result.
 *
 * A gated call is recognised by anything other than whitespace or a `(void)`
 * cast sitting between the start of the line and the call.
 */
static std::string
read_whole_file(const std::string &path)
{
	std::ifstream f(path);
	REQUIRE(f.good());
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

TEST_CASE("comp_layer_view_camera: no backend gates its draw on the return value (#1581)")
{
	// Every native compositor that composes non-projection layers through the
	// shared resolver, relative to the compositor source root.
	const char *const backends[] = {
	    "gl/comp_gl_compositor.cpp",
	    "metal/comp_metal_compositor.m",
	    "d3d11/comp_d3d11_renderer.cpp",
	    "d3d11_service/comp_d3d11_service.cpp",
	    // #1581: D3D12 resolves the same cameras for its quad draw.
	    "d3d12/comp_d3d12_renderer.cpp",
	};

	for (const char *rel : backends) {
		const std::string path = std::string(DXR_COMP_SRC_DIR) + "/" + rel;
		const std::string src = read_whole_file(path);

		size_t pos = 0;
		uint32_t calls = 0;
		while ((pos = src.find("comp_layer_view_camera_select", pos)) != std::string::npos) {
			const size_t nl = src.rfind('\n', pos);
			const size_t bol = nl == std::string::npos ? 0 : nl + 1;
			const std::string prefix = src.substr(bol, pos - bol);
			pos += 1;

			// Trim the indentation; a comment mention is not a call.
			const size_t first = prefix.find_first_not_of(" \t");
			const std::string lead = first == std::string::npos ? "" : prefix.substr(first);
			if (lead.rfind("*", 0) == 0 || lead.rfind("//", 0) == 0) {
				continue;
			}
			calls++;

			INFO(rel << ": \"" << lead << "\" precedes the call — the return value is a "
			         << "DIAGNOSTIC (see the @warning in comp_layer_view_camera.h); gating "
			         << "the draw on it drops every layer on a fallback frame");
			CHECK((lead.empty() || lead == "(void)"));
		}
		INFO(rel << " has no resolver call at all — did the file move?");
		CHECK(calls > 0);
	}
}

/*
 * #1621: the three-way blend rule is the SHARED helper's, in every backend.
 *
 * Metal and GL each shipped their own two-way quad mapping —
 * `flags & BLEND_TEXTURE_SOURCE_ALPHA_BIT ? straight : premultiplied` — which
 * is the INVERSE of the rule for the SOURCE_ALPHA-alone case, and has no
 * OPAQUE_COVER at all. The error came from the design brief those two ports
 * were written from, so both implemented it faithfully and each measured
 * self-consistently; only comparing backends on one layer exposed it.
 *
 * The truth table above pins the helper's own contract, which those backends
 * never called. What is worth pinning here is therefore STRUCTURAL: that the
 * backends CALL it rather than reimplementing it. A live composite would need
 * a D3D / Metal / GL device plus swapchains, which this harness has none of —
 * same reasoning as the #1615 gating test above, and the same file read.
 *
 * The negative half matters as much as the positive: a backend that still
 * names BLEND_TEXTURE_SOURCE_ALPHA_BIT in CODE is deciding the blend locally
 * again. Comment mentions are fine (and expected — that is where the rule is
 * explained); only code counts, and the allowance per backend is a number,
 * not a wildcard, so a NEW local test trips this even where one legal one
 * already exists.
 *
 * D3D11's one allowance is Local2D / window-space (XR_DXR, not Khronos): a
 * runtime-owned 2D channel whose submitters treat `layerFlags == 0` as
 * "premultiplied bytes" and expect a blend, so converging it on the shared
 * rule would turn every such panel into an opaque rectangle. #1599 stopped at
 * the Khronos layer types on purpose; see the call site.
 */
TEST_CASE("comp_layer_blend_mode: no backend reimplements the blend rule (#1621)")
{
	// Every native compositor that composites layers by their blend flags,
	// relative to the compositor source root, with the number of legal
	// CODE reads of the raw source-alpha bit left in it.
	const struct
	{
		const char *rel;
		uint32_t allowed_flag_reads;
	} backends[] = {
	    {"gl/comp_gl_compositor.cpp", 0},
	    {"metal/comp_metal_compositor.m", 0},
	    // The Local2D / window-space channel, above.
	    {"d3d11/comp_d3d11_renderer.cpp", 1},
	    /*
	     * The service has TWO legal reads, neither of them the blend rule:
	     *
	     *  - the same Local2D / window-space channel, which on this path is
	     *    composited in multi_compositor_render() rather than in the
	     *    per-client pass;
	     *  - the zones snapshot, which SYNTHESISES a flag value
	     *    (`projection_flags_snapshot = ...SOURCE_ALPHA_BIT`) to feed the
	     *    workspace tile blit, because a pure zones frame has no
	     *    projection layer to take flags from. Constructing an INPUT to
	     *    comp_layer_blend_mode() is not re-deriving its output.
	     */
	    {"d3d11_service/comp_d3d11_service.cpp", 2},
	    // #1581: D3D12's one allowance is the SAME Local2D / window-space
	    // channel D3D11's is — render_window_space_layer(), ported from it.
	    {"d3d12/comp_d3d12_renderer.cpp", 1},
	};

	for (const auto &backend : backends) {
		const char *const rel = backend.rel;
		const std::string path = std::string(DXR_COMP_SRC_DIR) + "/" + rel;
		const std::string src = read_whole_file(path);

		// Positive: the shared resolver is what decides the mode. All three
		// spellings count — comp_layer_tile_blend_mode() for the tile's
		// painter's-order gate, comp_layer_subrect_blend_mode() for a layer
		// that covers only part of a tile, and comp_layer_blend_mode() where
		// a caller names the rule directly. They are one rule; which name a
		// backend reaches for is not what this guard is about.
		INFO(rel << " must resolve its blend mode through comp_layer_blend_mode() / "
		         << "comp_layer_tile_blend_mode() / comp_layer_subrect_blend_mode() "
		         << "(comp_layer_view_camera.h)");
		CHECK((src.find("comp_layer_blend_mode(") != std::string::npos ||
		       src.find("comp_layer_tile_blend_mode(") != std::string::npos ||
		       src.find("comp_layer_subrect_blend_mode(") != std::string::npos));

		// ...and OPAQUE_COVER's alpha-of-one must be folded into the shader,
		// since no fixed-function blend factor can synthesise a constant one.
		INFO(rel << " resolves the mode but never folds OPAQUE_COVER's alpha-of-one — "
		         << "the blend state alone cannot produce it");
		CHECK(src.find("comp_layer_blend_fold_opaque_cover(") != std::string::npos);

		// Negative: no local re-derivation from the raw flag, in code.
		uint32_t flag_reads = 0;
		size_t pos = 0;
		while ((pos = src.find("XRT_LAYER_COMPOSITION_BLEND_TEXTURE_SOURCE_ALPHA_BIT", pos)) !=
		       std::string::npos) {
			const size_t nl = src.rfind('\n', pos);
			const size_t bol = nl == std::string::npos ? 0 : nl + 1;
			const std::string prefix = src.substr(bol, pos - bol);
			pos += 1;

			// Trim the indentation; a comment mention is not a read.
			const size_t first = prefix.find_first_not_of(" \t");
			const std::string lead = first == std::string::npos ? "" : prefix.substr(first);
			if (lead.rfind("*", 0) == 0 || lead.rfind("//", 0) == 0) {
				continue;
			}
			flag_reads++;
		}

		INFO(rel << " reads BLEND_TEXTURE_SOURCE_ALPHA_BIT in CODE " << flag_reads << " time(s), expected "
		         << backend.allowed_flag_reads
		         << ". The three-way rule is comp_layer_blend_mode()'s, and a local two-way test "
		         << "of this bit is how Metal and GL came to disagree with D3D11 (#1621). "
		         << "Explain it in a comment; decide it in the helper.");
		CHECK(flag_reads == backend.allowed_flag_reads);
	}
}

TEST_CASE("comp_layer_blend_mode: the OpenXR 10.6.2 truth table, all four flag combos")
{
	constexpr uint32_t kSrcAlpha = XRT_LAYER_COMPOSITION_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
	constexpr uint32_t kUnpremul = XRT_LAYER_COMPOSITION_UNPREMULTIPLIED_ALPHA_BIT;

	// No SOURCE_ALPHA -> the layer alpha is one, i.e. an opaque cover. This
	// is the case the D3D11 in-process renderer had INVERTED (#1599): it
	// read `(flags & SOURCE_ALPHA) == 0` as "premultiplied" and blended
	// exactly the layers that must not blend.
	//
	// OPAQUE_COVER, never REPLACE: the flags cannot ask for the base blit's
	// verbatim alpha. "Alpha is one" is a statement about the COMPOSITION
	// result, so such a layer must raise dst.a where it draws; REPLACE
	// would stamp the texture's own (possibly zero) alpha into the atlas
	// and punch a hole in a transparent-background session.
	CHECK(comp_layer_blend_mode(0) == COMP_LAYER_BLEND_OPAQUE_COVER);

	// ... and UNPREMULTIPLIED on its own means nothing without it. That
	// combination (unpremultiplied alpha, no source-alpha bit, alpha = 0
	// texture) is precisely what the CTS SourceAlphaBlending case submits,
	// and the old two-way rule turned it into a blend.
	CHECK(comp_layer_blend_mode(kUnpremul) == COMP_LAYER_BLEND_OPAQUE_COVER);

	CHECK(comp_layer_blend_mode(kSrcAlpha) == COMP_LAYER_BLEND_PREMULTIPLIED);
	CHECK(comp_layer_blend_mode(kSrcAlpha | kUnpremul) == COMP_LAYER_BLEND_STRAIGHT);

	// Unrelated bits never move the answer.
	CHECK(comp_layer_blend_mode(kSrcAlpha | XRT_LAYER_COMPOSITION_VIEW_SPACE_BIT) ==
	      COMP_LAYER_BLEND_PREMULTIPLIED);
	CHECK(comp_layer_blend_mode(XRT_LAYER_COMPOSITION_CORRECT_CHROMATIC_ABERRATION_BIT) ==
	      COMP_LAYER_BLEND_OPAQUE_COVER);

	// No flag combination reaches the base-blit mode.
	for (uint32_t flags : {0u, kSrcAlpha, kUnpremul, kSrcAlpha | kUnpremul}) {
		CHECK(comp_layer_blend_mode(flags) != COMP_LAYER_BLEND_REPLACE);
	}
}

TEST_CASE("comp_layer_blend_fold_opaque_cover: the alpha-of-one the shader emits")
{
	// The D3D11 blend state for OPAQUE_COVER is blending OFF, which writes
	// src.a — so the one has to come out of the pixel shader. Every layer
	// shader ends in `color * color_scale + color_bias`; this is that fold.
	SECTION("OPAQUE_COVER zeroes the alpha scale and biases it to one")
	{
		float scale[4] = {1.0f, 1.0f, 1.0f, 1.0f};
		float bias[4] = {0.0f, 0.0f, 0.0f, 0.0f};
		comp_layer_blend_fold_opaque_cover(COMP_LAYER_BLEND_OPAQUE_COVER, scale, bias);

		CHECK(scale[3] == 0.0f);
		CHECK(bias[3] == 1.0f);

		// out.a = src.a * 0 + 1 == 1 for ANY source alpha, which is the
		// property no fixed-function blend factor can provide.
		for (float src_a : {0.0f, 0.25f, 1.0f}) {
			CHECK(src_a * scale[3] + bias[3] == 1.0f);
		}

		// Colour is untouched: this mode covers, it does not recolour.
		CHECK(scale[0] == 1.0f);
		CHECK(scale[1] == 1.0f);
		CHECK(scale[2] == 1.0f);
		CHECK(bias[0] == 0.0f);
		CHECK(bias[1] == 0.0f);
		CHECK(bias[2] == 0.0f);
	}

	SECTION("it overrides an app's own alpha scale/bias, which is the spec order")
	{
		// XR_KHR_composition_layer_color_scale_bias applies to the
		// SOURCE; the layer alpha is then treated as one at composition
		// time. So the app's alpha scale cannot survive here.
		float scale[4] = {0.5f, 0.5f, 0.5f, 0.5f};
		float bias[4] = {0.1f, 0.1f, 0.1f, 0.25f};
		comp_layer_blend_fold_opaque_cover(COMP_LAYER_BLEND_OPAQUE_COVER, scale, bias);

		CHECK(scale[3] == 0.0f);
		CHECK(bias[3] == 1.0f);
		// ... while the colour scale/bias it asked for still applies.
		CHECK(scale[0] == 0.5f);
		CHECK(bias[0] == 0.1f);
	}

	SECTION("every other mode is left alone — REPLACE especially (#225)")
	{
		for (enum comp_layer_blend_mode mode :
		     {COMP_LAYER_BLEND_REPLACE, COMP_LAYER_BLEND_PREMULTIPLIED, COMP_LAYER_BLEND_STRAIGHT}) {
			float scale[4] = {1.0f, 1.0f, 1.0f, 1.0f};
			float bias[4] = {0.0f, 0.0f, 0.0f, 0.0f};
			comp_layer_blend_fold_opaque_cover(mode, scale, bias);

			// The base blit's alpha reaches the atlas verbatim.
			CHECK(scale[3] == 1.0f);
			CHECK(bias[3] == 0.0f);
		}
	}

	SECTION("a null buffer is a no-op, not a crash")
	{
		float scale[4] = {1.0f, 1.0f, 1.0f, 1.0f};
		float bias[4] = {0.0f, 0.0f, 0.0f, 0.0f};
		comp_layer_blend_fold_opaque_cover(COMP_LAYER_BLEND_OPAQUE_COVER, nullptr, bias);
		comp_layer_blend_fold_opaque_cover(COMP_LAYER_BLEND_OPAQUE_COVER, scale, nullptr);
		CHECK(scale[3] == 1.0f);
		CHECK(bias[3] == 0.0f);
	}
}

TEST_CASE("comp_layer_quad_is_front_facing: +Z is the front face (#1590)")
{
	// The camera at the origin, the quad a metre down -Z with an identity
	// rotation: its normal is +Z, pointing back at the camera. This is the
	// ordinary case, and the -Z formula the issue first proposed would have
	// dropped it.
	const struct xrt_vec3 camera = {0.0f, 0.0f, 0.0f};
	const struct xrt_pose facing_camera = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f}};
	CHECK(comp_layer_quad_is_front_facing(&facing_camera, &camera));

	// Rotated 180 degrees about Y: normal is now -Z, i.e. turned away.
	// "the back face is not visible and must not be drawn by the runtime"
	// — this is the CTS QuadOcclusion red quad.
	const struct xrt_pose turned_away = {{0.0f, 1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}};
	CHECK_FALSE(comp_layer_quad_is_front_facing(&turned_away, &camera));

	// Edge-on: the camera sits IN the quad's plane, so the dot product is
	// exactly zero. A zero-area sliver has no defined facing -> not drawn.
	const struct xrt_pose at_origin = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
	const struct xrt_vec3 in_plane = {1.0f, 0.0f, 0.0f};
	CHECK_FALSE(comp_layer_quad_is_front_facing(&at_origin, &in_plane));

	// The camera BEHIND a front-facing quad sees its back.
	const struct xrt_vec3 behind = {0.0f, 0.0f, -2.0f};
	CHECK_FALSE(comp_layer_quad_is_front_facing(&facing_camera, &behind));
	// ... and the turned-away quad is visible from there.
	CHECK(comp_layer_quad_is_front_facing(&turned_away, &behind));

	// Facing follows the ORIENTATION, not the position: a quad off to the
	// side but still square-on to the camera plane is front-facing.
	const struct xrt_pose off_axis = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.5f, 0.3f, -1.0f}};
	CHECK(comp_layer_quad_is_front_facing(&off_axis, &camera));

	// A missing input draws rather than drops.
	CHECK(comp_layer_quad_is_front_facing(nullptr, &camera));
	CHECK(comp_layer_quad_is_front_facing(&facing_camera, nullptr));
}

TEST_CASE("comp_layer_tile_blend_mode: the first layer into a tile REPLACES, whatever its flags")
{
	constexpr uint32_t kSrcAlpha = XRT_LAYER_COMPOSITION_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
	constexpr uint32_t kUnpremul = XRT_LAYER_COMPOSITION_UNPREMULTIPLIED_ALPHA_BIT;

	SECTION("a single-projection-layer frame is today's path, byte for byte")
	{
		/*
		 * Every shipping app submits ONE projection layer, and a
		 * transparent-background one sets SOURCE_ALPHA on it
		 * (cube_handle_gl_win, cube_handle_vk_win, every cube_zones_*).
		 * The gate must NOT look at that bit: blending that blit over
		 * the clear would drive dst.a to 1 everywhere and kill the
		 * display processor's alpha gate / compose-under (#225).
		 */
		struct comp_layer_tile_state tile = {};
		CHECK(comp_layer_is_first_in_tile(&tile));
		CHECK(comp_layer_tile_blend_mode(&tile, kSrcAlpha) == COMP_LAYER_BLEND_REPLACE);
		CHECK_FALSE(comp_layer_is_first_in_tile(&tile));
	}

	SECTION("first wins over every flag combination")
	{
		for (uint32_t flags : {0u, kSrcAlpha, kUnpremul, kSrcAlpha | kUnpremul}) {
			struct comp_layer_tile_state tile = {};
			CHECK(comp_layer_tile_blend_mode(&tile, flags) == COMP_LAYER_BLEND_REPLACE);
		}
	}

	SECTION("later layers take their flags — this is the painter's algorithm")
	{
		struct comp_layer_tile_state tile = {};
		// Layer 0: the base blit.
		CHECK(comp_layer_tile_blend_mode(&tile, 0) == COMP_LAYER_BLEND_REPLACE);
		// Layer 1: a blended overlay.
		CHECK(comp_layer_tile_blend_mode(&tile, kSrcAlpha) == COMP_LAYER_BLEND_PREMULTIPLIED);
		// Layer 2: straight alpha.
		CHECK(comp_layer_tile_blend_mode(&tile, kSrcAlpha | kUnpremul) == COMP_LAYER_BLEND_STRAIGHT);
		// Layer 3: no blend bit — legitimately covers what is under it
		// (§10.6.2: alpha initialised to one). NOT privileged as a base
		// layer, just opaque: an OPAQUE_COVER, which raises dst.a to
		// one, and NOT a second REPLACE, which would write the
		// texture's own alpha over content that is already there.
		CHECK(comp_layer_tile_blend_mode(&tile, 0) == COMP_LAYER_BLEND_OPAQUE_COVER);
	}

	SECTION("the same flags mean different things first and later")
	{
		// The defect this split fixes: an unflagged overlay quad used to
		// take the base blit's verbatim-alpha mode, so a texture with
		// alpha 0 punched a hole through the projection layer under it
		// and disappeared at the DP's alpha gate.
		for (uint32_t flags : {0u, kUnpremul}) {
			struct comp_layer_tile_state tile = {};
			CHECK(comp_layer_tile_blend_mode(&tile, flags) == COMP_LAYER_BLEND_REPLACE);
			CHECK(comp_layer_tile_blend_mode(&tile, flags) == COMP_LAYER_BLEND_OPAQUE_COVER);
		}
	}

	SECTION("tiles are independent: view 1 starts fresh")
	{
		struct comp_layer_tile_state tiles[2] = {};
		CHECK(comp_layer_tile_blend_mode(&tiles[0], kSrcAlpha) == COMP_LAYER_BLEND_REPLACE);
		CHECK(comp_layer_tile_blend_mode(&tiles[0], kSrcAlpha) == COMP_LAYER_BLEND_PREMULTIPLIED);
		// The second eye's tile has had nothing painted into it yet.
		CHECK(comp_layer_is_first_in_tile(&tiles[1]));
		CHECK(comp_layer_tile_blend_mode(&tiles[1], kSrcAlpha) == COMP_LAYER_BLEND_REPLACE);
	}

	SECTION("a NULL tile is 'treat as first' — a caller that tracks nothing never blends")
	{
		CHECK(comp_layer_is_first_in_tile(nullptr));
		CHECK(comp_layer_tile_blend_mode(nullptr, kSrcAlpha | kUnpremul) == COMP_LAYER_BLEND_REPLACE);
	}

	SECTION("marking is idempotent")
	{
		struct comp_layer_tile_state tile = {};
		comp_layer_tile_mark_composited(&tile);
		comp_layer_tile_mark_composited(&tile);
		CHECK_FALSE(comp_layer_is_first_in_tile(&tile));
		comp_layer_tile_mark_composited(nullptr); // must not crash
	}
}

/*
 * #1590 x #1598 — the HELPER CONTRACT that makes "apply your skips first" a
 * rule worth stating.
 *
 * Nothing here runs a backend: the loop below is re-implemented in the test, so
 * this case cannot and does not pin any backend's ORDERING — the structural
 * case further down does that, by naming. What it pins is the property the
 * ordering rule rests on. comp_layer_tile_blend_mode() resolves AND marks in
 * ONE call, so merely ASKING it about a layer that then turns out to be skipped
 * (per-eye visibility, a quad turned away) permanently spends the tile's base
 * slot and the layer behind it blends over a clear instead of replacing it. A
 * helper that only resolved, or only marked, would make the rule unnecessary;
 * these are the assertions showing it is not.
 *
 * The third section pins the other half: which layers may ask at all.
 */
TEST_CASE("comp_layer_tile_blend_mode: asking for a mode is what spends the base slot (#1590 x #1598)")
{
	const struct xrt_vec3 camera = {0.0f, 0.0f, 0.0f};
	const struct xrt_pose facing = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f}};
	const struct xrt_pose turned_away = {{0.0f, 1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}};

	SECTION("a back-facing quad does not consume the base slot")
	{
		struct comp_layer_tile_state tile = {};

		// Layer 0 is turned away: skipped, and the tile is untouched.
		if (comp_layer_quad_is_front_facing(&turned_away, &camera)) {
			(void)comp_layer_tile_blend_mode(&tile, 0);
		}
		CHECK(comp_layer_is_first_in_tile(&tile));

		// Layer 1 faces the camera, so IT is the first one in.
		REQUIRE(comp_layer_quad_is_front_facing(&facing, &camera));
		CHECK(comp_layer_tile_blend_mode(&tile, 0) == COMP_LAYER_BLEND_REPLACE);
	}

	SECTION("the same pair of quads, per eye, can disagree")
	{
		// The quad sits at the origin edge-on to eye 0 and square-on to
		// eye 1, so eye 1's tile gets a base layer and eye 0's does not.
		// A single shared tile state would leak one eye's answer into
		// the other; XRT_MAX_VIEWS of them is why the backends carry an
		// array rather than a scalar.
		const struct xrt_pose at_origin = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
		const struct xrt_vec3 eyes[2] = {{1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
		struct comp_layer_tile_state tiles[2] = {};

		for (uint32_t view = 0; view < 2; view++) {
			if (comp_layer_quad_is_front_facing(&at_origin, &eyes[view])) {
				(void)comp_layer_tile_blend_mode(&tiles[view], 0);
			}
		}
		CHECK(comp_layer_is_first_in_tile(&tiles[0]));       // edge-on -> skipped
		CHECK_FALSE(comp_layer_is_first_in_tile(&tiles[1])); // drawn
	}

	SECTION("a SUB-RECT layer never reaches REPLACE, painted tile or not")
	{
		/*
		 * Only a FULL-TILE projection-class layer can be a tile's
		 * base, so a quad / cylinder / equirect2 takes
		 * comp_layer_subrect_blend_mode() and gets its own flags
		 * whether or not anything has painted the tile yet.
		 *
		 * Routing one through the first-in-tile gate instead — which
		 * the D3D11 service's UI pass briefly did — made the first UI
		 * layer of a UI-ONLY frame a REPLACE. Two things are wrong
		 * with that: a layer covering PART of a tile cannot establish
		 * the tile's alpha, and the service's per-client atlas is not
		 * cleared per frame (signature-change only, and never in
		 * workspace mode or on the always-on pipeline), so "first into
		 * the tile" is a fiction there — the REPLACE stamped a
		 * verbatim, possibly-zero texture alpha over LAST frame's live
		 * content and a SOURCE_ALPHA quad stopped blending entirely.
		 */
		constexpr uint32_t kSrcAlpha = XRT_LAYER_COMPOSITION_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		constexpr uint32_t kUnpremul = XRT_LAYER_COMPOSITION_UNPREMULTIPLIED_ALPHA_BIT;

		// An UNPAINTED tile — the case that used to resolve REPLACE.
		struct comp_layer_tile_state fresh = {};
		REQUIRE(comp_layer_is_first_in_tile(&fresh));
		CHECK(comp_layer_subrect_blend_mode(&fresh, 0) == COMP_LAYER_BLEND_OPAQUE_COVER);

		// A PAINTED one gives the same answers, which IS the point:
		// the tile's history is not an input to a sub-rect layer.
		struct comp_layer_tile_state painted = {};
		painted.composited = true;
		CHECK(comp_layer_subrect_blend_mode(&painted, 0) == COMP_LAYER_BLEND_OPAQUE_COVER);

		// No flag combination, on a fresh tile, reaches the base blit;
		// every one of them is exactly comp_layer_blend_mode()'s.
		for (uint32_t flags : {0u, kSrcAlpha, kUnpremul, kSrcAlpha | kUnpremul}) {
			struct comp_layer_tile_state tile = {};
			CHECK(comp_layer_subrect_blend_mode(&tile, flags) != COMP_LAYER_BLEND_REPLACE);

			struct comp_layer_tile_state again = {};
			CHECK(comp_layer_subrect_blend_mode(&again, flags) == comp_layer_blend_mode(flags));
		}

		// It still MARKS, so a FULL-TILE layer composited after it
		// blends over it instead of erasing it.
		struct comp_layer_tile_state marked = {};
		(void)comp_layer_subrect_blend_mode(&marked, kSrcAlpha);
		CHECK_FALSE(comp_layer_is_first_in_tile(&marked));
		CHECK(comp_layer_tile_blend_mode(&marked, 0) == COMP_LAYER_BLEND_OPAQUE_COVER);

		// A NULL tile is legal where nothing downstream reads the mark
		// — the D3D11 service's UI pass is the last to paint the
		// per-client atlas.
		CHECK(comp_layer_subrect_blend_mode(nullptr, kSrcAlpha | kUnpremul) == COMP_LAYER_BLEND_STRAIGHT);
	}
}

/*
 * #1590 / #1598, structurally, for every path that draws a quad.
 *
 * The behaviour needs a live device and swapchains, which this harness has
 * none of — same reasoning as the two structural cases above. What is pinnable
 * is that each path CONSULTS the shared rules rather than owning a private
 * copy: the back-face predicate at its quad draw, and the painter's-order gate
 * at its layer loop.
 *
 * METAL is the one quad-drawing backend still outside this guard: its #1590 leg
 * is open, and a test that fails for a known-open leg is noise, not a guard.
 * D3D12 joined with #1581, which is where it learned to draw a quad at all; GL
 * joined with #1581's GL leg, which is where it learned to CULL one — before
 * that its quad pass drew back-faces, and the CTS QuadOcclusion case showed a
 * full-view red rectangle on that backend.
 */
TEST_CASE("the quad-drawing paths consult the shared facing and painter's rules (#1590, #1598)")
{
	const char *const backends[] = {
	    "d3d11/comp_d3d11_renderer.cpp",
	    "d3d11_service/comp_d3d11_service.cpp",
	    "d3d12/comp_d3d12_renderer.cpp",
	    "gl/comp_gl_compositor.cpp",
	};

	for (const char *rel : backends) {
		const std::string path = std::string(DXR_COMP_SRC_DIR) + "/" + rel;
		const std::string src = read_whole_file(path);

		INFO(rel << " draws quads but never asks comp_layer_quad_is_front_facing() — the spec says "
		         << "the back face MUST NOT be drawn, on every path (#1590)");
		CHECK(src.find("comp_layer_quad_is_front_facing(") != std::string::npos);

		INFO(rel << " never asks comp_layer_tile_blend_mode() — without the first-in-tile gate a "
		         << "layer either erases the one beneath it or blends over a clear (#1598)");
		CHECK(src.find("comp_layer_tile_blend_mode(") != std::string::npos);
	}

	/*
	 * ...and a SUB-RECT layer must not reach for that gate: only a full-tile
	 * projection-class layer can be a tile's base. The D3D11 service composes
	 * its sub-rect layers in a pass of their own; D3D12 and GL compose their
	 * quads in the same loop as their projection blits (#1581) and so owe the
	 * distinction most of all — there the quad is walking past the very gate
	 * it must not take. Named here rather than in the loop above because the
	 * in-process D3D11 renderer spells the same rule as a direct
	 * comp_layer_blend_mode() call at its quad draw, and converging the two
	 * spellings is not this change's business.
	 */
	const char *const subrect_backends[] = {
	    "d3d11_service/comp_d3d11_service.cpp",
	    "d3d12/comp_d3d12_renderer.cpp",
	    "gl/comp_gl_compositor.cpp",
	};

	for (const char *rel : subrect_backends) {
		const std::string path = std::string(DXR_COMP_SRC_DIR) + "/" + rel;
		const std::string src = read_whole_file(path);

		INFO(rel << " must resolve its quad / cylinder / equirect2 draws through "
		         << "comp_layer_subrect_blend_mode() — the first-in-tile REPLACE belongs to a "
		         << "full-tile projection-class base alone");
		CHECK(src.find("comp_layer_subrect_blend_mode(") != std::string::npos);
	}
}

/*
 * #1602, structurally, for the equirect2 draw on every backend that has one:
 * both D3D legs and, since the GL port, OpenGL.
 *
 * The guards above are FILE-scoped: they prove the file consults the shared
 * rules SOMEWHERE. That was enough while a file had one non-projection draw in
 * it; D3D12 and GL now have two each (quad, #1581; equirect2, #1602), and a
 * file-scoped check is satisfied by the quad alone — the exact hole a second
 * port falls through. So these read the equirect2 FUNCTION BODY.
 *
 * Both what it must ask and what it must NOT:
 *
 *  - It must take per-eye visibility from is_layer_view_visible_n() and build
 *    its frustum tangents from a per-view FOV. An equirect2 draw casts rays,
 *    so the FOV is the ONLY place the camera's framing enters — get it from
 *    anywhere but this frame's camera and the sphere lands on different display
 *    pixels than projection content at the same world pose (#1580).
 *  - It must fold OPAQUE_COVER's alpha-of-one, which no fixed-function blend
 *    factor can synthesise.
 *  - It must NOT reach comp_layer_tile_blend_mode(): a sphere section covers
 *    part of the tile, so it can never be the tile's base (#1598).
 *  - It must NOT apply comp_layer_quad_is_front_facing(). #1590 is normative
 *    for QUADS — "only front face of the quad surface is visible" — and a
 *    sphere has no back face. Culling here would drop a viewer standing INSIDE
 *    the sphere, which is the ordinary case for a 360 background and is
 *    literally CTS Equirect2 subtests 1-4.
 *
 * Comment mentions are not code, so the negatives run over a comment-stripped
 * copy: the reasons above are explained AT those call sites, and a guard that
 * failed on its own explanation would be worse than no guard.
 */
static std::string
code_only(const std::string &src)
{
	std::string out;
	size_t bol = 0;
	while (bol <= src.size()) {
		const size_t nl = src.find('\n', bol);
		const size_t eol = nl == std::string::npos ? src.size() : nl;
		const std::string line = src.substr(bol, eol - bol);

		const size_t first = line.find_first_not_of(" \t");
		const std::string lead = first == std::string::npos ? "" : line.substr(first);
		if (lead.rfind("*", 0) != 0 && lead.rfind("//", 0) != 0 && lead.rfind("/*", 0) != 0) {
			out += line;
		}
		out += "\n";

		if (nl == std::string::npos) {
			break;
		}
		bol = nl + 1;
	}
	return out;
}

/*!
 * The body of a top-level function definition, by name. Relies on this tree's
 * style — a definition's name starts a line and its closing brace is the next
 * `}` in column 0 — which every backend source here follows.
 */
static std::string
function_body(const std::string &src, const std::string &name)
{
	const std::string needle = "\n" + name + "(";
	const size_t start = src.find(needle);
	if (start == std::string::npos) {
		return "";
	}
	const size_t end = src.find("\n}", start);
	if (end == std::string::npos) {
		return "";
	}
	return src.substr(start, end - start);
}

TEST_CASE("the equirect2 draw consults the shared rules on every backend that has one (#1602)")
{
	// The backend source and the NAME of its equirect2 draw. GL prefixes its
	// statics, so the function name is per-backend rather than assumed.
	const struct
	{
		const char *rel;
		const char *fn;
	} backends[] = {
	    {"d3d11/comp_d3d11_renderer.cpp", "render_equirect2_layer"},
	    // #1602: D3D12 joined here; before it, the layer was accepted into
	    // the accumulator and silently never drawn.
	    {"d3d12/comp_d3d12_renderer.cpp", "render_equirect2_layer"},
	    // ...and GL joined next, off a GLSL twin of the same shader. It had
	    // the same defect for the same reason: accepted into the accumulator,
	    // never drawn, six blank CTS subtests on `-G opengl`.
	    {"gl/comp_gl_compositor.cpp", "gl_render_equirect2_layer"},
	};

	for (const auto &backend : backends) {
		const char *const rel = backend.rel;
		const std::string path = std::string(DXR_COMP_SRC_DIR) + "/" + rel;
		const std::string src = read_whole_file(path);
		const std::string body = function_body(src, backend.fn);

		INFO(rel << " has no " << backend.fn << "() definition — did it move, or did the "
		         << "equirect2 draw get dropped?");
		REQUIRE_FALSE(body.empty());

		const std::string code = code_only(body);

		INFO(rel << ": the equirect2 draw must take per-eye visibility from "
		         << "is_layer_view_visible_n() — the view-count-aware one (#1580)");
		CHECK(code.find("is_layer_view_visible_n(") != std::string::npos);

		INFO(rel << ": the equirect2 draw must build its frustum tangents from this view's FOV — "
		         << "that is the only place the camera's framing enters a ray-cast draw (#1580)");
		CHECK(code.find("angle_left") != std::string::npos);
		CHECK(code.find("angle_down") != std::string::npos);

		INFO(rel << ": the equirect2 draw must fold OPAQUE_COVER's alpha-of-one into the colour "
		         << "scale/bias — no fixed-function blend factor can produce a constant one");
		CHECK(code.find("comp_layer_blend_fold_opaque_cover(") != std::string::npos);

		INFO(rel << ": the equirect2 draw must NOT reach comp_layer_tile_blend_mode() — a sphere "
		         << "section covers part of the tile, so it can never be the tile's base (#1598)");
		CHECK(code.find("comp_layer_tile_blend_mode(") == std::string::npos);

		INFO(rel << ": the equirect2 draw must NOT apply comp_layer_quad_is_front_facing() — #1590 "
		         << "is normative for quads, and culling a sphere's 'back face' drops the viewer "
		         << "standing inside it, which is CTS Equirect2 1-4");
		CHECK(code.find("comp_layer_quad_is_front_facing(") == std::string::npos);
	}

	/*
	 * ...and the D3D12-specific half. Three things this leg can get wrong
	 * that D3D11 structurally cannot:
	 *
	 *  - the blend mode has to be named through the SUB-RECT spelling here,
	 *    because D3D12 composes its sub-rect layers in the SAME loop as its
	 *    projection blits — there is no separate pass whose every layer is
	 *    sub-rect by construction (the in-process D3D11 leg resolves it at
	 *    the call site instead, which is why this half is D3D12-only);
	 *  - the PSO bakes the render target's format, so the pick must go
	 *    through the helper that knows which target this frame paints
	 *    (#1610). A literal `equirect2_pso[...]` index would draw into the
	 *    `_SRGB` compose target with the UNORM-baked pipeline;
	 *  - the source SRV's format must come from layer_source_format(), the
	 *    one place that decides whether this frame samples format-honest
	 *    (#1589/#1694). A blend in the wrong space is not a crash — it is
	 *    one graphics API a stop brighter than another.
	 */
	const std::string d3d12 = read_whole_file(std::string(DXR_COMP_SRC_DIR) + "/d3d12/comp_d3d12_renderer.cpp");
	const std::string d3d12_body = code_only(function_body(d3d12, "render_equirect2_layer"));
	REQUIRE_FALSE(d3d12_body.empty());

	INFO("the D3D12 equirect2 draw must resolve its blend mode through comp_layer_subrect_blend_mode()");
	CHECK(d3d12_body.find("comp_layer_subrect_blend_mode(") != std::string::npos);

	INFO(
	    "the D3D12 equirect2 draw must pick its PSO through equirect2_pso_for(), the only place that "
	    "knows which render target this frame paints (#1610)");
	CHECK(d3d12_body.find("equirect2_pso_for(") != std::string::npos);

	INFO("the D3D12 equirect2 draw must take its source SRV format from layer_source_format() (#1589/#1694)");
	CHECK(d3d12_body.find("layer_source_format(") != std::string::npos);

	/*
	 * ...and the GL-specific half. Three things this leg can get wrong that
	 * neither D3D leg can:
	 *
	 *  - the same SUB-RECT spelling D3D12 owes, and for the same reason: GL
	 *    composes its sub-rect layers in the same loop as its projection
	 *    blits, so the equirect2 draw walks straight past the first-in-tile
	 *    gate it must not take;
	 *  - the source read must go through gl_bind_layer_source(), the GL twin
	 *    of layer_source_format(). In GL "does this sampler decode?" is
	 *    TEXTURE-OBJECT state, so a plain glBindTexture() here would inherit
	 *    whatever the previous draw left on that texture — a colour bug that
	 *    depends on layer ORDER, which is worse than a consistent one;
	 *  - it must NOT touch GL_FRAMEBUFFER_SRGB. That is PASS state (#1610):
	 *    on for the whole atlas pass when the pass paints the private
	 *    GL_SRGB8_ALPHA8 compose target, off when it paints the atlas
	 *    directly. A draw that toggled it would blend this layer in a
	 *    different space from the projection tile underneath it.
	 */
	const std::string gl = read_whole_file(std::string(DXR_COMP_SRC_DIR) + "/gl/comp_gl_compositor.cpp");
	const std::string gl_body = code_only(function_body(gl, "gl_render_equirect2_layer"));
	REQUIRE_FALSE(gl_body.empty());

	INFO("the GL equirect2 draw must resolve its blend mode through comp_layer_subrect_blend_mode()");
	CHECK(gl_body.find("comp_layer_subrect_blend_mode(") != std::string::npos);

	INFO(
	    "the GL equirect2 draw must bind its source through gl_bind_layer_source() (#1589/#1610) — "
	    "a bare glBindTexture() inherits the previous draw's sRGB-decode parameter");
	CHECK(gl_body.find("gl_bind_layer_source(") != std::string::npos);

	INFO(
	    "the GL equirect2 draw must not touch GL_FRAMEBUFFER_SRGB — the encode is PASS state, and "
	    "a per-draw toggle blends this layer in a different space from the tile under it (#1610)");
	CHECK(gl_body.find("GL_FRAMEBUFFER_SRGB") == std::string::npos);

	/*
	 * A sphere SECTION is a sub-rect contributor, so a frame carrying one owes
	 * a LINEAR blend and must not take the #1589 fast path — which would
	 * compose it in encoded space. Both same-loop backends decide that in one
	 * named predicate, and equirect2 has to be a case in it rather than
	 * falling into `default:`. Read as a function body, because the file
	 * mentions the enum in several other places.
	 */
	const struct
	{
		const char *rel;
		const char *fn;
	} fast_path_owners[] = {
	    {"d3d12/comp_d3d12_renderer.cpp", "renderer_frame_takes_fast_path"},
	    {"gl/comp_gl_compositor.cpp", "gl_frame_takes_fast_path"},
	};
	for (const auto &owner : fast_path_owners) {
		const std::string src = read_whole_file(std::string(DXR_COMP_SRC_DIR) + "/" + owner.rel);
		const std::string body = code_only(function_body(src, owner.fn));
		INFO(owner.rel << " has no " << owner.fn << "() — did the fast-path decision move?");
		REQUIRE_FALSE(body.empty());

		INFO(owner.rel << ": " << owner.fn
		               << "() must count XRT_LAYER_EQUIRECT2 as a CONTRIBUTING layer now that this "
		                  "backend draws it — left in `default:`, a lone equirect2 frame takes the "
		                  "fast path and blends in ENCODED space (#1589/#1610)");
		CHECK(body.find("XRT_LAYER_EQUIRECT2") != std::string::npos);
	}

	/*
	 * The shader is SHARED, not forked — once per LANGUAGE, never once per
	 * backend. d3d_shared/comp_equirect2_shaders.h exists because the
	 * ray/sphere march is ~100 lines that three D3D backends compile, and its
	 * own preamble says a per-path copy would drift the first time either was
	 * touched. `sphere_intersect` is the tell: no D3D backend may name it,
	 * because naming it means carrying a private copy of the HLSL.
	 *
	 * GL is deliberately NOT in this list: it cannot compile HLSL, so its GLSL
	 * twin (VS_EQUIRECT2 / FS_EQUIRECT2_*) is a second LANGUAGE rather than a
	 * second copy, and it does name `sphere_intersect`. vk_native is a third
	 * such expression, in shaders/layer.comp's `do_equirect2` compute squash.
	 */
	const char *const shader_consumers[] = {
	    "d3d11/comp_d3d11_renderer.cpp",
	    "d3d11_service/comp_d3d11_service.cpp",
	    "d3d12/comp_d3d12_renderer.cpp",
	};
	for (const char *rel : shader_consumers) {
		const std::string src = read_whole_file(std::string(DXR_COMP_SRC_DIR) + "/" + rel);
		INFO(rel << " carries its own copy of the equirect2 ray/sphere HLSL — it belongs in "
		         << "d3d_shared/comp_equirect2_shaders.h, which every D3D leg compiles (#1602)");
		CHECK(src.find("sphere_intersect") == std::string::npos);
	}
}
