// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Rig-composer tests (#1380, ADR-034 Amendment 4).
 *
 * The composer is the head device's pose source: it answers HEAD_POSE with
 * `rig(t)`, the voluntary fly-camera pose. These tests pin the whole state
 * machine — the qwerty floor, handover alignment, the invalid/valid hold, the
 * durable recenter, handback reseeding, hand-role churn, and thread safety —
 * with no hardware, no plug-in and no display.
 *
 * The qwerty side is a FAKE: `qwerty_set_hmd_pose` is stubbed here so the test
 * can see what the composer hands back without linking the driver.
 */

#include "xrt/xrt_defines.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_system.h"
#include "xrt/xrt_tracking.h"

#include "math/m_api.h"

#include "catch_amalgamated.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

extern "C" {
#include "target_rig_composer.h"
}


/*
 *
 * The qwerty stub. The composer's ONLY call into the driver.
 *
 */

namespace {
struct xrt_pose g_qwerty_set_pose = XRT_POSE_IDENTITY;
int g_qwerty_set_calls = 0;
//! Where the stub writes the pose, standing in for qwerty's integrator state.
struct xrt_pose *g_qwerty_target = nullptr;
} // namespace

extern "C" void
qwerty_set_hmd_pose(struct xrt_device *qwerty_hmd, const struct xrt_pose *pose)
{
	(void)qwerty_hmd;
	g_qwerty_set_pose = *pose;
	g_qwerty_set_calls++;
	if (g_qwerty_target != nullptr) {
		*g_qwerty_target = *pose;
	}
}


namespace {

constexpr float kTol = 1e-5f;

struct xrt_pose
pose_at(float x, float y, float z)
{
	struct xrt_pose p = XRT_POSE_IDENTITY;
	p.position = {x, y, z};
	return p;
}

//! A pose with a yaw about world +Y and a translation.
struct xrt_pose
pose_yaw_at(float radians, float x, float y, float z)
{
	struct xrt_pose p = XRT_POSE_IDENTITY;
	struct xrt_vec3 up = {0.f, 1.f, 0.f};
	math_quat_from_angle_vector(radians, &up, &p.orientation);
	p.position = {x, y, z};
	return p;
}

void
check_pose_eq(const struct xrt_pose &got, const struct xrt_pose &want)
{
	CHECK(got.position.x == Catch::Approx(want.position.x).margin(kTol));
	CHECK(got.position.y == Catch::Approx(want.position.y).margin(kTol));
	CHECK(got.position.z == Catch::Approx(want.position.z).margin(kTol));

	// A quaternion and its negation are the same rotation; compare against
	// whichever sign is closer so the test pins the ROTATION, not the
	// representation.
	float dot = got.orientation.x * want.orientation.x + got.orientation.y * want.orientation.y +
	            got.orientation.z * want.orientation.z + got.orientation.w * want.orientation.w;
	float s = dot < 0.f ? -1.f : 1.f;
	CHECK(got.orientation.x == Catch::Approx(s * want.orientation.x).margin(kTol));
	CHECK(got.orientation.y == Catch::Approx(s * want.orientation.y).margin(kTol));
	CHECK(got.orientation.z == Catch::Approx(s * want.orientation.z).margin(kTol));
	CHECK(got.orientation.w == Catch::Approx(s * want.orientation.w).margin(kTol));
}


/*
 *
 * Fakes.
 *
 */

//! A fake qwerty HMD: a settable pose, returned verbatim.
struct fake_qwerty
{
	struct xrt_device base{};
	struct xrt_tracking_origin origin{};
	struct xrt_pose pose = XRT_POSE_IDENTITY;
};

xrt_result_t
fake_qwerty_get_tracked_pose(struct xrt_device *xdev,
                             enum xrt_input_name name,
                             int64_t at_timestamp_ns,
                             struct xrt_space_relation *out_relation)
{
	(void)at_timestamp_ns;
	if (name != XRT_INPUT_GENERIC_HEAD_POSE) {
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	auto *fq = reinterpret_cast<fake_qwerty *>(xdev);

	struct xrt_space_relation zero = XRT_SPACE_RELATION_ZERO;
	*out_relation = zero;
	out_relation->pose = fq->pose;
	out_relation->relation_flags = (enum xrt_space_relation_flags)( //
	    XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |                  //
	    XRT_SPACE_RELATION_POSITION_VALID_BIT |                     //
	    XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT |                //
	    XRT_SPACE_RELATION_POSITION_TRACKED_BIT);
	return XRT_SUCCESS;
}

void
fake_qwerty_init(fake_qwerty *fq, const struct xrt_pose &pose)
{
	memset(&fq->base, 0, sizeof(fq->base));
	memset(&fq->origin, 0, sizeof(fq->origin));
	fq->pose = pose;
	fq->origin.type = XRT_TRACKING_TYPE_NONE;
	fq->origin.initial_offset = XRT_POSE_IDENTITY;
	snprintf(fq->origin.name, sizeof(fq->origin.name), "Fake qwerty origin");
	fq->base.tracking_origin = &fq->origin;
	fq->base.get_tracked_pose = fake_qwerty_get_tracked_pose;
	fq->base.device_type = XRT_DEVICE_TYPE_HMD;
	snprintf(fq->base.str, sizeof(fq->base.str), "Fake qwerty HMD");
}

/*!
 * A fake provider NAVIGATION device: a settable N, a settable validity, and a
 * durable level-with-timestamp RECENTER input.
 */
struct fake_nav
{
	struct xrt_device base{};
	struct xrt_tracking_origin origin{};
	struct xrt_input inputs[2]{};

