// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief #1594 acceptance: a VIEW-space composition layer lands in the SAME
 *        frame as the projection views.
 *
 * Device-free. `oxr_layer_space.c` is compiled straight into this unit (the
 * pattern `tests_oxr_view_config_views_change` already uses for
 * `oxr_views_change.c`), so the relations `oxr_space_locate_device()` would
 * have returned are injected by hand: no session, no compositor, no GPU, no
 * clock.
 *
 * The geometry mirrors the rig the defect was measured on (issue #1594):
 *
 *   root   the head xdev's TRACKING-ORIGIN space. This is the one frame every
 *          native compositor consumes -- `data.proj.v[i].pose` and
 *          `data.quad.pose` both arrive in it.
 *   LOCAL  root offset by the 1.6 m eye height (`u_space_overseer.c:958`).
 *   VIEW   the head device pose, with the #1502 eye-centroid offset composed
 *          in front of it (`oxr_space_ref_offset`).
 *
 * What is pinned: a quad submitted in VIEW at P and a quad submitted in LOCAL
 * at the pose that denotes the SAME physical place resolve to the SAME
 * compositor pose. Re-introducing the deleted head-relative shortcut (compose
 * only the eye-centroid offset, never the head pose) turns that equality into
 * a ~1.6 m miss -- which the last section measures explicitly, so the mutation
 * is caught with a number and not just a red assertion.
 */

#include "xrt/xrt_compiler.h"
#include "xrt/xrt_defines.h"

#include "math/m_api.h"
#include "math/m_space.h"

#include "oxr_layer_space.h"

#include "catch_amalgamated.hpp"

#include <cmath>


/*
 *
 * Fixtures.
 *
 */

namespace {

constexpr float kEps = 1e-4f;

constexpr xrt_space_relation_flags kFlagsValidTracked = (xrt_space_relation_flags)( //
    XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |                                      //
    XRT_SPACE_RELATION_POSITION_VALID_BIT |                                         //
    XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT |                                    //
    XRT_SPACE_RELATION_POSITION_TRACKED_BIT);                                       //

//! LOCAL is the root offset up by the eye height, so T_root_local is that offset.
xrt_pose
make_T_root_local()
{
	xrt_pose p = XRT_POSE_IDENTITY;
	p.position = {0.0f, 1.6f, 0.0f};
	return p;
}

//! A head that is neither at the origin nor axis-aligned -- a translation-only
//! head would let an orientation bug through.
xrt_pose
make_T_root_head()
{
	xrt_pose p = XRT_POSE_IDENTITY;
	const xrt_vec3 up = {0.0f, 1.0f, 0.0f};
	math_quat_from_angle_vector(0.5236f /* 30 deg */, &up, &p.orientation);
	p.position = {0.05f, 1.6f, -0.2f};
	return p;
}

//! #1502 eye centroid, the offset oxr_space_ref_offset() puts in front of VIEW.
xrt_pose
make_view_space_offset()
{
	xrt_pose p = XRT_POSE_IDENTITY;
	p.position = {0.0f, 0.1422f, 0.8783f};
	return p;
}

xrt_pose
compose(const xrt_pose &transform, const xrt_pose &pose)
{
	xrt_pose out = XRT_POSE_IDENTITY;
	math_pose_transform(&transform, &pose, &out);
	return out;
}

xrt_pose
invert(const xrt_pose &pose)
{
	xrt_pose out = XRT_POSE_IDENTITY;
	math_pose_invert(&pose, &out);
	return out;
}

/*!
 * What oxr_space_locate_device() hands handle_space(): the head's tracking
 * ORIGIN located in the layer's space -- the inverse of that space expressed
 * in root. (The overseer links a device to its origin space, not to its pose;
 * see the #1370 comment in oxr_session.c.)
 */
xrt_space_relation
origin_located_in(const xrt_pose &T_root_space)
{
	xrt_space_relation rel = XRT_SPACE_RELATION_ZERO;
	rel.relation_flags = kFlagsValidTracked;
	rel.pose = invert(T_root_space);
	return rel;
}

void
require_pose_eq(const xrt_pose &a, const xrt_pose &b)
{
	REQUIRE_THAT(a.position.x, Catch::Matchers::WithinAbs(b.position.x, kEps));
	REQUIRE_THAT(a.position.y, Catch::Matchers::WithinAbs(b.position.y, kEps));
	REQUIRE_THAT(a.position.z, Catch::Matchers::WithinAbs(b.position.z, kEps));

	// q and -q are the same rotation; compare through the dot product.
	const float dot = a.orientation.x * b.orientation.x + a.orientation.y * b.orientation.y +
	                  a.orientation.z * b.orientation.z + a.orientation.w * b.orientation.w;
	REQUIRE_THAT(std::fabs(dot), Catch::Matchers::WithinAbs(1.0f, kEps));
}

} // namespace


/*
 *
 * Tests.
 *
 */

TEST_CASE("oxr_layer_space: VIEW and LOCAL quads at the same place coincide", "[oxr_layer_space]")
{
	const xrt_pose T_root_local = make_T_root_local();
	const xrt_pose T_root_head = make_T_root_head();
	// VIEW = head o view_space_offset (oxr_space_ref_offset, #1502).
	const xrt_pose T_root_view = compose(T_root_head, make_view_space_offset());

	// The CTS interactive prompt quad: VIEW space, {0, +0.4, -1}.
	xrt_pose P_view = XRT_POSE_IDENTITY;
	P_view.position = {0.0f, 0.4f, -1.0f};

	// The LOCAL pose denoting the SAME physical place.
	const xrt_pose P_local = compose(invert(T_root_local), compose(T_root_view, P_view));

	xrt_pose out_view = XRT_POSE_IDENTITY;
	xrt_pose out_local = XRT_POSE_IDENTITY;

	const xrt_space_relation rel_view = origin_located_in(T_root_view);
	const xrt_space_relation rel_local = origin_located_in(T_root_local);
	REQUIRE(oxr_layer_pose_in_xdev_frame(&rel_view, &P_view, &out_view));
	REQUIRE(oxr_layer_pose_in_xdev_frame(&rel_local, &P_local, &out_local));

	// Both must be the layer expressed in root -- the frame the projection
	// views arrive in. That is the whole of #1594.
	require_pose_eq(out_view, out_local);
	require_pose_eq(out_view, compose(T_root_view, P_view));
}

TEST_CASE("oxr_layer_space: an app poseInReferenceSpace rides along", "[oxr_layer_space]")
{
	const xrt_pose T_root_local = make_T_root_local();
	const xrt_pose T_root_head = make_T_root_head();

	// A VIEW space created with a non-identity poseInReferenceSpace. The
	// overseer applies it as a child offset, so it sits behind the eye
	// centroid: VIEW' = head o view_space_offset o spc->pose.
	xrt_pose spc_pose = XRT_POSE_IDENTITY;
	spc_pose.position = {0.03f, -0.07f, 0.11f};
	const xrt_pose T_root_view = compose(compose(T_root_head, make_view_space_offset()), spc_pose);

	xrt_pose P_view = XRT_POSE_IDENTITY;
	P_view.position = {-0.12f, 0.25f, -0.9f};

	const xrt_pose P_local = compose(invert(T_root_local), compose(T_root_view, P_view));

	xrt_pose out_view = XRT_POSE_IDENTITY;
	xrt_pose out_local = XRT_POSE_IDENTITY;
	const xrt_space_relation rel_view = origin_located_in(T_root_view);
	const xrt_space_relation rel_local = origin_located_in(T_root_local);
	REQUIRE(oxr_layer_pose_in_xdev_frame(&rel_view, &P_view, &out_view));
	REQUIRE(oxr_layer_pose_in_xdev_frame(&rel_local, &P_local, &out_local));

	require_pose_eq(out_view, out_local);
}

TEST_CASE("oxr_layer_space: no head relation means no pose", "[oxr_layer_space]")
{
	xrt_space_relation rel = XRT_SPACE_RELATION_ZERO; // relation_flags == 0

	xrt_pose P = XRT_POSE_IDENTITY;
	P.position = {0.0f, 0.4f, -1.0f};

	xrt_pose out = XRT_POSE_IDENTITY;
	out.position = {9.0f, 9.0f, 9.0f};

	REQUIRE_FALSE(oxr_layer_pose_in_xdev_frame(&rel, &P, &out));

	// Untouched, so the caller can fall back (handle_space does, for VIEW).
	REQUIRE(out.position.y == 9.0f);
}

TEST_CASE("oxr_layer_space: the pre-#1594 head-relative shortcut misses by the head pose", "[oxr_layer_space]")
{
	const xrt_pose T_root_head = make_T_root_head();
	const xrt_pose view_offset = make_view_space_offset();
	const xrt_pose T_root_view = compose(T_root_head, view_offset);

	xrt_pose P_view = XRT_POSE_IDENTITY;
	P_view.position = {0.0f, 0.4f, -1.0f};

	xrt_pose fixed = XRT_POSE_IDENTITY;
	const xrt_space_relation rel_view = origin_located_in(T_root_view);
	REQUIRE(oxr_layer_pose_in_xdev_frame(&rel_view, &P_view, &fixed));

	// What the deleted branch computed: view_space_offset o spc->pose o P,
	// with the head pose never applied.
	const xrt_pose legacy = compose(view_offset, P_view);

	const float dy = fixed.position.y - legacy.position.y;
	REQUIRE_THAT(dy, Catch::Matchers::WithinAbs(T_root_head.position.y, 1e-3f));
	REQUIRE(dy > 1.5f); // the ~1.6 m the CTS prompt quads fell by
}
