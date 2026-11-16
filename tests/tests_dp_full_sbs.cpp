// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The stereo_layout field, its clamp and its back-compat gate.
 *
 * xrt_dp_scanout_caps::stereo_layout = 3 (HDMI 1.4b 3D_Structure "Side-by-Side
 * (Full)") makes the runtime present a target twice the window's width and
 * declare the window to a stereo desktop compositor. A wrong "yes" doubles a
 * window and declares a weave as stereo, so the reading must be "no" in every
 * case where the DP did not say 3: a plug-in built before get_scanout_caps
 * (whose slot bytes the runtime must never read), a NULL slot, a declining DP,
 * a DP that reports 0, and a DP that reports a number this runtime does not
 * know (read as 0, the way weave_scope is clamped). Only 3 declares anything.
 */

#include "catch_amalgamated.hpp"

#include "xrt/xrt_display_scanout.h"
#include "xrt/xrt_display_processor_vk.h"

#include <cstring>

namespace {

bool g_called = false;
bool g_answer = true;
uint32_t g_flags = 0;
uint32_t g_layout = 0;

bool
fake_get_scanout_caps(struct xrt_display_processor_vk *xdp, struct xrt_dp_scanout_caps *out_caps)
{
	(void)xdp;
	g_called = true;
	if (!g_answer) {
		return false;
	}
	out_caps->weave_scope = XRT_DP_WEAVE_SCOPE_CANVAS;
	out_caps->flags = g_flags;
	out_caps->stereo_layout = g_layout;
	return true;
}

struct xrt_display_processor_vk
make_dp(uint32_t struct_size, bool with_fn)
{
	struct xrt_display_processor_vk dp;
	std::memset(&dp, 0, sizeof(dp));
	dp.base.struct_size = struct_size;
	dp.get_scanout_caps = with_fn ? fake_get_scanout_caps : nullptr;
	return dp;
}

//! `base.struct_size` of a plug-in built one slot before get_scanout_caps.
constexpr uint32_t
size_without_slot()
{
	return (uint32_t)offsetof(struct xrt_display_processor_vk, get_scanout_caps);
}

} // namespace

TEST_CASE("dp_full_sbs: the field takes the first reserved word and 3 is HDMI's Side-by-Side (Full)")
{
	// Carried in a word of reserved[]: no change to the caps layout.
	CHECK(sizeof(struct xrt_dp_scanout_caps) == XRT_DP_SCANOUT_CAPS_SIZE_V1);
	CHECK(offsetof(struct xrt_dp_scanout_caps, stereo_layout) ==
	      offsetof(struct xrt_dp_scanout_caps, flags) + sizeof(uint32_t));
	CHECK(offsetof(struct xrt_dp_scanout_caps, reserved) ==
	      offsetof(struct xrt_dp_scanout_caps, stereo_layout) + sizeof(uint32_t));

	// HDMI 1.4b 3D_Structure number (include/linux/hdmi.h: SIDE_BY_SIDE_FULL is the fourth entry, 3).
	CHECK(XRT_DP_STEREO_LAYOUT_SIDE_BY_SIDE_FULL == 3);
	// 0 is "an ordinary weave" here, not HDMI's 0 (frame packing): what a plug-in built before the field wrote.
	CHECK(XRT_DP_STEREO_LAYOUT_WEAVE == 0);

	struct xrt_dp_scanout_caps caps;
	xrt_dp_scanout_caps_init(&caps);
	CHECK(caps.stereo_layout == 0);
}

TEST_CASE("dp_full_sbs: the clamp knows 3 and reads every other number as 0")
{
	CHECK(xrt_dp_stereo_layout_clamp(0) == XRT_DP_STEREO_LAYOUT_WEAVE);
	CHECK(xrt_dp_stereo_layout_clamp(3) == XRT_DP_STEREO_LAYOUT_SIDE_BY_SIDE_FULL);
	// The other HDMI 1.4b 3D_Structure numbers (frame packing is 0 above), and numbers past the table.
	for (uint32_t v : {1u, 2u, 4u, 5u, 6u, 7u, 8u, 9u, 15u, 16u, 255u, 0x80000003u, 0xFFFFFFFFu}) {
		CHECK(xrt_dp_stereo_layout_clamp(v) == XRT_DP_STEREO_LAYOUT_WEAVE);
	}
}

TEST_CASE("dp_full_sbs: a pre-append plug-in is not full SBS and is never called")
{
	g_called = false;
	g_answer = true;
	g_layout = XRT_DP_STEREO_LAYOUT_SIDE_BY_SIDE_FULL;
	struct xrt_display_processor_vk dp = make_dp(size_without_slot(), /* with_fn */ true);

	CHECK_FALSE(xrt_display_processor_vk_outputs_full_sbs(&dp));
	CHECK(xrt_display_processor_vk_stereo_layout(&dp) == XRT_DP_STEREO_LAYOUT_WEAVE);
	CHECK_FALSE(g_called);
}

TEST_CASE("dp_full_sbs: NULL slot, a declining DP and no DP are not full SBS")
{
	g_called = false;
	g_answer = true;
	g_layout = XRT_DP_STEREO_LAYOUT_SIDE_BY_SIDE_FULL;
	struct xrt_display_processor_vk no_fn = make_dp(sizeof(no_fn), /* with_fn */ false);
	CHECK_FALSE(xrt_display_processor_vk_outputs_full_sbs(&no_fn));
	CHECK_FALSE(g_called);

	g_answer = false;
	struct xrt_display_processor_vk declining = make_dp(sizeof(declining), /* with_fn */ true);
	CHECK_FALSE(xrt_display_processor_vk_outputs_full_sbs(&declining));
	CHECK(g_called);

	CHECK_FALSE(xrt_display_processor_vk_outputs_full_sbs(nullptr));
}

TEST_CASE("dp_full_sbs: only stereo_layout 3 says full SBS; every other value declares nothing")
{
	g_answer = true;
	struct xrt_display_processor_vk dp = make_dp(sizeof(dp), /* with_fn */ true);

	// A plug-in built before the field wrote 0 here: an ordinary weave.
	g_flags = 0;
	g_layout = 0;
	CHECK_FALSE(xrt_display_processor_vk_outputs_full_sbs(&dp));

	// No flag bit says it any more, and tolerating a resample (anaglyph, squeezed SBS) is not full SBS.
	g_flags = 0xFFFFFFFFu;
	CHECK_FALSE(xrt_display_processor_vk_outputs_full_sbs(&dp));
	CHECK(xrt_display_processor_vk_tolerates_resample(&dp));

	// A number the runtime does not know is read as 0, the way weave_scope is.
	for (uint32_t v : {1u, 2u, 4u, 5u, 6u, 7u, 8u, 9u, 255u, 0x80000003u, 0xFFFFFFFFu}) {
		g_layout = v;
		CHECK_FALSE(xrt_display_processor_vk_outputs_full_sbs(&dp));
		CHECK(xrt_display_processor_vk_stereo_layout(&dp) == XRT_DP_STEREO_LAYOUT_WEAVE);
	}

	// 3 alone, with or without the other flags: full SBS does not imply resample tolerance.
	g_layout = 3;
	g_flags = XRT_DP_SCANOUT_FLAG_TOLERATES_RESAMPLE;
	CHECK(xrt_display_processor_vk_outputs_full_sbs(&dp));
	CHECK(xrt_display_processor_vk_tolerates_resample(&dp));
	g_flags = 0;
	CHECK(xrt_display_processor_vk_outputs_full_sbs(&dp));
	CHECK_FALSE(xrt_display_processor_vk_tolerates_resample(&dp));
}
