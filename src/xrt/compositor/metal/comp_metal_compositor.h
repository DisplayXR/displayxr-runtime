// Copyright 2025, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Native Metal compositor — public C header.
 *
 * Mirrors the D3D11 native compositor: creates Metal swapchains directly,
 * renders layers into a tiled atlas texture, optionally weaves
 * through a display processor, and presents to a CAMetalLayer.
 *
 * @author David Fattal
 * @ingroup comp_metal
 */

#pragma once

#include "xrt/xrt_compositor.h"
#include "xrt/xrt_defines.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_display_metrics.h"

// Forward declarations
struct xrt_system_devices;
struct xrt_window_metrics;

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Create a native Metal compositor.
 *
 * @param xdev            HMD device for rendering info.
 * @param window_handle   App-provided NSView* (NULL = auto-create window).
 * @param command_queue   App's id<MTLCommandQueue> from XrGraphicsBindingMetalKHR.
 * @param dp_factory_metal Display processor factory (may be NULL).
 * @param offscreen       If true and window_handle is NULL, create a hidden
 *                        window (no visible UI). Used when the app provides a
 *                        Cocoa window binding with viewHandle=NULL to signal
 *                        offscreen / shared-texture mode.
 * @param shared_iosurface  IOSurfaceRef for shared texture output, or NULL.
 *                        When non-NULL, the compositor renders into this
 *                        IOSurface instead of the CAMetalLayer drawable.
 * @param transparent_background When true, configures the NSWindow (if owned)
 *                        and CAMetalLayer with isOpaque=NO so the desktop
 *                        shows through alpha < 1 regions of the composited
 *                        output. Per-pixel alpha flows through sim_display's
 *                        alpha-native output stage; no chroma-key trick on
 *                        macOS. (Sourced from
 *                        XR_DXR_cocoa_window_binding.transparentBackgroundEnabled.)
 * @param[out] out_xc     Created compositor on success.
 * @return XRT_SUCCESS on success.
 *
 * @ingroup comp_metal
 */
xrt_result_t
comp_metal_compositor_create(struct xrt_device *xdev,
                             void *window_handle,
                             void *command_queue,
                             void *dp_factory_metal,
                             bool offscreen,
                             void *shared_iosurface,
                             bool transparent_background,
                             struct xrt_compositor_native **out_xc);

/*!
 * Get predicted eye positions from the display processor.
 * Returns false if eye tracking is not available.
 */
bool
comp_metal_compositor_get_predicted_eye_positions(struct xrt_compositor *xc,
                                                  struct xrt_eye_positions *out_eye_pos);

/*!
 * Get physical display dimensions in meters.
 * Returns false if not available.
 */
bool
comp_metal_compositor_get_display_dimensions(struct xrt_compositor *xc,
                                             float *out_width_m,
                                             float *out_height_m);

/*!
 * Get window metrics for adaptive FOV calculation.
 * Returns false if not available.
 */
bool
comp_metal_compositor_get_window_metrics(struct xrt_compositor *xc,
                                         struct xrt_window_metrics *out_metrics);

/*
 * XR_DXR_local_3d_zone — authored 2D/3D mask consumer (#439 Phase 3; mirrors
 * the D3D11 entry points from Phase 1, comp_d3d11_compositor.h).
 *
 * The oxr handlers forward here. Tier 1 (whole window) and Tier 2 (rect
 * list) are CPU-authored into a canonical byte buffer and uploaded to an
 * R8Unorm MTLTexture on submit (sticky, last-submit-wins). Tier 3 (freeform
 * render target) has no Metal binding type in header v3 —
 * zone_mask_acquire_rt returns XRT_ERROR_NOT_IMPLEMENTED, which oxr maps to
 * XR_ERROR_FEATURE_UNSUPPORTED.
 *
 * While a submitted mask is active (or Local2D layers imply one), the
 * canvas output rect is superseded: the weave spans the client window and
 * the mask is the sole 2D/3D selector (Phase-2 rule, uniform).
 */

/*!
 * Create the compositor-side mask state (R8Unorm texture, w×h client px;
 * 0 lets the compositor choose the window backing size).
 *
 * @ingroup comp_metal
 */
xrt_result_t
comp_metal_compositor_zone_mask_create(struct xrt_compositor *xc,
                                       uint32_t w, uint32_t h,
                                       void **out_mask);

/*!
 * Tier 1 — fill the whole mask: all-3D (enable_3d) or all-2D.
 *
 * @ingroup comp_metal
 */
xrt_result_t
comp_metal_compositor_zone_mask_set_whole(struct xrt_compositor *xc,
                                          void *mask,
                                          bool enable_3d);

