// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The Vulkan DP's set_weave_sync slot (XR_DXR_weave v20, the Android
 *         sync_file weave): append position and the struct_size back-compat gate.
 *
 * An older plug-in's `base.struct_size` ends before `set_weave_sync`; the bytes
 * past it belong to the plug-in. The runtime must read "not taken" — never call
 * through them — and keep its CPU waits for that frame.
 */

#include "catch_amalgamated.hpp"

#include "xrt/xrt_display_processor_vk.h"

#include <cstring>

namespace {

int g_calls = 0;
bool g_accept = true;
VkSemaphore_XDP g_wait = {};
VkSemaphore_XDP g_signal = {};
uint64_t g_wait_value = 99;
uint64_t g_signal_value = 99;

bool
fake_set_weave_sync(struct xrt_display_processor_vk *xdp,
                    VkSemaphore_XDP wait_sem,
                    uint64_t wait_value,
                    VkSemaphore_XDP signal_sem,
                    uint64_t signal_value)
{
	(void)xdp;
	g_calls++;
	g_wait = wait_sem;
	g_signal = signal_sem;
	g_wait_value = wait_value;
	g_signal_value = signal_value;
	return g_accept;
}

struct xrt_display_processor_vk
make_dp(uint32_t struct_size, bool with_fn)
{
	struct xrt_display_processor_vk dp;
	std::memset(&dp, 0, sizeof(dp));
	dp.base.struct_size = struct_size;
	dp.set_weave_sync = with_fn ? fake_set_weave_sync : nullptr;
	return dp;
}

void
reset()
{
	g_calls = 0;
	g_accept = true;
	std::memset(&g_wait, 0, sizeof(g_wait));
	std::memset(&g_signal, 0, sizeof(g_signal));
	g_wait_value = 99;
	g_signal_value = 99;
}

// Two distinct non-null handle values; the fake never dereferences them.
VkSemaphore_XDP
sem(unsigned char fill)
{
	VkSemaphore_XDP s;
	std::memset(&s, fill, sizeof(s));
	return s;
}

} // namespace

TEST_CASE("dp_vk_weave_sync: slot 17 is appended after set_overlay_2d_filter_strength")
{
	const size_t base = sizeof(struct xrt_display_processor);
	CHECK(offsetof(struct xrt_display_processor_vk, set_weave_sync) == base + 17 * sizeof(void *));
	// N6 Part 1 (#2144) appended get_eye_positions_at (slot 18) after it.
	CHECK(offsetof(struct xrt_display_processor_vk, get_eye_positions_at) == base + 18 * sizeof(void *));
	// #2340 appended accepts_atlas_encoding (slot 19) after that.
	CHECK(offsetof(struct xrt_display_processor_vk, accepts_atlas_encoding) == base + 19 * sizeof(void *));
	CHECK(sizeof(struct xrt_display_processor_vk) == base + 20 * sizeof(void *));
	CHECK(sizeof(VkSemaphore_XDP) == 8); // pointer on 64-bit, uint64_t on 32-bit: always a VkSemaphore's size
#ifndef XRT_DP_VK_HAS_WEAVE_SYNC
	FAIL("XRT_DP_VK_HAS_WEAVE_SYNC must be defined by the header that carries the slot");
#endif
}

TEST_CASE("dp_vk_weave_sync: an older plug-in is not taken and never called")
{
	reset();
	// A real function pointer sits behind struct_size: a NULL-only check would call it.
	struct xrt_display_processor_vk dp =
	    make_dp((uint32_t)offsetof(struct xrt_display_processor_vk, set_weave_sync), /*with_fn=*/true);
	CHECK_FALSE(xrt_display_processor_vk_set_weave_sync(&dp, sem(0x11), 0, sem(0x22), 0));
	CHECK(g_calls == 0);
}

TEST_CASE("dp_vk_weave_sync: a NULL slot or NULL DP is not taken")
{
	reset();
	struct xrt_display_processor_vk dp = make_dp(sizeof(struct xrt_display_processor_vk), /*with_fn=*/false);
	CHECK_FALSE(xrt_display_processor_vk_set_weave_sync(&dp, sem(0x11), 0, sem(0x22), 0));
	CHECK_FALSE(xrt_display_processor_vk_set_weave_sync(nullptr, sem(0x11), 0, sem(0x22), 0));
	CHECK(g_calls == 0);
}

TEST_CASE("dp_vk_weave_sync: a covering plug-in gets both semaphores and its verdict is returned")
{
	reset();
	struct xrt_display_processor_vk dp = make_dp(sizeof(struct xrt_display_processor_vk), /*with_fn=*/true);
	const VkSemaphore_XDP w = sem(0x11);
	const VkSemaphore_XDP s = sem(0x22);
	CHECK(xrt_display_processor_vk_set_weave_sync(&dp, w, 0, s, 0));
	CHECK(g_calls == 1);
	CHECK(std::memcmp(&g_wait, &w, sizeof(w)) == 0);
	CHECK(std::memcmp(&g_signal, &s, sizeof(s)) == 0);
	CHECK(g_wait_value == 0);
	CHECK(g_signal_value == 0);

	g_accept = false; // a DP may decline per frame (e.g. a timeline value it does not support)
	CHECK_FALSE(xrt_display_processor_vk_set_weave_sync(&dp, w, 7, s, 8));
	CHECK(g_calls == 2);
	CHECK(g_wait_value == 7);
	CHECK(g_signal_value == 8);
}
