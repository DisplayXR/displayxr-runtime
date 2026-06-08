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
 * @param view_width Width of one view (half of atlas texture width for stereo).
 * @param view_height Height of the views.
 * @param target_height Height of the render target (window). The internal
 *        atlas texture height is max(view_height, target_height) so that
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
 * Render all accumulated layers to the tiled atlas texture.
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
 * Render the projection-class pass of the atlas (projection /
 * projection-depth / quad / cylinder / equirect / cube layers). Window-
 * space layers are skipped so the compositor can capture an
 * intermediate "projection-only" atlas state between this call and
 * @ref comp_d3d11_renderer_draw_window_space_pass for the runtime-side
 * @c capture_frame projection-only mode (see u_capture_intent.h, #210).
 *
 * Clears the atlas RTV and depth buffer, sets common state, then runs
 * the per-view loop with all non-window-space layer types. After this
 * returns the atlas contains the projection-only composite.
 *
 * @ingroup comp_d3d11
 */
xrt_result_t
comp_d3d11_renderer_draw_projection_pass(struct comp_d3d11_renderer *renderer,
                                          struct comp_layer_accum *layers,
                                          struct xrt_vec3 *left_eye,
                                          struct xrt_vec3 *right_eye,
                                          uint32_t target_width,
                                          uint32_t target_height,
                                          bool hardware_display_3d);

/*!
 * Render only window-space layers into the atlas, on top of whatever
 * the projection pass left behind. Does NOT clear the atlas. State
 * (shaders, blend) is set per-layer inside @c render_window_space_layer.
 *
 * Pair with @ref comp_d3d11_renderer_draw_projection_pass when an
 * intermediate capture point is needed; otherwise call
 * @ref comp_d3d11_renderer_draw which runs both back-to-back.
 *
 * @ingroup comp_d3d11
 */
xrt_result_t
comp_d3d11_renderer_draw_window_space_pass(struct comp_d3d11_renderer *renderer,
                                            struct comp_layer_accum *layers,
                                            uint32_t target_width,
                                            uint32_t target_height,
                                            bool hardware_display_3d);

/*!
 * Get the atlas texture SRV for weaving.
 *
 * Returns the shader resource view of the tiled atlas texture
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
 * Get the atlas render target view (ID3D11RenderTargetView*).
 * @ingroup comp_d3d11
 */
void *
comp_d3d11_renderer_get_atlas_rtv(struct comp_d3d11_renderer *renderer);

/*!
 * Get atlas texture dimensions (per-view).
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
 * @param out_tile_columns Number of tile columns (e.g. 2 for stereo).
 * @param out_tile_rows Number of tile rows (e.g. 1 for stereo).
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
 * Set legacy app tile scaling flag on the renderer.
 *
 * When true, set_tile_layout will not recompute view dimensions,
 * keeping them fixed at the legacy compromise scale.
 *
 * @param renderer The renderer.
 * @param legacy true if legacy app tile scaling is active.
 *
 * @ingroup comp_d3d11
 */
void
comp_d3d11_renderer_set_legacy_app_tile_scaling(struct comp_d3d11_renderer *renderer,
                                                 bool legacy);

/*!
 * Get the atlas texture for debug readback.
 *
 * Returns the ID3D11Texture2D of the tiled atlas texture.
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
 * Resize the renderer's atlas texture to match a new view size.
 *
 * Recreates the atlas texture, SRV, RTV, depth texture, and DSV at the
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
 * Composite a 2D layer over the weaved 3D output, gated by a per-pixel
 * region mask (unified 2D/3D compositing, #439 Phase 0 + Phase 1).
 *
 * The weave target (@p dst_texture) already holds the weaved 3D output.
 *
 * Phase 0 (rect path, @p mask_srv == NULL): derives a hard mask from the
 * canvas sub-rect: pixels INSIDE the canvas keep the weave (the pixel shader
 * discards), pixels OUTSIDE are written from @p twod_srv at 1:1. With a point
 * sampler + opaque output this is pixel-identical to the rectangular
 * strip-copy surround it generalizes.
 *
 * Phase 1 (authored-mask path, @p mask_srv != NULL): samples the scalar mask
 * M and lerps M·weave + (1−M)·twod per pixel — soft edges work, so the weave
 * must be readable: pass an SRV snapshot of dst as @p weave_srv.
 *
 * The pass writes only the @p region_w × @p region_h viewport at the top-left
 * of dst (#464: the 2D layer + mask are window-sized; the window content is
 * anchored top-left inside the worst-case surface). All SRVs are sampled with
 * uv spanning that region, so window-sized inputs map 1:1. Phase 0 passes
 * region == dst dims (full-surface behavior, unchanged).
 *
 * @param renderer The renderer.
 * @param dst_texture Weave target (ID3D11Texture2D*) — holds the weave; a
 *        temporary RTV is created on it.
 * @param twod_srv The 2D layer (ID3D11ShaderResourceView*): an SRV-capable
 *        scratch copy of the app surround texture, region-sized.
 * @param mask_srv Authored scalar mask (R8_UNORM SRV), or NULL for the
 *        Phase 0 analytic rect path.
 * @param weave_srv SRV snapshot of dst's region (weave readable for the
 *        lerp); required iff @p mask_srv is non-NULL.
 * @param region_w Composite region width in pixels (window dims, #464).
 * @param region_h Composite region height in pixels.
 * @param cx,cy,cw,ch The 3D canvas sub-rect (region px) → the Phase 0 mask.
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 *
 * @ingroup comp_d3d11
 */
xrt_result_t
comp_d3d11_renderer_composite_2d_masked(struct comp_d3d11_renderer *renderer,
                                        void *dst_texture,
                                        void *twod_srv,
                                        void *mask_srv,
                                        void *weave_srv,
                                        uint32_t region_w,
                                        uint32_t region_h,
                                        int32_t cx,
                                        int32_t cy,
                                        uint32_t cw,
                                        uint32_t ch,
                                        bool alpha_over);

/*!
 * Flatten one app Local2D layer image into the runtime 2D scratch (#439
 * Phase 3). Draws a textured quad restricted to @p dst_x,dst_y,dst_w,dst_h
 * (the clipped dest sub-rect, window px) into @p scratch_rtv, sampling
 * @p src_srv over the normalized source rect @p src_x,src_y,src_w,src_h
 * (the caller bakes the dest-clip fractions, the layer's norm_rect, and
 * flip_y — a negative @p src_h — into it). The caller clears the scratch
 * transparent once before the layer loop and submits layers in list order;
 * @p unpremultiplied selects straight-alpha over (vs premultiplied over).
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 *
 * @ingroup comp_d3d11
 */
xrt_result_t
comp_d3d11_renderer_flatten_local_2d(struct comp_d3d11_renderer *renderer,
                                     void *scratch_rtv,
                                     void *src_srv,
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
 * Blit the atlas texture to a back buffer with GPU stretching.
 *
 * Used for mono/2D fallback when the atlas texture is smaller than
 * the window (e.g. display processor present → texture_height = view_height
 * which may be less than window height). Renders a fullscreen quad from
 * the atlas texture SRV to the back buffer, stretching to fill the target.
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
