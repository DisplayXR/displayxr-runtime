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

/*
 * Rig follow-up (David, two panels, 2026-10-11): laptop camera blocked, face
 * in front of the DS1, two workspace tiles (one per panel).
 */

namespace {

constexpr uint64_t kFrame = 16666667ull; // 60 Hz
constexpr uint64_t kPrimary = 0;         // screen ids: 0 = "the primary" for the picker
constexpr uint64_t kDs1 = 0xb72ea4c616544d01ull;
constexpr uint64_t kOther = 0x1234ull;
constexpr uint64_t kDwell = U_TRACKING_SOURCE_DWELL_NS;

} // namespace

TEST_CASE("tracking_any: a secondary whose validity flickers per frame keeps the session tracked",
          "[tracking_any][rig]")
{
	// Primary untracked throughout; the secondary's DP answers tracked on
	// every other frame only (e.g. valid only right after its weave).
	u_tracking_any t{};
	uint64_t now = 0;
	CHECK(feed(t, now, false, true));
	for (int f = 1; f < 600; f++) { // 10 s
		now += kFrame;
		const bool secondary = (f % 2) == 0;
		CHECK(feed(t, now, false, secondary)); // never a false edge
	}
	// Worse flicker: tracked one frame in every 25 (gap 24 frames = 400 ms < hold).
	for (int f = 1; f < 600; f++) {
		now += kFrame;
		CHECK(feed(t, now, false, (f % 25) == 0));
	}
}

TEST_CASE("tracking_any: workspace scope — a tile on the primary counts every secondary presenter",
          "[tracking_any][rig]")
{
	// The tile on the laptop panel: its footprint touches only the primary.
	// Under the old footprint scope nothing else presented for it
	// (other_count 0 -> the pass-through), so blocking the laptop camera
	// dropped it at once although the DS1 saw the viewer.
	u_tracking_any footprint{};
	CHECK(feed(footprint, 0, true, false, 0));
	CHECK_FALSE(feed(footprint, kFrame, false, false, 0));

	// Workspace scope: every secondary presenter is noted, whatever the
	// tile's footprint. Each screen counts once.
	u_tracking_others o{};
	u_tracking_others_note(&o, kDs1, true);
	u_tracking_others_note(&o, kDs1, true);    // a segment DP on the same screen
	u_tracking_others_note(&o, kOther, false); // a third panel, nobody there
	u_tracking_others_note(&o, 0, true);       // "no screen" is ignored
	CHECK(o.count == 2);
	CHECK(o.tracked_screen == kDs1);

	u_tracking_any ws{};
	CHECK(feed(ws, 0, true, false, o.count));
	// Laptop camera blocked; the DS1 still sees him: the tile stays tracked.
	for (int f = 1; f < 120; f++) {
		CHECK(feed(ws, f * kFrame, false, o.tracked_screen != 0, o.count));
	}
}

TEST_CASE("tracking_any: others collector caps and keeps the first tracked screen", "[tracking_any]")
{
	u_tracking_others o{};
	u_tracking_others_note(&o, 7, false);
	u_tracking_others_note(&o, 8, true);
	u_tracking_others_note(&o, 9, true);
	CHECK(o.tracked_screen == 8);
	for (uint64_t id = 100; id < 100 + 2 * U_TRACKING_OTHERS_MAX; id++) {
		u_tracking_others_note(&o, id, false);
	}
	CHECK(o.count == U_TRACKING_OTHERS_MAX);
	// An already-known screen still reports when it acquires.
	u_tracking_others o2{};
	u_tracking_others_note(&o2, 5, false);
	u_tracking_others_note(&o2, 5, true);
	CHECK(o2.count == 1);
	CHECK(o2.tracked_screen == 5);
}

TEST_CASE("tracking_source: a shared presence edge a frame apart never flips the FRAME_TICK viewer",
          "[tracking_any][rig]")
{
	// The rig log: the primary is asked first in a frame, the DS1 second.
	// A machine-wide presence change landing between the two reads makes,
	// for exactly one frame, primary=lost + DS1=tracked (on loss) or the
	// reverse (on acquire). The old pick published the DS1 for that frame.
	u_tracking_source s{};
	uint64_t now = 0;
	CHECK(u_tracking_source_update(&s, now, true, 0, kDwell) == kPrimary);
	now += kFrame; // loss lands between the reads
	CHECK(u_tracking_source_update(&s, now, false, kDs1, kDwell) == kPrimary);
	now += kFrame; // both lost
	CHECK(u_tracking_source_update(&s, now, false, 0, kDwell) == kPrimary);
	CHECK(s.pending == 0);
	now += 10 * kFrame; // acquire lands between the reads
	CHECK(u_tracking_source_update(&s, now, false, kDs1, kDwell) == kPrimary);
	now += kFrame;
	CHECK(u_tracking_source_update(&s, now, true, kDs1, kDwell) == kPrimary);
}

TEST_CASE("tracking_source: a secondary seeing the viewer alone is followed after the dwell", "[tracking_any]")
{
	u_tracking_source s{};
	uint64_t now = 0;
	CHECK(u_tracking_source_update(&s, now, false, kDs1, kDwell) == kPrimary); // dwell starts
	now += kDwell - 1;
	CHECK(u_tracking_source_update(&s, now, false, kDs1, kDwell) == kPrimary);
	now += 1;
	CHECK(u_tracking_source_update(&s, now, false, kDs1, kDwell) == kDs1);
	now += kFrame;
	CHECK(u_tracking_source_update(&s, now, false, kDs1, kDwell) == kDs1); // kept
	// The primary acquires: back at once.
	now += kFrame;
	CHECK(u_tracking_source_update(&s, now, true, kDs1, kDwell) == kPrimary);
	// A different secondary must dwell afresh.
	now += kFrame;
	CHECK(u_tracking_source_update(&s, now, false, kOther, kDwell) == kPrimary);
	now += kDwell;
	CHECK(u_tracking_source_update(&s, now, false, kOther, kDwell) == kOther);
	// Nobody anywhere: the primary's (untracked) viewer.
	now += kFrame;
	CHECK(u_tracking_source_update(&s, now, false, 0, kDwell) == kPrimary);
}
