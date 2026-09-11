// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The sim_input scripted NAVIGATION device (#1380, ADR-034 Amendment 4).
 *
 * The hardware-free reference rig driver has exactly three properties the rig
 * composer is specified against, and all three are testable with nothing but
 * the device's own vtable:
 *
 *   1. `NAVIGATION_POSE` is a PURE FUNCTION of the requested timestamp — the
 *      property a real provider buys with `m_relation_history` + prediction
 *      and this one gets by construction. Without it a CI run is not
 *      reproducible and a composer regression cannot be attributed.
 *   2. Its relation flags are `POSITION_VALID | ORIENTATION_VALID` and
 *      nothing else — deliberately NOT the TRACKED bits, which the composer
 *      ignores for this input — and they CLEAR inside a scripted hold window,
 *      which is the provider saying "hold the rig".
 *   3. `NAVIGATION_RECENTER` is the durable level-with-timestamp of the
 *      contract (true forever, timestamp = the most recent reset, never
 *      cleared, monotonic), and the reset it names is PAIRED with a jump in
 *      the scripted pose — the same publication, exactly as the composer's
 *      recenter alignment assumes.
 *
 * The device is compiled straight into the unit (see tests/CMakeLists.txt): it
 * needs no plug-in, no loader and no display.
 */

#include "catch_amalgamated.hpp"

#include "xrt/xrt_device.h"
#include "xrt/xrt_tracking.h"

#include "sim_input_interface.h"

#include <cmath>


