// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  C-callable "which adapter should render?" resolver (ADR-037 §2).
 * @ingroup aux_d3d
 *
 * Windows only. The C++ flavour (returning the adapter itself) lives in
 * d3d_render_adapter.hpp; both are the same implementation. Render-side sibling
 * of d3d_scanout_helpers, which answers the *other* half of ADR-037's placement
 * rule. See #918 and docs/reference/adapter-selection.md.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Resolve the most capable render adapter per ADR-037 §2 and return its LUID
 * packed the same way Vulkan reports `VkPhysicalDeviceIDProperties::deviceLUID`
 * (i.e. the raw 8 bytes of the Windows `LUID`, little-endian: `LowPart` first).
 *
 * The panel rect is only consulted when `DXR_D3D_FORCE_GPU=scanout` is set — the
 * capability ranking itself needs nothing but DXGI. Pass zeroes when the rect is
 * not known; only the `scanout` keyword degrades (one WARN, then the ranking).
 *
 * @param panel_screen_left Panel left edge in OS virtual-screen coordinates.
 * @param panel_screen_top Panel top edge in OS virtual-screen coordinates.
 * @param panel_pixel_width Panel width in physical pixels (0 = unknown).
 * @param panel_pixel_height Panel height in physical pixels (0 = unknown).
 * @param[out] out_packed_luid Receives the packed LUID on success.
 * @param[out] out_provenance Optional. Receives a short static string naming the
 *        rule that decided (`"most VRAM"`, `"only candidate"`, `"env-forced:
 *        scanout"`, …). Never NULL on success; points to static storage, so it
 *        is safe to hold and safe to log. ADR-037 §4 makes overrides
 *        diagnosable, which requires saying *why*, not just *what*.
 *
 * @return true on success. false when DXGI is unavailable or no adapter
 * survives the exclusions — callers WARN once and fall back to their own
 * selection.
 */
bool
d3d_render_adapter_luid(int32_t panel_screen_left,
                        int32_t panel_screen_top,
                        uint32_t panel_pixel_width,
                        uint32_t panel_pixel_height,
                        uint64_t *out_packed_luid,
                        const char **out_provenance);

/*!
 * Is this adapter one the ADR-037 §2 ranking excludes outright — a software
 * rasterizer (a `DXGI_ADAPTER_FLAG_SOFTWARE` adapter, or the Microsoft Basic
 * Render Driver, i.e. WARP) or a remote adapter?
 *
 * The same predicate the ranking itself uses to drop a candidate (the
 * "software or remote" INFO line); exported because a caller that already
 * holds an adapter for another reason — the SCANOUT adapter, which is chosen
 * by the panel and never ranked — needs the same answer without re-deriving
 * it. Takes the three `DXGI_ADAPTER_DESC1` fields it reads rather than the
 * struct, so this stays a plain C header with no DXGI dependency.
 *
 * @param vendor_id `DXGI_ADAPTER_DESC1::VendorId`.
 * @param device_id `DXGI_ADAPTER_DESC1::DeviceId`.
 * @param dxgi_adapter_flags `DXGI_ADAPTER_DESC1::Flags`; pass 0 when only the
 *        legacy `DXGI_ADAPTER_DESC` is in hand — the Basic Render Driver is
 *        still caught by its vendor/device id.
 */
bool
d3d_adapter_is_software_or_remote(uint32_t vendor_id, uint32_t device_id, uint32_t dxgi_adapter_flags);

#ifdef __cplusplus
}
#endif
