// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Small CPU-readable preview of the live desktop behind the window,
 *         the XR_DXR_depth_budget source on macOS (#2264).
 *
 * On Windows and Linux the DP captures the desktop and hands the rear-budget
 * runner a downsampled preview through `get_background_preview`. On macOS the
 * RUNTIME owns that capture (comp_metal_desktop_bg), so the Metal compositor
 * builds the same preview itself: the desktop crop it hands the DP is copied
 * into a mipmapped texture, box-filtered down to <= 512 px on its long side,
 * and read back without ever blocking the frame. The newest completed readback
 * is handed out, normally the previous frame's.
 *
 * @ingroup comp_metal
 */

#pragma once

#include "xrt/xrt_display_processor.h" // struct xrt_dp_background_preview

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct comp_metal_bg_preview;

//! @p mtl_device is an id<MTLDevice>.
struct comp_metal_bg_preview *
comp_metal_bg_preview_create(void *mtl_device);

void
comp_metal_bg_preview_destroy(struct comp_metal_bg_preview **pv_ptr);

/*!
 * Downsample the @p width x @p height top-left region of @p mtl_texture
 * (id<MTLTexture>, BGRA8Unorm holding the desktop under the canvas) and queue
 * its readback, encoded on @p mtl_command_buffer. Skipped (no error) while
 * every slot is still in flight.
 */
void
comp_metal_bg_preview_record(struct comp_metal_bg_preview *pv,
                             void *mtl_command_buffer,
                             void *mtl_texture,
                             uint32_t width,
                             uint32_t height);

/*!
 * Fill @p out (pre-initialised with xrt_dp_background_preview_init) with the
 * newest completed preview. False when none has completed yet. The pixels stay
 * valid until the next call to this function.
 */
bool
comp_metal_bg_preview_get(struct comp_metal_bg_preview *pv, struct xrt_dp_background_preview *out);

#ifdef __cplusplus
}
#endif
