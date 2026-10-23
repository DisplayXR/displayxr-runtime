// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The D3D11 display processor's atlas-encoding query (#2273).
 *
 * Pins that `accepts_atlas_encoding` is the APPENDED LAST slot (ADR-020), that
 * its feature macro and @ref XRT_ATLAS_ENCODING_PQ come with the header, and
 * that the helper answers false for every "didn't answer" case, so the runtime
 * never sends PQ to a DP that did not opt in.
 */

#include "catch_amalgamated.hpp"

#include "xrt/xrt_display_color.h"
#include "xrt/xrt_display_processor_d3d11.h"

#include <cstddef>

#ifndef XRT_DP_D3D11_HAS_ATLAS_ENCODING_QUERY
#error "XRT_DP_D3D11_HAS_ATLAS_ENCODING_QUERY must be defined by the header that carries the slot"
#endif

static_assert(offsetof(xrt_display_processor_d3d11, accepts_atlas_encoding) ==
              offsetof(xrt_display_processor_d3d11, get_eye_positions_at) + sizeof(void *));
static_assert(offsetof(xrt_display_processor_d3d11, accepts_atlas_encoding) + sizeof(void *) ==
              sizeof(xrt_display_processor_d3d11));
// The two pre-existing values keep their numbers (LINEAR is matched by value).
static_assert(XRT_ATLAS_ENCODING_ENCODED == 0 && XRT_ATLAS_ENCODING_LINEAR == 1 && XRT_ATLAS_ENCODING_PQ == 2);

namespace {

int g_calls = 0;

bool
accepts_pq_only(xrt_display_processor_d3d11 *, xrt_atlas_encoding encoding)
{
	g_calls++;
	return encoding == XRT_ATLAS_ENCODING_PQ;
}

} // namespace

TEST_CASE("DP d3d11: accepts_atlas_encoding is struct_size-gated and false when unanswered")
{
	CHECK_FALSE(xrt_display_processor_d3d11_accepts_atlas_encoding(nullptr, XRT_ATLAS_ENCODING_PQ));

	xrt_display_processor_d3d11 dp{};
	dp.accepts_atlas_encoding = accepts_pq_only;

	// A plug-in built before the slot: never read.
	dp.struct_size = (uint32_t)offsetof(xrt_display_processor_d3d11, accepts_atlas_encoding);
	g_calls = 0;
	CHECK_FALSE(xrt_display_processor_d3d11_accepts_atlas_encoding(&dp, XRT_ATLAS_ENCODING_PQ));
	CHECK(g_calls == 0);

	// Full size, slot NULL.
	dp.struct_size = (uint32_t)sizeof(xrt_display_processor_d3d11);
	dp.accepts_atlas_encoding = nullptr;
	CHECK_FALSE(xrt_display_processor_d3d11_accepts_atlas_encoding(&dp, XRT_ATLAS_ENCODING_PQ));

	// Full size, slot present: the DP's answer, per encoding.
	dp.accepts_atlas_encoding = accepts_pq_only;
	g_calls = 0;
	CHECK(xrt_display_processor_d3d11_accepts_atlas_encoding(&dp, XRT_ATLAS_ENCODING_PQ));
	CHECK_FALSE(xrt_display_processor_d3d11_accepts_atlas_encoding(&dp, XRT_ATLAS_ENCODING_LINEAR));
	CHECK(g_calls == 2);
}
