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

#ifdef __cplusplus
extern "C" {
#endif

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
};

#ifdef __cplusplus
}
#endif
