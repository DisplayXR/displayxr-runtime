// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  D3D12 renderer for layer compositing.
 * @author David Fattal
 * @ingroup comp_d3d12
 */

#pragma once

#include "xrt/xrt_defines.h"
#include "xrt/xrt_results.h"

#include <stdint.h>
#include <stdbool.h>

// Forward declarations (C++ structs)
struct comp_d3d12_renderer;
struct comp_d3d12_compositor;
struct comp_layer_accum;

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Create a D3D12 renderer.
 *
 * @param c The D3D12 compositor.
 * @param view_width Width of one view (half of atlas texture width for stereo).
 * @param view_height Height of the views.
 * @param target_height Height of the render target (window).
 * @param out_renderer Pointer to receive the created renderer.
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 *
 * @ingroup comp_d3d12
 */
xrt_result_t
comp_d3d12_renderer_create(struct comp_d3d12_compositor *c,
                           uint32_t view_width,
                           uint32_t view_height,
                           uint32_t target_height,
                           struct comp_d3d12_renderer **out_renderer);

/*!
 * Destroy a D3D12 renderer.
 *
 * @ingroup comp_d3d12
 */
void
comp_d3d12_renderer_destroy(struct comp_d3d12_renderer **renderer_ptr);

/*!
 * Render all accumulated layers to the tiled atlas texture.
 *
 * @param renderer The renderer.
 * @param cmd_list D3D12 command list to record onto (void* = ID3D12GraphicsCommandList*).
 * @param layers The accumulated layers.
 * @param left_eye Left eye position for projection (NULL for default).
 * @param right_eye Right eye position for projection (NULL for default).
 * @param target_width Width of the render target (window).
 * @param target_height Height of the render target (window).
 * @param hardware_display_3d True when in 3D mode (stereo rendering),
 *        false for 2D passthrough (mono rendering).
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 *
 * @ingroup comp_d3d12
 */
xrt_result_t
comp_d3d12_renderer_draw(struct comp_d3d12_renderer *renderer,
                         void *cmd_list,
                         struct comp_layer_accum *layers,
                         struct xrt_vec3 *left_eye,
                         struct xrt_vec3 *right_eye,
                         uint32_t target_width,
                         uint32_t target_height,
                         bool hardware_display_3d);

/*!
 * Project-pass half of @ref comp_d3d12_renderer_draw. Records into
 * @p cmd_list:
 *   - barrier atlas PIXEL_SHADER_RESOURCE → RENDER_TARGET
 *   - clear atlas
 *   - per-projection-layer stretch-blit into atlas tiles
 * Atlas is left in RENDER_TARGET (uncommitted; cmd_list still open).
 * Pair with @ref comp_d3d12_renderer_draw_window_space_pass; insert
 * a capture call between them for the projection-only mode (#210).
 *
 * @ingroup comp_d3d12
 */
xrt_result_t
comp_d3d12_renderer_draw_projection_pass(struct comp_d3d12_renderer *renderer,
                                          void *cmd_list,
                                          struct comp_layer_accum *layers,
                                          struct xrt_vec3 *left_eye,
                                          struct xrt_vec3 *right_eye,
                                          uint32_t target_width,
                                          uint32_t target_height,
                                          bool hardware_display_3d);

/*!
 * Window-space-pass half of @ref comp_d3d12_renderer_draw. Records:
 *   - per-window-space-layer textured quad in each atlas tile
 *   - final barrier atlas RENDER_TARGET → PIXEL_SHADER_RESOURCE for DP
 * Expects atlas in RENDER_TARGET on entry (caller transitions back if
 * a capture happened in between, since capture leaves atlas in
 * PIXEL_SHADER_RESOURCE).
 *
 * @ingroup comp_d3d12
 */
xrt_result_t
comp_d3d12_renderer_draw_window_space_pass(struct comp_d3d12_renderer *renderer,
                                            void *cmd_list,
                                            struct comp_layer_accum *layers,
                                            uint32_t target_width,
                                            uint32_t target_height,
                                            bool hardware_display_3d);

