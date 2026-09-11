// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Rig composition tests for @ref u_space_overseer.
 *
 * The 3D-display class rule: a device whose tracking volume is bolted to the
 * rig (a desk-mounted hand tracker sitting on the same table as the panel) must
 * follow **voluntary** rig motion — translation AND rotation — and must NOT
 * follow eye-tracked head parallax. See ADR-034 Amendment 2.
 *
 * Parallax never reaches the head *device* pose (it is applied later, at
 * view-pose level), so "does not follow parallax" is tested here as "a head
 * device pose that does not change leaves the hand exactly where it was".
 */

#include "xrt/xrt_device.h"
#include "xrt/xrt_space.h"
#include "xrt/xrt_tracking.h"

#include "math/m_api.h"

#include "util/u_builders.h"
#include "util/u_space_overseer.h"

#include "catch_amalgamated.hpp"

#include <cmath>
#include <cstring>


namespace {

constexpr float kHalfPi = 1.57079632679489661923f;

/*!
 * The smallest device that the space overseer will accept: a settable pose and
 * its own tracking origin.
 */
struct fake_device
{
	struct xrt_device base;
	struct xrt_tracking_origin origin;
	struct xrt_pose pose;
};

xrt_result_t
fake_get_tracked_pose(struct xrt_device *xdev,
                      enum xrt_input_name name,
                      int64_t at_timestamp_ns,
                      struct xrt_space_relation *out_relation)
{
	(void)name;
	(void)at_timestamp_ns;

	auto *fd = reinterpret_cast<fake_device *>(xdev);

	*out_relation = XRT_SPACE_RELATION_ZERO;
	out_relation->pose = fd->pose;
	out_relation->relation_flags = (enum xrt_space_relation_flags)( //
	    XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |                  //
	    XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT |                //
	    XRT_SPACE_RELATION_POSITION_VALID_BIT |                     //
	    XRT_SPACE_RELATION_POSITION_TRACKED_BIT);

	return XRT_SUCCESS;
}

void
fake_device_init(fake_device *fd, const char *name, const struct xrt_pose &pose)
{
	memset(fd, 0, sizeof(*fd));

	fd->pose = pose;

	// TRACKING_TYPE_OTHER so u_builder-style arm-model offsets never apply.
	fd->origin.type = XRT_TRACKING_TYPE_OTHER;
	fd->origin.initial_offset = XRT_POSE_IDENTITY;
	snprintf(fd->origin.name, sizeof(fd->origin.name), "%s origin", name);

	fd->base.tracking_origin = &fd->origin;
	fd->base.get_tracked_pose = fake_get_tracked_pose;
	snprintf(fd->base.str, sizeof(fd->base.str), "%s", name);
}

struct xrt_pose
pose_at(float x, float y, float z)
{
	struct xrt_pose p = XRT_POSE_IDENTITY;
	p.position = {x, y, z};
	return p;
}

//! Rotation about the world Y axis, in radians.
struct xrt_pose
pose_yaw(const struct xrt_pose &base, float radians)
{
	struct xrt_pose p = base;
	struct xrt_vec3 up = {0.f, 1.f, 0.f};
	math_quat_from_angle_vector(radians, &up, &p.orientation);
	return p;
}

/*!
 * A whole two-device world: a head that is the voluntary rig, and a hand whose
 * volume sits on the desk. Mirrors what the builders wire up.
 */
struct rig_fixture
{
	fake_device head{};
	fake_device hand{};
	struct u_space_overseer *uso = nullptr;

	//! @p rig_relative mirrors the builder flag; false = the old behaviour.
	explicit rig_fixture(bool rig_relative, const struct xrt_pose &hand_pose = pose_at(0.f, 1.45f, -0.10f))
	{
		fake_device_init(&head, "Fake Head", pose_at(0.f, 1.6f, 0.f));
		fake_device_init(&hand, "Fake Hand", hand_pose);

		uso = u_space_overseer_create(nullptr);

		struct xrt_device *xdevs[] = {&head.base, &hand.base};
		struct xrt_pose local_offset = pose_at(0.f, 1.6f, 0.f);

		u_space_overseer_legacy_setup(uso, xdevs, 2, &head.base, &local_offset, false, false);

		if (rig_relative) {
			u_space_overseer_set_rig_source(uso, &head.base, XRT_INPUT_GENERIC_HEAD_POSE);
			u_space_overseer_set_device_rig_relative(uso, &hand.base);
		}
	}

