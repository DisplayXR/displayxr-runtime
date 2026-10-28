// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The drag phase-snap slot on the GL and D3D12 display-processor
 *         vtables (#2405).
 *
 * Pins that `snap_window_rect` is the APPENDED LAST slot on both (ADR-020:
 * append-only, no ABI bump), that its feature macro is defined by the header
 * that carries it, and that the wrapper is `struct_size`-gated: a plug-in
 * built before the slot never has it read and reports "no snap", while a DP
 * with the slot gets the caller's points and returns its answer.
 */

#include "catch_amalgamated.hpp"

#include "xrt/xrt_display_processor_d3d12.h"
#include "xrt/xrt_display_processor_gl.h"

#include <cstddef>

#ifndef XRT_DP_GL_HAS_SNAP_WINDOW_RECT
#error "XRT_DP_GL_HAS_SNAP_WINDOW_RECT must be defined by the header that carries the slot"
#endif
#ifndef XRT_DP_D3D12_HAS_SNAP_WINDOW_RECT
#error "XRT_DP_D3D12_HAS_SNAP_WINDOW_RECT must be defined by the header that carries the slot"
#endif

static_assert(offsetof(xrt_display_processor_gl, snap_window_rect) + sizeof(void *) ==
              sizeof(xrt_display_processor_gl));
static_assert(offsetof(xrt_display_processor_d3d12, snap_window_rect) + sizeof(void *) ==
              sizeof(xrt_display_processor_d3d12));

static int g_snap_calls = 0;

template <typename DP>
static bool
fake_snap(DP *xdp, int32_t ox, int32_t oy, int32_t tx, int32_t ty, int32_t *out_x, int32_t *out_y)
{
	(void)xdp;
	(void)ox;
	(void)oy;
	g_snap_calls++;
	*out_x = tx + 1;
	*out_y = ty;
	return true;
}

#define SNAP_GATE_CASE(API)                                                                                            \
	TEST_CASE("DP " #API ": snap_window_rect is struct_size-gated and reports no snap when absent")                \
	{                                                                                                              \
		xrt_display_processor_##API dp{};                                                                      \
		dp.snap_window_rect = fake_snap<xrt_display_processor_##API>;                                          \
		int32_t x = 0;                                                                                         \
		int32_t y = 0;                                                                                         \
		g_snap_calls = 0;                                                                                      \
                                                                                                                       \
		/* Built against the header before #2405: the slot is past struct_size. */                            \
		dp.struct_size = offsetof(xrt_display_processor_##API, snap_window_rect);                             \
		CHECK_FALSE(xrt_display_processor_##API##_snap_window_rect(&dp, 0, 0, 10, 20, &x, &y));               \
		CHECK(g_snap_calls == 0);                                                                              \
                                                                                                                       \
		/* Carries the slot, left NULL. */                                                                     \
		dp.struct_size = sizeof(dp);                                                                           \
		dp.snap_window_rect = nullptr;                                                                         \
		CHECK_FALSE(xrt_display_processor_##API##_snap_window_rect(&dp, 0, 0, 10, 20, &x, &y));               \
		CHECK(g_snap_calls == 0);                                                                              \
                                                                                                                       \
		/* Carries and implements it: the DP's answer comes back. */                                           \
		dp.snap_window_rect = fake_snap<xrt_display_processor_##API>;                                          \
		REQUIRE(xrt_display_processor_##API##_snap_window_rect(&dp, 0, 0, 10, 20, &x, &y));                   \
		CHECK(g_snap_calls == 1);                                                                              \
		CHECK(x == 11);                                                                                        \
		CHECK(y == 20);                                                                                        \
	}

SNAP_GATE_CASE(gl)
SNAP_GATE_CASE(d3d12)
