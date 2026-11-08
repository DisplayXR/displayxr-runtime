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

#include "comp_xbridge_plane_policy.h"
#include "comp_xbridge_format_policy.h"

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
	const struct xb_plane_box dirty = box(0, 0, 128, 128);
	const struct xb_plane_box nothing = box(0, 0, 0, 0);

	SECTION("a slot on this generation owing nothing is a change-skip")
	{
		CHECK(xb_plane_decide(7, 7, &nothing, &dirty, false, 100, 0) == XB_PLANE_SKIP_UNCHANGED);
	}

	/*
	 * THE ORDER IS THE POINT. The change-skip asks what the slot OWES, before
	 * this frame's staged box is folded in. Deciding on the union instead makes
	 * every slot that already holds the current generation look dirty — a copy
	 * on every frame, for content that has not changed, which is the entire
	 * bandwidth argument inverted. (Caught in a live run: 61 copies/s and zero
	 * skips where the design says three copies then skips for ever.)
	 */
	SECTION("this frame's staged box does NOT defeat the change-skip")
	{
		CHECK(xb_plane_decide(7, 7, &nothing, &dirty, false, 100, 0) == XB_PLANE_SKIP_UNCHANGED);
		CHECK(xb_plane_decide(7, 7, &nothing, &owes, false, 101, 0) == XB_PLANE_SKIP_UNCHANGED);
	}

	SECTION("a slot on this generation that still owes a box copies")
	{
		CHECK(xb_plane_decide(7, 7, &owes, &nothing, false, 100, 0) == XB_PLANE_COPY);
	}

	SECTION("a slot on an older generation copies, even owing nothing")
	{
		// New content this frame, a slot that has not seen it: the staged box
		// is what gets transported.
		CHECK(xb_plane_decide(6, 7, &nothing, &dirty, false, 100, 0) == XB_PLANE_COPY);
		CHECK(xb_plane_decide(6, 7, &owes, &dirty, false, 100, 0) == XB_PLANE_COPY);
	}

	SECTION("the half-rate latch drops the off seq and keeps the on seq")
	{
		CHECK(xb_plane_decide(6, 7, &owes, &dirty, true, 100, 1) == XB_PLANE_SKIP_HALF_RATE);
		CHECK(xb_plane_decide(6, 7, &owes, &dirty, true, 101, 1) == XB_PLANE_COPY);
	}

	SECTION("nothing owed and nothing staged is not a copy")
	{
		CHECK(xb_plane_decide(6, 7, &nothing, &nothing, false, 100, 0) == XB_PLANE_SKIP_EMPTY);
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
		// comp_xbridge_stage_plane, only a CHANGE re-pends every slot.
		const uint64_t stage_seq = authored;
		if (stage_seq != last_staged) {
			last_staged = stage_seq;
			struct xb_plane_box staged = box(0, 0, 256, 256);
			for (int i = 0; i < kRing; i++) {
				xb_plane_box_union(&pend[i], &staged);
			}
		}

		// Submit. The staged box is this frame's own; `pend[eg]` is what the
		// slot owed before it — decided in that order, then folded.
		const struct xb_plane_box frame_box = box(0, 0, 256, 256);
		if (xb_plane_decide(slot_seq[eg], stage_seq, &pend[eg], &frame_box, false, seq, 0) == XB_PLANE_COPY) {
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

/*
 * A latched (half-rate) plane must never show content OLDER than the frame
 * before it. The bug: with a ring of 3 and a copy every other seq, a skipped
 * slot kept content from three frames back, so a moving progress bar stepped
 * backwards on every skipped frame. The catch-up copies the newest slot locally.
 */
static void
run_half_rate_ring(bool catchup, uint64_t *out_backsteps, uint64_t *out_max_lag, uint64_t *out_blanks = nullptr)
{
	constexpr int kRing = 3;
	uint64_t slot_seq[kRing] = {0, 0, 0};
	uint64_t slot_copy_n[kRing] = {0, 0, 0};
	struct xb_plane_box pend[kRing];
	for (int i = 0; i < kRing; i++) {
		pend[i] = box(0, 0, 256, 256);
	}
	uint64_t copy_n = 0;
	uint64_t shown_prev = 0;
	*out_backsteps = 0;
	*out_max_lag = 0;
	uint64_t blanks = 0;
	for (uint64_t seq = 1; seq <= 60; seq++) {
		const int eg = (int)(seq % kRing);
		// Content changes every frame; the generation IS the frame number here so
		// "older" is directly comparable (the real seqs are hashes).
		const uint64_t stage_seq = seq;
		const struct xb_plane_box staged = box(0, 0, 256, 16);
		for (int i = 0; i < kRing; i++) {
			xb_plane_box_union(&pend[i], &staged);
		}
		const enum xb_plane_action act = xb_plane_decide(slot_seq[eg], stage_seq, &pend[eg], &staged,
		                                                 /*half_rate=*/true, seq, /*parity=*/0);
		if (act == XB_PLANE_COPY) {
			slot_seq[eg] = stage_seq;
			slot_copy_n[eg] = ++copy_n;
			pend[eg] = box(0, 0, 0, 0);
		} else if (act == XB_PLANE_SKIP_HALF_RATE && catchup) {
			const int src = xb_plane_catchup_source(slot_seq, slot_copy_n, kRing, eg);
			if (src >= 0) {
				slot_seq[eg] = slot_seq[src];
				slot_copy_n[eg] = slot_copy_n[src];
				pend[eg] = pend[src];
			}
		}
		// A frame whose plane is not presentable composites NO 2D at all (a blink).
		if (copy_n > 0 &&
		    !xb_plane_slot_is_presentable(true, true, slot_seq[eg], stage_seq, true, slot_copy_n[eg], copy_n)) {
			blanks++;
		}
		// The consume half shows whatever the slot holds once it holds anything.
		const uint64_t shown = slot_seq[eg];
		if (shown == 0) {
			continue;
		}
		if (shown < shown_prev) {
			(*out_backsteps)++;
		}
		if (seq - shown > *out_max_lag) {
			*out_max_lag = seq - shown;
		}
		shown_prev = shown;
	}
	if (out_blanks != nullptr) {
		*out_blanks = blanks;
	}
}

TEST_CASE("a half-rate plane never steps back in time")
{
	uint64_t back = 0, lag = 0;

	// The pre-fix behaviour, kept as a record of the failure mode.
	run_half_rate_ring(/*catchup=*/false, &back, &lag);
	CHECK(back > 0);

	run_half_rate_ring(/*catchup=*/true, &back, &lag);
	CHECK(back == 0);
	// Half rate costs at most one frame of lag, never the ring's depth.
	CHECK(lag <= 1);
}

TEST_CASE("a half-rate plane never blinks: every frame after the first copy composites")
{
	uint64_t back = 0, lag = 0, blanks = 0;
	// Pre-fix: a skipped slot was stamped invalid and its frame went out without 2D.
	run_half_rate_ring(/*catchup=*/false, &back, &lag, &blanks);
	CHECK(blanks > 0);

	run_half_rate_ring(/*catchup=*/true, &back, &lag, &blanks);
	CHECK(blanks == 0);
}

TEST_CASE("xb_plane_catchup_source picks the newest written slot, never an older one")
{
	const uint64_t seq[3] = {11, 22, 33};
	const uint64_t n[3] = {5, 7, 6};
	CHECK(xb_plane_catchup_source(seq, n, 3, 0) == 1);
	CHECK(xb_plane_catchup_source(seq, n, 3, 2) == 1);
	// The write slot is already the newest: nothing to catch up from.
	CHECK(xb_plane_catchup_source(seq, n, 3, 1) == -1);
	// A never-written slot is not a source however its counter reads.
	const uint64_t seq0[3] = {11, 0, 33};
	CHECK(xb_plane_catchup_source(seq0, n, 3, 0) == 2);
}

TEST_CASE("xb_gate_decide: the bandwidth gate's verdict")
{
	constexpr uint64_t G = 2000ull * 1000ull * 1000ull; // gate
	constexpr uint64_t F = 250ull * 1000ull * 1000ull;  // plane floor
	constexpr uint64_t GB = 1000ull * 1000ull * 1000ull;

	SECTION("inside budget, nothing latched: nothing to do")
	{
		CHECK(xb_gate_decide(G, G / 2, false, G, F) == XB_GATE_NONE);
	}
	SECTION("over budget because of the planes: latch")
	{
		CHECK(xb_gate_decide(3 * GB, 2 * GB, false, G, F) == XB_GATE_LATCH);
	}
	SECTION("over budget on the atlas alone: no lever")
	{
		CHECK(xb_gate_decide(3 * GB, F / 2, false, G, F) == XB_GATE_ATLAS_ONLY);
	}
	SECTION("8K regression: atlas alone over budget, latched UI goes idle -> releases")
	{
		// 3.1 GB/s atlas, an idle UI plane: the old "total under budget" rule never fired.
		CHECK(xb_gate_decide(3100 * 1000ull * 1000ull, 10ull * 1000ull * 1000ull, true, G, F) == XB_GATE_SETTLED);
	}
	SECTION("latched and the plane is still busy over a heavy atlas: hold, never release")
	{
		CHECK(xb_gate_decide(3 * GB + GB, GB, true, G, F) == XB_GATE_LATCH);
	}
	SECTION("latched, total under budget but full rate would not fit: hold")
	{
		// 1.9 GB/s now with a 0.5 GB/s half-rate plane = 2.4 GB/s at full rate.
		CHECK(xb_gate_decide(1900ull * 1000ull * 1000ull, GB / 2, true, G, F) == XB_GATE_NONE);
	}
	SECTION("latched and full rate would fit: release")
	{
		CHECK(xb_gate_decide(GB, GB / 2, true, G, F) == XB_GATE_SETTLED);
	}
}

/*
 * #1663 — the bridge's ingress format decision.
 *
 * The guard around it is a throttled U_LOG_E on a live bridge, so on hardware
 * the only two outcomes anyone ever sees are "it works" and "the panel is black
 * and the bridge degraded". Both refusals below are invisible in a screenshot,
 * and the same-family one is worse than invisible: the copy is LEGAL, so a
 * reader meeting the guard later has every reason to think it is over-strict and
 * relax it back to a plain same-family test. That is why it is a test.
 */
/*
 * #2273 — the bridge now builds its chain in a deep atlas format too. The rule
 * is unchanged (own format or its family's TYPELESS member), and the refusal
 * the panel run hit before the fix stays a refusal: a 10-bit source into the
 * default RGBA8 chain is a cross-family copy, which D3D drops silently.
 */
TEST_CASE("xb_source_format_compatible: deep atlas chains (#2273)")
{
	CHECK(xb_source_format_compatible(DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM) ==
	      XB_SRC_FMT_EXACT);
	CHECK(xb_source_format_compatible(DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT) ==
	      XB_SRC_FMT_EXACT);
	CHECK(xb_source_format_compatible(DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_TYPELESS) ==
	      XB_SRC_FMT_TYPELESS_TO_CONCRETE);
	CHECK(xb_source_format_compatible(DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_TYPELESS) ==
	      XB_SRC_FMT_TYPELESS_TO_CONCRETE);
	CHECK_FALSE(xb_source_format_accepted(
	    xb_source_format_compatible(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM)));
	CHECK_FALSE(xb_source_format_accepted(
	    xb_source_format_compatible(DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT)));
}

TEST_CASE("xb_source_format_compatible accepts exactly two sources")
{
	SECTION("the chain's own format is the trivial accept")
	{
		CHECK(xb_source_format_compatible(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM) ==
		      XB_SRC_FMT_EXACT);
		CHECK(xb_source_format_accepted(
		    xb_source_format_compatible(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM)));
	}

	/*
	 * The #1663 case itself. The per-client atlas is allocated TYPELESS so
	 * UNORM and UNORM_SRGB views can share it; a display-filling submission
	 * bypasses the UNORM crop texture and hands that atlas to the bridge. The
	 * copy is a plain byte move and the source carries no encoding of its own,
	 * so the chain's supplies it. Refusing this refused every full-screen frame
	 * under the split.
	 */
	SECTION("the TYPELESS member of the chain's family is accepted")
	{
		CHECK(xb_source_format_compatible(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_TYPELESS) ==
		      XB_SRC_FMT_TYPELESS_TO_CONCRETE);
		CHECK(xb_source_format_accepted(
		    xb_source_format_compatible(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_TYPELESS)));
		// The same holds for an SRGB chain: the encoding is the CHAIN's.
		CHECK(xb_source_format_compatible(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_TYPELESS) ==
		      XB_SRC_FMT_TYPELESS_TO_CONCRETE);
		// ...and for the other family, so this is a rule and not a special case.
		CHECK(xb_source_format_compatible(DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_TYPELESS) ==
		      XB_SRC_FMT_TYPELESS_TO_CONCRETE);
	}

	/*
	 * The trap this guard must NOT be relaxed into. One typeless family, so the
	 * copy is legal and moves the right bits — and the next sampler then reads
	 * them under a different transfer function (#1589 / #1610). A silent colour
	 * error is worse than a loud refusal.
	 */
	SECTION("two CONCRETE formats in one family are refused, both directions")
	{
		CHECK(xb_source_format_compatible(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) ==
		      XB_SRC_FMT_REFUSED_SAME_FAMILY_UNSAFE);
		CHECK(xb_source_format_compatible(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM) ==
		      XB_SRC_FMT_REFUSED_SAME_FAMILY_UNSAFE);
		CHECK_FALSE(xb_source_format_accepted(
		    xb_source_format_compatible(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)));
	}

	/*
	 * #1178's original hazard, unchanged: different channel order, so Windows
	 * drops the copy without failing it and the destination keeps what it held.
	 */
	SECTION("a cross-family source stays refused")
	{
		CHECK(xb_source_format_compatible(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM) ==
		      XB_SRC_FMT_REFUSED_CROSS_FAMILY);
		CHECK(xb_source_format_compatible(DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM) ==
		      XB_SRC_FMT_REFUSED_CROSS_FAMILY);
		// Not even the typeless member of the WRONG family.
		CHECK(xb_source_format_compatible(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_TYPELESS) ==
		      XB_SRC_FMT_REFUSED_CROSS_FAMILY);
		CHECK_FALSE(xb_source_format_accepted(
		    xb_source_format_compatible(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM)));
	}

	SECTION("a format this unit cannot place is refused, even against itself")
	{
		CHECK(xb_source_format_compatible(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_BC7_UNORM) ==
		      XB_SRC_FMT_REFUSED_UNKNOWN);
		CHECK(xb_source_format_compatible(DXGI_FORMAT_BC7_UNORM, DXGI_FORMAT_BC7_UNORM) ==
		      XB_SRC_FMT_REFUSED_UNKNOWN);
		CHECK(xb_source_format_compatible(DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN) ==
		      XB_SRC_FMT_REFUSED_UNKNOWN);
	}
}

/*
 * The family map has to be CLOSED for the guard's "is the source the typeless
 * member of the chain's family" test to mean anything: the value it returns must
 * itself map to the same family, or a representative would be unplaceable and
 * every typeless source would fall into REFUSED_UNKNOWN.
 */
TEST_CASE("xb_typeless_family is closed over its own representatives")
{
	const DXGI_FORMAT known[] = {
	    DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM,    DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
	    DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_UNORM,    DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
	    DXGI_FORMAT_R8_UNORM,          DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT,
	};
	for (DXGI_FORMAT f : known) {
		const DXGI_FORMAT fam = xb_typeless_family(f);
		INFO("DXGI_FORMAT " << (unsigned)f);
		CHECK(fam != DXGI_FORMAT_UNKNOWN);
		CHECK(xb_typeless_family(fam) == fam);
	}
	CHECK(xb_typeless_family(DXGI_FORMAT_BC7_UNORM) == DXGI_FORMAT_UNKNOWN);
}
