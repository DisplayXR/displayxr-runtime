// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Per-frame "is the woven content transparent?" probe for the Metal
 *         compositor — the measurement half of lazy transparency, ported from
 *         comp_vk_native_alpha_probe (docs/architecture/transparency-modes.md
 *         § "Capable is not active").
 *
 * Transparency CAPABILITY is fixed at xrCreateSession; whether the content IS
 * transparent is a per-frame fact only the pixels carry (a Ctrl+T toggle
 * changes no layer flag). This probe reads it off the atlas the DP is about to
 * weave: one compute dispatch sampling one texel per stride x stride block and
 * counting alpha < 1. The count is read back without ever blocking, from the
 * newest probe whose command buffer has completed (normally the previous
 * frame's); the caller's hysteresis absorbs a stale read.
 *
 * @ingroup comp_metal
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct comp_metal_alpha_probe;

//! @p mtl_device is an id<MTLDevice>. NULL if the kernel fails to build.
struct comp_metal_alpha_probe *
comp_metal_alpha_probe_create(void *mtl_device);

void
comp_metal_alpha_probe_destroy(struct comp_metal_alpha_probe **probe_ptr);

/*!
 * Count transparent samples in the @p width x @p height top-left region of
 * @p mtl_texture (id<MTLTexture>, any sampleable colour format), encoded on
 * @p mtl_command_buffer.
 */
void
comp_metal_alpha_probe_record(struct comp_metal_alpha_probe *p,
                              void *mtl_command_buffer,
                              void *mtl_texture,
                              uint32_t width,
                              uint32_t height);

/*!
 * The newest completed result: true and @p out_transparent_samples filled when
 * a probe completed since the last read, false otherwise (nothing new).
 */
bool
comp_metal_alpha_probe_read(struct comp_metal_alpha_probe *p, uint32_t *out_transparent_samples);

#ifdef __cplusplus
}
#endif
