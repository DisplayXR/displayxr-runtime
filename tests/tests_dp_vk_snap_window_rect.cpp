// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The Vulkan DP's window-drag phase snap + its back-compat gate (#1588).
 *
 * `snap_window_rect` is the Vulkan twin of D3D11 slot 18: the window owner asks
 * where its window may LAND, the display processor answers, and the lens pitch
 * never crosses the boundary (ADR-019). Two things are worth pinning, and they
 * are the two that would break silently:
 *
 * 1. **A plug-in built before the slot existed must read as "no snap" without
 *    the runtime ever dereferencing the slot** — those bytes belong to the
 *    plug-in, and reading them is exactly the corruption ADR-020 exists to
 *    prevent. Today's shipping Leia Linux plug-in is such a plug-in.
 * 2. **The outputs are always written.** Unlike the D3D11 helper (which leaves
 *    them untouched on failure), the Vulkan helper writes the TARGET through,
 *    because its callers feed the result straight on. A caller that ignored the
 *    return value and got uninitialised coordinates would move a window to
 *    garbage.
 * 3. **The absolute frame cancels, so the runtime must not convert.** The
 *    vendor uses only `target - origin`: it canonicalises the displacement,
 *    snaps from (0, 0) and re-adds the origin, and its phase search minimises
 *    `remainder(ph - ph0, 1.0)`. A constant added to BOTH points shifts `ph`
 *    and `ph0` equally. The fake below models exactly that shape rather than
 *    snapping an absolute coordinate, because a fake that snapped absolutely
 *    would happily pass a runtime that converted frames — and converting is
 *    the thing that could only ever introduce a mismatch between the two
 *    arguments.
 *
 * What these tests deliberately do NOT cover: units. Translation cancels, a
 * scale factor does not, so device-vs-logical pixels is a real failure mode —
 * but it lives at the call sites that read window geometry, not in this
 * arithmetic, and a unit test here could only restate the multiplication.
 */

#include "catch_amalgamated.hpp"

#include "xrt/xrt_display_processor_vk.h"

#include <cmath>
#include <cstring>

namespace {

//! Set when the vtable slot is entered, so a test can assert it was NOT.
bool g_called = false;
bool g_answer = true;
int32_t g_last_origin_x = 0, g_last_origin_y = 0;
int32_t g_last_target_x = 0, g_last_target_y = 0;

//! Pitch of the fake lattice below. Not a real number; a small one that moves.
constexpr int32_t kPitch = 8;

/*!
 * A stand-in for a vendor snap, shaped like the real one.
 *
 * Two properties are modelled on purpose:
 *
 * - it snaps the SLANTED invariant `u = x + y` (slant 1.0), because a
 *   y-dependent x snap is the shape the runtime must not assume away — both
 *   coordinates go in and the x that comes back depends on both;
 * - it works on the DISPLACEMENT and re-adds the origin, so the result
 *   preserves the phase the window had at the origin and the absolute frame
 *   cancels. Snapping `target` absolutely would be the easier fake and a
 *   strictly worse one: it would not catch a caller that mixed frames.
 */
bool
fake_snap(struct xrt_display_processor_vk *xdp,
          int32_t origin_x,
          int32_t origin_y,
          int32_t target_x,
          int32_t target_y,
          int32_t *out_x,
          int32_t *out_y)
{
	(void)xdp;
	g_called = true;
	g_last_origin_x = origin_x;
	g_last_origin_y = origin_y;
	g_last_target_x = target_x;
	g_last_target_y = target_y;
	if (!g_answer) {
		return false;
	}
	const int32_t dx = target_x - origin_x;
	const int32_t dy = target_y - origin_y;
	// Canonicalise, snap from (0, 0), re-add the origin.
	const int32_t du = dx + dy;
	const int32_t du_snap = (int32_t)std::lround((double)du / kPitch) * kPitch;
	*out_x = origin_x + (du_snap - dy);
	*out_y = origin_y + dy;
	return true;
}

//! A vtable whose `base.struct_size` the test controls, as a plug-in's would be.
struct xrt_display_processor_vk
make_dp(uint32_t struct_size, bool with_fn)
{
	struct xrt_display_processor_vk dp;
	std::memset(&dp, 0, sizeof(dp));
	dp.base.struct_size = struct_size;
	dp.snap_window_rect = with_fn ? fake_snap : nullptr;
	return dp;
}

//! `base.struct_size` of a plug-in built one slot before this one existed.
constexpr uint32_t
size_without_slot()
{
	return (uint32_t)offsetof(struct xrt_display_processor_vk, snap_window_rect);
}

} // namespace

