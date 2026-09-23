// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Lazy transparency: the debounce policy and the DP slot's back-compat
 *         gate.
 *
 * Two things would break silently and are pinned here:
 *
 * 1. **The debounce.** Active on the FIRST transparent frame (reacting late
 *    costs a frame whose transparent background weaves opaque), idle only
 *    after a full run of opaque frames (restarting a desktop capture is a
 *    D-Bus session plus a PipeWire stream, so a toggle or a flickering pixel
 *    must not thrash it). A single transparent frame inside an opaque run
 *    restarts the count.
 * 2. **An older plug-in is never told anything.** Its `base.struct_size` ends
 *    before `set_transparency_active`; the bytes past it belong to the
 *    plug-in. The runtime must read "unsupported" and leave transparency
 *    active for the whole session, which is exactly the pre-slot behaviour.
 */

#include "catch_amalgamated.hpp"

#include "xrt/xrt_display_processor_vk.h"
#include "util/comp_lazy_transparency.h"

#include <cstring>

namespace {

int g_calls = 0;
bool g_last_active = true;

void
fake_set_active(struct xrt_display_processor_vk *xdp, bool active)
{
	(void)xdp;
	g_calls++;
	g_last_active = active;
}

struct xrt_display_processor_vk
make_dp(uint32_t struct_size, bool with_fn)
{
	struct xrt_display_processor_vk dp;
	std::memset(&dp, 0, sizeof(dp));
	dp.base.struct_size = struct_size;
	dp.set_transparency_active = with_fn ? fake_set_active : nullptr;
	return dp;
}

constexpr uint32_t
size_without_slot()
{
	return (uint32_t)offsetof(struct xrt_display_processor_vk, set_transparency_active);
}

} // namespace

TEST_CASE("lazy_transparency: the slot is appended, not inserted")
{
	CHECK(size_without_slot() == sizeof(struct xrt_display_processor) + 14 * sizeof(void *));
	CHECK(sizeof(struct xrt_display_processor_vk) == sizeof(struct xrt_display_processor) + 15 * sizeof(void *));
}

TEST_CASE("lazy_transparency: an older plug-in is unsupported and never called")
{
	g_calls = 0;
	// The memory behind struct_size holds a real function pointer; a runtime
	// that checked only for NULL would call into it.
	struct xrt_display_processor_vk dp = make_dp(size_without_slot(), /*with_fn=*/true);
	CHECK_FALSE(xrt_display_processor_vk_supports_transparency_active(&dp));
	CHECK_FALSE(xrt_display_processor_vk_set_transparency_active(&dp, false));
	CHECK(g_calls == 0);
}

TEST_CASE("lazy_transparency: a NULL slot or NULL DP is unsupported")
{
	g_calls = 0;
	struct xrt_display_processor_vk dp = make_dp(sizeof(struct xrt_display_processor_vk), /*with_fn=*/false);
	CHECK_FALSE(xrt_display_processor_vk_supports_transparency_active(&dp));
	CHECK_FALSE(xrt_display_processor_vk_set_transparency_active(&dp, true));
	CHECK_FALSE(xrt_display_processor_vk_supports_transparency_active(nullptr));
	CHECK(g_calls == 0);
}

TEST_CASE("lazy_transparency: a current plug-in is called with the state")
{
	g_calls = 0;
	struct xrt_display_processor_vk dp = make_dp(sizeof(struct xrt_display_processor_vk), /*with_fn=*/true);
	CHECK(xrt_display_processor_vk_supports_transparency_active(&dp));
	CHECK(xrt_display_processor_vk_set_transparency_active(&dp, false));
	CHECK(g_calls == 1);
	CHECK(g_last_active == false);
	CHECK(xrt_display_processor_vk_set_transparency_active(&dp, true));
	CHECK(g_calls == 2);
	CHECK(g_last_active == true);
}

TEST_CASE("lazy_transparency: disengaged policy never transitions")
{
	struct comp_lazy_transparency s = {};
	CHECK(comp_lazy_transparency_update(&s, true) == COMP_LAZY_TRANSPARENCY_NONE);
	CHECK(comp_lazy_transparency_update(&s, false) == COMP_LAZY_TRANSPARENCY_NONE);
}

TEST_CASE("lazy_transparency: an opaque session never goes active")
{
	struct comp_lazy_transparency s = {};
	comp_lazy_transparency_engage(&s, 0);
	CHECK(s.idle_after == COMP_LAZY_TRANSPARENCY_IDLE_FRAMES);
	CHECK_FALSE(s.active);
	for (int i = 0; i < 1000; i++) {
		REQUIRE(comp_lazy_transparency_update(&s, false) == COMP_LAZY_TRANSPARENCY_NONE);
	}
	CHECK_FALSE(s.active);
}

TEST_CASE("lazy_transparency: first transparent frame activates; idle after exactly N opaque")
{
	struct comp_lazy_transparency s = {};
	comp_lazy_transparency_engage(&s, 60);

	CHECK(comp_lazy_transparency_update(&s, true) == COMP_LAZY_TRANSPARENCY_TO_ACTIVE);
	CHECK(comp_lazy_transparency_update(&s, true) == COMP_LAZY_TRANSPARENCY_NONE); // no re-send

	for (int i = 0; i < 59; i++) {
		REQUIRE(comp_lazy_transparency_update(&s, false) == COMP_LAZY_TRANSPARENCY_NONE);
	}
	CHECK(s.active);
	CHECK(comp_lazy_transparency_update(&s, false) == COMP_LAZY_TRANSPARENCY_TO_IDLE);
	CHECK_FALSE(s.active);
	CHECK(comp_lazy_transparency_update(&s, false) == COMP_LAZY_TRANSPARENCY_NONE);
}

TEST_CASE("lazy_transparency: a transparent frame inside an opaque run restarts the count")
{
	struct comp_lazy_transparency s = {};
	comp_lazy_transparency_engage(&s, 60);
	CHECK(comp_lazy_transparency_update(&s, true) == COMP_LAZY_TRANSPARENCY_TO_ACTIVE);

	for (int i = 0; i < 50; i++) {
		REQUIRE(comp_lazy_transparency_update(&s, false) == COMP_LAZY_TRANSPARENCY_NONE);
	}
	CHECK(comp_lazy_transparency_update(&s, true) == COMP_LAZY_TRANSPARENCY_NONE); // still active
	for (int i = 0; i < 59; i++) {
		REQUIRE(comp_lazy_transparency_update(&s, false) == COMP_LAZY_TRANSPARENCY_NONE);
	}
	CHECK(comp_lazy_transparency_update(&s, false) == COMP_LAZY_TRANSPARENCY_TO_IDLE);
}

TEST_CASE("lazy_transparency: rapid toggling costs one transition per state change, not per frame")
{
	struct comp_lazy_transparency s = {};
	comp_lazy_transparency_engage(&s, 60);
	int to_active = 0, to_idle = 0;
	// A user flipping a transparency toggle every 10 frames for 10 s.
	for (int f = 0; f < 600; f++) {
		const bool transparent = ((f / 10) % 2) == 0;
		switch (comp_lazy_transparency_update(&s, transparent)) {
		case COMP_LAZY_TRANSPARENCY_TO_ACTIVE: to_active++; break;
		case COMP_LAZY_TRANSPARENCY_TO_IDLE: to_idle++; break;
		default: break;
		}
	}
	CHECK(to_active == 1);
	CHECK(to_idle == 0);
}
