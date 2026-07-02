// Copyright 2024-2025, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  D3D11 debug GUI readback module.
 * @author David Fattal
 * @ingroup comp_d3d11
 */

#pragma once

#include "xrt/xrt_results.h"

#include <stdbool.h>
#include <stdint.h>

// Forward declarations for D3D11 types
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

struct comp_d3d11_debug;

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Create debug readback module for D3D11 compositor.
 *
 * Creates a staging texture for CPU access and sets up the u_var
 * sink for displaying frames in the debug GUI.
 *
 * @param device D3D11 device for creating resources.
 * @param width Width of the stereo texture.
 * @param height Height of the stereo texture.
 * @param out_debug Pointer to receive the created debug module.
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 *
 * @ingroup comp_d3d11
 */
xrt_result_t
comp_d3d11_debug_create(struct ID3D11Device *device,
                        uint32_t width,
                        uint32_t height,
                        struct comp_d3d11_debug **out_debug);

/*!
 * Copy rendered frame to staging texture and update debug GUI preview.
 *
 * This performs a GPU copy to a staging texture, then maps it for
 * CPU access to push the frame to the u_sink_debug for display.
 *
 * @param debug The debug module.
 * @param context D3D11 immediate context for GPU operations.
 * @param source_texture The stereo texture to read back.
 *
 * @ingroup comp_d3d11
 */
void
comp_d3d11_debug_update_preview(struct comp_d3d11_debug *debug,
                                struct ID3D11DeviceContext *context,
                                struct ID3D11Texture2D *source_texture);

/*!
 * Add debug variables to the u_var system.
 *
 * @param debug The debug module.
 *
 * @ingroup comp_d3d11
 */
void
comp_d3d11_debug_add_vars(struct comp_d3d11_debug *debug);

/*!
 * Check if the debug GUI is active and ready for frames.
 *
 * @param debug The debug module (may be NULL).
 *
 * @return true if debug GUI is active and should receive frames.
 *
 * @ingroup comp_d3d11
 */
bool
comp_d3d11_debug_is_active(struct comp_d3d11_debug *debug);

/*!
 * Destroy the debug module.
 *
 * @param debug_ptr Pointer to the debug module pointer (set to NULL on return).
 *
 * @ingroup comp_d3d11
 */
void
comp_d3d11_debug_destroy(struct comp_d3d11_debug **debug_ptr);

#ifdef __cplusplus
}
#endif
