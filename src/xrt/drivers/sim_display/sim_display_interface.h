// Copyright 2025, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Public interface for the simulation 3D display driver.
 * @author David Fattal
 * @ingroup drv_sim_display
 */

#pragma once

#include "xrt/xrt_results.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct xrt_device;
struct xrt_display_processor;
struct xrt_display_processor_d3d11;
struct xrt_display_processor_d3d12;
struct xrt_display_processor_metal;
struct xrt_display_processor_gl;
struct vk_bundle;
struct xrt_plugin_instance;
struct xrt_screen_binding;
struct xrt_eye_positions;

/*!
 * @defgroup drv_sim_display Simulation 3D Display Driver
 * @ingroup drv
 *
 * @brief Simulates a tracked 3D display on any 2D screen.
 *
 * Supports multiple output modes selected via SIM_DISPLAY_OUTPUT env var:
 * - "2d" (default): single-view full-resolution passthrough
 * - "sbs": side-by-side left/right views
 * - "anaglyph": red-cyan anaglyph stereoscopy
 * - "blend": 50/50 alpha blend of both views
 * - "interlaced": 1-pixel column interlace (phase-sensitive weave proxy, #817)
 *
 * Always available as fallback. Use FORCE_SIM_DISPLAY=1 to override vendor drivers.
 */

/*!
 * Output mode for the simulation display processor.
 * @ingroup drv_sim_display
 */
enum sim_display_output_mode
{
	SIM_DISPLAY_OUTPUT_SBS = 0,          //!< Side-by-side stereo (center crop)
	SIM_DISPLAY_OUTPUT_ANAGLYPH = 1,     //!< Red-cyan anaglyph
	SIM_DISPLAY_OUTPUT_BLEND = 2,        //!< 50/50 alpha blend
	SIM_DISPLAY_OUTPUT_SQUEEZED_SBS = 3, //!< Squeezed SBS (no crop, tiles placed as-is)
	SIM_DISPLAY_OUTPUT_QUAD = 4,         //!< 2x2 quad view (4 views)
	SIM_DISPLAY_OUTPUT_PASSTHROUGH = 5,  //!< Passthrough (2D mode — first tile fills screen)
	/*!
	 * #817 — 1-pixel-period column interlace: a phase-sensitive proxy for a
	 * lenticular weave.
	 *
	 * Output pixel at PANEL column X shows view 0 when
	 * `(X + phase) % 2 == 0` and view 1 otherwise, both sampled at the same
	 * normalized UV (position-preserving, like anaglyph — not a rearranging
	 * layout like SBS). `phase` is the weave target's panel-relative X
	 * origin (`canvas_offset_x`, as handed to `process_atlas`).
	 *
	 * Every other sim_display mode renders "correctly" at any size, any
	 * offset and through any resample, so sim_display is structurally blind
	 * to the two properties a real weaver lives or dies by. This one is not:
	 * a resample (desktop scale != 100%, a mis-sized swapchain) smears the
	 * alternating columns into grey/moire, and a wrong origin flips which
	 * eye lands on the even columns. `SIM_DISPLAY_INTERLACE_PERIOD=N`
	 * widens the stripes so the pattern is visible by eye; 1 is the default
	 * because 1 is what a lenticular actually needs.
	 *
	 * Still a proxy, not a weaver: there is no lens model here, so it can
	 * show that geometry is wrong but never that a vendor weave is right.
	 */
	SIM_DISPLAY_OUTPUT_INTERLACED = 6,
};

/*!
 * Get the current runtime output mode.
 *
 * Thread-safe (atomic). The display processor reads this each frame
 * to select which shader pipeline to use.
 *
 * @return Current output mode.
 * @ingroup drv_sim_display
 */
enum sim_display_output_mode
sim_display_get_output_mode(void);

/*!
 * Set the output mode at runtime.
 *
 * Thread-safe (atomic). Call from the event pump (main thread) when
 * the user presses 1/2/3 to switch modes.
 *
 * @param mode The new output mode.
 * @ingroup drv_sim_display
 */
