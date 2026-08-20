// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  C-callable "which adapter scans out the 3D panel?" resolver.
 * @ingroup aux_d3d
 *
 * Windows only. The C++ flavour (returning the adapter itself) lives in
 * d3d_scanout_helpers.hpp; both are the same implementation. See #918 / #846
 * and docs/reference/adapter-selection.md.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Resolve the adapter that owns the output the 3D panel is scanned out on, and
 * return its LUID packed the same way Vulkan reports
 * `VkPhysicalDeviceIDProperties::deviceLUID` (i.e. the raw 8 bytes of the
 * Windows `LUID`, little-endian: `LowPart` first).
 *
 * The panel rect is the one the display-processor plug-in reports
 * (`xrt_system_compositor_info::display_screen_left/top` +
 * `display_pixel_width/height`). Both origin values being 0 is fine — that is
 * the primary monitor / "no preference" case and still resolves correctly.
 *
 * @param panel_screen_left Panel left edge in OS virtual-screen coordinates.
 * @param panel_screen_top Panel top edge in OS virtual-screen coordinates.
 * @param panel_pixel_width Panel width in physical pixels (0 = unknown → fail).
 * @param panel_pixel_height Panel height in physical pixels (0 = unknown → fail).
 * @param[out] out_packed_luid Receives the packed LUID on success.
 *
 * @return true on success. false (with no logging of its own) when the rect is
 * unusable, DXGI is unavailable, or no adapter claims the panel's monitor —
 * callers WARN once and fall back to normal selection.
 */
bool
d3d_scanout_adapter_luid(int32_t panel_screen_left,
                         int32_t panel_screen_top,
                         uint32_t panel_pixel_width,
                         uint32_t panel_pixel_height,
                         uint64_t *out_packed_luid);

#ifdef __cplusplus
}
#endif
