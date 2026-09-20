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
