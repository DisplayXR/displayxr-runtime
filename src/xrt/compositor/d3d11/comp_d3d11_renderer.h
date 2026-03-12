// Copyright 2024-2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  D3D11 renderer for layer compositing.
 * @author David Fattal
 * @ingroup comp_d3d11
 */

#pragma once

#include "xrt/xrt_defines.h"
#include "xrt/xrt_results.h"

#include <stdint.h>
#include <stdbool.h>

// Forward declarations (C++ structs)
struct comp_d3d11_renderer;
struct comp_d3d11_compositor;
struct comp_layer_accum;

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Create a D3D11 renderer.
 *
 * @param c The D3D11 compositor.
 * @param view_width Width of one view (half of stereo texture width).
 * @param view_height Height of the views.
 * @param target_height Height of the render target (window). The internal
 *        stereo texture height is max(view_height, target_height) so that
 *        mono (2D) rendering can fill the full window without cropping.
 * @param out_renderer Pointer to receive the created renderer.
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 *
 * @ingroup comp_d3d11
 */
xrt_result_t
comp_d3d11_renderer_create(struct comp_d3d11_compositor *c,
                           uint32_t view_width,
                           uint32_t view_height,
                           uint32_t target_height,
                           struct comp_d3d11_renderer **out_renderer);

/*!
 * Destroy a D3D11 renderer.
 *
 * @ingroup comp_d3d11
 */
void
comp_d3d11_renderer_destroy(struct comp_d3d11_renderer **renderer_ptr);

/*!
 * Render all accumulated layers to the side-by-side stereo texture.
 *
 * @param renderer The renderer.
 * @param layers The accumulated layers.
 * @param left_eye Left eye position for projection (NULL for default).
 * @param right_eye Right eye position for projection (NULL for default).
 * @param target_width Width of the render target (window). Used for mono
 *        viewport sizing so 2D content fills the full window.
 * @param target_height Height of the render target (window).
 * @param hardware_display_3d True when in 3D mode (stereo rendering),
 *        false for 2D passthrough (mono rendering, single view fills window).
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 *
 * @ingroup comp_d3d11
 */
xrt_result_t
comp_d3d11_renderer_draw(struct comp_d3d11_renderer *renderer,
                         struct comp_layer_accum *layers,
                         struct xrt_vec3 *left_eye,
                         struct xrt_vec3 *right_eye,
                         uint32_t target_width,
                         uint32_t target_height,
                         bool hardware_display_3d);

/*!
 * Get the stereo texture SRV for weaving.
 *
 * Returns the shader resource view of the side-by-side stereo texture
 * that should be passed to the weaver.
 *
 * @param renderer The renderer.
 *
 * @return Pointer to the D3D11 shader resource view (ID3D11ShaderResourceView*).
 *
 * @ingroup comp_d3d11
 */
void *
comp_d3d11_renderer_get_atlas_srv(struct comp_d3d11_renderer *renderer);

/*!
 * Get stereo texture dimensions.
 *
 * @param renderer The renderer.
 * @param out_view_width Width of one view.
 * @param out_view_height Height of views.
 *
 * @ingroup comp_d3d11
 */
void
comp_d3d11_renderer_get_view_dimensions(struct comp_d3d11_renderer *renderer,
                                        uint32_t *out_view_width,
                                        uint32_t *out_view_height);

/*!
 * Get the tile layout of the atlas texture.
 *
 * @param renderer The renderer.
 * @param out_tile_columns Number of tile columns (e.g. 2 for stereo SBS).
 * @param out_tile_rows Number of tile rows (e.g. 1 for stereo SBS).
 *
 * @ingroup comp_d3d11
 */
void
comp_d3d11_renderer_get_tile_layout(struct comp_d3d11_renderer *renderer,
                                    uint32_t *out_tile_columns,
                                    uint32_t *out_tile_rows);

/*!
 * Set the tile layout of the atlas texture.
 *
 * @param renderer The renderer.
 * @param tile_columns Number of tile columns.
 * @param tile_rows Number of tile rows.
 *
 * @ingroup comp_d3d11
 */
void
comp_d3d11_renderer_set_tile_layout(struct comp_d3d11_renderer *renderer,
                                    uint32_t tile_columns,
                                    uint32_t tile_rows);

/*!
 * Get the stereo texture for debug readback.
 *
 * Returns the ID3D11Texture2D of the side-by-side stereo texture.
 *
 * @param renderer The renderer.
 *
 * @return Pointer to the D3D11 texture (ID3D11Texture2D*).
 *
 * @ingroup comp_d3d11
 */
void *
comp_d3d11_renderer_get_atlas_texture(struct comp_d3d11_renderer *renderer);

/*!
 * Resize the renderer's stereo texture to match a new view size.
 *
 * Recreates the stereo texture, SRV, RTV, depth texture, and DSV at the
 * new dimensions. Shaders, samplers, and pipeline state objects are NOT
 * recreated. Does nothing if the dimensions are unchanged.
 *
 * @param renderer The renderer.
 * @param new_view_width New width per view (clamped to minimum 64).
 * @param new_view_height New height per view (clamped to minimum 64).
 * @param new_target_height New render target (window) height. The texture
 *        height is max(new_view_height, new_target_height).
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 *
 * @ingroup comp_d3d11
 */
xrt_result_t
comp_d3d11_renderer_resize(struct comp_d3d11_renderer *renderer,
                           uint32_t new_view_width,
                           uint32_t new_view_height,
                           uint32_t new_target_height);

/*!
 * Blit the stereo texture to a back buffer with GPU stretching.
 *
 * Used for mono/2D fallback when the stereo texture is smaller than
 * the window (e.g. display processor present → texture_height = view_height
 * which may be less than window height). Renders a fullscreen quad from
 * the stereo texture SRV to the back buffer, stretching to fill the target.
 *
 * @param renderer The renderer.
 * @param back_buffer_texture The back buffer (ID3D11Texture2D*) to blit to.
 * @param target_width Width of the back buffer in pixels.
 * @param target_height Height of the back buffer in pixels.
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 *
 * @ingroup comp_d3d11
 */
xrt_result_t
comp_d3d11_renderer_blit_stretch(struct comp_d3d11_renderer *renderer,
                                 void *back_buffer_texture,
                                 uint32_t target_width,
                                 uint32_t target_height);

#ifdef __cplusplus
}
#endif