void
sim_display_set_output_mode(enum sim_display_output_mode mode);

/*!
 * Get the current view count for the active rendering mode.
 *
 * Thread-safe (atomic). The display processor reads this each frame
 * to determine how many eye positions to return.
 *
 * @return Current view count (1 for 2D, 2 for stereo, 4 for quad).
 * @ingroup drv_sim_display
 */
uint32_t
sim_display_get_view_count(void);

/*!
 * Dev-only fake-tracking toggle (#441): `SIM_DISPLAY_FAKE_TRACKING=1`.
 *
 * sim_display has no real eye tracker — by default it advertises
 * `supported_eye_tracking_modes = 0`, leaves every rendering mode untracked
 * (`mode_flags = 0`), and its DPs report `is_tracking = false`. This toggle
 * re-enables MANUAL_BIT + tracked 3D modes so the MANUAL code path and
 * XrEventDataEyeTrackingStateChangedDXR can be exercised without hardware.
 *
 * @return True iff the env toggle is set (cached on first call).
 * @ingroup drv_sim_display
 */
bool
sim_display_fake_tracking_enabled(void);

/*!
 * The simulated per-frame tracking state (#441).
 *
 * False when the fake-tracking toggle is off (honest: sim never tracks).
 * When on: true, or a square wave if `SIM_DISPLAY_FAKE_TRACKING_PERIOD_MS=N`
 * is also set — flips every N ms to exercise tracking-loss/recovery edges
 * and the tracking-state-changed event.
 *
 * @return The simulated is_tracking value for this instant.
 * @ingroup drv_sim_display
 */
bool
sim_display_fake_tracking_is_tracking(void);

/*!
 * One sample of the simulated tracking state AND how many tracking edges the
 * square wave has produced so far, read off the same clock reading so the two
 * always agree (ADR-051: the per-screen status `change_counter` moves exactly
 * when @ref sim_display_fake_tracking_is_tracking flips).
 *
 * @param[out] out_edges Edge count: 0 when fake tracking is off or has no
 *                       period, else elapsed half-periods (monotonic). May be
 *                       NULL.
 * @return The same value @ref sim_display_fake_tracking_is_tracking returns.
 * @ingroup drv_sim_display
 */
bool
sim_display_fake_tracking_sample(uint64_t *out_edges);

/*!
 * Webcam eye tracking (#1855), opt-in: `SIM_DISPLAY_WEBCAM_TRACKING=1`.
 * Was it asked for? (Cached env read; all of the below are no-ops without it.)
 * @ingroup drv_sim_display
 */
bool
sim_display_webcam_tracking_requested(void);

/*!
 * Requested AND a face estimator is built in — only then does sim_display
 * advertise MANAGED tracking (and HAS_TRACKING on its 3D modes) and open a
 * camera. See sim_display_face_estimator.h.
 * @ingroup drv_sim_display
 */
bool
sim_display_webcam_tracking_enabled(void);

/*!
 * Override a DP's nominal @p out with the webcam-tracked eyes (keeping
 * out->count's layout: 1 = midpoint, 2 = the pair, 4 = the pair 32 mm below
 * and above). The first call with *@p dp_ref false takes a reference on the
 * process tracker (starting it); @ref sim_display_webcam_tracking_release
 * drops it. Pass @p dp_ref NULL for a screen-bound DP (never tracked).
 * Never blocks on the camera.
 * @return false (and @p out untouched) when webcam tracking is off or not running.
 * @ingroup drv_sim_display
 */
bool
sim_display_webcam_tracking_apply(
    struct xrt_eye_positions *out, float nominal_x_m, float nominal_y_m, float nominal_z_m, float ipd_m, bool *dp_ref);

//! Drop a DP's tracker reference (DP destroy). Stops the camera with the last one.
void
sim_display_webcam_tracking_release(bool *dp_ref);