TEST_CASE("dp_vk_snap: the slot is appended, not inserted")
{
	// If this ever fails, an existing slot moved and every built plug-in
	// misdispatches. The header's own static_asserts pin the offsets; this
	// pins the relationship the gate depends on.
	CHECK(size_without_slot() == sizeof(struct xrt_display_processor) + 13 * sizeof(void *));
	CHECK(sizeof(struct xrt_display_processor_vk) >= sizeof(struct xrt_display_processor) + 14 * sizeof(void *));
}

TEST_CASE("dp_vk_snap: an older plug-in is identity and is never called")
{
	g_called = false;
	g_answer = true;
	// The struct_size says "I do not have that slot", but the memory behind
	// it is a real function pointer — a runtime that checked only for NULL
	// would call into it.
	struct xrt_display_processor_vk dp = make_dp(size_without_slot(), /*with_fn=*/true);

	int32_t x = -1, y = -1;
	const bool snapped = xrt_display_processor_vk_snap_window_rect(&dp, 100, 200, 137, 211, &x, &y);

	CHECK(snapped == false);
	CHECK(g_called == false); // the slot was NOT dereferenced
	CHECK(x == 137);          // identity: the target passed through
	CHECK(y == 211);
}

TEST_CASE("dp_vk_snap: a NULL slot on a current plug-in is identity")
{
	g_called = false;
	struct xrt_display_processor_vk dp = make_dp((uint32_t)sizeof(dp), /*with_fn=*/false);

	int32_t x = -1, y = -1;
	CHECK(xrt_display_processor_vk_snap_window_rect(&dp, 0, 0, 137, 211, &x, &y) == false);
	CHECK(g_called == false);
	CHECK(x == 137);
	CHECK(y == 211);
}

TEST_CASE("dp_vk_snap: a NULL DP is identity")
{
	int32_t x = -1, y = -1;
	CHECK(xrt_display_processor_vk_snap_window_rect(nullptr, 0, 0, 137, 211, &x, &y) == false);
	CHECK(x == 137);
	CHECK(y == 211);
}

TEST_CASE("dp_vk_snap: a DP that declines leaves the target unchanged")
{
	g_called = false;
	g_answer = false;
	struct xrt_display_processor_vk dp = make_dp((uint32_t)sizeof(dp), /*with_fn=*/true);

	int32_t x = -1, y = -1;
	CHECK(xrt_display_processor_vk_snap_window_rect(&dp, 100, 200, 137, 211, &x, &y) == false);
	CHECK(g_called == true); // it WAS asked
	CHECK(x == 137);         // ...and its refusal is the target, not garbage
	CHECK(y == 211);
	g_answer = true;
}

