// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  #918 bridge-plane transport policy tests.
 * @ingroup tests
 *
 * These cover the decisions the #918 review found wrong, at the level they are
 * actually taken. None of them need a GPU: the bugs were never in the D3D12
 * calls, they were in what to copy, how far, and when a slot may be called
 * current.
 */

#include "comp_d3d11_plane_policy.h"

#include "catch_amalgamated.hpp"

static struct xb_plane_box
box(int32_t x, int32_t y, uint32_t w, uint32_t h)
{
	struct xb_plane_box b = {x, y, w, h};
	return b;
}

TEST_CASE("xb_plane_box_union accumulates what a slot owes")
{
	SECTION("an empty accumulator becomes the addend")
	{
		struct xb_plane_box acc = box(0, 0, 0, 0);
		const struct xb_plane_box add = box(10, 20, 30, 40);
		xb_plane_box_union(&acc, &add);
		CHECK(acc.x == 10);
		CHECK(acc.y == 20);
		CHECK(acc.w == 30);
		CHECK(acc.h == 40);
	}

	SECTION("an empty addend changes nothing")
	{
		struct xb_plane_box acc = box(10, 20, 30, 40);
		const struct xb_plane_box add = box(0, 0, 0, 0);
		xb_plane_box_union(&acc, &add);
		CHECK(acc.x == 10);
		CHECK(acc.w == 30);
	}

	SECTION("disjoint boxes give the bounding box")
	{
		struct xb_plane_box acc = box(0, 0, 10, 10);
		const struct xb_plane_box add = box(90, 90, 10, 10);
		xb_plane_box_union(&acc, &add);
		CHECK(acc.x == 0);
		CHECK(acc.y == 0);
		CHECK(acc.w == 100);
		CHECK(acc.h == 100);
	}

	/*
	 * #918 review F4 — the ghost. An overlay in the far corner, then the window
	 * shrinks and the content moves: the union must still cover where the content
	 * WAS, or the pixels it vacated are never refreshed and stay on the plane for
	 * ever. The caller folds in the previous box for exactly this reason.
	 */
	SECTION("the union covers where content was as well as where it is")
	{
		struct xb_plane_box acc = box(0, 0, 0, 0);
		const struct xb_plane_box was = box(1800, 900, 100, 100);
		const struct xb_plane_box now = box(10, 10, 100, 100);
		xb_plane_box_union(&acc, &was);
		xb_plane_box_union(&acc, &now);
		CHECK(acc.x == 10);
		CHECK(acc.y == 10);
		CHECK(acc.x + (int32_t)acc.w == 1900);
		CHECK(acc.y + (int32_t)acc.h == 1000);
	}
}

TEST_CASE("xb_plane_box_clamp keeps a box inside the transportable extent")
{
	SECTION("a box already inside is unchanged")
	{
		const struct xb_plane_box in = box(4, 4, 16, 16);
		const struct xb_plane_box out = xb_plane_box_clamp(&in, 100, 100);
		CHECK(out.x == 4);
		CHECK(out.w == 16);
		CHECK(out.h == 16);
	}

	SECTION("a box hanging off the right/bottom is trimmed, not dropped")
	{
		const struct xb_plane_box in = box(90, 90, 40, 40);
		const struct xb_plane_box out = xb_plane_box_clamp(&in, 100, 100);
		CHECK(out.x == 90);
		CHECK(out.w == 10);
		CHECK(out.h == 10);
	}

	SECTION("negative origins clip to zero")
	{
		const struct xb_plane_box in = box(-20, -30, 40, 40);
		const struct xb_plane_box out = xb_plane_box_clamp(&in, 100, 100);
		CHECK(out.x == 0);
		CHECK(out.y == 0);
		CHECK(out.w == 20);
		CHECK(out.h == 10);
	}

	SECTION("a box entirely outside comes back empty")
	{
		const struct xb_plane_box in = box(200, 200, 40, 40);
		const struct xb_plane_box out = xb_plane_box_clamp(&in, 100, 100);
		CHECK(xb_plane_box_empty(&out));
	}
}

