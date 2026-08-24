// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Weave-scope declaration + its back-compat gate.
 *
 * The whole value of this slot is that a plug-in built BEFORE it existed keeps
 * behaving identically, so the gate is what these tests pin: an older
 * `struct_size` must resolve to @ref XRT_DP_WEAVE_SCOPE_CANVAS *without the
 * runtime ever dereferencing the slot* — the bytes behind it belong to the
 * plug-in, and reading them is the silent-corruption failure ADR-020 exists to
 * prevent.
 */

#include "catch_amalgamated.hpp"

#include "xrt/xrt_display_scanout.h"
#include "xrt/xrt_display_processor_d3d11.h"

#include <cstring>

namespace {

//! Set when the vtable slot is entered, so a test can assert it was NOT.
bool g_called = false;
uint32_t g_reported = XRT_DP_WEAVE_SCOPE_CANVAS;
bool g_answer = true;

bool
fake_get_scanout_caps(struct xrt_display_processor_d3d11 *xdp, struct xrt_dp_scanout_caps *out_caps)
{
	(void)xdp;
	g_called = true;
	if (!g_answer) {
		return false;
	}
	out_caps->weave_scope = g_reported;
	return true;
}

//! A vtable whose `struct_size` the test controls, as a plug-in's would be.
struct xrt_display_processor_d3d11
make_dp(uint32_t struct_size, bool with_fn)
{
	struct xrt_display_processor_d3d11 dp;
	std::memset(&dp, 0, sizeof(dp));
	dp.struct_size = struct_size;
	dp.get_scanout_caps = with_fn ? fake_get_scanout_caps : nullptr;
	return dp;
}

//! `struct_size` of a plug-in built one slot before this one existed.
constexpr uint32_t
size_without_slot()
{
	return (uint32_t)offsetof(struct xrt_display_processor_d3d11, get_scanout_caps);
}

} // namespace

TEST_CASE("dp_weave_scope: caps init stamps the caller's size")
{
	struct xrt_dp_scanout_caps caps;
	std::memset(&caps, 0xAB, sizeof(caps));
	xrt_dp_scanout_caps_init(&caps);

	CHECK(caps.struct_size == sizeof(struct xrt_dp_scanout_caps));
	CHECK(caps.weave_scope == XRT_DP_WEAVE_SCOPE_CANVAS);
	for (size_t i = 0; i < sizeof(caps.reserved) / sizeof(caps.reserved[0]); i++) {
		CHECK(caps.reserved[i] == 0);
	}
}

TEST_CASE("dp_weave_scope: V1 size constant matches the struct")
{
	CHECK(sizeof(struct xrt_dp_scanout_caps) == XRT_DP_SCANOUT_CAPS_SIZE_V1);
}

TEST_CASE("dp_weave_scope: unknown scopes clamp to canvas")
{
	CHECK(xrt_dp_weave_scope_clamp(XRT_DP_WEAVE_SCOPE_CANVAS) == XRT_DP_WEAVE_SCOPE_CANVAS);
	CHECK(xrt_dp_weave_scope_clamp(XRT_DP_WEAVE_SCOPE_REGION) == XRT_DP_WEAVE_SCOPE_REGION);
	CHECK(xrt_dp_weave_scope_clamp(XRT_DP_WEAVE_SCOPE_SCANOUT) == XRT_DP_WEAVE_SCOPE_SCANOUT);

	// A plug-in built against a NEWER header may name a scope this build has
	// never heard of. The only safe reading is the default one.
	CHECK(xrt_dp_weave_scope_clamp(3) == XRT_DP_WEAVE_SCOPE_CANVAS);
	CHECK(xrt_dp_weave_scope_clamp(0xFFFFFFFFu) == XRT_DP_WEAVE_SCOPE_CANVAS);
}

