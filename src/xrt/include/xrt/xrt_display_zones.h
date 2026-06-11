// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Local 2D/3D zone types shared across the display processor
 *         interface (#224, docs/roadmap/local-3d-zones.md).
 *
 * The hardware-consumer leg of the shared XR_EXT_local_3d_zone mask: the
 * runtime publishes the authored, screen-anchored 3D-zone mask to the vendor
 * display processor so switchable-lens panels can track which window regions
 * are 3D. The struct below is the capability half of that contract; the
 * publish methods live on the per-API DP vtables (appended per ADR-020).
 *
 * Lives in its own leaf header (like xrt_display_color.h) because the per-API
 * DP headers are independent translation units — each includes this one so
 * the caps struct is a single source of truth.
 *
 * @ingroup xrt_iface
 */

#pragma once

#include <stdint.h>
#include <stddef.h> // offsetof — used by the V1-size tripwire below

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Advisory switching granularity a display processor reports through
 * @ref xrt_dp_local_zone_caps::switch_granularity (ADR-027 Decision 5).
 * Purely informational for the runtime's diagnostics (displayxr-cli) and
 * future scheduling heuristics — NEVER surfaced through the app-facing
 * OpenXR API (no panel-cell geometry leaks past the runtime, Decision 6).
 */
enum xrt_dp_switch_granularity
{
	XRT_DP_SWITCH_GRANULARITY_UNKNOWN = 0,     //!< DP didn't say (or pre-append plug-in: reads as 0).
	XRT_DP_SWITCH_GRANULARITY_GLOBAL = 1,      //!< Whole panel switches as one.
	XRT_DP_SWITCH_GRANULARITY_COLUMN_BAND = 2, //!< Full-height vertical bands (lenticular columns).
	XRT_DP_SWITCH_GRANULARITY_ROW_BAND = 3,    //!< Full-width horizontal bands.
	XRT_DP_SWITCH_GRANULARITY_CELL_GRID = 4,   //!< Independent 2D cells.
};

/*!
 * Local-zone capabilities of a display processor (#224 Phase 0).
 *
 * Crosses the runtime ↔ plug-in ABI boundary, so it follows the ADR-020
 * struct conventions: the CALLER (runtime) sets @ref struct_size to its
 * compile-time sizeof before the query; the plug-in must only write fields
 * that fall inside that size, so the struct is append-only extensible within
 * an ABI major. All fields are fixed-width — no bool — for deterministic
 * layout across compilers.
 *
 * Three meaningful shapes (docs/roadmap/local-3d-zones.md):
 *  - `supported == 0`: legacy DP — the runtime keeps the existing global
 *    request_display_mode path and never calls the publish methods.
 *  - `supported == 1, zone_grid == 1×1`: the DP implements the zone API but
 *    the hardware is single-zone. The OR-union of published masks collapses
 *    to "any client wants 3D anywhere → panel is 3D" — bit-compatible with
 *    today's global arbitration.
 *  - `supported == 1, zone_grid > 1×1`: real per-zone hardware.
 *
 * Append contract (ADR-027 Decision 5, ADR-020 append-only): the CALLER
 * ZEROES the whole struct, then sets @ref struct_size to its own compile-time
 * sizeof before the query. The plug-in writes appended fields ONLY when the
 * caller's struct_size covers them; on the caller side a field an older
 * plug-in never wrote therefore reads 0 (UNKNOWN / not-fractional) — absence
 * is always the conservative default.
 */
struct xrt_dp_local_zone_caps
{
	//! sizeof(struct xrt_dp_local_zone_caps) at the RUNTIME's compile time.
	//! Set by the caller before get_local_zone_caps; the plug-in writes only
	//! fields within this size (ADR-020 append-only).
	uint32_t struct_size;

	//! 1 when this DP consumes published zone masks, 0 for legacy DPs.
	uint32_t supported;

	//! Hardware switchable cells across (1 = global on/off).
	uint32_t zone_grid_width;
	//! Hardware switchable cells down.
	uint32_t zone_grid_height;

	//! Upper bound on the published mask resolution the DP will sample.
	//! 0 = no preference (runtime publishes at its native mask resolution).
	uint32_t max_mask_width;
	uint32_t max_mask_height;

	//! Vendor's preferred max mask-refresh rate; 0 = unlimited. Advisory in
	//! Phase 0 — the runtime publishes per frame and the vendor coalesces.
	uint32_t max_update_hz;

	/*
	 * ── ADR-027 wish-generalization appends (everything below is past the
	 *    V1 shape; struct_size-gated per the append contract above). ──
	 */

	//! 1 = the DP meaningfully consumes fractional wish values M ∈ (0,1)
	//! (blend, dither, partial drive — its call). 0 = the DP quantizes by
	//! the existing any-nonzero rule (the default, conformant quantization
	//! of the wish) — fractional values still produce the runtime's visual
	//! blend; only the physical switch is binary.
	uint32_t wish_fractional;

	//! Advisory @ref xrt_dp_switch_granularity (as uint32_t for fixed-width
	//! ABI layout). Informational only — never app-visible (ADR-027
	//! Decision 6).
	uint32_t switch_granularity;

	//! Reserved for the v2 effective-state readback caps (ADR-027 Decision 2
	//! — what the firmware actually switched, post-quantization). Plug-ins
	//! MUST write 0.
	uint32_t reserved[4];
};

/*!
 * Size of the V1 (pre-ADR-027) caps shape: the 7 uint32 fields up to and
 * including @ref xrt_dp_local_zone_caps::max_update_hz. Plug-ins accept any
 * caller struct_size >= this (rejecting only callers older than the zone API
 * itself), so a plug-in built against the appended header keeps working with
 * an older runtime that only knows the V1 shape.
 */
#define XRT_DP_LOCAL_ZONE_CAPS_SIZE_V1 28

#ifndef XRT_DP_ABI_ASSERT
#if defined(__cplusplus)
#define XRT_DP_ABI_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define XRT_DP_ABI_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif
#endif

// V1-size tripwire: the first appended field must sit exactly at the old
// sizeof. If this trips you inserted mid-struct — that's an ABI break
// (ADR-020): append at the end instead.
XRT_DP_ABI_ASSERT(offsetof(struct xrt_dp_local_zone_caps, wish_fractional) == XRT_DP_LOCAL_ZONE_CAPS_SIZE_V1,
                  "xrt_dp_local_zone_caps V1 layout changed — append-only (ADR-020/ADR-027)");
XRT_DP_ABI_ASSERT(sizeof(struct xrt_dp_local_zone_caps) == XRT_DP_LOCAL_ZONE_CAPS_SIZE_V1 + 6 * sizeof(uint32_t),
                  "xrt_dp_local_zone_caps appended layout changed — update the size assert + ADR-027 notes");

#ifdef __cplusplus
}
#endif
