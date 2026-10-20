// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Live desktop behind the window for transparent sessions on a
 *         weaving display processor — the Metal compositor's client of the
 *         displayxr-desktop-capture helper (#2264).
 *
 * The compositor says, once per frame, whether it wants a background and for
 * which display (@ref comp_metal_desktop_bg_want). Only then does this connect
 * to the helper; going unwanted disconnects at once, which stops the capture
 * (and the menu-bar recording indicator) when no other client wants it.
 * Frames are whole-panel, device pixels, sRGB-encoded BGRA8, latest-wins.
 *
 * Missing helper, no grant, or no frame yet ⟹ @ref comp_metal_desktop_bg_acquire
 * returns NULL and the caller weaves without a desktop background, exactly as
 * before this existed. Nothing here ever blocks the render thread.
 *
 * @ingroup comp_metal
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct comp_metal_desktop_bg;

//! @p mtl_device is the compositor's id<MTLDevice>.
struct comp_metal_desktop_bg *
comp_metal_desktop_bg_create(void *mtl_device);

void
comp_metal_desktop_bg_destroy(struct comp_metal_desktop_bg **bg_ptr);

/*!
 * Per frame: whether this session wants the live desktop behind it, and from
 * which display (CGDirectDisplayID). Cheap; connects/disconnects on edges.
 */
void
comp_metal_desktop_bg_want(struct comp_metal_desktop_bg *bg, bool want, uint32_t display_id);

/*!
 * The latest whole-panel frame of @p display_id as an id<MTLTexture>
 * (BGRA8Unorm, sRGB-encoded bytes), or NULL. The texture's IOSurface is held
 * until @p mtl_command_buffer completes, so sample it only on that buffer.
 * The returned pointer is owned by @p bg and valid until the next call.
 */
void *
comp_metal_desktop_bg_acquire(struct comp_metal_desktop_bg *bg,
                              void *mtl_command_buffer,
                              uint32_t display_id,
                              uint32_t *out_width,
                              uint32_t *out_height);

#ifdef __cplusplus
}
#endif