TEST_CASE("dp_weave_scope: only scanout scope needs the whole panel")
{
	CHECK_FALSE(xrt_dp_weave_scope_needs_panel(XRT_DP_WEAVE_SCOPE_CANVAS));
	CHECK_FALSE(xrt_dp_weave_scope_needs_panel(XRT_DP_WEAVE_SCOPE_REGION));
	CHECK(xrt_dp_weave_scope_needs_panel(XRT_DP_WEAVE_SCOPE_SCANOUT));

	CHECK(std::strcmp(xrt_dp_weave_scope_name(XRT_DP_WEAVE_SCOPE_CANVAS), "canvas") == 0);
	CHECK(std::strcmp(xrt_dp_weave_scope_name(XRT_DP_WEAVE_SCOPE_REGION), "region") == 0);
	CHECK(std::strcmp(xrt_dp_weave_scope_name(XRT_DP_WEAVE_SCOPE_SCANOUT), "scanout") == 0);
}

TEST_CASE("dp_weave_scope: a pre-append plug-in is canvas and is never called")
{
	// The exact shape of every plug-in that shipped before this slot: the
	// function pointer bytes are NOT part of what it allocated, so the gate
	// must refuse to touch them even though our local struct has them.
	g_called = false;
	g_answer = true;
	g_reported = XRT_DP_WEAVE_SCOPE_SCANOUT;

	struct xrt_display_processor_d3d11 dp = make_dp(size_without_slot(), /* with_fn */ true);

	CHECK(xrt_display_processor_d3d11_get_weave_scope(&dp) == XRT_DP_WEAVE_SCOPE_CANVAS);
	CHECK_FALSE(g_called);
}

TEST_CASE("dp_weave_scope: NULL slot on a current plug-in is canvas")
{
	g_called = false;
	struct xrt_display_processor_d3d11 dp = make_dp(sizeof(dp), /* with_fn */ false);

	CHECK(xrt_display_processor_d3d11_get_weave_scope(&dp) == XRT_DP_WEAVE_SCOPE_CANVAS);
	CHECK_FALSE(g_called);
}

TEST_CASE("dp_weave_scope: a declining DP is canvas")
{
	g_called = false;
	g_answer = false;
	struct xrt_display_processor_d3d11 dp = make_dp(sizeof(dp), /* with_fn */ true);

	CHECK(xrt_display_processor_d3d11_get_weave_scope(&dp) == XRT_DP_WEAVE_SCOPE_CANVAS);
	CHECK(g_called);
}

TEST_CASE("dp_weave_scope: a hardware weaver's declaration survives the round trip")
{
	struct xrt_display_processor_d3d11 dp = make_dp(sizeof(dp), /* with_fn */ true);
	g_answer = true;

	g_reported = XRT_DP_WEAVE_SCOPE_REGION;
	CHECK(xrt_display_processor_d3d11_get_weave_scope(&dp) == XRT_DP_WEAVE_SCOPE_REGION);

	g_reported = XRT_DP_WEAVE_SCOPE_SCANOUT;
	CHECK(xrt_display_processor_d3d11_get_weave_scope(&dp) == XRT_DP_WEAVE_SCOPE_SCANOUT);

	// Forward-version scope from a newer plug-in: clamped, not trusted.
	g_reported = 42;
	CHECK(xrt_display_processor_d3d11_get_weave_scope(&dp) == XRT_DP_WEAVE_SCOPE_CANVAS);
}

TEST_CASE("dp_weave_scope: the caps call honours the caller's struct_size")
{
	struct xrt_display_processor_d3d11 dp = make_dp(sizeof(dp), /* with_fn */ true);
	g_answer = true;
	g_reported = XRT_DP_WEAVE_SCOPE_SCANOUT;

	struct xrt_dp_scanout_caps caps;
	xrt_dp_scanout_caps_init(&caps);
	REQUIRE(xrt_display_processor_d3d11_get_scanout_caps(&dp, &caps));
	CHECK(caps.weave_scope == XRT_DP_WEAVE_SCOPE_SCANOUT);
	CHECK(caps.struct_size == sizeof(caps));
}
