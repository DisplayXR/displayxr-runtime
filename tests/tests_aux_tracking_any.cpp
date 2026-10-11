// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Session tracking across screens: the OR + hand-over debounce.
 *
 * Pins @ref u_tracking_any_update, the rule every session-level tracking
 * consumer runs on since ADR-047 Amendment 7 (the state tracker's
 * XrViewEyeTrackingStateDXR / tracking-state event, the service's per-client
 * eye query and XR_DXR_weave eyesTracking): a session is tracked when ANY
 * display processor presenting for it sees a viewer, it never flaps on a
 * screen hand-over, and a session on one screen behaves exactly as before.
 */

#include "catch_amalgamated.hpp"

#include "util/u_tracking_any.h"

namespace {

constexpr uint64_t kMs = 1000ull * 1000ull;
constexpr uint64_t kHold = U_TRACKING_ANY_HOLD_NS;

//! One sample: own DP, the other DPs' OR, how many other DPs presented.
bool
feed(u_tracking_any &t, uint64_t now, bool own, bool others, uint32_t count = 1)
{
	return u_tracking_any_update(&t, now, own, others, count, kHold);
}

} // namespace

TEST_CASE("tracking_any: primary untracked, secondary tracked -> tracked", "[tracking_any]")
{
	u_tracking_any t{};
	CHECK_FALSE(feed(t, 0, false, false));
	CHECK(feed(t, 10 * kMs, false, true)); // rises immediately through the other screen
	CHECK(feed(t, 20 * kMs, false, true));
	CHECK(feed(t, 30 * kMs, true, false)); // the viewer moves to the primary
}

TEST_CASE("tracking_any: both untracked longer than the hold -> untracked", "[tracking_any]")
{
	u_tracking_any t{};
	CHECK(feed(t, 0, false, true));
	CHECK(feed(t, 1 * kMs, false, false));                   // hold starts
	CHECK(feed(t, 1 * kMs + kHold - 1, false, false));       // still held
	CHECK_FALSE(feed(t, 1 * kMs + kHold, false, false));     // hold elapsed
	CHECK_FALSE(feed(t, 1 * kMs + 2 * kHold, false, false)); // and stays down
}

TEST_CASE("tracking_any: alternation between screens never flaps", "[tracking_any]")
{
	u_tracking_any t{};
	CHECK(feed(t, 0, true, false));
	uint64_t now = 0;
	// The viewer walks back and forth: each screen loses before the other
	// acquires, with gaps shorter than the hold (and no gap at all).
	const uint64_t gaps[] = {0, 100 * kMs, kHold - 1, 250 * kMs, 0, kHold / 2};
	bool on_primary = true;
	for (uint64_t gap : gaps) {
		now += 300 * kMs;
		CHECK(feed(t, now, on_primary, !on_primary)); // tracked on one screen
		now += 1;
		CHECK(feed(t, now, false, false)); // the hand-over gap starts
		now += gap;
		CHECK(feed(t, now, false, false)); // still inside the hold
		on_primary = !on_primary;
		now += 1;
		CHECK(feed(t, now, on_primary, !on_primary)); // the other screen acquires
	}
}

TEST_CASE("tracking_any: one presenting DP is the pass-through, unchanged", "[tracking_any]")
{
	u_tracking_any t{};
	// other_count == 0: a window on one screen (every single-screen box).
	CHECK(feed(t, 0, true, false, 0));
	CHECK_FALSE(feed(t, 1 * kMs, false, false, 0)); // falls at once: MANUAL's "immediate" intact
	CHECK(feed(t, 2 * kMs, true, false, 0));
	// A stale "others" flag with no other DP presenting is ignored.
	CHECK_FALSE(feed(t, 3 * kMs, false, true, 0));
}

TEST_CASE("tracking_any: a hold in flight ends when the window leaves the other screen", "[tracking_any]")
{
	u_tracking_any t{};
	CHECK(feed(t, 0, false, true, 1));
	CHECK(feed(t, 1 * kMs, false, false, 1)); // held
	// The window is now wholly on the (untracked) own screen: nothing else
	// presents, so nothing is held.
	CHECK_FALSE(feed(t, 2 * kMs, false, false, 0));
}

TEST_CASE("tracking_any: reacquire inside the hold cancels it", "[tracking_any]")
{
	u_tracking_any t{};
	CHECK(feed(t, 0, false, true));
	CHECK(feed(t, 1 * kMs, false, false));
	CHECK(feed(t, 100 * kMs, false, true)); // back before the hold elapsed
	// A fresh loss starts a fresh hold, measured from the new fall.
	CHECK(feed(t, 200 * kMs, false, false));
	CHECK(feed(t, 200 * kMs + kHold - 1, false, false));
	CHECK_FALSE(feed(t, 200 * kMs + kHold, false, false));
}

TEST_CASE("tracking_any: reset and the presence hook", "[tracking_any]")
{
	u_tracking_any t{};
	CHECK(feed(t, 0, true, false));
	u_tracking_any_reset(&t);
	CHECK_FALSE(t.state);
	CHECK_FALSE(t.falling);

	xrt_eye_positions e{};
	CHECK_FALSE(u_tracking_dp_viewer_present(&e));
	CHECK_FALSE(u_tracking_dp_viewer_present(nullptr));
	e.valid = true;
	CHECK_FALSE(u_tracking_dp_viewer_present(&e)); // nominal / fallback eyes: valid, nobody tracked
	e.is_tracking = true;
	CHECK(u_tracking_dp_viewer_present(&e));
	e.valid = false;
	CHECK_FALSE(u_tracking_dp_viewer_present(&e)); // no answer is not a viewer
}