/*!
 * Tier 2 — rasterize client-window-pixel rects as the 3D region (M=1 inside,
 * M=0 elsewhere).
 *
 * @ingroup comp_metal
 */
xrt_result_t
comp_metal_compositor_zone_mask_set_rects(struct xrt_compositor *xc,
                                          void *mask,
                                          uint32_t count,
                                          const struct xrt_rect *rects);

/*!
 * Tier 3 — not available on Metal (no Metal render-target binding in
 * XR_DXR_local_3d_zone v3); always returns XRT_ERROR_NOT_IMPLEMENTED.
 *
 * @ingroup comp_metal
 */
xrt_result_t
comp_metal_compositor_zone_mask_acquire_rt(struct xrt_compositor *xc,
                                           void *mask,
                                           void **out_rt,
                                           uint32_t *out_w,
                                           uint32_t *out_h);

/*!
 * Stage the mask's current contents for the next frame submission (atomic
 * with that frame's weave — spec §9 Q3).
 *
 * @ingroup comp_metal
 */
xrt_result_t
comp_metal_compositor_zone_mask_submit(struct xrt_compositor *xc, void *mask);

/*!
 * Destroy the compositor-side mask state.
 *
 * @ingroup comp_metal
 */
void
comp_metal_compositor_zone_mask_destroy(struct xrt_compositor *xc, void *mask);

/*!
 * XR_DXR_display_zones (ADR-027): set the frame's explicit wish for the next
 * layer_commit — @p mask is the compositor-side mask state of the
 * XrLocal3DZoneMaskDXR referenced via XrDisplayZonesFrameEndInfoDXR.wishMask
 * (oxr_local_3d_zone_ext::comp_mask), or NULL to auto-derive the wish from
 * the frame's zone rects. Called by oxr on every zones frame before
 * xrt_comp_layer_commit; consumed by that commit. No-op outside zones frames.
 *
 * @ingroup comp_metal
 */
void
comp_metal_compositor_zones_set_frame_wish(struct xrt_compositor *xc, void *mask);

/*!
 * Query the display processor's hardware zone grid. Always 0×0 on macOS
 * (sim_display — compositor consumer only); returns false.
 *
 * @ingroup comp_metal
 */
bool
comp_metal_compositor_zone_get_hw_caps(struct xrt_compositor *xc,
                                       uint32_t *out_grid_w,
                                       uint32_t *out_grid_h);

/*!
 * Current recommended per-view render size (client-window-derived when a
 * mask is active, canvas-derived otherwise). Polled by oxr at frame end to
 * fire XrEventDataLocal3DZoneViewSizeChangedDXR on change (#439 Phase 3 Q4).
 *
 * @ingroup comp_metal
 */
bool
comp_metal_compositor_get_recommended_view_size(struct xrt_compositor *xc,
                                                uint32_t *out_w,
                                                uint32_t *out_h);

/*!
 * Request a display mode switch (2D/3D).
 * Returns false if not supported.
 */
bool
comp_metal_compositor_request_display_mode(struct xrt_compositor *xc, bool enable_3d);

/*!
 * Select the eye-tracking control mode (MANAGED=0 / MANUAL=1) on the Metal
 * display processor — the policy counterpart to @ref
 * comp_metal_compositor_request_display_mode. No-op if the DP doesn't react.
 */
void
comp_metal_compositor_set_eye_tracking_mode(struct xrt_compositor *xc, uint32_t mode);

/*!
 * Set system devices for qwerty driver support.
 */
void
comp_metal_compositor_set_system_devices(struct xrt_compositor *xc,
                                         struct xrt_system_devices *xsysd);

/*!
 * Set system compositor info (for HUD display dimensions/nominal viewer).
 */
void
comp_metal_compositor_set_sys_info(struct xrt_compositor *xc,
                                    const struct xrt_system_compositor_info *info);

/*!
 * Mark the compositor's swapchain content as coming from OpenGL (bottom-up origin).
 * The compositor will flip Y when sampling swapchain textures.
 */
void
comp_metal_compositor_set_source_gl(struct xrt_compositor *xc);

/*!
 * Get the MTLTexture (as void*) for a given swapchain image index.
 * Used by Metal native apps to enumerate swapchain images.
 * The handle stored in xrt_image_native is an IOSurfaceRef for cross-API sharing;
 * this function returns the actual id<MTLTexture> wrapping that IOSurface.
 */
void *
comp_metal_swapchain_get_texture(struct xrt_swapchain *xsc, uint32_t index);

/*!
 * Get the system default Metal device (id<MTLDevice> as void*).
 * Used by xrGetMetalGraphicsRequirementsKHR to return a real device pointer.
 */
void *
comp_metal_get_system_default_device(void);

#ifdef __cplusplus
}
#endif
