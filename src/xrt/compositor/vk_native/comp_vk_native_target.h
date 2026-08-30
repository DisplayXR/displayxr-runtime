// Copyright 2025, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Vulkan presentation target (Win32 surface + VkSwapchainKHR).
 * @author David Fattal
 * @ingroup comp_vk_native
 */

#pragma once

#include "xrt/xrt_defines.h"
#include "xrt/xrt_results.h"
#include "xrt/xrt_vulkan_includes.h"

#include <stdint.h>
#include <stdbool.h>

// Forward declarations
struct comp_vk_native_target;
struct comp_vk_native_compositor;

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Create a Vulkan presentation target (Win32 surface + swapchain).
 *
 * @param c The Vulkan native compositor.
 * @param hwnd The window handle to present to.
 * @param width Preferred width.
 * @param height Preferred height.
 * @param transparent_background If true, request VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR
 *        on the swapchain (falls back to INHERIT, then OPAQUE if neither is supported).
 * @param out_target Pointer to receive the created target.
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 *
 * @ingroup comp_vk_native
 */
xrt_result_t
comp_vk_native_target_create(struct comp_vk_native_compositor *c,
                              void *hwnd,
                              bool is_wayland,
                              uint32_t width,
                              uint32_t height,
                              bool transparent_background,
                              struct comp_vk_native_target **out_target);

/*!
 * Create a target from an already-built VkSurfaceKHR (direct-scanout path,
 * ST-5539). The window backend owns the surface — the target borrows it and
 * never destroys it. Skips per-platform surface creation; reuses the shared
 * semaphore + swapchain setup. Dimensions are the connector's fixed scanout
 * mode (no resize).
 *
 * @param c          The Vulkan native compositor.
 * @param surface    A valid display-plane VkSurfaceKHR owned by the caller.
 * @param width      Scanout width in pixels.
 * @param height     Scanout height in pixels.
 * @param out_target Pointer to receive the created target.
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 *
 * @ingroup comp_vk_native
 */
xrt_result_t
comp_vk_native_target_create_from_surface(struct comp_vk_native_compositor *c,
                                          VkSurfaceKHR surface,
                                          uint32_t width,
                                          uint32_t height,
                                          struct comp_vk_native_target **out_target);

/*!
 * Destroy a Vulkan presentation target.
 *
 * @ingroup comp_vk_native
 */
void
comp_vk_native_target_destroy(struct comp_vk_native_target **target_ptr);

/*!
 * #902: the MEASURED panel refresh period in ns, or 0 when unavailable.
 *
 * Fed from VK_GOOGLE_display_timing where the driver offers it. Returns 0
 * unless the grid holds both a measured period and a fresh phase anchor — and
 * a 0 means "keep your existing behaviour", never "substitute a default". The
 * open-loop guess this replaces is exactly what a caller-side fallback would
 * reintroduce.
 *
 * Relevant because the panel is variable-refresh: measured on the NP02J
 * switching 59.86 <-> 119.71 Hz within a single session, with no swapchain
 * recreate, while the repaint loop paced a hardcoded 60.
 */
uint64_t
comp_vk_native_target_vblank_period_ns(struct comp_vk_native_target *target);

/*!
 * Acquire the next swapchain image for rendering.
 *
 * @param target The target.
 * @param out_index Index of the acquired image.
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 *
 * @ingroup comp_vk_native
 */
xrt_result_t
comp_vk_native_target_acquire(struct comp_vk_native_target *target, uint32_t *out_index, VkQueue queue, bool is_repaint);

/*!
 * Present the rendered image.
 *
 * @param target The target.
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 *
 * @ingroup comp_vk_native
 */
xrt_result_t
comp_vk_native_target_present(struct comp_vk_native_target *target, VkQueue queue);

/*!
 * Weave-latency harness (DXR_WEAVE_LATENCY_CSV): timestamp the moment the
 * weave is recorded for the current frame. No-op when the harness is off.
 */
void
comp_vk_native_target_weave_mark(struct comp_vk_native_target *target, bool mode_3d);

/*!
 * #868: repaint counterpart of @ref comp_vk_native_target_weave_mark.
 *
 * Stamps only what the weave→scanout harness needs, staying out of the #867
 * frame-cost EMA, the wait_frame→weave lookahead and the residual handed to
 * the display processor. A repaint is not an app frame, and letting one into
 * those averages corrupts the eye-prediction horizon — measured on D3D11,
 * where it dropped the residual's agreement with reality from 93% to 2%.
 */
void
comp_vk_native_target_weave_mark_repaint(struct comp_vk_native_target *target, bool mode_3d);