	struct xrt_pose n = XRT_POSE_IDENTITY;
	bool valid = true;
	//! Does this device expose a RECENTER input at all? (It is optional.)
	bool has_recenter = true;
	int update_inputs_calls = 0;
};

xrt_result_t
fake_nav_update_inputs(struct xrt_device *xdev)
{
	reinterpret_cast<fake_nav *>(xdev)->update_inputs_calls++;
	return XRT_SUCCESS;
}

xrt_result_t
fake_nav_get_tracked_pose(struct xrt_device *xdev,
                          enum xrt_input_name name,
                          int64_t at_timestamp_ns,
                          struct xrt_space_relation *out_relation)
{
	(void)at_timestamp_ns;
	if (name != XRT_INPUT_GENERIC_NAVIGATION_POSE) {
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	auto *fn = reinterpret_cast<fake_nav *>(xdev);

	struct xrt_space_relation zero = XRT_SPACE_RELATION_ZERO;
	*out_relation = zero;
	out_relation->pose = fn->n;
	if (fn->valid) {
		out_relation->relation_flags = (enum xrt_space_relation_flags)( //
		    XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |                  //
		    XRT_SPACE_RELATION_POSITION_VALID_BIT);
	}
	return XRT_SUCCESS;
}

void
fake_nav_init(fake_nav *fn, const struct xrt_pose &n, bool has_recenter = true)
{
	memset(&fn->base, 0, sizeof(fn->base));
	memset(&fn->origin, 0, sizeof(fn->origin));
	memset(fn->inputs, 0, sizeof(fn->inputs));

	fn->n = n;
	fn->valid = true;
	fn->has_recenter = has_recenter;
	fn->update_inputs_calls = 0;

	fn->origin.type = XRT_TRACKING_TYPE_OTHER;
	fn->origin.initial_offset = XRT_POSE_IDENTITY;
	snprintf(fn->origin.name, sizeof(fn->origin.name), "Fake nav origin");

	fn->inputs[0].active = true;
	fn->inputs[0].name = XRT_INPUT_GENERIC_NAVIGATION_POSE;
	fn->inputs[1].active = true;
	fn->inputs[1].name = XRT_INPUT_GENERIC_NAVIGATION_RECENTER;
	fn->inputs[1].value.boolean = false;
	fn->inputs[1].timestamp = 0;

	fn->base.tracking_origin = &fn->origin;
	fn->base.device_type = XRT_DEVICE_TYPE_NAVIGATION;
	fn->base.inputs = fn->inputs;
	fn->base.input_count = has_recenter ? 2 : 1;
	fn->base.update_inputs = fake_nav_update_inputs;
	fn->base.get_tracked_pose = fake_nav_get_tracked_pose;
	snprintf(fn->base.str, sizeof(fn->base.str), "Fake navigation");
}

//! Publish a recenter: the new timestamp and the post-reset N, atomically.
void
fake_nav_publish_recenter(fake_nav *fn, int64_t ts, const struct xrt_pose &new_n)
{
	fn->inputs[1].active = true;
	fn->inputs[1].value.boolean = true;
	fn->inputs[1].timestamp = ts;
	fn->n = new_n;
}

/*!
 * A fake xrt_system_devices with a settable roles block. `rig` is the field
 * under test; `left`/`right` are here so the test can churn the HAND roles
 * (and the shared generation counter) without touching the rig.
 */
struct fake_sysd
{
	struct xrt_system_devices base{};
	std::atomic<int32_t> rig{-1};
	std::atomic<int32_t> left{-1};
	std::atomic<int32_t> right{-1};
	std::atomic<uint64_t> generation{1};
};

xrt_result_t
fake_sysd_get_roles(struct xrt_system_devices *xsysd, struct xrt_system_roles *out_roles)
{
	auto *fs = reinterpret_cast<fake_sysd *>(xsysd);
	struct xrt_system_roles roles = XRT_SYSTEM_ROLES_INIT;
	roles.generation_id = fs->generation.load();
	roles.left = fs->left.load();
	roles.right = fs->right.load();
	roles.rig = fs->rig.load();
	*out_roles = roles;
	return XRT_SUCCESS;
}

void
fake_sysd_init(fake_sysd *fs)
{
	memset(&fs->base, 0, sizeof(fs->base));
	fs->base.get_roles = fake_sysd_get_roles;
	fs->rig = -1;
	fs->left = -1;
	fs->right = -1;
	fs->generation = 1;
}

//! Set the rig role and bump the shared generation counter, as the arbiter does.
void
set_rig_role(fake_sysd *fs, int32_t rig)
{
	fs->rig = rig;
	fs->generation++;
}


/*
 *
 * Fixture.
 *
 */

struct composer_fixture
{
	fake_qwerty qwerty{};
	fake_nav nav{};
	fake_sysd sysd{};
	struct xrt_device *composer = nullptr;