/*!
 * Tracker state for the per-screen status (ADR-051): is_tracking now and the
 * edge count. @return false when webcam tracking is not enabled.
 */
bool
sim_display_webcam_tracking_status(bool *out_tracking, uint64_t *out_edges);

/*!
 * The eye-tracking control contracts the session display advertises:
 * MANUAL under SIM_DISPLAY_FAKE_TRACKING, MANAGED under enabled webcam
 * tracking (the default when both), none otherwise.
 */
void
sim_display_eye_tracking_caps(uint32_t *out_supported, uint32_t *out_default);

/*!
 * Set the view count for the active rendering mode.
 *
 * Thread-safe (atomic). Called when the rendering mode changes.
 *
 * @param count The view count for the new mode.
 * @ingroup drv_sim_display
 */
void
sim_display_set_view_count(uint32_t count);

/*!
 * Display info for a sim_display device.
 * Used by target_instance.c to populate xrt_system_compositor_info.
 * @ingroup drv_sim_display
 */
struct sim_display_info
{
	float display_width_m;
	float display_height_m;
	float nominal_y_m;
	float nominal_z_m;
	uint32_t display_pixel_width;
	uint32_t display_pixel_height;
	float zoom_scale;
};

/*!
 * Query display info from a sim_display device.
 *
 * @param xdev     The device to query (must be a sim_display HMD).
 * @param out_info Receives the display info.
 * @return true on success, false if xdev is not a sim_display device.
 * @ingroup drv_sim_display
 */
bool
sim_display_get_display_info(struct xrt_device *xdev, struct sim_display_info *out_info);

/*!
 * Panel geometry published for the display-processor variants (#856).
 *
 * The DP variants have no @ref xrt_device pointer, but must answer
 * get_display_dimensions / get_display_pixel_info — the compositor needs both
 * to compute WINDOW-scoped Kooima. Valid after sim_display_hmd_create(); zeroed
 * before (callers must treat 0 pixels as "not available" and return false).
 *
 * @ingroup drv_sim_display
 */
void
sim_display_get_panel_metrics(float *out_w_m, float *out_h_m, uint32_t *out_px_w, uint32_t *out_px_h);

/*!
 * The simulated panel's DEFAULT geometry — physical size (SIM_DISPLAY_WIDTH_M /
 * SIM_DISPLAY_HEIGHT_M), nominal eye height and viewing distance
 * (SIM_DISPLAY_NOMINAL_Z_M) — without a device. Used to describe a monitor
 * whose EDID gives no physical size (multi-screen M1,
 * get_display_info_for_monitor). Any out-param may be NULL.
 *
 * @ingroup drv_sim_display
 */
void
sim_display_get_default_viewer(float *out_w_m, float *out_h_m, float *out_nominal_y_m, float *out_nominal_z_m);

/*!
 * Create a simulated 3D display HMD device.
 *
 * Display properties are configurable via environment variables:
 * - SIM_DISPLAY_WIDTH_M (default: 0.344)
 * - SIM_DISPLAY_HEIGHT_M (default: 0.194)
 * - SIM_DISPLAY_NOMINAL_Z_M (default: 0.65)
 * - SIM_DISPLAY_PIXEL_W (default: 1920)
 * - SIM_DISPLAY_PIXEL_H (default: 1080)
 *
 * @return A new xrt_device acting as a 3D display HMD, or NULL on failure.
 * @ingroup drv_sim_display
 */
struct xrt_device *
sim_display_hmd_create(void);

/*!
 * Create a simulation Vulkan display processor.
 *
 * For SBS mode, @p vk and @p target_format are ignored (no Vulkan resources needed).
 * For anaglyph and blend modes, creates a full Vulkan pipeline with fragment shaders.
 *
 * @param mode          Output mode (SBS, anaglyph, or blend).
 * @param vk            Vulkan bundle (ignored for SBS mode).
 * @param target_format Swapchain target format (ignored for SBS mode).
 * @param[out] out_xdp  Receives the created display processor.
 * @return XRT_SUCCESS on success.
 * @ingroup drv_sim_display
 */