	~rig_fixture()
	{
		xrt_space_reference(&hand_space, NULL);

		struct xrt_space_overseer *xso = (struct xrt_space_overseer *)uso;
		xrt_space_overseer_destroy(&xso);
	}

	struct xrt_space_overseer *
	xso()
	{
		return (struct xrt_space_overseer *)uso;
	}

	/*!
	 * The hand's grip pose in stage space — the action-space path an app
	 * actually reads.
	 */
	struct xrt_pose
	locate_hand()
	{
		if (hand_space == nullptr) {
			xrt_space_overseer_create_pose_space(xso(), &hand.base, XRT_INPUT_SIMPLE_GRIP_POSE,
			                                     &hand_space);
			REQUIRE(hand_space != nullptr);
		}
		return locate_in_stage(hand_space);
	}

	//! Locate any space in stage space.
	struct xrt_pose
	locate_in_stage(struct xrt_space *space)
	{
		struct xrt_pose ident = XRT_POSE_IDENTITY;
		struct xrt_space_relation rel = XRT_SPACE_RELATION_ZERO;

		xrt_space_overseer_locate_space(xso(), xso()->semantic.stage, &ident, 1, space, &ident, &rel);

		REQUIRE((rel.relation_flags & XRT_SPACE_RELATION_POSITION_VALID_BIT) != 0);
		return rel.pose;
	}

	/*!
	 * The hand device's *volume* in stage space (@ref xrt_space_overseer
	 * ::locate_device) — the base that hand-tracking joints are reported
	 * against, so it has to travel with the rig too.
	 */
	struct xrt_pose
	locate_hand_volume()
	{
		struct xrt_pose ident = XRT_POSE_IDENTITY;
		struct xrt_space_relation rel = XRT_SPACE_RELATION_ZERO;

		xrt_space_overseer_locate_device(xso(), xso()->semantic.stage, &ident, 1, &hand.base, &rel);
		return rel.pose;
	}

	struct xrt_space *hand_space = nullptr;
};

void
check_vec3(const struct xrt_vec3 &got, float x, float y, float z)
{
	CHECK(got.x == Catch::Approx(x).margin(0.0001));
	CHECK(got.y == Catch::Approx(y).margin(0.0001));
	CHECK(got.z == Catch::Approx(z).margin(0.0001));
}

} // namespace


