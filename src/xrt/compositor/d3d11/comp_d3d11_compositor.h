// Copyright 2024-2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Native D3D11 compositor that bypasses Vulkan entirely.
 * @author David Fattal
 * @ingroup comp_d3d11
 */

#pragma once

#include "xrt/xrt_compositor.h"
#include "xrt/xrt_defines.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_results.h"
#include "xrt/xrt_display_metrics.h"

// Forward declarations
struct comp_d3d11_compositor;
struct xrt_system_devices;

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Create a native D3D11 compositor.
 *
 * This compositor renders directly using D3D11 without any Vulkan involvement,
 * solving interop issues on Intel integrated GPUs where importing D3D11 textures
 * into Vulkan fails with VK_ERROR_FORMAT_NOT_SUPPORTED.
 *
 * @param xdev The device we are displaying to.
 * @param hwnd The window handle from XR_EXT_win32_window_binding (or NULL for fullscreen).
 * @param d3d11_device The D3D11 device from the application's graphics binding.
 * @param dp_factory_d3d11 Display processor factory (xrt_dp_factory_d3d11_fn_t), or NULL.
 * @param transparent_background When true (and hwnd != NULL), bind the swapchain via
 *                               DirectComposition with ALPHA_MODE_PREMULTIPLIED for
 *                               desktop transparency. Otherwise opaque (#163 default).
 * @param chroma_key_color When non-zero (and transparent_background is true), enable
 *                         a post-weave shader pass that converts pixels matching this
 *                         RGB (0x00BBGGRR / Win32 COLORREF) to alpha=0. Required when
 *                         the bound display processor strips alpha during weaving.
 * @param display_screen_left Display top-left X in OS screen coords (from xsysc->info,
 *                            populated by the vendor plug-in iface). 0 = primary.
 * @param display_screen_top  Display top-left Y in OS screen coords. 0 = primary.
 * @param out_xc Pointer to receive the created compositor.
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 *
 * @ingroup comp_d3d11
 */
xrt_result_t
comp_d3d11_compositor_create(struct xrt_device *xdev,
                             void *hwnd,
                             void *d3d11_device,
                             void *dp_factory_d3d11,
                             void *shared_texture_handle,
                             bool transparent_background,
                             uint32_t chroma_key_color,
                             int32_t display_screen_left,
                             int32_t display_screen_top,
                             struct xrt_compositor_native **out_xc);

/*!
 * Set the output rect within the app's client area where the shared
 * texture will be displayed. The runtime positions the hidden SR weaver
 * window at this sub-rect for correct interlacing alignment.
 *
 * Only meaningful in shared-texture mode with an app HWND.
 * Call when the blit viewport changes (e.g. on window resize).
 *
 * @param xc The compositor.
 * @param x  Left edge in client-area pixels.
 * @param y  Top edge in client-area pixels.
 * @param w  Width in pixels.
 * @param h  Height in pixels.
 *
 * @ingroup comp_d3d11
 */
void
comp_d3d11_compositor_set_output_rect(struct xrt_compositor *xc,
                                       int32_t x, int32_t y,
                                       uint32_t w, uint32_t h);

/*!
 * Register a full-window 2D shared texture for the surround region (Spec v6).
 *
 * Pairs with comp_d3d11_compositor_set_output_rect: the canvas rect tells
 * the runtime where to weave 3D content, this call tells it where to find
 * 2D pixels for the rest of the target swapchain. The compositor blits
 * non-canvas pixels from this texture into the target each frame.
 *
 * Pass shared_handle == NULL to clear the surround (falls back to undefined
 * non-canvas pixels — the pre-v6 behavior).
 *
 * Texture format: DXGI_FORMAT_R8G8B8A8_UNORM or *_UNORM_SRGB. Synchronization
 * uses IDXGIKeyedMutex on key 0. Dimensions must equal the HWND client area.
 *
 * @param xc            The compositor.
 * @param shared_handle D3D11 shared NT HANDLE, or NULL to clear.
 * @param w             Texture width in pixels.
 * @param h             Texture height in pixels.
 *
 * @ingroup comp_d3d11
 */
void
comp_d3d11_compositor_set_surround_2d(struct xrt_compositor *xc,
                                       void *shared_handle,
                                       uint32_t w, uint32_t h);

/*
 * XR_EXT_local_3d_zone — authored 2D/3D mask consumer (Phase 1 of unified
 * 2D/3D compositing, docs/roadmap/unified-2d-3d-phase1-impl.md §3–§5).
 *
 * The oxr handlers forward here; the mask generalizes the surround path's
 * rect-derived 2D region to an arbitrary scalar mask (the Phase-0 shader's
 * use_rect_mask = 0 path). Each function takes/returns an opaque per-mask
 * pointer owned by the compositor (an R8_UNORM texture + views).
 *
 * Submission is sticky, last-submit-wins: zone_mask_submit snapshots the
 * mask and makes it THE active mask until a later submit or its destroy
 * (destroy reverts the compositor to the rect-surround behavior). The mask
 * and the 2D layer are window-sized (client-window pixels, #464); the
 * consumer composites the window region at the top-left anchor of the
 * worst-case surface and never writes beyond it.
 */

/*!
 * Create the compositor-side mask state (R8_UNORM texture, w×h client px).
 *
 * @param xc       The compositor.
 * @param w        Mask width in client-window pixels.
 * @param h        Mask height in client-window pixels.
 * @param out_mask Receives the opaque per-mask state.
 *
 * @ingroup comp_d3d11
 */
xrt_result_t
comp_d3d11_compositor_zone_mask_create(struct xrt_compositor *xc,
                                       uint32_t w, uint32_t h,
                                       void **out_mask);

/*!
 * Tier 1 — fill the whole mask: all-3D (enable_3d) or all-2D.
 *
 * @ingroup comp_d3d11
 */
xrt_result_t
comp_d3d11_compositor_zone_mask_set_whole(struct xrt_compositor *xc,
                                          void *mask,
                                          bool enable_3d);

/*!
 * Tier 2 — rasterize client-window-pixel rects as the 3D region (M=1 inside,
 * M=0 elsewhere).
 *
 * @ingroup comp_d3d11
 */
xrt_result_t
comp_d3d11_compositor_zone_mask_set_rects(struct xrt_compositor *xc,
                                          void *mask,
                                          uint32_t count,
                                          const struct xrt_rect *rects);

/*!
 * Tier 3 — hand back the RTV on the mask texture for freeform app drawing.
 *
 * @param out_rtv Receives an ID3D11RenderTargetView* (as void*).
 *
 * @ingroup comp_d3d11
 */
xrt_result_t
comp_d3d11_compositor_zone_mask_acquire_rt(struct xrt_compositor *xc,
                                           void *mask,
                                           void **out_rtv,
                                           uint32_t *out_w,
                                           uint32_t *out_h);

/*!
 * Stage the mask's current contents for the next frame submission (atomic
 * with that frame's weave — spec §9 Q3).
 *
 * @ingroup comp_d3d11
 */
xrt_result_t
comp_d3d11_compositor_zone_mask_submit(struct xrt_compositor *xc, void *mask);

/*!
 * Destroy the compositor-side mask state.
 *
 * @ingroup comp_d3d11
 */
void
comp_d3d11_compositor_zone_mask_destroy(struct xrt_compositor *xc, void *mask);

/*!
 * Query the display processor's hardware zone grid (#224 hardware-DP leg).
 * Returns false on a legacy DP (no zone support) with grids zeroed; on true
 * the grid dims feed XrLocal3DZoneCapabilitiesEXT.hardwareZoneGridWidth/Height
 * (1×1 = global on/off panel).
 *
 * @ingroup comp_d3d11
 */
bool
comp_d3d11_compositor_zone_get_hw_caps(struct xrt_compositor *xc,
                                       uint32_t *out_grid_w,
                                       uint32_t *out_grid_h);

/*!
 * Get the predicted eye positions from the display processor.
 *
 * @param xc The compositor.
 * @param out_eye_pos Output eye positions (N-view).
 *
 * @return true if eye tracking is available and positions were retrieved.
 *
 * @ingroup comp_d3d11
 */
bool
comp_d3d11_compositor_get_predicted_eye_positions(struct xrt_compositor *xc,
                                                  struct xrt_eye_positions *out_eye_pos);

/*!
 * Get the display dimensions from the display processor.
 *
 * @param xc The compositor.
 * @param out_width_m Pointer to receive display width in meters.
 * @param out_height_m Pointer to receive display height in meters.
 *
 * @return true if dimensions are available.
 *
 * @ingroup comp_d3d11
 */
bool
comp_d3d11_compositor_get_display_dimensions(struct xrt_compositor *xc,
                                              float *out_width_m,
                                              float *out_height_m);

/*!
 * Get window metrics for adaptive FOV and eye position adjustment.
 *
 * Computes display pixel info, window client area geometry, and derived
 * physical sizes / center offsets needed for Kooima FOV correction.
 *
 * @param xc The compositor.
 * @param[out] out_metrics Pointer to receive the computed window metrics.
 *
 * @return true if valid window metrics are available.
 *
 * @ingroup comp_d3d11
 */
bool
comp_d3d11_compositor_get_window_metrics(struct xrt_compositor *xc,
                                          struct xrt_window_metrics *out_metrics);

/*!
 * Request display mode switch (2D/3D) via display processor.
 *
 * @param xc The compositor.
 * @param enable_3d true to switch to 3D mode, false for 2D mode.
 * @return true on success.
 *
 * @ingroup comp_d3d11
 */
bool
comp_d3d11_compositor_request_display_mode(struct xrt_compositor *xc, bool enable_3d);

/*!
 * Set the system devices for the debug GUI (needed for qwerty driver support).
 *
 * This should be called after creating the compositor when xsysd is available.
 * The debug GUI needs xsysd to route keyboard events to qwerty devices.
 *
 * @param xc The compositor.
 * @param xsysd The system devices (may be NULL to disable qwerty support).
 *
 * @ingroup comp_d3d11
 */
void
comp_d3d11_compositor_set_system_devices(struct xrt_compositor *xc,
                                          struct xrt_system_devices *xsysd);

/*!
 * Set the legacy app tile scaling flag for the compositor.
 *
 * When true, the compositor disables direct rendering mode selection via
 * qwerty 1/2/3 keys and keeps view dimensions fixed at the compromise scale.
 *
 * @param xc The compositor.
 * @param legacy true if legacy app tile scaling is active.
 * @param scale_x Compromise view scale X (e.g. 0.5 for SBS).
 * @param scale_y Compromise view scale Y (e.g. 1.0 for SBS).
 * @param view_w Recommended per-view width in pixels (from oxr_system_fill_in).
 * @param view_h Recommended per-view height in pixels (from oxr_system_fill_in).
 *
 * @ingroup comp_d3d11
 */
void
comp_d3d11_compositor_set_legacy_app_tile_scaling(struct xrt_compositor *xc,
                                                   bool legacy,
                                                   float scale_x,
                                                   float scale_y,
                                                   uint32_t view_w,
                                                   uint32_t view_h);

#ifdef __cplusplus
}
#endif