xrt_result_t
sim_display_processor_create(enum sim_display_output_mode mode,
                             struct vk_bundle *vk,
                             int32_t target_format,
                             struct xrt_display_processor **out_xdp);

/*!
 * Create a simulation D3D11 display processor.
 *
 * For SBS mode, @p d3d11_device is ignored (no shader compilation needed).
 * For anaglyph and blend modes, compiles HLSL shaders for stereo compositing.
 *
 * @param mode          Output mode (SBS, anaglyph, or blend).
 * @param d3d11_device  D3D11 device for shader compilation (ignored for SBS).
 * @param[out] out_xdp  Receives the created display processor.
 * @return XRT_SUCCESS on success.
 * @ingroup drv_sim_display
 */
xrt_result_t
sim_display_processor_d3d11_create(enum sim_display_output_mode mode,
                                   void *d3d11_device,
                                   struct xrt_display_processor_d3d11 **out_xdp);

/*!
 * Factory function for creating a sim_display D3D11 display processor.
 *
 * Matches the @ref xrt_dp_factory_d3d11_fn_t signature.
 * Reads SIM_DISPLAY_OUTPUT env var internally to determine the initial mode.
 *
 * Set this as dp_factory_d3d11 in xrt_system_compositor_info from
 * target_instance.c when sim_display is the active driver.
 *
 * @param d3d11_device   D3D11 device (ID3D11Device*).
 * @param d3d11_context  D3D11 immediate context (unused by sim_display, may be NULL).
 * @param window_handle  Unused by sim_display (may be NULL).
 * @param[out] out_xdp   Receives the created display processor.
 * @return XRT_SUCCESS on success.
 * @ingroup drv_sim_display
 */
xrt_result_t
sim_display_dp_factory_d3d11(void *d3d11_device,
                              void *d3d11_context,
                              void *window_handle,
                              struct xrt_display_processor_d3d11 **out_xdp);

/*!
 * Multi-screen M6: per-screen D3D11 factory, the Windows twin of
 * `sim_display_dp_factory_vk_for_screen` — matches
 * `xrt_plugin_iface::create_dp_d3d11_for_screen`. The DP describes the bound
 * screen and confines its draw to the canvas it is handed.
 *
 * @ingroup drv_sim_display
 */
xrt_result_t
sim_display_dp_factory_d3d11_for_screen(struct xrt_plugin_instance *inst,
                                        void *d3d11_device,
                                        void *d3d11_context,
                                        void *window_handle,
                                        const struct xrt_screen_binding *binding,
                                        struct xrt_display_processor_d3d11 **out_xdp);

/*!
 * Create a simulation D3D12 display processor.
 *
 * For SBS mode, shaders are still compiled but act as a pass-through.
 * For anaglyph and blend modes, HLSL shaders perform stereo compositing.
 *
 * @param mode          Output mode (SBS, anaglyph, or blend).
 * @param d3d12_device  D3D12 device for PSO creation.
 * @param[out] out_xdp  Receives the created display processor.
 * @return XRT_SUCCESS on success.
 * @ingroup drv_sim_display
 */
xrt_result_t
sim_display_processor_d3d12_create(enum sim_display_output_mode mode,
                                   void *d3d12_device,
                                   struct xrt_display_processor_d3d12 **out_xdp);

/*!
 * Factory function for creating a sim_display D3D12 display processor.
 *
 * Matches the @ref xrt_dp_factory_d3d12_fn_t signature.
 * Reads SIM_DISPLAY_OUTPUT env var internally to determine the initial mode.
 *
 * Set this as dp_factory_d3d12 in xrt_system_compositor_info from
 * target_instance.c when sim_display is the active driver.
 *
 * @param d3d12_device        D3D12 device (ID3D12Device*).
 * @param d3d12_command_queue D3D12 command queue (unused by sim_display, may be NULL).
 * @param window_handle       Unused by sim_display (may be NULL).
 * @param[out] out_xdp        Receives the created display processor.
 * @return XRT_SUCCESS on success.
 * @ingroup drv_sim_display
 */