TEST_CASE("u_space_overseer rig composition")
{
	SECTION("un-flagged device is world-fixed — the rig flies away without it")
	{
		rig_fixture f{false};

		struct xrt_pose before = f.locate_hand();
		check_vec3(before.position, 0.f, 1.45f, -0.10f);

		f.head.pose = pose_at(0.f, 1.6f, -3.f); // Walk 3 m forward.

		struct xrt_pose after = f.locate_hand();
		check_vec3(after.position, 0.f, 1.45f, -0.10f); // Stranded at the desk.
	}

	SECTION("flagging alone changes nothing while the rig is still")
	{
		rig_fixture f{true};
		check_vec3(f.locate_hand().position, 0.f, 1.45f, -0.10f);
	}

	SECTION("rig translation carries the hand")
	{
		rig_fixture f{true};

		f.head.pose = pose_at(0.5f, 1.6f, -3.f); // WASD: 3 m forward, 0.5 m right.

		struct xrt_pose after = f.locate_hand();
		check_vec3(after.position, 0.5f, 1.45f, -3.10f);
	}

	SECTION("rig yaw carries the hand, orbiting the rig")
	{
		rig_fixture f{true};

		// Mouse-look 90 deg left about the rig, which stands at x=z=0.
		f.head.pose = pose_yaw(pose_at(0.f, 1.6f, 0.f), kHalfPi);

		struct xrt_pose after = f.locate_hand();

		// +90 deg about +Y maps (x,z) = (0, -0.10) -> (-0.10, 0).
		check_vec3(after.position, -0.10f, 1.45f, 0.f);

		// The hand's own orientation yaws with it.
		struct xrt_vec3 fwd_in = {0.f, 0.f, -1.f};
		struct xrt_vec3 fwd_out = XRT_VEC3_ZERO;
		math_quat_rotate_vec3(&after.orientation, &fwd_in, &fwd_out);
		check_vec3(fwd_out, -1.f, 0.f, 0.f);
	}

	SECTION("rig yaw about a translated rig orbits the NEW rig position")
	{
		rig_fixture f{true};

		// Walk to z = -2, then turn 90 deg. Order matters: the hand must
		// end up beside the rig's new position, not the world origin.
		f.head.pose = pose_yaw(pose_at(0.f, 1.6f, -2.f), kHalfPi);

		struct xrt_pose after = f.locate_hand();
		check_vec3(after.position, -0.10f, 1.45f, -2.f);
	}

	SECTION("a rig that returns to where it started returns the hand too")
	{
		rig_fixture f{true};

		f.head.pose = pose_yaw(pose_at(1.f, 1.6f, -4.f), 1.234f);
		(void)f.locate_hand();

		f.head.pose = pose_at(0.f, 1.6f, 0.f);
		check_vec3(f.locate_hand().position, 0.f, 1.45f, -0.10f);
	}

	SECTION("the head itself is never composed with its own delta")
	{
		rig_fixture f{true};

		f.head.pose = pose_at(0.f, 1.6f, -3.f);

		// View space is the head's own pose space.
		check_vec3(f.locate_in_stage(f.xso()->semantic.view).position, 0.f, 1.6f, -3.f); // Not doubled.
	}

	SECTION("the hand-tracking base volume travels with the rig too")
	{
		rig_fixture f{true};

		check_vec3(f.locate_hand_volume().position, 0.f, 0.f, 0.f);

		f.head.pose = pose_at(0.f, 1.6f, -3.f);
		check_vec3(f.locate_hand_volume().position, 0.f, 0.f, -3.f);
	}

	SECTION("locating the rig-relative device as the BASE space inverts cleanly")
	{
		rig_fixture f{true};

		f.head.pose = pose_yaw(pose_at(0.f, 1.6f, -2.f), 0.7f);

		struct xrt_pose ident = XRT_POSE_IDENTITY;
		struct xrt_pose fwd = f.locate_hand(); // Also creates f.hand_space.

		struct xrt_space_relation inv = XRT_SPACE_RELATION_ZERO;
		xrt_space_overseer_locate_space(f.xso(), f.hand_space, &ident, 1, f.xso()->semantic.stage, &ident,
		                                &inv);

		struct xrt_pose round_trip = XRT_POSE_IDENTITY;
		math_pose_transform(&fwd, &inv.pose, &round_trip);
		check_vec3(round_trip.position, 0.f, 0.f, 0.f);
		CHECK(std::fabs(round_trip.orientation.w) == Catch::Approx(1.0).margin(0.0001));
	}
}


/*
 * #1380 — display-plane-relative (RIG_LOCAL) provider origins.
 *
 * A provider that navigates publishes its controllers relative to the display
 * plane and knows nothing about standing height or mount. The BUILDER anchors
 * that origin at the initial rig, so the pose an app reads is
 * `world = rig(t) o L` with no provider-side reference frame. Everything below
 * goes through `u_builder_create_space_overseer_legacy` — the anchoring lives
 * inside it, between the tracking-origin setup and the moment the overseer
 * freezes each origin offset into a space.
 */
namespace {

//! The builder's own sequence: create the overseer, then arm rig composition.
struct builder_fixture
{
	fake_device head{};
	fake_device dev{};
	struct xrt_space_overseer *xso = nullptr;

	builder_fixture(enum xrt_tracking_type dev_origin_type, const struct xrt_pose &dev_pose)
	{
		fake_device_init(&head, "Fake Head", pose_at(0.f, 1.6f, 0.f));
		fake_device_init(&dev, "Fake Provider Hand", dev_pose);

		// TYPE_OTHER on the head keeps u_builder_setup_tracking_origins
		// from bolting its own 1.6 m on top of the pose.
		head.origin.type = XRT_TRACKING_TYPE_OTHER;
		dev.origin.type = dev_origin_type;

		struct xrt_device *xdevs[] = {&head.base, &dev.base};

		u_builder_create_space_overseer_legacy(nullptr, &head.base, nullptr, nullptr, nullptr, xdevs, 2, false,
		                                       false, &xso);
		REQUIRE(xso != nullptr);

		auto *uso = (struct u_space_overseer *)xso;
		u_space_overseer_set_rig_source(uso, &head.base, XRT_INPUT_GENERIC_HEAD_POSE);
		u_space_overseer_set_device_rig_relative(uso, &dev.base);
	}

	~builder_fixture()
	{
		xrt_space_reference(&dev_space, NULL);
		xrt_space_overseer_destroy(&xso);
	}