	explicit composer_fixture(const struct xrt_pose &seed = pose_at(0.f, 1.6f, 0.f),
	                          const struct xrt_pose &n0 = XRT_POSE_IDENTITY,
	                          bool has_recenter = true)
	{
		g_qwerty_set_calls = 0;
		g_qwerty_set_pose = XRT_POSE_IDENTITY;

		fake_qwerty_init(&qwerty, seed);
		g_qwerty_target = &qwerty.pose;
		fake_nav_init(&nav, n0, has_recenter);
		fake_sysd_init(&sysd);

		// Index 0 is the navigation device; the qwerty floor is -1.
		sysd.base.xdevs[0] = &nav.base;
		sysd.base.xdev_count = 1;

		composer = t_rig_composer_create(&qwerty.base, &sysd.base, nullptr);
		REQUIRE(composer != nullptr);
	}

	~composer_fixture()
	{
		t_rig_composer_destroy(&composer);
		g_qwerty_target = nullptr;
	}

	composer_fixture(const composer_fixture &) = delete;
	composer_fixture &
	operator=(const composer_fixture &) = delete;

	//! Poll the composer's HEAD_POSE at @p t and return the pose.
	struct xrt_pose
	poll(int64_t t = 1000)
	{
		struct xrt_space_relation rel = XRT_SPACE_RELATION_ZERO;
		REQUIRE(xrt_device_get_tracked_pose(composer, XRT_INPUT_GENERIC_HEAD_POSE, t, &rel) == XRT_SUCCESS);
		CHECK((rel.relation_flags & XRT_SPACE_RELATION_POSITION_VALID_BIT) != 0);
		CHECK((rel.relation_flags & XRT_SPACE_RELATION_ORIENTATION_VALID_BIT) != 0);
		return rel.pose;
	}
};

} // namespace


/*
 *
 * 1. The qwerty floor.
 *
 */

TEST_CASE("rig_composer: qwerty holds the rig")
{
	composer_fixture f(pose_at(0.f, 1.6f, 0.f));

	check_pose_eq(f.poll(), pose_at(0.f, 1.6f, 0.f));

	// WASD.
	f.qwerty.pose = pose_at(0.5f, 1.6f, -2.f);
	check_pose_eq(f.poll(), pose_at(0.5f, 1.6f, -2.f));

	// Mouse-look.
	f.qwerty.pose = pose_yaw_at(0.75f, 0.5f, 1.6f, -2.f);
	check_pose_eq(f.poll(), pose_yaw_at(0.75f, 0.5f, 1.6f, -2.f));

	// The navigation device is never touched while the floor holds.
	CHECK(f.nav.update_inputs_calls == 0);
}


/*
 *
 * 2. Handover to a provider: continuity, then provider-local motion.
 *
 */

TEST_CASE("rig_composer: handover to a provider aligns and then moves along N's own forward")
{
	composer_fixture f(pose_at(0.f, 1.6f, 0.f));

	// Drive the rig somewhere non-trivial on the qwerty floor first, so the
	// continuity check is not hiding behind the identity.
	const struct xrt_pose rig_h = pose_yaw_at(0.4f, 1.f, 1.6f, -3.f);
	f.qwerty.pose = rig_h;
	check_pose_eq(f.poll(), rig_h);

	// N(h) is deliberately NOT the identity: yaw 30 deg at (1,0,0).
	const float deg30 = 30.f * 3.14159265358979323846f / 180.f;
	const struct xrt_pose n_h = pose_yaw_at(deg30, 1.f, 0.f, 0.f);
	f.nav.n = n_h;

	set_rig_role(&f.sysd, 0);

	// The first composed rig IS rig_last: continuity by construction.
	check_pose_eq(f.poll(), rig_h);
	CHECK(f.nav.update_inputs_calls == 1);

	// A provider-local step N -> N(h) o T(0,0,-1) must move the rig along
	// its OWN forward, i.e. rig == rig_h o T(0,0,-1).
	const struct xrt_pose step = pose_at(0.f, 0.f, -1.f);
	struct xrt_pose n_t = XRT_POSE_IDENTITY;
	math_pose_transform(&n_h, &step, &n_t);
	f.nav.n = n_t;

	struct xrt_pose want = XRT_POSE_IDENTITY;
	math_pose_transform(&rig_h, &step, &want);

	check_pose_eq(f.poll(), want);
}


/*
 *
 * 3. Invalid -> hold -> valid re-alignment.
 *
 */

TEST_CASE("rig_composer: N invalid holds the rig, and the next valid sample re-aligns there")
{
	composer_fixture f(pose_at(0.f, 1.6f, 0.f));

	f.nav.n = XRT_POSE_IDENTITY;
	set_rig_role(&f.sysd, 0);
	const struct xrt_pose rig_h = f.poll();

	// Move the rig with the provider so the held value is not the seed.
	f.nav.n = pose_at(0.f, 0.f, -2.f);
	const struct xrt_pose rig_moved = f.poll();
	check_pose_eq(rig_moved, pose_at(0.f, 1.6f, -2.f));
	CHECK(rig_h.position.z == Catch::Approx(0.f).margin(kTol));

	// Optical loss: the rig HOLDS, no matter what N says.
	f.nav.valid = false;
	f.nav.n = pose_at(50.f, 50.f, 50.f);
	check_pose_eq(f.poll(), rig_moved);
	check_pose_eq(f.poll(), rig_moved);

	// Re-acquire somewhere completely different: the rig must NOT jump.
	f.nav.valid = true;
	f.nav.n = pose_at(-7.f, 0.f, 3.f);
	check_pose_eq(f.poll(), rig_moved);

	// And subsequent motion is relative to the NEW N.
	f.nav.n = pose_at(-7.f, 0.f, 2.f);
	check_pose_eq(f.poll(), pose_at(0.f, 1.6f, -3.f));
}


/*
 *
 * 4. Recenter (durable, level-with-timestamp).
 *
 */

TEST_CASE("rig_composer: a newer recenter timestamp returns the rig to rig_initial, exactly once")
{
	const struct xrt_pose home = pose_at(0.f, 1.6f, 0.f);
	composer_fixture f(home);

	set_rig_role(&f.sysd, 0);
	f.poll(1000);

	// Drive away from home.
	f.nav.n = pose_at(3.f, 0.f, -4.f);
	check_pose_eq(f.poll(1000), pose_at(3.f, 1.6f, -4.f));

	SECTION("t >= r: recenters to rig_initial and re-aligns to the new N")
	{
		// The reset timestamp and the post-reset N land in ONE publication.
		fake_nav_publish_recenter(&f.nav, 2000, pose_at(-9.f, 0.f, 9.f));

		check_pose_eq(f.poll(2000), home);

		// Re-aligned to the new N: a step from there moves from home.
		f.nav.n = pose_at(-9.f, 0.f, 8.f);
		check_pose_eq(f.poll(2001), pose_at(0.f, 1.6f, -1.f));

		// An UNCHANGED timestamp does not recenter again.
		f.nav.n = pose_at(-9.f, 0.f, 7.f);
		check_pose_eq(f.poll(2002), pose_at(0.f, 1.6f, -2.f));
	}

	SECTION("t < r: the recenter stays pending and the rig holds")
	{
		const struct xrt_pose held = pose_at(3.f, 1.6f, -4.f);
		fake_nav_publish_recenter(&f.nav, 5000, pose_at(-9.f, 0.f, 9.f));

		// Requested time is before the reset: hold, stay pending.
		check_pose_eq(f.poll(4000), held);
		check_pose_eq(f.poll(4999), held);

		// At/after the reset time it fires.
		check_pose_eq(f.poll(5000), home);
	}

	SECTION("two resets between polls collapse to one")
	{
		fake_nav_publish_recenter(&f.nav, 2000, pose_at(1.f, 0.f, 1.f));
		fake_nav_publish_recenter(&f.nav, 2500, pose_at(-9.f, 0.f, 9.f));

		// One recenter, aligned against the N delivered with the LATEST reset.
		check_pose_eq(f.poll(3000), home);

		f.nav.n = pose_at(-9.f, 0.f, 8.f);
		check_pose_eq(f.poll(3001), pose_at(0.f, 1.6f, -1.f));

		// Nothing left pending: no second jump home.
		f.nav.n = pose_at(-9.f, 0.f, 7.f);
		check_pose_eq(f.poll(3002), pose_at(0.f, 1.6f, -2.f));
	}
}


/*
 *
 * 5. Recenter while N is invalid.
 *
 */

TEST_CASE("rig_composer: a recenter published while N is invalid stays pending until N returns")
{
	const struct xrt_pose home = pose_at(0.f, 1.6f, 0.f);
	composer_fixture f(home);

	set_rig_role(&f.sysd, 0);
	f.poll(1000);
	f.nav.n = pose_at(3.f, 0.f, -4.f);
	const struct xrt_pose held = f.poll(1000);
	check_pose_eq(held, pose_at(3.f, 1.6f, -4.f));

	// Reset arrives during optical loss.
	f.nav.valid = false;
	fake_nav_publish_recenter(&f.nav, 2000, pose_at(-9.f, 0.f, 9.f));

	check_pose_eq(f.poll(2001), held);
	check_pose_eq(f.poll(2002), held);

	// N returns: the pending recenter fires now.
	f.nav.valid = true;
	check_pose_eq(f.poll(2003), home);

	// And it aligned against the N it saw when it fired.
	f.nav.n = pose_at(-9.f, 0.f, 8.f);
	check_pose_eq(f.poll(2004), pose_at(0.f, 1.6f, -1.f));
}


/*
 *
 * 6. A pending recenter does not survive its provider.
 *
 */

TEST_CASE("rig_composer: a pending recenter dies with its provider and does not fire for the next one")
{
	const struct xrt_pose home = pose_at(0.f, 1.6f, 0.f);
	composer_fixture f(home);

	set_rig_role(&f.sysd, 0);
	f.poll(1000);

	// Drive away from home so "did it recenter?" is not hiding behind the
	// seed pose.
	f.nav.n = pose_at(3.f, 0.f, -4.f);
	const struct xrt_pose held = f.poll(1000);
	check_pose_eq(held, pose_at(3.f, 1.6f, -4.f));

	// A recenter is raised, but N is invalid — so it is still PENDING when
	// the provider disappears.
	f.nav.valid = false;
	fake_nav_publish_recenter(&f.nav, 2000, pose_at(-9.f, 0.f, 9.f));
	check_pose_eq(f.poll(2001), held);

	// The provider unplugs: back to the qwerty floor.
	set_rig_role(&f.sysd, -1);
	check_pose_eq(f.poll(2002), held);
	CHECK(g_qwerty_set_calls == 1);

	// A new provider takes the role, still republishing the SAME (stale)
	// recenter timestamp. The rig must continue at `held` — the pending
	// request died with the previous tenure, and the timestamp is not newer
	// than the one already consumed.
	f.nav.valid = true;
	f.nav.n = pose_at(5.f, 0.f, 5.f);
	set_rig_role(&f.sysd, 0);
	check_pose_eq(f.poll(2003), held);

	// Alignment really is at `held`, not at home: a provider-local step of
	// one metre along -Z moves the rig one metre along -Z from `held`.
	f.nav.n = pose_at(5.f, 0.f, 4.f);
	check_pose_eq(f.poll(2004), pose_at(3.f, 1.6f, -5.f));

	// Republishing the stale timestamp outright changes nothing either.
	fake_nav_publish_recenter(&f.nav, 2000, pose_at(5.f, 0.f, 4.f));
	check_pose_eq(f.poll(2005), pose_at(3.f, 1.6f, -5.f));

	// Only a NEWER timestamp recenters — and then it lands exactly on home.
	fake_nav_publish_recenter(&f.nav, 3000, pose_at(-2.f, 0.f, 7.f));
	check_pose_eq(f.poll(3001), home);
}


/*
 *
 * 7. Handover back to qwerty.
 *
 */

TEST_CASE("rig_composer: handback reseeds qwerty at the current rig")
{
	composer_fixture f(pose_at(0.f, 1.6f, 0.f));

	set_rig_role(&f.sysd, 0);
	f.poll();

	// Drive to a non-identity rig with a yaw, through the provider.
	const float deg45 = 45.f * 3.14159265358979323846f / 180.f;
	f.nav.n = pose_yaw_at(deg45, 2.f, 0.f, -5.f);
	const struct xrt_pose rig_last = f.poll();
	CHECK(g_qwerty_set_calls == 0);

	// Unplug: the rig role falls back to the qwerty floor.
	set_rig_role(&f.sysd, -1);

	// Whatever qwerty's own integrator drifted to while it was not being
	// read must NOT leak back into the rig -- the setter overwrites it.
	f.qwerty.pose = pose_at(99.f, 99.f, 99.f);

	check_pose_eq(f.poll(), rig_last);

	CHECK(g_qwerty_set_calls == 1);
	check_pose_eq(g_qwerty_set_pose, rig_last);

	// WASD continues from there.
	struct xrt_pose walked = rig_last;
	walked.position.x += 1.f;
	f.qwerty.pose = walked;
	check_pose_eq(f.poll(), walked);

	// Back to the provider: continuity again, from the qwerty value.
	f.nav.n = pose_at(11.f, 0.f, 11.f);
	set_rig_role(&f.sysd, 0);
	check_pose_eq(f.poll(), walked);
}


/*
 *
 * 8. Hand-role churn must not disturb the rig.
 *
 */

TEST_CASE("rig_composer: hand-role churn does not re-align the rig")
{
	composer_fixture f(pose_at(0.f, 1.6f, 0.f));

	set_rig_role(&f.sysd, 0);
	f.poll();

	// Mid-drag.
	f.nav.n = pose_at(0.f, 0.f, -1.f);
	check_pose_eq(f.poll(), pose_at(0.f, 1.6f, -1.f));

	// A controller hotplug bumps the SHARED generation counter and changes
	// the hand roles — the rig index is unchanged, so nothing re-aligns and
	// the in-progress navigation step is not swallowed.
	f.sysd.left = 0;
	f.sysd.right = 0;
	f.sysd.generation++;

	f.nav.n = pose_at(0.f, 0.f, -2.f);
	check_pose_eq(f.poll(), pose_at(0.f, 1.6f, -2.f));

	f.sysd.left = -1;
	f.sysd.generation++;

	f.nav.n = pose_at(0.f, 0.f, -3.f);
	check_pose_eq(f.poll(), pose_at(0.f, 1.6f, -3.f));

	// qwerty was never reseeded: the rig never came back to the floor.
	CHECK(g_qwerty_set_calls == 0);
}


/*
 *
 * 9. Concurrency smoke.
 *
 */

TEST_CASE("rig_composer: concurrent polls while the rig role flips")
{
	composer_fixture f(pose_at(0.f, 1.6f, 0.f));

	std::atomic<bool> stop{false};
	std::atomic<int> bad{0};

	auto poller = [&]() {
		int64_t t = 1;
		while (!stop.load()) {
			struct xrt_space_relation rel = XRT_SPACE_RELATION_ZERO;
			if (xrt_device_get_tracked_pose(f.composer, XRT_INPUT_GENERIC_HEAD_POSE, t++, &rel) !=
			    XRT_SUCCESS) {
				bad++;
				continue;
			}
			if (!std::isfinite(rel.pose.position.x) || !std::isfinite(rel.pose.position.y) ||
			    !std::isfinite(rel.pose.position.z) || !std::isfinite(rel.pose.orientation.x) ||
			    !std::isfinite(rel.pose.orientation.y) || !std::isfinite(rel.pose.orientation.z) ||
			    !std::isfinite(rel.pose.orientation.w)) {
				bad++;
			}
		}
	};

	std::thread a(poller);
	std::thread b(poller);

	std::thread flipper([&]() {
		for (int i = 0; i < 2000; i++) {
			set_rig_role(&f.sysd, (i & 1) ? 0 : -1);
			f.nav.valid = (i % 3) != 0;
			f.nav.n = pose_at((float)(i % 7), 0.f, -(float)(i % 5));
		}
	});

	flipper.join();
	stop = true;
	a.join();
	b.join();

	CHECK(bad.load() == 0);
}