xrt_result_t
sim_display_dp_factory_d3d12(void *d3d12_device,
                              void *d3d12_command_queue,
                              void *window_handle,
                              struct xrt_display_processor_d3d12 **out_xdp);

/*!
 * Multi-screen M6: per-screen D3D12 factory, the D3D12 twin of
 * `sim_display_dp_factory_d3d11_for_screen` — matches
 * `xrt_plugin_iface::create_dp_d3d12_for_screen`. The DP describes the bound
 * screen and confines its draw (viewport + scissor) to the canvas it is handed.
 *
 * @ingroup drv_sim_display
 */
xrt_result_t
sim_display_dp_factory_d3d12_for_screen(struct xrt_plugin_instance *inst,
                                        void *d3d12_device,
                                        void *d3d12_command_queue,
                                        void *window_handle,
                                        const struct xrt_screen_binding *binding,
                                        struct xrt_display_processor_d3d12 **out_xdp);

/*!
 * Set an external device as the pose source for a sim_display HMD.
 *
 * When set, the sim_display HMD delegates get_tracked_pose to the
 * source device (e.g. a qwerty HMD for WASD/mouse camera control).
 *
 * @param sim_hmd  The sim_display HMD device.
 * @param source   The device providing pose data, or NULL to use static pose.
 * @ingroup drv_sim_display
 */
void
sim_display_hmd_set_pose_source(struct xrt_device *sim_hmd, struct xrt_device *source);

/*!
 * Enable or disable EXT app mode for a sim_display HMD.
 *
 * When enabled, get_tracked_pose returns the raw hmd->pose (eye offset
 * relative to display center) without composing with the qwerty pose source.
 * Used when the session has an external window handle and the app owns the
 * virtual display model.
 *
 * @param xdev    The sim_display HMD device.
 * @param enabled true to enable EXT app mode.
 * @ingroup drv_sim_display
 */
void
sim_display_hmd_set_ext_app_mode(struct xrt_device *xdev, bool enabled);

/*!
 * Factory function for creating a sim_display Vulkan display processor.
 *
 * Matches the @ref xrt_dp_factory_vk_fn_t signature.
 * Reads SIM_DISPLAY_OUTPUT env var internally to determine the initial mode.
 *
 * Set this as dp_factory_vk in xrt_system_compositor_info from
 * target_instance.c when sim_display is the active driver.
 *
 * @param vk_bundle      Opaque pointer to struct vk_bundle.
 * @param vk_cmd_pool    Vulkan command pool (unused by sim_display, may be NULL).
 * @param window_handle  Unused by sim_display (may be NULL).
 * @param target_format  Swapchain target format (VkFormat as int32_t).
 * @param[out] out_xdp   Receives the created display processor.
 * @return XRT_SUCCESS on success.
 * @ingroup drv_sim_display
 */
xrt_result_t
sim_display_dp_factory_vk(void *vk_bundle,
                          void *vk_cmd_pool,
                          void *window_handle,
                          int32_t target_format,
                          struct xrt_display_processor **out_xdp);

/*!
 * Per-screen twin of @ref sim_display_dp_factory_vk (multi-screen M2) —
 * `xrt_plugin_iface::create_dp_vk_for_screen`. The DP describes the bound
 * screen (its EDID size, native pixels, desktop origin), confines its output to
 * the canvas it is handed, and preserves the target outside it, so several of
 * these can weave one window's segments into one surface.
 *
 * @ingroup drv_sim_display
 */
xrt_result_t
sim_display_dp_factory_vk_for_screen(struct xrt_plugin_instance *inst,
                                     void *vk_bundle,
                                     void *vk_cmd_pool,
                                     void *window_handle,
                                     int32_t target_format,
                                     const struct xrt_screen_binding *binding,
                                     struct xrt_display_processor **out_xdp);

