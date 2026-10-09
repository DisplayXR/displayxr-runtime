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
#include "xrt/xrt_screen.h"
#include "util/u_cursor_depth.h"

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
 * Runtime-owned window placement on macOS (ADR-050): take over the bound
 * window's title-bar drag, edge / corner resize and zoom, phase-snap every
 * step through the DP's `snap_window_rect` and apply it in the same Core
 * Animation transaction as the frame woven for it. A no-op (AppKit keeps the
 * window, logged once) when @p app_owned (the app chained
 * `XR_COCOA_WINDOW_PLACEMENT_APP_OWNED_BIT_DXR`), `DXR_MACOS_NATIVE_DRAG=1`,
 * an offscreen / shared-IOSurface / workspace session, or a bound view that
 * is not its window's contentView. Call once, after creation.
 *
 * @ingroup comp_metal
 */
void
comp_metal_compositor_setup_window_placement(struct xrt_compositor *xc, bool app_owned);

/*!
 * Phase-snap a proposed window origin with the session's display processor
 * (xrt_display_processor_metal::snap_window_rect): the in-process route of
 * xrWeaveSnapWindowRectDXR, for an app that owns its own placement. Backing
 * px, same frame for both points. Outputs are always written (the target on
 * false = no snap support / declined).
 *
 * @ingroup comp_metal
 */
bool
comp_metal_compositor_snap_window_rect(struct xrt_compositor *xc,
                                       int32_t origin_x,
                                       int32_t origin_y,
                                       int32_t target_x,
                                       int32_t target_y,
                                       int32_t *out_x,
                                       int32_t *out_y);

/*!
 * Multi-screen on macOS (ADR-047 D2): hand the compositor the system's screen
 * list and DP registry, so a window whose content view spans displays is
 * woven per segment, each by its own display's DP
 * (`create_dp_metal_for_screen`, windowless, phase from set_present_origin).
 * A session pinned to one display (@p pinned_display_id != 0, from
 * `XrSessionDisplayBindingDXR`) or `DXR_SEGMENTS=0` is never segmented. A
 * window on the primary display only keeps the single-DP path exactly.
 *
 * @ingroup comp_metal
 */
void
comp_metal_compositor_set_screens(struct xrt_compositor *xc,
                                  const struct xrt_screen_list *list,
                                  const struct xrt_system_compositor_info *info,
                                  uint64_t pinned_display_id);

/*!
 * Multi-screen M3 (macOS): the segment table the last weave took, with each
 * segment's eyes predicted NOW (the primary from the session's DP, the others
 * from their segment DP), for xrLocateViews to frame per-segment views.
 * Returns false (count 0) when the window is not segmented.
 *
 * @ingroup comp_metal
 */
bool
comp_metal_compositor_get_segment_metrics(struct xrt_compositor *xc, struct xrt_segment_metrics *out);

/*!
 * Multi-screen M3 (macOS): which views of the next frame belong to which
 * window segment — what the app's last xrLocateViews handed out. The
 * projection pass paints each segment's views into that segment's rect of
 * every tile (a mosaic) and the segment path crops them out per display.
 *
 * @ingroup comp_metal
 */
void
comp_metal_compositor_set_view_routing(struct xrt_compositor *xc, const struct xrt_segment_view_routing *routing);

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
 * XR_DXR_cursor_depth v2 (ADR-046 Phase 3a): read a cursor-sized patch of the
 * submitted depth at the NEXT layer_commit only. Called by the state tracker
 * only on a frame whose locate chained XrCursorDepthSourceDXR; a compositor
 * never handed a request does no cursor work at all.
 */
void
comp_metal_compositor_set_cursor_depth_request(struct xrt_compositor *xc,
                                               const struct u_cursor_depth_patch_request *req);

/*!
 * The newest finished depth-patch read (asynchronous: one or more frames old).
 *
 * @return out->valid.
 */
bool
comp_metal_compositor_get_cursor_depth_result(struct xrt_compositor *xc, struct u_cursor_depth_patch_result *out);

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