/*!
 * Get the atlas texture SRV GPU descriptor handle for weaving.
 *
 * @param renderer The renderer.
 * @return D3D12_GPU_DESCRIPTOR_HANDLE as uint64_t.
 *
 * @ingroup comp_d3d12
 */
uint64_t
comp_d3d12_renderer_get_atlas_srv_handle(struct comp_d3d12_renderer *renderer);

/*!
 * Get the atlas texture SRV CPU descriptor handle (for copying to another heap).
 *
 * @param renderer The renderer.
 * @return D3D12_CPU_DESCRIPTOR_HANDLE as uint64_t.
 *
 * @ingroup comp_d3d12
 */
uint64_t
comp_d3d12_renderer_get_atlas_srv_cpu_handle(struct comp_d3d12_renderer *renderer);

/*!
 * Get atlas texture dimensions (per-view).
 *
 * @param renderer The renderer.
 * @param out_view_width Width of one view.
 * @param out_view_height Height of views.
 *
 * @ingroup comp_d3d12
 */
void
comp_d3d12_renderer_get_view_dimensions(struct comp_d3d12_renderer *renderer,
                                        uint32_t *out_view_width,
                                        uint32_t *out_view_height);

/*!
 * Get the tile layout of the atlas texture.
 *
 * @param renderer The renderer.
 * @param out_tile_columns Number of tile columns (e.g. 2 for stereo).
 * @param out_tile_rows Number of tile rows (e.g. 1 for stereo).
 *
 * @ingroup comp_d3d12
 */
void
comp_d3d12_renderer_get_tile_layout(struct comp_d3d12_renderer *renderer,
                                    uint32_t *out_tile_columns,
                                    uint32_t *out_tile_rows);

/*!
 * Set the tile layout of the atlas texture.
 *
 * @param renderer The renderer.
 * @param tile_columns Number of tile columns.
 * @param tile_rows Number of tile rows.
 *
 * @ingroup comp_d3d12
 */
void
comp_d3d12_renderer_set_tile_layout(struct comp_d3d12_renderer *renderer,
                                    uint32_t tile_columns,
                                    uint32_t tile_rows);

/*!
 * Set legacy app tile scaling flag on the renderer.
 *
 * @param renderer The renderer.
 * @param legacy true if legacy app tile scaling is active.
 *
 * @ingroup comp_d3d12
 */
void
comp_d3d12_renderer_set_legacy_app_tile_scaling(struct comp_d3d12_renderer *renderer,
                                                 bool legacy);

/*!
 * Get the atlas texture resource for direct copy.
 *
 * @param renderer The renderer.
 * @return Pointer to ID3D12Resource.
 *
 * @ingroup comp_d3d12
 */
void *
comp_d3d12_renderer_get_atlas_resource(struct comp_d3d12_renderer *renderer);

/*!
 * Masked 2D-over-3D composite (#439, XR_EXT_local_3d_zone): records a
 * fullscreen-triangle draw into @p cmd_list that writes every pixel of the
 * window region as `M*weave + (1-M)*twod` (per-channel, preserving each
 * layer's own alpha — spec §4.2).
 *
 * The caller owns all resource states: the three sources must be in
 * PIXEL_SHADER_RESOURCE and the destination in RENDER_TARGET when the draw
 * executes. Leaves the composite heap/root-signature/PSO bound on the list.
 *
 * @param renderer The renderer.
 * @param cmd_list Open D3D12 command list (void* = ID3D12GraphicsCommandList*).
 * @param dst_rtv_handle CPU RTV handle of the weave target (D3D12_CPU_DESCRIPTOR_HANDLE::ptr).
 * @param dst_format DXGI_FORMAT of the weave target — selects the PSO
 *                   (R8G8B8A8_UNORM or B8G8R8A8_UNORM; anything else errors).
 * @param twod_resource 2D surround scratch (ID3D12Resource*, region-sized).
 * @param mask_resource Authored mask staged copy (ID3D12Resource*, R8_UNORM).
 * @param weave_resource Weave snapshot scratch (ID3D12Resource*, region-sized).
 * @param region_w,region_h Window region (viewport) in pixels (#464 top-left anchor).
 * @param cx,cy,cw,ch Canvas rect for the analytic-rect shader path (parity only).
 *
 * @ingroup comp_d3d12
 */