TEST_CASE("dp_vk_snap: both coordinates reach the vendor, and both may come back")
{
	g_called = false;
	g_answer = true;
	struct xrt_display_processor_vk dp = make_dp((uint32_t)sizeof(dp), /*with_fn=*/true);

	int32_t x = 0, y = 0;
	CHECK(xrt_display_processor_vk_snap_window_rect(&dp, 100, 200, 137, 213, &x, &y) == true);
	CHECK(g_called == true);
	// The runtime forwards the pair VERBATIM - it does no arithmetic of its own,
	// and in particular no frame conversion.
	CHECK(g_last_origin_x == 100);
	CHECK(g_last_origin_y == 200);
	CHECK(g_last_target_x == 137);
	CHECK(g_last_target_y == 213);
	// displacement (37, 13) -> du = 50 -> nearest multiple of 8 is 48
	// -> x = 100 + (48 - 13) = 135, y = 200 + 13 = 213.
	CHECK(x == 135);
	CHECK(y == 213);
	// What a snap preserves is the ORIGIN's phase, not a coordinate.
	CHECK((((x - 100) + (y - 200)) % kPitch) == 0);
	// ...and it gets there by moving a little: 2 px of canonical travel here,
	// which is also the vendor's actual search radius. A result far from the
	// target means mixed frames or scaled pixels, not a large pitch.
	CHECK(std::abs((x + y) - (137 + 213)) <= 2);
}

TEST_CASE("dp_vk_snap: NULL outputs are refused, not crashed")
{
	struct xrt_display_processor_vk dp = make_dp((uint32_t)sizeof(dp), /*with_fn=*/true);
	int32_t x = 0;
	CHECK(xrt_display_processor_vk_snap_window_rect(&dp, 0, 0, 1, 2, nullptr, nullptr) == false);
	CHECK(xrt_display_processor_vk_snap_window_rect(&dp, 0, 0, 1, 2, &x, nullptr) == false);
}

/*
 * ── The absolute frame cancels (#1588) ───────────────────────────────────
 *
 * This is the property that says the runtime must not convert frames, so it is
 * worth an assertion rather than only a comment in a header.
 */

TEST_CASE("dp_vk_snap: shifting BOTH points by a constant shifts the answer by that constant")
{
	g_answer = true;
	struct xrt_display_processor_vk dp = make_dp((uint32_t)sizeof(dp), /*with_fn=*/true);

	// The same drag expressed panel-relative, then desktop-absolute for a
	// panel whose top-left sits at (3456, 12) - the DS1's real offset, whose
	// x is a multiple of the fake pitch and whose y is deliberately not, so a
	// frame-sensitive implementation could not pass by luck.
	const int32_t off_x = 3456, off_y = 12;

	int32_t px = 0, py = 0, ax = 0, ay = 0;
	const bool a = xrt_display_processor_vk_snap_window_rect(&dp, 100, 200, 137, 213, &px, &py);
	const bool b = xrt_display_processor_vk_snap_window_rect( //
	    &dp, 100 + off_x, 200 + off_y, 137 + off_x, 213 + off_y, &ax, &ay);

	CHECK(a == b);
	CHECK(ax == px + off_x);
	CHECK(ay == py + off_y);
}

TEST_CASE("dp_vk_snap: a snap PRESERVES the origin's phase rather than finding a good one")
{
	g_answer = true;
	struct xrt_display_processor_vk dp = make_dp((uint32_t)sizeof(dp), /*with_fn=*/true);

	// An origin deliberately off the lattice (u = 101 + 200 = 301, not a
	// multiple of 8). A snap must NOT quietly fix that: the window keeps
	// whatever phase it had, and the call still succeeds. Success means "the
	// drag did not make it worse", never "the 3D is correct".
	int32_t x = 0, y = 0;
	CHECK(xrt_display_processor_vk_snap_window_rect(&dp, 101, 200, 138, 213, &x, &y) == true);
	CHECK((((x - 101) + (y - 200)) % kPitch) == 0); // same phase as the origin
	CHECK(((x + y) % kPitch) != 0);                 // still off the absolute lattice
}

TEST_CASE("dp_vk_snap: a zero-length drag is a no-op")
{
	g_answer = true;
	struct xrt_display_processor_vk dp = make_dp((uint32_t)sizeof(dp), /*with_fn=*/true);

	// target == origin => displacement 0 => already on the lattice relative to
	// itself. The window must not twitch when the pointer has not moved.
	int32_t x = 0, y = 0;
	CHECK(xrt_display_processor_vk_snap_window_rect(&dp, 4156, 374, 4156, 374, &x, &y) == true);
	CHECK(x == 4156);
	CHECK(y == 374);
}
