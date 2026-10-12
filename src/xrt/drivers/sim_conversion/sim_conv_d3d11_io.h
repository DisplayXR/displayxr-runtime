// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The sim conversion provider's D3D11 NT-handle import path (Windows).
 *
 * Exists only to exercise XRT_CONV_MEM_WIN32_NT_HANDLE + XRT_CONV_SYNC_FENCE_NT
 * (in-service, and later across the satellite boundary, on WARP in CI). The
 * compute stays on the CPU so the bytes are identical to the CPU path: an
 * imported input is read back through a staging copy, the output is
 * synthesised into CPU scratch and uploaded with UpdateSubresource.
 *
 * Owns its own D3D11 device (the provider never receives a runtime device),
 * created on the adapter an image names (adapter_luid), else the default
 * hardware adapter, else WARP. Imports are cached by import_key.
 *
 * @ingroup drv_sim_conversion
 */

#pragma once

#include "xrt/xrt_conversion_plugin.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct sim_conv_d3d11_io;

struct sim_conv_d3d11_io *
sim_conv_d3d11_io_create(void);

void
sim_conv_d3d11_io_destroy(struct sim_conv_d3d11_io *io);

//! Drop every cached import (stream_destroy / evict).
void
sim_conv_d3d11_io_clear_cache(struct sim_conv_d3d11_io *io);

/*!
 * Read an NT-handle image into CPU memory owned by @p io, slot @p slot
 * (0 .. 3), waiting its acquire fence first. *out_pixels / *out_stride stay
 * valid until the next read into the same slot.
 */
bool
sim_conv_d3d11_io_read(struct sim_conv_d3d11_io *io,
                       const struct xrt_conv_image *img,
                       uint32_t slot,
                       const uint8_t **out_pixels,
                       uint32_t *out_stride);

//! Upload tightly described CPU pixels into an NT-handle target (its array_layer, or @p layer_override when ≥ 0).
bool
sim_conv_d3d11_io_write(struct sim_conv_d3d11_io *io,
                        const struct xrt_conv_image *img,
                        int32_t layer_override,
                        const void *pixels,
                        uint32_t stride);

/*!
 * After every write: signal each written target's release fence, or — for
 * SYNC_NONE — drain the device so the target is complete before return.
 */
bool
sim_conv_d3d11_io_finish(struct sim_conv_d3d11_io *io, const struct xrt_conv_image *const *written, uint32_t count);

#ifdef __cplusplus
}
#endif
