// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The target-time eye query on every display-processor vtable
 *         (ADR-054 §4.1 / N6 Part 1, #2144).
 *
 * Pins, for all five API variants, that `get_eye_positions_at` is the
 * APPENDED LAST slot (ADR-020: append-only, no ABI bump), that its feature
 * macro is defined by the header that carries it, that the `_has_` helper is
 * `struct_size`-gated (a plug-in built before the slot never has it read),
 * and that the wrapper falls back to `get_predicted_eye_positions` — so an
 * older plug-in keeps working and its answer means "the vendor's default
 * horizon" — while a DP with the slot gets the caller's target.
 */

#include "catch_amalgamated.hpp"

#include "xrt/xrt_display_metrics.h"
#include "xrt/xrt_display_processor.h"
#include "xrt/xrt_display_processor_d3d11.h"
#include "xrt/xrt_display_processor_d3d12.h"
#include "xrt/xrt_display_processor_gl.h"
#include "xrt/xrt_display_processor_metal.h"
#include "xrt/xrt_display_processor_vk.h"

#include <cstddef>

#ifndef XRT_DP_D3D11_HAS_EYE_POSITIONS_AT
#error "XRT_DP_D3D11_HAS_EYE_POSITIONS_AT must be defined by the header that carries the slot"
#endif
#ifndef XRT_DP_D3D12_HAS_EYE_POSITIONS_AT
#error "XRT_DP_D3D12_HAS_EYE_POSITIONS_AT must be defined by the header that carries the slot"
#endif
#ifndef XRT_DP_GL_HAS_EYE_POSITIONS_AT
#error "XRT_DP_GL_HAS_EYE_POSITIONS_AT must be defined by the header that carries the slot"
#endif
#ifndef XRT_DP_METAL_HAS_EYE_POSITIONS_AT
#error "XRT_DP_METAL_HAS_EYE_POSITIONS_AT must be defined by the header that carries the slot"
#endif
#ifndef XRT_DP_VK_HAS_EYE_POSITIONS_AT
#error "XRT_DP_VK_HAS_EYE_POSITIONS_AT must be defined by the header that carries the slot"
#endif

// Append-only: the slot ends each vtable, right after what used to end it.
static_assert(offsetof(xrt_display_processor_d3d11, get_eye_positions_at) ==
              offsetof(xrt_display_processor_d3d11, lift_get_depth) + sizeof(void *));
static_assert(offsetof(xrt_display_processor_d3d11, get_eye_positions_at) + sizeof(void *) ==
              sizeof(xrt_display_processor_d3d11));
static_assert(offsetof(xrt_display_processor_d3d12, get_eye_positions_at) ==
              offsetof(xrt_display_processor_d3d12, set_present_origin) + sizeof(void *));
static_assert(offsetof(xrt_display_processor_d3d12, get_eye_positions_at) + sizeof(void *) ==
              sizeof(xrt_display_processor_d3d12));
static_assert(offsetof(xrt_display_processor_gl, get_eye_positions_at) ==
              offsetof(xrt_display_processor_gl, set_present_origin) + sizeof(void *));
static_assert(offsetof(xrt_display_processor_gl, get_eye_positions_at) + sizeof(void *) ==
              sizeof(xrt_display_processor_gl));
static_assert(offsetof(xrt_display_processor_metal, get_eye_positions_at) ==
              offsetof(xrt_display_processor_metal, snap_window_rect) + sizeof(void *));
static_assert(offsetof(xrt_display_processor_metal, get_eye_positions_at) + sizeof(void *) ==
              sizeof(xrt_display_processor_metal));
// Vulkan: on the VARIANT (the base is embedded by value; a base append would
// move every variant slot), after set_weave_sync.
static_assert(offsetof(xrt_display_processor_vk, get_eye_positions_at) ==
              offsetof(xrt_display_processor_vk, set_weave_sync) + sizeof(void *));
static_assert(offsetof(xrt_display_processor_vk, get_eye_positions_at) + sizeof(void *) ==
              sizeof(xrt_display_processor_vk));
// And the base itself did NOT grow (that would be the silent break).
static_assert(sizeof(xrt_display_processor) == offsetof(xrt_display_processor, clear_local_zone_mask) + sizeof(void *));

namespace {

constexpr int64_t kTarget = 123456789ll;

int g_predicted_calls = 0;
int g_at_calls = 0;
int64_t g_at_target = 0;

template <typename DP>
bool
fake_predicted(DP *, xrt_eye_positions *out)
{
	g_predicted_calls++;
	*out = {};
	out->count = 2;
	out->eyes[0] = {-0.032f, 0.1f, 0.6f};
	out->eyes[1] = {0.032f, 0.1f, 0.6f};
	out->valid = true;
	out->timestamp_ns = 1;
	return true;
}

template <typename DP>
bool
fake_at(DP *, int64_t target_ns, xrt_eye_positions *out)
{
	g_at_calls++;
	g_at_target = target_ns;
	*out = {};
	out->count = 2;
	out->eyes[0] = {-0.032f + 0.01f, 0.1f, 0.6f};
	out->eyes[1] = {0.032f + 0.01f, 0.1f, 0.6f};
	out->valid = true;
	out->timestamp_ns = 2;
	return true;
}

void
reset()
{
	g_predicted_calls = 0;
	g_at_calls = 0;
	g_at_target = 0;
}

} // namespace

// The four flat variants share one shape; the macro keeps the five cases
// literally identical so a drift in one header shows up as one failing case.
#define DP_EYE_AT_CASE(API, DPTYPE, FIRST_FIELD)                                                                       \
	TEST_CASE("DP " #API ": get_eye_positions_at is struct_size-gated and falls back to the vendor horizon")       \
	{                                                                                                              \
		DPTYPE dp{};                                                                                           \
		dp.get_predicted_eye_positions = fake_predicted<DPTYPE>;                                               \
		dp.get_eye_positions_at = fake_at<DPTYPE>;                                                             \
		xrt_eye_positions out{};                                                                               \
		/* A plug-in built before the slot: never read, the fallback answers. */                               \
		dp.FIRST_FIELD = (uint32_t)offsetof(DPTYPE, get_eye_positions_at);                                     \
		CHECK_FALSE(xrt_display_processor_##API##_has_eye_positions_at(&dp));                                  \
		reset();                                                                                               \
		CHECK(xrt_display_processor_##API##_get_eye_positions_at(&dp, kTarget, &out));                         \
		CHECK(g_predicted_calls == 1);                                                                         \
		CHECK(g_at_calls == 0);                                                                                \
		CHECK(out.timestamp_ns == 1);                                                                          \
		/* Full size, slot NULL: the same fallback. */                                                         \
		dp.FIRST_FIELD = (uint32_t)sizeof(DPTYPE);                                                             \
		dp.get_eye_positions_at = nullptr;                                                                     \
		CHECK_FALSE(xrt_display_processor_##API##_has_eye_positions_at(&dp));                                  \
		reset();                                                                                               \
		CHECK(xrt_display_processor_##API##_get_eye_positions_at(&dp, kTarget, &out));                         \
		CHECK(g_predicted_calls == 1);                                                                         \
		CHECK(g_at_calls == 0);                                                                                \
		/* The slot present: the target reaches the DP, the old slot is not touched. */                        \
		dp.get_eye_positions_at = fake_at<DPTYPE>;                                                             \
		CHECK(xrt_display_processor_##API##_has_eye_positions_at(&dp));                                        \
		reset();                                                                                               \
		CHECK(xrt_display_processor_##API##_get_eye_positions_at(&dp, kTarget, &out));                         \
		CHECK(g_at_calls == 1);                                                                                \
		CHECK(g_at_target == kTarget);                                                                         \
		CHECK(g_predicted_calls == 0);                                                                         \
		CHECK(out.timestamp_ns == 2);                                                                          \
		CHECK(out.eyes[0].x == Catch::Approx(-0.022f));                                                        \
		CHECK_FALSE(xrt_display_processor_##API##_has_eye_positions_at(nullptr));                              \
		CHECK_FALSE(xrt_display_processor_##API##_get_eye_positions_at(nullptr, kTarget, &out));               \
	}

DP_EYE_AT_CASE(d3d11, xrt_display_processor_d3d11, struct_size)
DP_EYE_AT_CASE(d3d12, xrt_display_processor_d3d12, struct_size)
DP_EYE_AT_CASE(gl, xrt_display_processor_gl, struct_size)
DP_EYE_AT_CASE(metal, xrt_display_processor_metal, struct_size)

TEST_CASE("DP vk: get_eye_positions_at is base.struct_size-gated and falls back to the base's vendor horizon")
{
	xrt_display_processor_vk dp{};
	dp.base.get_predicted_eye_positions = fake_predicted<xrt_display_processor>;
	dp.get_eye_positions_at = fake_at<xrt_display_processor_vk>;
	xrt_eye_positions out{};

	// A plug-in built against the pre-N6 variant (18 appended slots).
	dp.base.struct_size = (uint32_t)offsetof(xrt_display_processor_vk, get_eye_positions_at);
	CHECK_FALSE(xrt_display_processor_vk_has_eye_positions_at(&dp));
	reset();
	CHECK(xrt_display_processor_vk_get_eye_positions_at(&dp, kTarget, &out));
	CHECK(g_predicted_calls == 1);
	CHECK(g_at_calls == 0);
	CHECK(out.timestamp_ns == 1);

	// A plug-in built against the BASE only (sim_display before the variant).
	dp.base.struct_size = (uint32_t)sizeof(xrt_display_processor);
	CHECK_FALSE(xrt_display_processor_vk_has_eye_positions_at(&dp));
	reset();
	CHECK(xrt_display_processor_vk_get_eye_positions_at(&dp, kTarget, &out));
	CHECK(g_predicted_calls == 1);

	dp.base.struct_size = (uint32_t)sizeof(xrt_display_processor_vk);
	dp.get_eye_positions_at = nullptr;
	CHECK_FALSE(xrt_display_processor_vk_has_eye_positions_at(&dp));
	reset();
	CHECK(xrt_display_processor_vk_get_eye_positions_at(&dp, kTarget, &out));
	CHECK(g_predicted_calls == 1);
	CHECK(g_at_calls == 0);

	dp.get_eye_positions_at = fake_at<xrt_display_processor_vk>;
	CHECK(xrt_display_processor_vk_has_eye_positions_at(&dp));
	reset();
	CHECK(xrt_display_processor_vk_get_eye_positions_at(&dp, kTarget, &out));
	CHECK(g_at_calls == 1);
	CHECK(g_at_target == kTarget);
	CHECK(g_predicted_calls == 0);
	CHECK(out.timestamp_ns == 2);

	// The base wrapper still works on the same object (the variant embeds it).
	reset();
	CHECK(xrt_display_processor_get_predicted_eye_positions(&dp.base, &out));
	CHECK(g_predicted_calls == 1);

	CHECK_FALSE(xrt_display_processor_vk_has_eye_positions_at(nullptr));
	CHECK_FALSE(xrt_display_processor_vk_get_eye_positions_at(nullptr, kTarget, &out));
}
