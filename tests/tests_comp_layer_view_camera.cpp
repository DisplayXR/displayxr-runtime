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
 *     `handle_space()` call in oxr_session_frame_end.c — including the #1502
 *     VIEW-reference-space path, which composes the eye-centroid offset in
 *     exactly as a locate does. Both are therefore already in the head
 *     device's space, so reusing a projection layer's per-view pose as the
 *     quad's camera needs NO re-basing.
 *
 *  2. Every CTS quad sets XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
 *     so straight (unpremultiplied-source) alpha is the variant conformance
 *     actually exercises — the D3D11 quad draw must honour that bit.
 *
 * `comp_layer_accum` is a plain aggregate (an array of `struct comp_layer` plus
 * a count), so the fixtures below fill it directly rather than going through
 * `comp_layer_accum_quad()` / `_projection()`, which want live
 * `xrt_swapchain` references this test has no device to create.
 */

#include "catch_amalgamated.hpp"

#include "util/comp_layer_accum.h"
#include "util/comp_layer_view_camera.h"

#include <cmath>
#include <cstring>
#include <initializer_list>

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

	// The pose is the eye verbatim, identity orientation — the same thing
	// xrLocateViews reports, in the same head-relative space the quad pose
	// already lives in.
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
	// Pose stays in the LAYER space (display-plane-relative) — untouched.
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

TEST_CASE("comp_layer_view_camera: (b) a quad-only frame composes through the LOCATED view")
{
	/*
	 * The reported #1580 shape: the Khronos CTS QuadPoses case on the
	 * sim display. Quad layers only — nothing to borrow a camera from — with
	 * the sim DP's nominal viewer 10 cm ABOVE the panel centre
	 * (sim_display_processor.c: nominal_y_m = 0.1f) at
	 * SIM_DISPLAY_NOMINAL_Z_M = 0.2, over the default 0.344 x 0.194 m panel
	 * (sim_display_device.c). That is a violently asymmetric vertical
	 * frustum — and it is precisely the frustum xrLocateViews reports, so
	 * the compositor must reproduce it exactly rather than approximate it.
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

TEST_CASE("comp_layer_view_camera: a NULL accum just skips branch (a)")
{
	struct xrt_vec3 eye = {0.0f, 0.0f, 0.5f};
	struct comp_layer_view_camera cam = {};
	REQUIRE(comp_layer_view_camera_select(nullptr, 0, &eye, 0.5f, 0.3f, &cam));
	CHECK(cam.source == COMP_LAYER_VIEW_CAMERA_FROM_DISPLAY3D);
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

TEST_CASE("CTS quads set BLEND_TEXTURE_SOURCE_ALPHA, so straight alpha is the exercised path")
{
	// Not a renderer test (no D3D device here) — it pins the flag the D3D11
	// quad draw must branch on, so a later refactor that drops the bit from
	// the blend-state choice trips something.
	struct comp_layer_accum accum = {};
	push_quad(accum, {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f}});

	CHECK((accum.layers[0].data.flags & XRT_LAYER_COMPOSITION_BLEND_TEXTURE_SOURCE_ALPHA_BIT) != 0);
	CHECK((accum.layers[0].data.flags & XRT_LAYER_COMPOSITION_UNPREMULTIPLIED_ALPHA_BIT) == 0);
}