xrt_result_t
comp_d3d12_renderer_composite_2d_masked(struct comp_d3d12_renderer *renderer,
                                        void *cmd_list,
                                        uint64_t dst_rtv_handle,
                                        uint32_t dst_format,
                                        void *twod_resource,
                                        void *mask_resource,
                                        void *weave_resource,
                                        uint32_t region_w,
                                        uint32_t region_h,
                                        int32_t cx,
                                        int32_t cy,
                                        uint32_t cw,
                                        uint32_t ch,
                                        bool alpha_over);

/*!
 * Flatten one Local2D layer into the scratch RTV (#439 Phase 3, the `twod`
 * source for comp_d3d12_renderer_composite_2d_masked). Records a
 * fullscreen-triangle draw into @p cmd_list whose viewport is the clipped dest
 * sub-rect, sampling the source image through src_rect (flip_y via negative
 * src_h). Premultiplied vs straight "over" is chosen by @p unpremultiplied.
 *
 * The caller has transitioned the scratch to RENDER_TARGET and cleared it; this
 * function transitions the source image RENDER_TARGET<->PIXEL_SHADER_RESOURCE
 * around its own draw. Each call needs a distinct @p slot_index (D3D12 consumes
 * descriptors at GPU-execute time).
 *
 * @param renderer The renderer.
 * @param cmd_list Open D3D12 command list (void* = ID3D12GraphicsCommandList*).
 * @param scratch_rtv_handle CPU RTV handle of the Local2D scratch (R8G8B8A8_UNORM).
 * @param src_resource The layer's swapchain image (ID3D12Resource*).
 * @param slot_index Unique flatten-heap SRV slot for this draw (< XRT_MAX_LAYERS).
 * @param dst_x,dst_y,dst_w,dst_h Clipped dest sub-rect (viewport) in pixels.
 * @param src_x,src_y,src_w,src_h Source rect (normalized; src_h < 0 => flip_y).
 * @param unpremultiplied True for straight-alpha layers (XRT_LAYER_COMPOSITION_UNPREMULTIPLIED_ALPHA_BIT).
 *
 * @ingroup comp_d3d12
 */
xrt_result_t
comp_d3d12_renderer_flatten_local_2d(struct comp_d3d12_renderer *renderer,
                                     void *cmd_list,
                                     uint64_t scratch_rtv_handle,
                                     void *src_resource,
                                     uint32_t slot_index,
                                     int32_t dst_x,
                                     int32_t dst_y,
                                     uint32_t dst_w,
                                     uint32_t dst_h,
                                     float src_x,
                                     float src_y,
                                     float src_w,
                                     float src_h,
                                     bool unpremultiplied);

/*!
 * Resize the renderer's atlas texture to match a new view size.
 *
 * Recreates the atlas texture, RTV, and SRV at the new dimensions.
 * Shaders and pipeline state are NOT recreated.
 * Does nothing if the dimensions are unchanged.
 *
 * @param renderer The renderer.
 * @param new_view_width New width per view (clamped to minimum 64).
 * @param new_view_height New height per view (clamped to minimum 64).
 * @param new_target_height New render target (window) height.
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 *
 * @ingroup comp_d3d12
 */
xrt_result_t
comp_d3d12_renderer_resize(struct comp_d3d12_renderer *renderer,
                           uint32_t new_view_width,
                           uint32_t new_view_height,
                           uint32_t new_target_height);

#ifdef __cplusplus
}
#endif
