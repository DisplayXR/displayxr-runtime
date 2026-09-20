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
 */

#include "catch_amalgamated.hpp"

#include "xrt/xrt_display_processor_vk.h"

#include <cstring>

namespace {

//! Set when the vtable slot is entered, so a test can assert it was NOT.
bool g_called = false;
bool g_answer = true;
int32_t g_last_origin_x = 0, g_last_origin_y = 0;
int32_t g_last_target_x = 0, g_last_target_y = 0;

/*!
 * A stand-in for a vendor snap. Deliberately snaps on the SLANTED invariant
 * `u = x + y` with period 8 — not because 8 or a slant of 1.0 are real, but
 * because a y-dependent x snap is the shape the runtime must not assume away:
 * both coordinates go in and the x that comes back depends on both.
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
	const int32_t u = target_x + target_y;
	const int32_t u_snap = ((u + 4) / 8) * 8;
	*out_x = u_snap - target_y;
	*out_y = target_y;
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
	CHECK(sizeof(struct xrt_display_processor_vk) == sizeof(struct xrt_display_processor) + 14 * sizeof(void *));
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
	CHECK(xrt_display_processor_vk_snap_window_rect(&dp, 100, 200, 137, 211, &x, &y) == true);
	CHECK(g_called == true);
	// The runtime forwards the pair verbatim — it does no arithmetic of its own.
	CHECK(g_last_origin_x == 100);
	CHECK(g_last_origin_y == 200);
	CHECK(g_last_target_x == 137);
	CHECK(g_last_target_y == 211);
	// u = 137 + 211 = 348 -> nearest multiple of 8 is 352 -> x = 352 - 211 = 141.
	CHECK(x == 141);
	CHECK(y == 211);
	// The invariant, not the coordinate, is what the snap preserves.
	CHECK(((x + y) % 8) == 0);
}

TEST_CASE("dp_vk_snap: NULL outputs are refused, not crashed")
{
	struct xrt_display_processor_vk dp = make_dp((uint32_t)sizeof(dp), /*with_fn=*/true);
	int32_t x = 0;
	CHECK(xrt_display_processor_vk_snap_window_rect(&dp, 0, 0, 1, 2, nullptr, nullptr) == false);
	CHECK(xrt_display_processor_vk_snap_window_rect(&dp, 0, 0, 1, 2, &x, nullptr) == false);
}