/*!
 * Create a simulation Metal display processor.
 *
 * For SBS mode, shaders are still compiled but act as a pass-through.
 * For anaglyph and blend modes, MSL shaders perform stereo compositing.
 *
 * @param mode          Output mode (SBS, anaglyph, or blend).
 * @param metal_device  Metal device (id<MTLDevice>).
 * @param[out] out_xdp  Receives the created display processor.
 * @return XRT_SUCCESS on success.
 * @ingroup drv_sim_display
 */
xrt_result_t
sim_display_processor_metal_create(enum sim_display_output_mode mode,
                                   void *metal_device,
                                   struct xrt_display_processor_metal **out_xdp);

/*!
 * Factory function for creating a sim_display Metal display processor.
 *
 * Matches the @ref xrt_dp_factory_metal_fn_t signature.
 * Reads SIM_DISPLAY_OUTPUT env var internally to determine the initial mode.
 *
 * Set this as dp_factory_metal in xrt_system_compositor_info from
 * target_instance.c when sim_display is the active driver.
 *
 * @param metal_device   Metal device (id<MTLDevice>).
 * @param command_queue  Metal command queue (unused by sim_display, may be NULL).
 * @param window_handle  Unused by sim_display (may be NULL).
 * @param[out] out_xdp   Receives the created display processor.
 * @return XRT_SUCCESS on success.
 * @ingroup drv_sim_display
 */
xrt_result_t
sim_display_dp_factory_metal(void *metal_device,
                             void *command_queue,
                             void *window_handle,
                             struct xrt_display_processor_metal **out_xdp);

/*!
 * Multi-screen on macOS: per-screen Metal factory, the twin of
 * `sim_display_dp_factory_d3d11_for_screen` — matches
 * `xrt_plugin_iface::create_dp_metal_for_screen`. The DP describes the bound
 * screen and confines its draw to the canvas it is handed (load, scissor).
 *
 * @ingroup drv_sim_display
 */
xrt_result_t
sim_display_dp_factory_metal_for_screen(struct xrt_plugin_instance *inst,
                                        void *metal_device,
                                        void *command_queue,
                                        void *window_handle,
                                        const struct xrt_screen_binding *binding,
                                        struct xrt_display_processor_metal **out_xdp);

/*!
 * Create a simulation GL display processor.
 *
 * All 3 GLSL shaders (SBS, anaglyph, blend) are compiled at init
 * for instant runtime switching via 1/2/3 keys.
 *
 * @param mode          Output mode (SBS, anaglyph, or blend).
 * @param[out] out_xdp  Receives the created display processor.
 * @return XRT_SUCCESS on success.
 * @ingroup drv_sim_display
 */
xrt_result_t
sim_display_processor_gl_create(enum sim_display_output_mode mode,
                                 struct xrt_display_processor_gl **out_xdp);

/*!
 * Factory function for creating a sim_display GL display processor.
 *
 * Matches the @ref xrt_dp_factory_gl_fn_t signature.
 * Reads SIM_DISPLAY_OUTPUT env var internally to determine the initial mode.
 *
 * Set this as dp_factory_gl in xrt_system_compositor_info from
 * target_instance.c when sim_display is the active driver.
 *
 * @param window_handle  Unused by sim_display (may be NULL).
 * @param[out] out_xdp   Receives the created display processor.
 * @return XRT_SUCCESS on success.
 * @ingroup drv_sim_display
 */
xrt_result_t
sim_display_dp_factory_gl(void *window_handle,
                           struct xrt_display_processor_gl **out_xdp);

/*!
 * Create the simulation display system builder.
 *
 * Always available as fallback. Use FORCE_SIM_DISPLAY=1 to override vendor drivers.
 *
 * @return A new xrt_builder, or NULL on failure.
 * @ingroup drv_sim_display
 */
struct xrt_builder *
t_builder_sim_display_create(void);

#ifdef __cplusplus
}
#endif