/*
 * #918 review F5/F6. The 4-pixel snap is what pushes a box out of range, and an
 * over-range CopyTextureRegion is DROPPED rather than failed — so the clamp after
 * the snap is the only thing standing between an app-chosen mask dimension and a
 * destination nothing ever wrote.
 */
TEST_CASE("xb_plane_box_snap_clamp snaps out, then clamps in")
{
	SECTION("an unaligned box grows to the 4-pixel grid")
	{
		const struct xb_plane_box in = box(5, 7, 10, 10);
		const struct xb_plane_box out = xb_plane_box_snap_clamp(&in, 1000, 1000);
		CHECK(out.x == 4);
		CHECK(out.y == 4);
		CHECK(out.x + (int32_t)out.w == 16);
		CHECK(out.y + (int32_t)out.h == 20);
		CHECK(out.x % 4 == 0);
		CHECK(out.y % 4 == 0);
	}

	SECTION("the snap never escapes a source that is not a multiple of 4")
	{
		// A mask created at 1366x769: snapping the full-plane refresh out would
		// ask for 1368x772 and the copy would silently not happen.
		const struct xb_plane_box in = box(0, 0, 1366, 769);
		const struct xb_plane_box out = xb_plane_box_snap_clamp(&in, 1366, 769);
		CHECK(out.x == 0);
		CHECK(out.y == 0);
		CHECK(out.w == 1366);
		CHECK(out.h == 769);
	}

	SECTION("a box that snaps entirely past the extent comes back empty")
	{
		const struct xb_plane_box in = box(1400, 0, 8, 8);
		const struct xb_plane_box out = xb_plane_box_snap_clamp(&in, 1366, 769);
		CHECK(xb_plane_box_empty(&out));
	}
}

TEST_CASE("xb_plane_limits is min(source, allocation), with 0 meaning unknown")
{
	uint32_t w = 0, h = 0;

	SECTION("a mask-sized plane transports the whole mask")
	{
		xb_plane_limits(1920, 1080, 1920, 1080, &w, &h);
		CHECK(w == 1920);
		CHECK(h == 1080);
	}

	SECTION("a source smaller than its allocation limits the copy")
	{
		xb_plane_limits(640, 480, 3840, 2160, &w, &h);
		CHECK(w == 640);
		CHECK(h == 480);
	}

	SECTION("an unknown source falls back to the allocation")
	{
		xb_plane_limits(0, 0, 3840, 2160, &w, &h);
		CHECK(w == 3840);
		CHECK(h == 2160);
	}
}

TEST_CASE("xb_plane_decide")
{
	const struct xb_plane_box owes = box(0, 0, 64, 64);
	const struct xb_plane_box nothing = box(0, 0, 0, 0);

	SECTION("a slot on this generation owing nothing is a change-skip")
	{
		CHECK(xb_plane_decide(7, 7, &nothing, false, 100, 0) == XB_PLANE_SKIP_UNCHANGED);
	}

	SECTION("a slot on this generation that still owes a box copies")
	{
		CHECK(xb_plane_decide(7, 7, &owes, false, 100, 0) == XB_PLANE_COPY);
	}

	SECTION("a slot on an older generation copies")
	{
		CHECK(xb_plane_decide(6, 7, &owes, false, 100, 0) == XB_PLANE_COPY);
	}

	SECTION("the half-rate latch drops the off seq and keeps the on seq")
	{
		CHECK(xb_plane_decide(6, 7, &owes, true, 100, 1) == XB_PLANE_SKIP_HALF_RATE);
		CHECK(xb_plane_decide(6, 7, &owes, true, 101, 1) == XB_PLANE_COPY);
	}

	SECTION("an empty box with nothing to say is not a copy")
	{
		CHECK(xb_plane_decide(6, 7, &nothing, false, 100, 0) == XB_PLANE_SKIP_EMPTY);
	}
}

