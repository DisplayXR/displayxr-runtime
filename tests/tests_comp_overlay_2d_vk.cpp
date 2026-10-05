// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The Vulkan DP's 2D over-layer slots (ADR-027 Amendment, "2D under
 *         the lens"): append position and the struct_size back-compat gate.
 *
 * An older plug-in's `base.struct_size` ends before `set_overlay_2d`; the bytes
 * past it belong to the plug-in. The runtime must read "unsupported" — never
 * call through them — and keep compositing the layer post-weave itself.
 */

#include "catch_amalgamated.hpp"

#include "xrt/xrt_display_processor_vk.h"

#include <cstring>

namespace {

int g_overlay_calls = 0;
int g_strength_calls = 0;
float g_last_strength = 0.0f;
bool g_last_unchanged = false;
bool g_accept = true;

bool
fake_set_overlay(struct xrt_display_processor_vk *xdp,
                 VkImageView view,
                 VkFormat_XDP format,
                 uint32_t width,
                 uint32_t height,
                 enum xrt_atlas_encoding encoding,
                 bool layer_unchanged)
{
	(void)xdp;
	(void)view;
	(void)format;
	(void)width;
	(void)height;
	(void)encoding;
	g_overlay_calls++;
	g_last_unchanged = layer_unchanged;
	return g_accept;
}

void
fake_set_strength(struct xrt_display_processor_vk *xdp, float strength)
{
	(void)xdp;
	g_strength_calls++;
	g_last_strength = strength;
}

struct xrt_display_processor_vk
make_dp(uint32_t struct_size, bool with_fns)
{
	struct xrt_display_processor_vk dp;
	std::memset(&dp, 0, sizeof(dp));
	dp.base.struct_size = struct_size;
	dp.set_overlay_2d = with_fns ? fake_set_overlay : nullptr;
	dp.set_overlay_2d_filter_strength = with_fns ? fake_set_strength : nullptr;
	return dp;
}

void
reset()
{
	g_overlay_calls = 0;
	g_strength_calls = 0;
	g_last_strength = 0.0f;
	g_last_unchanged = false;
	g_accept = true;
}

// A non-null handle value; the fakes never dereference it.
VkImageView
some_view()
{
	VkImageView v;
	std::memset(&v, 0x5a, sizeof(v));
	return v;
}

} // namespace

TEST_CASE("overlay_2d_vk: both slots are appended after set_transparency_active")
{
	const size_t base = sizeof(struct xrt_display_processor);
	CHECK(offsetof(struct xrt_display_processor_vk, set_overlay_2d) == base + 15 * sizeof(void *));
	CHECK(offsetof(struct xrt_display_processor_vk, set_overlay_2d_filter_strength) == base + 16 * sizeof(void *));
	CHECK(sizeof(struct xrt_display_processor_vk) == base + 17 * sizeof(void *));
}

TEST_CASE("overlay_2d_vk: an older plug-in is unsupported and never called")
{
	reset();
	// Real function pointers sit behind struct_size: a NULL-only check would call them.
	struct xrt_display_processor_vk dp =
	    make_dp((uint32_t)offsetof(struct xrt_display_processor_vk, set_overlay_2d), /*with_fns=*/true);
	CHECK_FALSE(xrt_display_processor_vk_supports_overlay_2d(&dp));
	CHECK_FALSE(
	    xrt_display_processor_vk_set_overlay_2d(&dp, some_view(), 44, 16, 16, XRT_ATLAS_ENCODING_ENCODED, false));
	xrt_display_processor_vk_set_overlay_2d_filter_strength(&dp, 0.5f);
	CHECK(g_overlay_calls == 0);
	CHECK(g_strength_calls == 0);
}

TEST_CASE("overlay_2d_vk: a plug-in with the overlay slot but not the strength slot")
{
	reset();
	struct xrt_display_processor_vk dp = make_dp(
	    (uint32_t)offsetof(struct xrt_display_processor_vk, set_overlay_2d_filter_strength), /*with_fns=*/true);
	CHECK(xrt_display_processor_vk_supports_overlay_2d(&dp));
	xrt_display_processor_vk_set_overlay_2d_filter_strength(&dp, 0.5f);
	CHECK(g_strength_calls == 0);
	CHECK(xrt_display_processor_vk_set_overlay_2d(&dp, some_view(), 44, 16, 16, XRT_ATLAS_ENCODING_ENCODED, true));
	CHECK(g_overlay_calls == 1);
	CHECK(g_last_unchanged);
}

TEST_CASE("overlay_2d_vk: NULL slots or NULL DP are unsupported")
{
	reset();
	struct xrt_display_processor_vk dp = make_dp(sizeof(struct xrt_display_processor_vk), /*with_fns=*/false);
	CHECK_FALSE(xrt_display_processor_vk_supports_overlay_2d(&dp));
	CHECK_FALSE(
	    xrt_display_processor_vk_set_overlay_2d(&dp, some_view(), 44, 16, 16, XRT_ATLAS_ENCODING_ENCODED, false));
	xrt_display_processor_vk_set_overlay_2d_filter_strength(&dp, 0.5f);
	CHECK_FALSE(xrt_display_processor_vk_supports_overlay_2d(nullptr));
	CHECK_FALSE(xrt_display_processor_vk_set_overlay_2d(nullptr, some_view(), 44, 16, 16,
	                                                    XRT_ATLAS_ENCODING_ENCODED, false));
	xrt_display_processor_vk_set_overlay_2d_filter_strength(nullptr, 0.5f);
	CHECK(g_overlay_calls == 0);
	CHECK(g_strength_calls == 0);
}

TEST_CASE("overlay_2d_vk: a current plug-in's verdict is passed through")
{
	reset();
	struct xrt_display_processor_vk dp = make_dp(sizeof(struct xrt_display_processor_vk), /*with_fns=*/true);
	xrt_display_processor_vk_set_overlay_2d_filter_strength(&dp, -1.0f);
	CHECK(g_strength_calls == 1);
	CHECK(g_last_strength == -1.0f);
	CHECK(xrt_display_processor_vk_set_overlay_2d(&dp, some_view(), 44, 16, 16, XRT_ATLAS_ENCODING_ENCODED, false));
	g_accept = false; // a DP that declines: the runtime blends the layer itself
	CHECK_FALSE(
	    xrt_display_processor_vk_set_overlay_2d(&dp, some_view(), 44, 16, 16, XRT_ATLAS_ENCODING_ENCODED, false));
	CHECK(g_overlay_calls == 2);
}