	//! The provider device's grip pose in stage space.
	struct xrt_pose
	locate_dev()
	{
		if (dev_space == nullptr) {
			xrt_space_overseer_create_pose_space(xso, &dev.base, XRT_INPUT_SIMPLE_GRIP_POSE, &dev_space);
			REQUIRE(dev_space != nullptr);
		}

		struct xrt_pose ident = XRT_POSE_IDENTITY;
		struct xrt_space_relation rel = XRT_SPACE_RELATION_ZERO;
		xrt_space_overseer_locate_space(xso, xso->semantic.stage, &ident, 1, dev_space, &ident, &rel);
		REQUIRE((rel.relation_flags & XRT_SPACE_RELATION_POSITION_VALID_BIT) != 0);
		return rel.pose;
	}

	struct xrt_space *dev_space = nullptr;
};

} // namespace


TEST_CASE("u_builders anchors RIG_LOCAL origins at the initial rig")
{
	// Display-plane-relative: 10 cm right of the display centre, 5 cm up,
	// 30 cm toward the viewer. No standing height anywhere in it.
	const struct xrt_pose L = pose_at(0.10f, 0.05f, 0.30f);

	SECTION("a RIG_LOCAL device starts at rig_initial o L")
	{
		builder_fixture f{XRT_TRACKING_TYPE_RIG_LOCAL, L};

		// rig_initial is the head at (0, 1.6, 0), so the provider's
		// display-plane pose lands at panel height without the
		// provider knowing the number.
		check_vec3(f.locate_dev().position, 0.10f, 1.65f, 0.30f);
	}

	SECTION("and then follows the rig: world = rig(t) o L")
	{
		builder_fixture f{XRT_TRACKING_TYPE_RIG_LOCAL, L};
		(void)f.locate_dev();

		f.head.pose = pose_at(0.5f, 1.6f, -3.f); // WASD.
		check_vec3(f.locate_dev().position, 0.60f, 1.65f, -2.70f);

		// Yaw 90 deg about +Y at the new rig position: (x, z) of L
		// maps (0.10, 0.30) -> (0.30, -0.10) around the rig.
		f.head.pose = pose_yaw(pose_at(0.f, 1.6f, -2.f), kHalfPi);
		check_vec3(f.locate_dev().position, 0.30f, 1.65f, -2.10f);

		// Home again: the device comes home with it.
		f.head.pose = pose_at(0.f, 1.6f, 0.f);
		check_vec3(f.locate_dev().position, 0.10f, 1.65f, 0.30f);
	}

	SECTION("a TYPE_OTHER device is untouched — today's stage-anchored behaviour")
	{
		builder_fixture f{XRT_TRACKING_TYPE_OTHER, L};

		// No anchoring: the origin offset stays identity, so the pose
		// is read straight out of the volume.
		check_vec3(f.locate_dev().position, 0.10f, 0.05f, 0.30f);

		// It still rides the rig delta, exactly as before #1380.
		f.head.pose = pose_at(0.5f, 1.6f, -3.f);
		check_vec3(f.locate_dev().position, 0.60f, 0.05f, -2.70f);
	}

	SECTION("devices sharing one RIG_LOCAL origin are anchored once, not twice")
	{
		fake_device head{};
		fake_device left{};
		fake_device right{};

		fake_device_init(&head, "Fake Head", pose_at(0.f, 1.6f, 0.f));
		fake_device_init(&left, "Fake Left", pose_at(-0.1f, 0.f, 0.2f));
		fake_device_init(&right, "Fake Right", pose_at(0.1f, 0.f, 0.2f));

		head.origin.type = XRT_TRACKING_TYPE_OTHER;

		// One shared origin, as a provider's controller pair has.
		left.origin.type = XRT_TRACKING_TYPE_RIG_LOCAL;
		right.base.tracking_origin = &left.origin;

		struct xrt_device *xdevs[] = {&head.base, &left.base, &right.base};
		struct xrt_space_overseer *xso = nullptr;
		u_builder_create_space_overseer_legacy(nullptr, &head.base, nullptr, nullptr, nullptr, xdevs, 3, false,
		                                       false, &xso);
		REQUIRE(xso != nullptr);

		// Anchored to rig_initial exactly once — a second pass would
		// have composed (0, 1.6, 0) onto itself.
		check_vec3(left.origin.initial_offset.position, 0.f, 1.6f, 0.f);

		xrt_space_overseer_destroy(&xso);
	}
}