/*
 * #918 review F3. The rule the consume half depends on: VALID means "these
 * pixels are the generation the frame staged", not "this slot has pixels".
 */
TEST_CASE("xb_plane_slot_is_valid")
{
	SECTION("current pixels are valid")
	{
		CHECK(xb_plane_slot_is_valid(true, true, 7, 7));
	}

	SECTION("a slot left on an older generation by a skip is NOT valid")
	{
		// The half-rate and empty-box skips both land here. Stamping these
		// valid is what let a stale 2D band composite under a current frame.
		CHECK_FALSE(xb_plane_slot_is_valid(true, true, 6, 7));
	}

	SECTION("a never-written slot is not valid")
	{
		CHECK_FALSE(xb_plane_slot_is_valid(true, true, 0, 7));
	}

	SECTION("a plane the frame did not stage is not valid")
	{
		CHECK_FALSE(xb_plane_slot_is_valid(true, false, 7, 7));
	}

	SECTION("a dead plane is not valid")
	{
		CHECK_FALSE(xb_plane_slot_is_valid(false, true, 7, 7));
	}
}

/*
 * #918 review F1 — the BOOTSTRAP, at the level the policy can speak to it.
 *
 * The defect was a cycle: the frame that authored a mask asked whether the
 * plane's pixels had landed, found they had not, and abandoned the deposit — so
 * nothing was ever transported and the next frame asked the identical question.
 * The invariant that breaks it is that STAGING IS INDEPENDENT OF CONSUMPTION:
 * the authoring frame's submit must record a copy even though every slot is
 * empty, and the slot becomes valid on the frame after.
 *
 * Simulated over the ring the same way the bridge drives it, so a regression
 * that reintroduces a "has it landed yet?" precondition to the transport shows
 * up as a copy that never happens.
 */
TEST_CASE("a freshly authored plane transports on its own frame and lands on the next")
{
	constexpr int kRing = 3;
	uint64_t slot_seq[kRing] = {0, 0, 0};
	struct xb_plane_box pend[kRing];
	for (int i = 0; i < kRing; i++) {
		// A fresh chain: every slot owes a full refresh (xb_plane_invalidate_slots).
		pend[i] = box(0, 0, 256, 256);
	}

	const uint64_t authored = 0xABCDEF01ull; // the mask's content generation
	uint64_t last_staged = 0;
	int copies = 0;
	bool ever_valid = false;

	for (uint64_t seq = 1; seq <= 6; seq++) {
		const int eg = (int)(seq % kRing);

		// Deposit: the frame stages the authored mask unconditionally. It does
		// NOT consult any slot — that is the invariant under test. As in
		// comp_d3d11_xbridge_stage_plane, only a CHANGE re-pends every slot.
		const uint64_t stage_seq = authored;
		if (stage_seq != last_staged) {
			last_staged = stage_seq;
			struct xb_plane_box staged = box(0, 0, 256, 256);
			for (int i = 0; i < kRing; i++) {
				xb_plane_box_union(&pend[i], &staged);
			}
		}

		// Submit.
		if (xb_plane_decide(slot_seq[eg], stage_seq, &pend[eg], false, seq, 0) == XB_PLANE_COPY) {
			slot_seq[eg] = stage_seq;
			pend[eg] = box(0, 0, 0, 0);
			copies++;
		}

		// Stamp.
		if (xb_plane_slot_is_valid(true, true, slot_seq[eg], stage_seq)) {
			ever_valid = true;
		}

		if (seq == 1) {
			// The authoring frame transported, with no slot having held
			// anything beforehand.
			CHECK(copies == 1);
			CHECK(ever_valid);
		}
	}

	// One copy per ring slot to fill the ring, then the change-skip takes over —
	// a mask drawn once and reused costs exactly `kRing` transports for the whole
	// session, which is the documented cost of the Tier-3 plane.
	CHECK(copies == kRing);
	CHECK(ever_valid);
}
