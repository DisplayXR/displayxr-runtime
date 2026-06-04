// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Color-management encoding-state contract shared across the display
 *         processor interface (ADR-021).
 *
 * Two tiny enums that pin the runtime ↔ display-processor color contract:
 *  - @ref xrt_atlas_encoding  — the runtime's per-frame declaration (carried by
 *    @ref xrt_display_processor::process_atlas) of the encoding state of the
 *    atlas it is handing off.
 *  - @ref xrt_dp_color_capability — the DP's static declaration of which
 *    encoding state(s) it can accept at handoff.
 *
 * Lives in its own leaf header because the per-API DP headers
 * (`xrt_display_processor_{d3d11,d3d12,gl,metal}.h`) are independent
 * translation units that do not include the Vulkan base header — all five
 * include this one so the enums are a single source of truth.
 *
 * @ingroup xrt_iface
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Encoding state of a texture's pixels (ADR-021).
 *
 * A texture holds pixels either scene-referred (proportional to light — the
 * only space in which blending/filtering/MSAA-resolve are correct) or
 * display-referred (in the standard sRGB transfer function — what a display
 * physically wants).
 *
 * This is the value the runtime passes through the @ref
 * xrt_display_processor::process_atlas `atlas_encoding` argument each frame,
 * declaring the encoding of the atlas it is actually sending. It *replaces*
 * the formerly-dead hardcoded graphics-API `format` argument (which DPs
 * ignored for colorspace).
 *
 * Back-compat contract (load-bearing): @ref XRT_ATLAS_ENCODING_ENCODED is 0 so
 * that any stale or real graphics-API format integer a caller might still pass
 * (e.g. `DXGI_FORMAT_R8G8B8A8_UNORM` == 28) does NOT collide with
 * @ref XRT_ATLAS_ENCODING_LINEAR. A DP reading this argument MUST treat
 * `== XRT_ATLAS_ENCODING_LINEAR` specially and ANYTHING ELSE as ENCODED, so a
 * mismatched/legacy runtime degrades to encoded passthrough, never to a
 * spurious "linear" interpretation.
 */
enum xrt_atlas_encoding
{
	XRT_ATLAS_ENCODING_ENCODED = 0, //!< sRGB / display-referred (Model A default).
	XRT_ATLAS_ENCODING_LINEAR = 1,  //!< scene-referred / radiance (Model B).
};

/*!
 * Which atlas encoding state(s) a display processor can accept at handoff
 * (ADR-021 §3). Declared statically by the DP via
 * @ref xrt_display_processor::get_handoff_color_capability.
 *
 * The runtime guarantees it sends an encoding the DP can accept. Per ADR-020
 * (append-only, `struct_size`-gated), the capability slot is optional: an
 * absent slot (older plug-in) or a NULL pointer ⟹ @ref XRT_DP_COLOR_ENCODED,
 * which preserves the de-facto Model-A passthrough behavior for existing
 * plug-ins.
 *
 * A DP whose weaver exposes an explicit input/output sRGB-conversion control
 * declares @ref XRT_DP_COLOR_EITHER and performs the final output encode
 * itself — so the runtime never needs an encode shader (ADR-021 §4).
 */
enum xrt_dp_color_capability
{
	XRT_DP_COLOR_ENCODED = 0, //!< Accepts encoded only (default for legacy plug-ins).
	XRT_DP_COLOR_LINEAR = 1,  //!< Accepts linear only.
	XRT_DP_COLOR_EITHER = 2,  //!< Weaver has a conversion knob; accepts either.
};

#ifdef __cplusplus
}
#endif