namespace {

//! One lap of the scripted figure-eight, as sim_input_nav.c defines it.
constexpr int64_t kNavPeriodNs = 20LL * 1000 * 1000 * 1000;

struct device_guard
{
	struct xrt_device *xdev = nullptr;
	explicit device_guard(struct xrt_device *d) : xdev(d) {}
	~device_guard()
	{
		if (xdev != nullptr) {
			xrt_device_destroy(&xdev);
		}
	}
	device_guard(const device_guard &) = delete;
	device_guard &
	operator=(const device_guard &) = delete;
};

struct xrt_space_relation
sample(struct xrt_device *xdev, int64_t at_ns)
{
	struct xrt_space_relation rel = XRT_SPACE_RELATION_ZERO;
	REQUIRE(xrt_device_get_tracked_pose(xdev, XRT_INPUT_GENERIC_NAVIGATION_POSE, at_ns, &rel) == XRT_SUCCESS);
	return rel;
}

float
distance(const struct xrt_pose &a, const struct xrt_pose &b)
{
	const float dx = a.position.x - b.position.x;
	const float dy = a.position.y - b.position.y;
	const float dz = a.position.z - b.position.z;
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

struct xrt_input *
find_input(struct xrt_device *xdev, enum xrt_input_name name)
{
	for (size_t i = 0; i < xdev->input_count; i++) {
		if (xdev->inputs[i].name == name) {
			return &xdev->inputs[i];
		}
	}
	return nullptr;
}

} // namespace


TEST_CASE("sim_input nav: the device self-describes as the rig source")
{
	device_guard g(sim_input_create_navigation(0, 0));
	REQUIRE(g.xdev != nullptr);

	// The arbiter picks the rig holder on device_type alone, and the
	// composer finds RECENTER by input name. Both are the contract.
	CHECK(g.xdev->device_type == XRT_DEVICE_TYPE_NAVIGATION);
	CHECK(find_input(g.xdev, XRT_INPUT_GENERIC_NAVIGATION_POSE) != nullptr);
	CHECK(find_input(g.xdev, XRT_INPUT_GENERIC_NAVIGATION_RECENTER) != nullptr);

	// N lives in the provider's PRIVATE frame F, which the runtime aligns
	// against — it is not a rig-local volume, so it must not be anchored at
	// the initial rig the way the controllers are.
	REQUIRE(g.xdev->tracking_origin != nullptr);
	CHECK(g.xdev->tracking_origin->type == XRT_TRACKING_TYPE_OTHER);

	// Anything but NAVIGATION_POSE is a caller error, not a silent identity.
	struct xrt_space_relation rel = XRT_SPACE_RELATION_ZERO;
	CHECK(xrt_device_get_tracked_pose(g.xdev, XRT_INPUT_GENERIC_HEAD_POSE, 1000, &rel) ==
	      XRT_ERROR_INPUT_UNSUPPORTED);
}

TEST_CASE("sim_input nav: the pose is a pure function of the requested timestamp")
{
	device_guard a(sim_input_create_navigation(0, 0));
	device_guard b(sim_input_create_navigation(0, 0));
	REQUIRE(a.xdev != nullptr);
	REQUIRE(b.xdev != nullptr);

	const int64_t t = 7LL * 1000 * 1000 * 1000 + 123456789LL;

	const struct xrt_space_relation r1 = sample(a.xdev, t);
	const struct xrt_space_relation r2 = sample(a.xdev, t);
	// A second, independently created device: no hidden per-device epoch.
	const struct xrt_space_relation r3 = sample(b.xdev, t);

	CHECK(r1.pose.position.x == r2.pose.position.x);
	CHECK(r1.pose.position.z == r2.pose.position.z);
	CHECK(r1.pose.orientation.y == r2.pose.orientation.y);
	CHECK(r3.pose.position.x == r1.pose.position.x);
	CHECK(r3.pose.position.z == r1.pose.position.z);
	CHECK(r3.pose.orientation.y == r1.pose.orientation.y);

	// Out-of-order queries (the composer answers historical AND predicted
	// head times) are answered from the script, never from a cursor.
	const struct xrt_space_relation past = sample(a.xdev, t - 250 * 1000 * 1000);
	const struct xrt_space_relation again = sample(a.xdev, t);
	CHECK(again.pose.position.x == r1.pose.position.x);
	CHECK(distance(past.pose, r1.pose) > 0.0f);
}

TEST_CASE("sim_input nav: the pose moves, stays finite and stays bounded")
{
	device_guard g(sim_input_create_navigation(0, 0));
	REQUIRE(g.xdev != nullptr);

	struct xrt_pose first = sample(g.xdev, kNavPeriodNs / 8).pose;
	bool moved = false;

	for (int i = 0; i < 64; i++) {
		const int64_t t = kNavPeriodNs / 8 + (int64_t)i * (kNavPeriodNs / 64);
		const struct xrt_pose p = sample(g.xdev, t).pose;

		CHECK(std::isfinite(p.position.x));
		CHECK(std::isfinite(p.position.y));
		CHECK(std::isfinite(p.position.z));
		CHECK(std::isfinite(p.orientation.w));

		// Half-width 0.5 m, y pinned to the plane: a rig that wandered
		// out of that box would mean the script, not the composer, is
		// what a failing integration test is measuring.
		CHECK(std::fabs(p.position.x) <= 0.5001f);
		CHECK(p.position.y == 0.0f);
		CHECK(std::fabs(p.position.z) <= 0.2501f);

		const float qlen = std::sqrt(p.orientation.x * p.orientation.x + p.orientation.y * p.orientation.y +
		                             p.orientation.z * p.orientation.z + p.orientation.w * p.orientation.w);
		CHECK(qlen == Catch::Approx(1.0f).margin(1e-5));

		moved = moved || distance(p, first) > 1e-4f;
	}

	CHECK(moved);
}

TEST_CASE("sim_input nav: valid bits are VALID-only, and clear inside a hold window")
{
	SECTION("no hold knob: always valid")
	{
		device_guard g(sim_input_create_navigation(0, 0));
		REQUIRE(g.xdev != nullptr);

		const struct xrt_space_relation rel = sample(g.xdev, 3LL * 1000 * 1000 * 1000);
		CHECK((rel.relation_flags & XRT_SPACE_RELATION_POSITION_VALID_BIT) != 0);
		CHECK((rel.relation_flags & XRT_SPACE_RELATION_ORIENTATION_VALID_BIT) != 0);
		// The composer ignores TRACKED for this input by contract, and a
		// scripted pose has no tracker behind it — so they must be clear.
		CHECK((rel.relation_flags & XRT_SPACE_RELATION_POSITION_TRACKED_BIT) == 0);
		CHECK((rel.relation_flags & XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT) == 0);
	}

	SECTION("hold knob: invalid for the first 500 ms of every period")
	{
		const int64_t hold_ms = 2000;
		const int64_t hold_ns = hold_ms * 1000 * 1000;
		device_guard g(sim_input_create_navigation(hold_ms, 0));
		REQUIRE(g.xdev != nullptr);

		const int64_t base = 100 * hold_ns; // a period boundary

		// Inside the window: both VALID bits gone — "hold the rig".
		for (int64_t off_ms : {0LL, 1LL, 250LL, 499LL}) {
			const struct xrt_space_relation rel = sample(g.xdev, base + off_ms * 1000 * 1000);
			INFO("offset ms = " << off_ms);
			CHECK(rel.relation_flags == 0);
		}

		// Outside it: valid again, so the composer re-aligns at the held
		// rig rather than snapping.
		for (int64_t off_ms : {500LL, 501LL, 1200LL, 1999LL}) {
			const struct xrt_space_relation rel = sample(g.xdev, base + off_ms * 1000 * 1000);
			INFO("offset ms = " << off_ms);
			CHECK((rel.relation_flags & XRT_SPACE_RELATION_POSITION_VALID_BIT) != 0);
			CHECK((rel.relation_flags & XRT_SPACE_RELATION_ORIENTATION_VALID_BIT) != 0);
		}
	}
}

TEST_CASE("sim_input nav: recenter is durable, monotonic and paired with the phase jump")
{
	// The recenter period is set equal to the figure-eight period, which
	// puts every reset boundary at theta = 0 (mod 2*pi) and makes the
	// phase jump provably large — this test asserts the PAIRING, so it must
	// not be able to fail for a merely unlucky sampling point.
	const int64_t period_ms = 20000;
	const int64_t period_ns = period_ms * 1000 * 1000;
	device_guard g(sim_input_create_navigation(0, period_ms));
	REQUIRE(g.xdev != nullptr);

	struct xrt_input *rc = find_input(g.xdev, XRT_INPUT_GENERIC_NAVIGATION_RECENTER);
	REQUIRE(rc != nullptr);

	REQUIRE(xrt_device_update_inputs(g.xdev) == XRT_SUCCESS);
	const int64_t ts1 = rc->timestamp;

	// Durable LEVEL, not a pulse: xrSyncActions sweeps update_inputs over
	// every device, so a pulse would be consumed before the composer ever
	// polled. Value stays true; the timestamp names the reset.
	CHECK(rc->active);
	CHECK(rc->value.boolean);
	CHECK(ts1 > 0);
	CHECK(ts1 % period_ns == 0);

	// Repeated sweeps must not invent a newer reset — the composer consumes
	// on `timestamp > last`, so a moving timestamp would recenter forever.
	for (int i = 0; i < 5; i++) {
		REQUIRE(xrt_device_update_inputs(g.xdev) == XRT_SUCCESS);
		CHECK(rc->value.boolean);
		CHECK(rc->timestamp >= ts1); // monotonic
		CHECK(rc->timestamp % period_ns == 0);
	}

	// …and the reset that timestamp names is the SAME event that moved the
	// scripted pose. Straddle a boundary by 1 ms either side: the jump must
	// dwarf the ~0.16 m/s the script can travel in 2 ms (< 0.001 m).
	const int64_t boundary = 1000 * period_ns;
	const int64_t ms = 1000 * 1000;

	const struct xrt_pose before = sample(g.xdev, boundary - ms).pose;
	const struct xrt_pose after = sample(g.xdev, boundary + ms).pose;
	CHECK(distance(before, after) > 0.05f);

	// Two samples the same distance apart but NOT straddling a boundary
	// move continuously — so the jump above is the reset, not the script.
	const struct xrt_pose a = sample(g.xdev, boundary + 10 * ms).pose;
	const struct xrt_pose b = sample(g.xdev, boundary + 12 * ms).pose;
	CHECK(distance(a, b) < 0.01f);

	// The recenter timestamp is exactly the boundary, so a query AT it is
	// already post-reset: that is the "same publication" the composer's
	// recenter alignment is specified against.
	const struct xrt_pose at = sample(g.xdev, boundary).pose;
	CHECK(distance(at, after) < 0.01f);
	CHECK(distance(at, before) > 0.05f);
}

TEST_CASE("sim_input nav: with the recenter knob off, nothing ever asks for one")
{
	device_guard g(sim_input_create_navigation(0, 0));
	REQUIRE(g.xdev != nullptr);

	struct xrt_input *rc = find_input(g.xdev, XRT_INPUT_GENERIC_NAVIGATION_RECENTER);
	REQUIRE(rc != nullptr);

	for (int i = 0; i < 3; i++) {
		REQUIRE(xrt_device_update_inputs(g.xdev) == XRT_SUCCESS);
		CHECK(rc->active);
		CHECK_FALSE(rc->value.boolean);
	}

	// And the pose has no phase jumps at all: the whole timeline is one
	// continuous figure-eight.
	const int64_t ms = 1000 * 1000;
	for (int i = 0; i < 32; i++) {
		const int64_t t = 500LL * 1000 * ms + (int64_t)i * 400 * ms;
		const struct xrt_pose p0 = sample(g.xdev, t).pose;
		const struct xrt_pose p1 = sample(g.xdev, t + 2 * ms).pose;
		INFO("step " << i);
		CHECK(distance(p0, p1) < 0.01f);
	}
}