/*!
 * #868: pace a repaint to the panel. Runs WITHOUT the compositor lock.
 *
 * Waits on scanout only, never on a frame-latency/acquire token: those are
 * semaphores, and a non-presenting thread consuming one starves the app's own
 * frame loop (measured on D3D12: 31.9 -> 16.9 fps with essentially zero
 * repaints issued).
 */
void
comp_vk_native_target_repaint_pace(struct comp_vk_native_target *target);

/*!
 * Measured weave→scanout residual of the last completed frame in ns (0 =
 * unknown), from the present_wait pacing loop. Feeds the DP's
 * set_frame_timing control loop.
 */
uint64_t
comp_vk_native_target_get_measured_weave_ns(struct comp_vk_native_target *target);

/*!
 * Note that xrWaitFrame just returned, so the span to this frame's weave can
 * be measured (#867).
 */
void
comp_vk_native_target_mark_wait_frame(struct comp_vk_native_target *target);

/*!
 * Seed the panel period used by the #867 lookahead estimate.
 */
void
comp_vk_native_target_set_display_period(struct comp_vk_native_target *target, uint64_t period_ns);

/*!
 * The present origin just changed (window drag) — clamp the paced bridge
 * queue shallow while motion is recent so the weave phase stays snapped to
 * the window position (#912 drag-shallow). No-op off Windows.
 */
void
comp_vk_native_target_note_origin_motion(struct comp_vk_native_target *target);

/*!
 * App-visible wait_frame->scanout lookahead in ns from measured frame cost +
 * measured weave->scanout residual (#867). 0 when unmeasured.
 */
uint64_t
comp_vk_native_target_get_predicted_lookahead_ns(struct comp_vk_native_target *target);

/*!
 * Get target dimensions.
 *
 * @ingroup comp_vk_native
 */
void
comp_vk_native_target_get_dimensions(struct comp_vk_native_target *target,
                                      uint32_t *out_width,
                                      uint32_t *out_height);

/*!
 * Get the current swapchain image and image view for direct rendering.
 *
 * @param target The target.
 * @param out_image VkImage of the current swapchain image (as uint64_t).
 * @param out_view VkImageView of the current swapchain image (as uint64_t).
 *
 * @ingroup comp_vk_native
 */
void
comp_vk_native_target_get_current_image(struct comp_vk_native_target *target,
                                         uint64_t *out_image,
                                         uint64_t *out_view);

/*!
 * Get the target swapchain image format.
 *
 * @ingroup comp_vk_native
 */
VkFormat
comp_vk_native_target_get_format(struct comp_vk_native_target *target);

/*!
 * Get the target's image-set generation (#602). Bumped each time the target
 * image set is (re)created — the compositor forwards it to the display
 * processor so caches keyed by the target VkImage handle can be invalidated
 * across a swapchain/DComp-bridge rebuild (Vulkan recycles freed handles).
 *
 * @ingroup comp_vk_native
 */
uint32_t
comp_vk_native_target_get_generation(struct comp_vk_native_target *target);

/*!
 * Resize the target swapchain.
 *
 * @param target The target.
 * @param width New width.
 * @param height New height.
 *
 * @return XRT_SUCCESS on success, error code otherwise.
 *
 * @ingroup comp_vk_native
 */
xrt_result_t
comp_vk_native_target_resize(struct comp_vk_native_target *target,
                               uint32_t width,
                               uint32_t height);

/*!
 * Outcome of re-syncing the target against the current Android output surface.
 * @ingroup comp_vk_native
 */
enum comp_vk_native_target_surface_state
{
	//! Surface unchanged and live — safe to acquire/present.
	COMP_VK_NATIVE_TARGET_SURFACE_READY = 0,
	//! No live surface (backgrounded) — the swapchain + VkSurfaceKHR were torn
	//! down; the caller must skip acquire/present this frame.
	COMP_VK_NATIVE_TARGET_SURFACE_LOST,
	//! A new surface arrived (resume) — VkSurfaceKHR + swapchain were rebuilt;
	//! safe to acquire/present.
	COMP_VK_NATIVE_TARGET_SURFACE_RECREATED,
};

/*!
 * Android only: re-sync the target's VkSurfaceKHR + swapchain against the
 * ANativeWindow currently published by aux_android (which the SurfaceView
 * destroy/recreate callbacks drive). Called once per frame before acquire.
 *
 * On surface loss it idles the GPU and destroys the swapchain + VkSurfaceKHR so
 * the compositor never blocks acquiring/presenting on a dead window; on a new
 * surface it rebuilds both. A no-op (returns READY) on non-Android. #507
 *
 * @ingroup comp_vk_native
 */
enum comp_vk_native_target_surface_state
comp_vk_native_target_sync_surface(struct comp_vk_native_target *target);

#ifdef __cplusplus
}
#endif
