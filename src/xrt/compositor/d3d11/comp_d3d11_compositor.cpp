// Copyright 2024-2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Native D3D11 compositor implementation.
 * @author David Fattal
 * @ingroup comp_d3d11
 */

#include "comp_d3d11_compositor.h"
#include "comp_d3d11_swapchain.h"
#include "comp_d3d11_target.h"
#include "comp_d3d11_renderer.h"
#include "comp_d3d11_window.h"

#include "util/comp_layer_accum.h"

#include "xrt/xrt_handles.h"
#include "xrt/xrt_config_build.h"
#include "xrt/xrt_limits.h"
#include "xrt/xrt_display_metrics.h"

#include "util/u_logging.h"
#include "util/u_misc.h"
#include "util/u_time.h"
#include "os/os_time.h"

#include "xrt/xrt_system.h"

#ifdef XRT_FEATURE_DEBUG_GUI
#include "util/u_debug_gui.h"
#include "comp_d3d11_debug.h"
#endif

#include "xrt/xrt_display_processor_d3d11.h"

#include "util/u_hud.h"

#include "math/m_api.h"
#include "util/u_tiling.h"
#include "util/u_canvas.h"

#ifdef XRT_BUILD_DRIVER_QWERTY
#include "qwerty_interface.h"
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <mutex>
#include <cmath>

/*!
 * Minimal settings struct for D3D11 compositor (replaces deleted main/comp_settings.h).
 */
struct comp_settings
{
	struct
	{
		uint32_t width;
		uint32_t height;
	} preferred;

	//! Nominal frame interval in nanoseconds.
	int64_t nominal_frame_interval_ns;
};

/*!
 * The D3D11 native compositor structure.
 */
struct comp_d3d11_compositor
{
	//! Base type - must be first!
	struct xrt_compositor_native base;

	//! The device we are rendering for.
	struct xrt_device *xdev;

	//! D3D11 device (from app's graphics binding, we add a reference).
	ID3D11Device *device;

	//! D3D11 immediate context.
	ID3D11DeviceContext *context;

	//! DXGI factory for swapchain creation.
	IDXGIFactory4 *dxgi_factory;

	//! Output target (DXGI swapchain).
	struct comp_d3d11_target *target;

	//! Renderer for layer compositing.
	struct comp_d3d11_renderer *renderer;

	//! Accumulated layers for the current frame.
	struct comp_layer_accum layer_accum;

	//! Compositor settings.
	struct comp_settings settings;

	//! Window handle for rendering (either from app or self-created).
	HWND hwnd;

	//! App's window handle for position tracking in shared-texture mode.
	//! When non-NULL, the hidden weaver window is repositioned each frame
	//! to match this window's client rect on screen.
	HWND app_hwnd;

	//! Self-created window (NULL if app provided window).
	struct comp_d3d11_window *own_window;

	//! True if we created the window ourselves.
	bool owns_window;

	//! Shared texture opened from app's shared HANDLE (may be NULL).
	ID3D11Texture2D *shared_texture;

	//! Render target view for shared texture (may be NULL).
	ID3D11RenderTargetView *shared_rtv;

	//! True if shared texture mode is active.
	bool has_shared_texture;

	//! Canvas output rect for shared-texture apps.
	//! When valid, the hidden weaver window is positioned to match this
	//! sub-rect instead of the full client rect.
	struct u_canvas_rect canvas;

	//! Generic D3D11 display processor (vendor-agnostic weaving).
	struct xrt_display_processor_d3d11 *display_processor;

	//! System devices (for qwerty driver keyboard input and display mode toggle).
	struct xrt_system_devices *xsysd;

#ifdef XRT_FEATURE_DEBUG_GUI
	//! Debug GUI window.
	struct u_debug_gui *debug_gui;

	//! Debug readback module.
	struct comp_d3d11_debug *debug;
#endif

	//! Current frame ID.
	int64_t frame_id;

	//! Display refresh rate in Hz.
	float display_refresh_rate;

	//! Time of the last predicted display time.
	uint64_t last_display_time_ns;



	//! HUD overlay (runtime-owned windows only).
	struct u_hud *hud;

	//! D3D11 staging texture for HUD pixel upload.
	ID3D11Texture2D *hud_texture;

	//! True if HUD GPU resources are initialized.
	bool hud_initialized;

	//! Last frame timestamp for FPS calculation.
	uint64_t last_frame_time_ns;

	//! Smoothed frame time for display.
	float smoothed_frame_time_ms;

	//! True when display is in 3D mode (weaver active). False = 2D passthrough.
	bool hardware_display_3d;

	//! Last known 3D rendering mode index (for V-key toggle restore).
	uint32_t last_3d_mode_index;

	//! True when a legacy app is using a compromise view scale that doesn't
	//! match the 3D mode's native scale, so direct rendering mode selection
	//! via 1/2/3 keys should be disabled.
	bool legacy_app_tile_scaling;

	//! Compromise view scale for legacy apps. Only valid when legacy_app_tile_scaling is true.
	float legacy_view_scale_x;
	float legacy_view_scale_y;

	//! Lazily allocated intermediate texture for cropping atlas to content dims
	//! before passing to display processor. NULL when not needed (zero-copy case).
	ID3D11Texture2D *dp_input_texture;

	//! SRV for dp_input_texture.
	ID3D11ShaderResourceView *dp_input_srv;

	//! Cached dimensions for lazy reallocation.
	uint32_t dp_input_width, dp_input_height;

	//! Thread safety.
	std::mutex mutex;
};

/*
 *
 * Helper functions
 *
 */

static inline struct comp_d3d11_compositor *
d3d11_comp(struct xrt_compositor *xc)
{
	return reinterpret_cast<struct comp_d3d11_compositor *>(xc);
}

/*
 *
 * xrt_compositor member functions
 *
 */

static xrt_result_t
d3d11_compositor_get_swapchain_create_properties(struct xrt_compositor *xc,
                                                  const struct xrt_swapchain_create_info *info,
                                                  struct xrt_swapchain_create_properties *xsccp)
{
	// D3D11 native compositor can handle all standard properties
	xsccp->image_count = 3; // Triple buffering
	xsccp->extra_bits = (enum xrt_swapchain_usage_bits)0;

	return XRT_SUCCESS;
}

static xrt_result_t
d3d11_compositor_create_swapchain(struct xrt_compositor *xc,
                                   const struct xrt_swapchain_create_info *info,
                                   struct xrt_swapchain **out_xsc)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);

	return comp_d3d11_swapchain_create(c, info, out_xsc);
}

static xrt_result_t
d3d11_compositor_import_swapchain(struct xrt_compositor *xc,
                                   const struct xrt_swapchain_create_info *info,
                                   struct xrt_image_native *native_images,
                                   uint32_t image_count,
                                   struct xrt_swapchain **out_xsc)
{
	// For now, don't support importing external swapchains
	// The D3D11 client code should create swapchains directly
	return XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED;
}

static xrt_result_t
d3d11_compositor_import_fence(struct xrt_compositor *xc,
                               xrt_graphics_sync_handle_t handle,
                               struct xrt_compositor_fence **out_xcf)
{
	// D3D11 native compositor uses D3D11 synchronization primitives
	return XRT_ERROR_FENCE_CREATE_FAILED;
}

static xrt_result_t
d3d11_compositor_create_semaphore(struct xrt_compositor *xc,
                                   xrt_graphics_sync_handle_t *out_handle,
                                   struct xrt_compositor_semaphore **out_xcsem)
{
	// D3D11 native compositor doesn't expose semaphores
	return XRT_ERROR_FENCE_CREATE_FAILED;
}

static xrt_result_t
d3d11_compositor_begin_session(struct xrt_compositor *xc, const struct xrt_begin_session_info *info)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);

	U_LOG_I("D3D11 compositor session begin - hwnd=%p, owns_window=%d, target=%p, renderer=%p",
	        (void *)c->hwnd, c->owns_window, (void *)c->target, (void *)c->renderer);

#ifdef XRT_FEATURE_DEBUG_GUI
	// Start the debug GUI thread now that session is beginning
	// xsysd should have been set via comp_d3d11_compositor_set_system_devices
	if (c->debug_gui != nullptr) {
		u_debug_gui_start(c->debug_gui, NULL, c->xsysd);
	}
#endif

	return XRT_SUCCESS;
}

static xrt_result_t
d3d11_compositor_end_session(struct xrt_compositor *xc)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);
	(void)c;

	U_LOG_I("D3D11 compositor session end");

	return XRT_SUCCESS;
}

static xrt_result_t
d3d11_compositor_predict_frame(struct xrt_compositor *xc,
                                int64_t *out_frame_id,
                                int64_t *out_wake_time_ns,
                                int64_t *out_predicted_gpu_time_ns,
                                int64_t *out_predicted_display_time_ns,
                                int64_t *out_predicted_display_period_ns)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);

	std::lock_guard<std::mutex> lock(c->mutex);

	c->frame_id++;

	*out_frame_id = c->frame_id;

	// Use queried display refresh rate
	int64_t now_ns = static_cast<int64_t>(os_monotonic_get_ns());
	int64_t period_ns = static_cast<int64_t>(U_TIME_1S_IN_NS / c->display_refresh_rate);

	*out_predicted_display_time_ns = now_ns + period_ns * 2;
	*out_predicted_display_period_ns = period_ns;
	*out_wake_time_ns = now_ns;
	*out_predicted_gpu_time_ns = period_ns;

	c->last_display_time_ns = static_cast<uint64_t>(*out_predicted_display_time_ns);

	return XRT_SUCCESS;
}

static xrt_result_t
d3d11_compositor_wait_frame(struct xrt_compositor *xc,
                             int64_t *out_frame_id,
                             int64_t *out_predicted_display_time_ns,
                             int64_t *out_predicted_display_period_ns)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);

	// Check if window was closed (user pressed ESC or closed window).
	// Skip for shared texture mode (hidden window) — session lifetime is
	// controlled by the app, not our hidden weaver window.
	if (c->owns_window && c->own_window != nullptr && c->hwnd != nullptr &&
	    !comp_d3d11_window_is_valid(c->own_window)) {
		U_LOG_I("Window closed - signaling session exit");
		return XRT_ERROR_IPC_FAILURE;
	}

	// During drag, synchronize with the window thread's WM_PAINT cycle.
	// This ensures the window position is stable between weave() and Present(),
	// so the interlacing pattern matches the actual displayed position.
	if (c->owns_window && c->own_window != nullptr && c->hwnd != nullptr &&
	    comp_d3d11_window_is_in_size_move(c->own_window)) {
		comp_d3d11_window_wait_for_paint(c->own_window);
	}

	// Use queried display refresh rate
	int64_t period_ns = static_cast<int64_t>(U_TIME_1S_IN_NS / c->display_refresh_rate);

	// Frame pacing is handled by Present(1) + SetMaximumFrameLatency(1).
	// Present(1) blocks until vsync, and the frame latency limit of 1
	// prevents burst/stall queuing. No additional sleep is needed here —
	// adding one would eat into the vsync margin and cause the pipeline
	// to miss vsync deadlines during window drag (dropping from 60→30Hz).

	std::lock_guard<std::mutex> lock(c->mutex);

	c->frame_id++;

	*out_frame_id = c->frame_id;

	int64_t now_ns = static_cast<int64_t>(os_monotonic_get_ns());

	*out_predicted_display_time_ns = now_ns + period_ns * 2;
	*out_predicted_display_period_ns = period_ns;

	c->last_display_time_ns = static_cast<uint64_t>(*out_predicted_display_time_ns);

	return XRT_SUCCESS;
}

static xrt_result_t
d3d11_compositor_mark_frame(struct xrt_compositor *xc,
                             int64_t frame_id,
                             enum xrt_compositor_frame_point point,
                             int64_t when_ns)
{
	// Frame timing telemetry - optional
	return XRT_SUCCESS;
}

static xrt_result_t
d3d11_compositor_begin_frame(struct xrt_compositor *xc, int64_t frame_id)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);

	std::lock_guard<std::mutex> lock(c->mutex);

	// Check for window resize and handle it.
	// During a modal drag/resize (in_size_move), we still resize the swapchain target
	// to keep DXGI in sync, but defer the expensive atlas texture reallocation until
	// the drag ends. This avoids per-pixel texture churn that causes stutter.
	bool in_size_move = false;
	if (c->owns_window && c->own_window != nullptr) {
		in_size_move = comp_d3d11_window_is_in_size_move(c->own_window);
	}

	if (c->hwnd != nullptr) {
		RECT rect;
		if (GetClientRect(c->hwnd, &rect)) {
			uint32_t new_width = static_cast<uint32_t>(rect.right - rect.left);
			uint32_t new_height = static_cast<uint32_t>(rect.bottom - rect.top);

			// Only resize if dimensions actually changed and are valid
			if (new_width > 0 && new_height > 0) {
				uint32_t current_width, current_height;
				comp_d3d11_target_get_dimensions(c->target, &current_width, &current_height);

				if (new_width != current_width || new_height != current_height) {
					U_LOG_I("Window resized: %ux%u -> %ux%u",
					        current_width, current_height, new_width, new_height);

					xrt_result_t xret = comp_d3d11_target_resize(c->target, new_width, new_height);
					if (xret != XRT_SUCCESS) {
						U_LOG_E("Failed to resize target");
						// Continue anyway, rendering will just be wrong size
					} else {
						// Update settings to reflect new size
						c->settings.preferred.width = new_width;
						c->settings.preferred.height = new_height;

						// Renderer atlas sizing is handled in layer_commit based on
						// the active rendering mode — no resize needed here.
						// (Matches GL compositor's begin_frame which does nothing.)
					}
				}
			}
		}
	}

	// Reset layer accumulator for this frame
	c->layer_accum.layer_count = 0;

	return XRT_SUCCESS;
}

static xrt_result_t
d3d11_compositor_discard_frame(struct xrt_compositor *xc, int64_t frame_id)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);

	std::lock_guard<std::mutex> lock(c->mutex);

	// Clear layers
	c->layer_accum.layer_count = 0;

	return XRT_SUCCESS;
}

static xrt_result_t
d3d11_compositor_layer_begin(struct xrt_compositor *xc, const struct xrt_layer_frame_data *data)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);

	std::lock_guard<std::mutex> lock(c->mutex);

	comp_layer_accum_begin(&c->layer_accum, data);

	return XRT_SUCCESS;
}

static xrt_result_t
d3d11_compositor_layer_projection(struct xrt_compositor *xc,
                                   struct xrt_device *xdev,
                                   struct xrt_swapchain *xsc[XRT_MAX_VIEWS],
                                   const struct xrt_layer_data *data)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);

	std::lock_guard<std::mutex> lock(c->mutex);

	// Store the layer data
	comp_layer_accum_projection(&c->layer_accum, xsc, data);

	return XRT_SUCCESS;
}

static xrt_result_t
d3d11_compositor_layer_projection_depth(struct xrt_compositor *xc,
                                         struct xrt_device *xdev,
                                         struct xrt_swapchain *xsc[XRT_MAX_VIEWS],
                                         struct xrt_swapchain *d_xsc[XRT_MAX_VIEWS],
                                         const struct xrt_layer_data *data)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);

	std::lock_guard<std::mutex> lock(c->mutex);

	// Store the layer data (ignore depth for now)
	comp_layer_accum_projection_depth(&c->layer_accum, xsc, d_xsc, data);

	return XRT_SUCCESS;
}

static xrt_result_t
d3d11_compositor_layer_quad(struct xrt_compositor *xc,
                             struct xrt_device *xdev,
                             struct xrt_swapchain *xsc,
                             const struct xrt_layer_data *data)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);

	std::lock_guard<std::mutex> lock(c->mutex);

	comp_layer_accum_quad(&c->layer_accum, xsc, data);

	return XRT_SUCCESS;
}

static xrt_result_t
d3d11_compositor_layer_cube(struct xrt_compositor *xc,
                             struct xrt_device *xdev,
                             struct xrt_swapchain *xsc,
                             const struct xrt_layer_data *data)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);

	std::lock_guard<std::mutex> lock(c->mutex);

	comp_layer_accum_cube(&c->layer_accum, xsc, data);

	return XRT_SUCCESS;
}

static xrt_result_t
d3d11_compositor_layer_cylinder(struct xrt_compositor *xc,
                                 struct xrt_device *xdev,
                                 struct xrt_swapchain *xsc,
                                 const struct xrt_layer_data *data)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);

	std::lock_guard<std::mutex> lock(c->mutex);

	comp_layer_accum_cylinder(&c->layer_accum, xsc, data);

	return XRT_SUCCESS;
}

static xrt_result_t
d3d11_compositor_layer_equirect1(struct xrt_compositor *xc,
                                  struct xrt_device *xdev,
                                  struct xrt_swapchain *xsc,
                                  const struct xrt_layer_data *data)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);

	std::lock_guard<std::mutex> lock(c->mutex);

	comp_layer_accum_equirect1(&c->layer_accum, xsc, data);

	return XRT_SUCCESS;
}

static xrt_result_t
d3d11_compositor_layer_equirect2(struct xrt_compositor *xc,
                                  struct xrt_device *xdev,
                                  struct xrt_swapchain *xsc,
                                  const struct xrt_layer_data *data)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);

	std::lock_guard<std::mutex> lock(c->mutex);

	comp_layer_accum_equirect2(&c->layer_accum, xsc, data);

	return XRT_SUCCESS;
}

static xrt_result_t
d3d11_compositor_layer_passthrough(struct xrt_compositor *xc,
                                    struct xrt_device *xdev,
                                    const struct xrt_layer_data *data)
{
	// Passthrough not supported on D3D11 native compositor
	return XRT_SUCCESS;
}

static xrt_result_t
d3d11_compositor_layer_window_space(struct xrt_compositor *xc,
                                     struct xrt_device *xdev,
                                     struct xrt_swapchain *xsc,
                                     const struct xrt_layer_data *data)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);

	std::lock_guard<std::mutex> lock(c->mutex);

	comp_layer_accum_window_space(&c->layer_accum, xsc, data);

	return XRT_SUCCESS;
}

/*!
 * Render the HUD overlay onto the back buffer (post-weave).
 * Uses CopySubresourceRegion for zero-shader simplicity.
 */
static void
d3d11_render_hud_overlay(struct comp_d3d11_compositor *c, bool weaving_done,
                         const struct xrt_eye_positions *eye_pos)
{
	if (!c->owns_window || c->hud == NULL || !u_hud_is_visible()) {
		return;
	}

	// Compute FPS from frame timestamps
	uint64_t now_ns = os_monotonic_get_ns();
	if (c->last_frame_time_ns != 0) {
		float dt_ms = (float)(now_ns - c->last_frame_time_ns) / 1e6f;
		// Exponential moving average (alpha=0.1 for smooth display)
		c->smoothed_frame_time_ms = c->smoothed_frame_time_ms * 0.9f + dt_ms * 0.1f;
	}
	c->last_frame_time_ns = now_ns;

	float fps = (c->smoothed_frame_time_ms > 0.0f) ? (1000.0f / c->smoothed_frame_time_ms) : 0.0f;

	// Get render and window dimensions
	uint32_t render_w = 0, render_h = 0;
	if (c->renderer != nullptr) {
		comp_d3d11_renderer_get_view_dimensions(c->renderer, &render_w, &render_h);
	}
	uint32_t win_w = c->settings.preferred.width;
	uint32_t win_h = c->settings.preferred.height;
	if (c->target != nullptr) {
		comp_d3d11_target_get_dimensions(c->target, &win_w, &win_h);
	}

	// Get display physical dimensions from display processor
	float disp_w_m = 0.0f, disp_h_m = 0.0f;
	float nom_x = 0.0f, nom_y = 0.0f, nom_z = 600.0f;
	comp_d3d11_compositor_get_display_dimensions(&c->base.base, &disp_w_m, &disp_h_m);
	float disp_w_mm = disp_w_m * 1000.0f;
	float disp_h_mm = disp_h_m * 1000.0f;

	// Fill HUD data
	struct u_hud_data data = {};
	data.device_name = c->xdev->str;
	data.fps = fps;
	data.frame_time_ms = c->smoothed_frame_time_ms;
	data.mode_3d = c->hardware_display_3d;
	if (c->xdev != NULL && c->xdev->hmd != NULL) {
		uint32_t idx = c->xdev->hmd->active_rendering_mode_index;
		if (idx < c->xdev->rendering_mode_count) {
			data.rendering_mode_name = c->xdev->rendering_modes[idx].mode_name;
		}
	}
	data.render_width = render_w;
	data.render_height = render_h;
	if (c->xdev != NULL && c->xdev->rendering_mode_count > 0) {
		u_tiling_compute_system_atlas(c->xdev->rendering_modes, c->xdev->rendering_mode_count,
		                              &data.swapchain_width, &data.swapchain_height);
	}
	data.window_width = win_w;
	data.window_height = win_h;
	data.display_width_mm = disp_w_mm;
	data.display_height_mm = disp_h_mm;
	data.nominal_x = nom_x;
	data.nominal_y = nom_y;
	data.nominal_z = nom_z;
	// Use the active rendering mode's view_count for eye display (not eye_pos->count,
	// which may report more eyes than the mode uses — e.g. tracker returns L/R in 2D mode).
	uint32_t mode_eye_count = eye_pos->count;
	if (c->xdev != NULL && c->xdev->hmd != NULL) {
		uint32_t midx = c->xdev->hmd->active_rendering_mode_index;
		if (midx < c->xdev->rendering_mode_count) {
			mode_eye_count = c->xdev->rendering_modes[midx].view_count;
		}
	}
	if (mode_eye_count > eye_pos->count) {
		mode_eye_count = eye_pos->count;
	}
	data.eye_count = mode_eye_count;
	for (uint32_t e = 0; e < mode_eye_count && e < 8; e++) {
		data.eyes[e].x = eye_pos->eyes[e].x * 1000.0f;
		data.eyes[e].y = eye_pos->eyes[e].y * 1000.0f;
		data.eyes[e].z = eye_pos->eyes[e].z * 1000.0f;
	}
	data.eye_tracking_active = eye_pos->is_tracking;

#ifdef XRT_BUILD_DRIVER_QWERTY
	if (c->xsysd != nullptr) {
		// Virtual display position + forward vector from qwerty device pose.
		struct xrt_pose qwerty_pose;
		if (qwerty_get_hmd_pose(c->xsysd->xdevs, c->xsysd->xdev_count, &qwerty_pose)) {
			data.vdisp_x = qwerty_pose.position.x;
			data.vdisp_y = qwerty_pose.position.y;
			data.vdisp_z = qwerty_pose.position.z;
			struct xrt_vec3 fwd_in = {0, 0, -1};
			struct xrt_vec3 fwd_out;
			math_quat_rotate_vec3(&qwerty_pose.orientation, &fwd_in, &fwd_out);
			data.forward_x = fwd_out.x;
			data.forward_y = fwd_out.y;
			data.forward_z = fwd_out.z;
		}

		struct qwerty_view_state ss;
		if (qwerty_get_view_state(c->xsysd->xdevs, c->xsysd->xdev_count, &ss)) {
			data.camera_mode = ss.camera_mode;
			data.cam_spread_factor = ss.cam_spread_factor;
			data.cam_parallax_factor = ss.cam_parallax_factor;
			data.cam_convergence = ss.cam_convergence;
			data.cam_half_tan_vfov = ss.cam_half_tan_vfov;
			data.disp_spread_factor = ss.disp_spread_factor;
			data.disp_parallax_factor = ss.disp_parallax_factor;
			data.disp_vHeight = ss.disp_vHeight;
			data.nominal_viewer_z = ss.nominal_viewer_z;
			data.screen_height_m = ss.screen_height_m;
		}
	}
#endif

	bool dirty = u_hud_update(c->hud, &data);

	// Lazy-create the D3D11 staging texture
	if (!c->hud_initialized) {
		uint32_t hud_w = u_hud_get_width(c->hud);
		uint32_t hud_h = u_hud_get_height(c->hud);

		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = hud_w;
		desc.Height = hud_h;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = 0; // No shader binding needed, just copy source

		HRESULT hr = c->device->CreateTexture2D(&desc, nullptr, &c->hud_texture);
		if (FAILED(hr)) {
			U_LOG_E("Failed to create HUD texture: 0x%08x", hr);
			return;
		}
		c->hud_initialized = true;
		dirty = true; // Force initial upload
	}

	// Upload pixels to staging texture if changed
	if (dirty && c->hud_texture != nullptr) {
		uint32_t hud_w = u_hud_get_width(c->hud);
		c->context->UpdateSubresource(c->hud_texture, 0, nullptr,
		                               u_hud_get_pixels(c->hud),
		                               hud_w * 4, 0);
	}

	// Blit HUD texture to bottom-left of back buffer
	if (c->hud_texture != nullptr && c->target != nullptr) {
		ID3D11Texture2D *back_buffer =
		    static_cast<ID3D11Texture2D *>(comp_d3d11_target_get_back_buffer(c->target));
		if (back_buffer != nullptr) {
			uint32_t hud_w = u_hud_get_width(c->hud);
			uint32_t hud_h = u_hud_get_height(c->hud);

			// Position at bottom-left with 10px margin
			uint32_t dst_x = 10;
			uint32_t dst_y = (win_h > hud_h + 10) ? (win_h - hud_h - 10) : 0;

			D3D11_BOX src_box = {0, 0, 0, hud_w, hud_h, 1};
			c->context->CopySubresourceRegion(back_buffer, 0, dst_x, dst_y, 0,
			                                   c->hud_texture, 0, &src_box);
		}
	}
}

/*!
 * Crop atlas to content dimensions before passing to display processor.
 * Returns the SRV to pass to process_atlas() — either the original (zero-copy)
 * or a cropped intermediate texture.
 */
static void *
d3d11_crop_atlas_for_dp(struct comp_d3d11_compositor *c,
                        void *atlas_srv,
                        uint32_t content_w,
                        uint32_t content_h)
{
	// Get atlas texture dimensions from the SRV's underlying resource
	ID3D11ShaderResourceView *srv = static_cast<ID3D11ShaderResourceView *>(atlas_srv);
	ID3D11Resource *resource = nullptr;
	srv->GetResource(&resource);
	ID3D11Texture2D *atlas_tex = static_cast<ID3D11Texture2D *>(resource);
	D3D11_TEXTURE2D_DESC atlas_desc;
	atlas_tex->GetDesc(&atlas_desc);

	// Zero-copy: atlas already matches content dimensions
	if (content_w == atlas_desc.Width && content_h == atlas_desc.Height) {
		atlas_tex->Release();
		return atlas_srv;
	}

	// Lazily (re)create intermediate texture at content dimensions
	if (c->dp_input_width != content_w || c->dp_input_height != content_h) {
		if (c->dp_input_srv != nullptr) {
			c->dp_input_srv->Release();
			c->dp_input_srv = nullptr;
		}
		if (c->dp_input_texture != nullptr) {
			c->dp_input_texture->Release();
			c->dp_input_texture = nullptr;
		}

		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = content_w;
		desc.Height = content_h;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = atlas_desc.Format;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

		HRESULT hr = c->device->CreateTexture2D(&desc, nullptr, &c->dp_input_texture);
		if (FAILED(hr)) {
			U_LOG_E("Failed to create DP input texture %ux%u: 0x%lx", content_w, content_h, hr);
			atlas_tex->Release();
			return atlas_srv; // fallback to original
		}

		hr = c->device->CreateShaderResourceView(c->dp_input_texture, nullptr, &c->dp_input_srv);
		if (FAILED(hr)) {
			U_LOG_E("Failed to create DP input SRV: 0x%lx", hr);
			c->dp_input_texture->Release();
			c->dp_input_texture = nullptr;
			atlas_tex->Release();
			return atlas_srv;
		}

		c->dp_input_width = content_w;
		c->dp_input_height = content_h;
		U_LOG_I("D3D11 crop: created DP input texture %ux%u (atlas %ux%u)",
		        content_w, content_h, atlas_desc.Width, atlas_desc.Height);
	}

	// Copy content region from atlas to intermediate
	D3D11_BOX src_box = {0, 0, 0, content_w, content_h, 1};
	c->context->CopySubresourceRegion(c->dp_input_texture, 0, 0, 0, 0,
	                                   atlas_tex, 0, &src_box);
	atlas_tex->Release();

	return c->dp_input_srv;
}

static xrt_result_t
d3d11_compositor_layer_commit(struct xrt_compositor *xc, xrt_graphics_sync_handle_t sync_handle)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);

	std::lock_guard<std::mutex> lock(c->mutex);

	// Get predicted eye positions
	struct xrt_eye_positions eye_pos = {};
	if (c->display_processor != nullptr) {
		xrt_display_processor_d3d11_get_predicted_eye_positions(c->display_processor, &eye_pos);
	}
	if (!eye_pos.valid) {
		// Use view_count from the active rendering mode for the fallback
		uint32_t fallback_count = 2;
		if (c->xdev != NULL && c->xdev->hmd != NULL) {
			uint32_t idx = c->xdev->hmd->active_rendering_mode_index;
			if (idx < c->xdev->rendering_mode_count) {
				fallback_count = c->xdev->rendering_modes[idx].view_count;
			}
		}
		if (fallback_count == 1) {
			eye_pos.count = 1;
			eye_pos.eyes[0] = {0.0f, 0.0f, 0.6f};
		} else {
			eye_pos.count = 2;
			eye_pos.eyes[0] = {-0.032f, 0.0f, 0.6f};
			eye_pos.eyes[1] = { 0.032f, 0.0f, 0.6f};
		}
	}

	// Extract eye positions for renderer (display processor still needs L/R)
	struct xrt_vec3 left_eye = {eye_pos.eyes[0].x, eye_pos.eyes[0].y, eye_pos.eyes[0].z};
	struct xrt_vec3 right_eye = {eye_pos.eyes[1].x, eye_pos.eyes[1].y, eye_pos.eyes[1].z};

	// Sync hardware_display_3d and tile layout from device's active rendering mode
	if (c->xdev != NULL && c->xdev->hmd != NULL) {
		uint32_t idx = c->xdev->hmd->active_rendering_mode_index;
		if (idx < c->xdev->rendering_mode_count) {
			const struct xrt_rendering_mode *mode = &c->xdev->rendering_modes[idx];
			c->hardware_display_3d = mode->hardware_display_3d;
			// Clamp eye count to the active mode's view_count
			if (eye_pos.count > mode->view_count) {
				eye_pos.count = mode->view_count;
			}
			if (mode->tile_columns > 0 && c->renderer != NULL) {
				comp_d3d11_renderer_set_tile_layout(
				    c->renderer, mode->tile_columns, mode->tile_rows);
			}
		}
	}

	// Runtime-side 2D/3D toggle from qwerty V key
#ifdef XRT_BUILD_DRIVER_QWERTY
	if (c->xsysd != nullptr) {
		bool force_2d = false;
		bool toggled = qwerty_check_display_mode_toggle(c->xsysd->xdevs, c->xsysd->xdev_count, &force_2d);
		if (toggled) {
			struct xrt_device *head = c->xsysd->static_roles.head;
			if (head != nullptr && head->hmd != NULL) {
				if (force_2d) {
					// Save current 3D mode index before switching to 2D
					uint32_t cur = head->hmd->active_rendering_mode_index;
					if (cur < head->rendering_mode_count &&
					    head->rendering_modes[cur].hardware_display_3d) {
						c->last_3d_mode_index = cur;
					}
					head->hmd->active_rendering_mode_index = 0;
				} else {
					// Restore last 3D mode index
					head->hmd->active_rendering_mode_index = c->last_3d_mode_index;
				}
			}
			comp_d3d11_compositor_request_display_mode(&c->base.base, !force_2d);
		}

		// Rendering mode change from qwerty 1/2/3 keys.
		// Legacy apps (no XR_EXT_display_info) only support V toggle between
		// mode 0 (2D) and mode 1 (default 3D) — skip direct mode selection.
		if (!c->legacy_app_tile_scaling) {
			int render_mode = -1;
			if (qwerty_check_rendering_mode_change(c->xsysd->xdevs, c->xsysd->xdev_count, &render_mode)) {
				struct xrt_device *head = c->xsysd->static_roles.head;
				if (head != nullptr) {
					xrt_device_set_property(head, XRT_DEVICE_PROPERTY_OUTPUT_MODE, render_mode);
				}
			}
		}
	}
#endif

	// Get target (window) dimensions for mono viewport sizing.
	// In shared texture mode (no target), use canvas dims if available —
	// the DP weaves at canvas resolution, not full shared texture size.
	uint32_t tgt_width = c->settings.preferred.width;
	uint32_t tgt_height = c->settings.preferred.height;
	if (c->target != nullptr) {
		comp_d3d11_target_get_dimensions(c->target, &tgt_width, &tgt_height);
	} else if (c->canvas.valid && c->canvas.w > 0 && c->canvas.h > 0) {
		tgt_width = c->canvas.w;
		tgt_height = c->canvas.h;
	}

	// Sync renderer view dims from active mode — set_tile_layout derives
	// view dims from atlas invariance, but actual mode dims may differ
	// (e.g. 2D mode needs full display height). Resize if needed.
	// Legacy apps: view dims are fixed at compromise scale, skip mode sync.
	if (!c->legacy_app_tile_scaling &&
	    c->xdev != NULL && c->xdev->hmd != NULL && c->renderer != NULL) {
		uint32_t idx = c->xdev->hmd->active_rendering_mode_index;
		if (idx < c->xdev->rendering_mode_count) {
			const struct xrt_rendering_mode *mode = &c->xdev->rendering_modes[idx];
			if (mode->view_width_pixels > 0) {
				uint32_t new_vw = mode->view_width_pixels;
				uint32_t new_vh = mode->view_height_pixels;
				if (c->canvas.valid) {
					u_tiling_compute_canvas_view(mode, c->canvas.w, c->canvas.h,
					                             &new_vw, &new_vh);
				}
				uint32_t cur_vw, cur_vh;
				comp_d3d11_renderer_get_view_dimensions(c->renderer, &cur_vw, &cur_vh);
				if (cur_vw != new_vw || cur_vh != new_vh) {
					uint32_t resize_target_h = (c->display_processor != NULL) ? new_vh : tgt_height;
					comp_d3d11_renderer_resize(
					    c->renderer,
					    new_vw,
					    new_vh,
					    resize_target_h);
				}
			}
		}
	}

	// Zero-copy check: can we pass the app's swapchain directly to the DP?
	bool zero_copy = false;
	void *zc_srv = nullptr;
	{
		const struct xrt_rendering_mode *mode = NULL;
		if (c->xdev != NULL && c->xdev->hmd != NULL) {
			uint32_t idx = c->xdev->hmd->active_rendering_mode_index;
			if (idx < c->xdev->rendering_mode_count)
				mode = &c->xdev->rendering_modes[idx];
		}
		if (mode != NULL && c->layer_accum.layer_count == 1) {
			struct comp_layer *layer = &c->layer_accum.layers[0];
			if (layer->data.type == XRT_LAYER_PROJECTION ||
			    layer->data.type == XRT_LAYER_PROJECTION_DEPTH) {
				uint32_t vc = mode->view_count;
				bool same_sc = (vc > 0 && vc <= XRT_MAX_VIEWS && layer->sc_array[0] != NULL);
				for (uint32_t v = 1; v < vc && same_sc; v++) {
					if (layer->sc_array[v] != layer->sc_array[0])
						same_sc = false;
				}
				if (same_sc) {
					uint32_t img_idx = layer->data.proj.v[0].sub.image_index;
					bool same_idx = true;
					for (uint32_t v = 1; v < vc; v++) {
						if (layer->data.proj.v[v].sub.image_index != img_idx) {
							same_idx = false;
							break;
						}
					}
					bool all_array_zero = same_idx;
					for (uint32_t v = 0; v < vc && all_array_zero; v++) {
						if (layer->data.proj.v[v].sub.array_index != 0)
							all_array_zero = false;
					}
					if (all_array_zero) {
						uint32_t sw, sh;
						comp_d3d11_swapchain_get_dimensions(layer->sc_array[0], &sw, &sh);
						int32_t rxs[XRT_MAX_VIEWS], rys[XRT_MAX_VIEWS];
						uint32_t rws[XRT_MAX_VIEWS], rhs_arr[XRT_MAX_VIEWS];
						for (uint32_t v = 0; v < vc; v++) {
							rxs[v] = layer->data.proj.v[v].sub.rect.offset.w;
							rys[v] = layer->data.proj.v[v].sub.rect.offset.h;
							rws[v] = layer->data.proj.v[v].sub.rect.extent.w;
							rhs_arr[v] = layer->data.proj.v[v].sub.rect.extent.h;
						}
						if (u_tiling_can_zero_copy(vc, rxs, rys, rws, rhs_arr, sw, sh, mode)) {
							zc_srv = comp_d3d11_swapchain_get_srv(layer->sc_array[0], img_idx);
							if (zc_srv != nullptr)
								zero_copy = true;
						}
					}
				}
			}
		}
	}

	// Wait for GPU completion of all projection swapchain textures before reading.
	//
	// barrier_image(TO_COMP) at xrReleaseSwapchainImage inserts a Flush() then
	// signals an ID3D11Fence.  Waiting here blocks until the GPU has processed
	// all commands up to that release point — no spinning, no per-frame allocs.
	// Falls back to Flush+D3D11_QUERY_EVENT spin-wait on pre-D3D11.4 hardware.
	//
	// Deduplicates swapchains so two eyes on one swapchain only wait once.
	{
		struct xrt_swapchain *waited[XRT_MAX_LAYERS * XRT_MAX_VIEWS] = {};
		uint32_t wait_count = 0;

		for (uint32_t li = 0; li < c->layer_accum.layer_count; li++) {
			struct comp_layer *layer = &c->layer_accum.layers[li];

			// Determine which swapchains to wait on based on layer type
			uint32_t sc_count = 0;
			if (layer->data.type == XRT_LAYER_PROJECTION ||
			    layer->data.type == XRT_LAYER_PROJECTION_DEPTH) {
				sc_count = layer->data.view_count < XRT_MAX_VIEWS
				               ? layer->data.view_count
				               : XRT_MAX_VIEWS;
			} else if (layer->data.type == XRT_LAYER_WINDOW_SPACE) {
				sc_count = 1; // Window-space layers have one swapchain in sc_array[0]
			} else {
				continue;
			}

			for (uint32_t v = 0; v < sc_count; v++) {
				struct xrt_swapchain *xsc = layer->sc_array[v];
				if (xsc == nullptr) {
					continue;
				}
				// Skip if already waited on this swapchain this frame.
				bool seen = false;
				for (uint32_t w = 0; w < wait_count; w++) {
					if (waited[w] == xsc) {
						seen = true;
						break;
					}
				}
				if (!seen && wait_count < ARRAY_SIZE(waited)) {
					waited[wait_count++] = xsc;
					comp_d3d11_swapchain_wait_gpu_complete(xsc, 100);
				}
			}
		}
	}

	// Verify app renders at the expected resolution (not stretched)
	{
		static int rect_check_log = 0;
		uint32_t expected_vw, expected_vh;
		comp_d3d11_renderer_get_view_dimensions(c->renderer, &expected_vw, &expected_vh);
		for (uint32_t li = 0; li < c->layer_accum.layer_count && rect_check_log < 8; li++) {
			struct comp_layer *layer = &c->layer_accum.layers[li];
			if (layer->data.type != XRT_LAYER_PROJECTION &&
			    layer->data.type != XRT_LAYER_PROJECTION_DEPTH)
				continue;
			for (uint32_t v = 0; v < layer->data.view_count && v < XRT_MAX_VIEWS; v++) {
				const struct xrt_rect *r = &layer->data.proj.v[v].sub.rect;
				if ((uint32_t)r->extent.w != expected_vw || (uint32_t)r->extent.h != expected_vh) {
					if (rect_check_log < 5) {
						U_LOG_W("VIEW SIZE MISMATCH: view[%u] app_rect=%dx%d "
						        "expected=%ux%u (legacy=%d)",
						        v, r->extent.w, r->extent.h,
						        expected_vw, expected_vh,
						        c->legacy_app_tile_scaling);
					}
					rect_check_log++;
				} else if (rect_check_log < 3) {
					U_LOG_I("VIEW SIZE OK: view[%u] app_rect=%dx%d matches expected=%ux%u",
					        v, r->extent.w, r->extent.h, expected_vw, expected_vh);
					rect_check_log++;
				}
			}
		}
	}

	// Render layers to atlas texture (skip if zero-copy).
	// Pass hardware_display_3d so the renderer knows the current display mode.
	xrt_result_t xret = XRT_SUCCESS;
	if (!zero_copy) {
		xret = comp_d3d11_renderer_draw(
		    c->renderer, &c->layer_accum, &left_eye, &right_eye, tgt_width, tgt_height, c->hardware_display_3d);
		if (xret != XRT_SUCCESS) {
			U_LOG_E("Failed to render layers");
			return xret;
		}
	}

#ifdef XRT_FEATURE_DEBUG_GUI
	// Update debug GUI preview with the rendered atlas texture
	if (comp_d3d11_debug_is_active(c->debug)) {
		ID3D11Texture2D *atlas_texture =
		    static_cast<ID3D11Texture2D *>(comp_d3d11_renderer_get_atlas_texture(c->renderer));
		comp_d3d11_debug_update_preview(c->debug, c->context, atlas_texture);
	}
#endif

	bool weaving_done = false;

	// Offscreen shared-texture-only path: no DXGI target
	if (c->target == nullptr) {
		// Weave/blit directly into the shared texture
		if (c->display_processor != NULL && c->shared_rtv != nullptr) {
			void *atlas_srv = zero_copy ? zc_srv : comp_d3d11_renderer_get_atlas_srv(c->renderer);
			uint32_t view_width, view_height;
			comp_d3d11_renderer_get_view_dimensions(c->renderer, &view_width, &view_height);
			uint32_t tile_columns, tile_rows;
			comp_d3d11_renderer_get_tile_layout(c->renderer, &tile_columns, &tile_rows);

			// Crop atlas to content dimensions before passing to DP
			uint32_t content_w = tile_columns * view_width;
			uint32_t content_h = tile_rows * view_height;
			atlas_srv = d3d11_crop_atlas_for_dp(c, atlas_srv, content_w, content_h);

			D3D11_TEXTURE2D_DESC st_desc;
			c->shared_texture->GetDesc(&st_desc);

			// Use canvas dimensions as DP target when available.
			// The shared texture is display-sized (worst case), but the DP
			// should weave at canvas resolution — the actual displayed area.
			uint32_t dp_target_w = st_desc.Width;
			uint32_t dp_target_h = st_desc.Height;
			if (c->canvas.valid && c->canvas.w > 0 && c->canvas.h > 0) {
				dp_target_w = c->canvas.w;
				dp_target_h = c->canvas.h;
			}

			// Set viewport to canvas region within the shared texture
			D3D11_VIEWPORT dp_vp = {};
			dp_vp.Width = (FLOAT)dp_target_w;
			dp_vp.Height = (FLOAT)dp_target_h;
			dp_vp.MaxDepth = 1.0f;
			c->context->RSSetViewports(1, &dp_vp);

			// Bind shared texture as render target for the weaver
			c->context->OMSetRenderTargets(1, &c->shared_rtv, nullptr);

			xrt_display_processor_d3d11_process_atlas(
			    c->display_processor, c->context, atlas_srv, view_width, view_height,
			    tile_columns, tile_rows, DXGI_FORMAT_R8G8B8A8_UNORM, dp_target_w, dp_target_h,
			    c->canvas.valid ? c->canvas.x : 0,
			    c->canvas.valid ? c->canvas.y : 0,
			    c->canvas.valid ? c->canvas.w : 0,
			    c->canvas.valid ? c->canvas.h : 0);
			weaving_done = true;
		}

		c->context->Flush();
		return XRT_SUCCESS;
	}

	// Normal path: acquire DXGI target image
	uint32_t target_index;
	xret = comp_d3d11_target_acquire(c->target, &target_index);
	if (xret != XRT_SUCCESS) {
		U_LOG_E("Failed to acquire target");
		return xret;
	}

	// Use generic display processor for weaving/blit (vendor-agnostic path)
	// The DP may be a no-op in 2D mode (returns without rendering).
	if (c->display_processor != NULL) {
		static bool dp_logged = false;
		if (!dp_logged) {
			U_LOG_W("D3D11 weaving via display processor interface");
			dp_logged = true;
		}

		// Re-bind target RTV before DP — the renderer may have changed it to the atlas RTV.
		// The DP writes to the currently bound render target (see xrt_display_processor_d3d11.h).
		comp_d3d11_target_bind(c->target);

		void *atlas_srv = zero_copy ? zc_srv : comp_d3d11_renderer_get_atlas_srv(c->renderer);

		uint32_t view_width, view_height;
		comp_d3d11_renderer_get_view_dimensions(c->renderer, &view_width, &view_height);
		uint32_t tile_columns, tile_rows;
		comp_d3d11_renderer_get_tile_layout(c->renderer, &tile_columns, &tile_rows);

		// Crop atlas to content dimensions before passing to DP
		uint32_t content_w = tile_columns * view_width;
		uint32_t content_h = tile_rows * view_height;
		atlas_srv = d3d11_crop_atlas_for_dp(c, atlas_srv, content_w, content_h);

		uint32_t target_width, target_height;
		comp_d3d11_target_get_dimensions(c->target, &target_width, &target_height);

		xrt_display_processor_d3d11_process_atlas(
		    c->display_processor, c->context, atlas_srv, view_width, view_height,
		    tile_columns, tile_rows, DXGI_FORMAT_R8G8B8A8_UNORM, target_width, target_height,
		    c->canvas.valid ? c->canvas.x : 0,
		    c->canvas.valid ? c->canvas.y : 0,
		    c->canvas.valid ? c->canvas.w : 0,
		    c->canvas.valid ? c->canvas.h : 0);
		weaving_done = true;
	}

	// HUD overlay (post-processing, always readable)
	d3d11_render_hud_overlay(c, weaving_done, &eye_pos);

	// Copy composited output into shared texture if active (dual output: window + shared)
	if (c->has_shared_texture && c->shared_texture != nullptr) {
		ID3D11Texture2D *back_buffer = static_cast<ID3D11Texture2D *>(
		    comp_d3d11_target_get_back_buffer(c->target));
		if (back_buffer != nullptr) {
			D3D11_TEXTURE2D_DESC src_desc, dst_desc;
			back_buffer->GetDesc(&src_desc);
			c->shared_texture->GetDesc(&dst_desc);

			if (src_desc.Width == dst_desc.Width && src_desc.Height == dst_desc.Height) {
				c->context->CopyResource(c->shared_texture, back_buffer);
			} else {
				UINT copy_width = (src_desc.Width < dst_desc.Width) ? src_desc.Width : dst_desc.Width;
				UINT copy_height = (src_desc.Height < dst_desc.Height) ? src_desc.Height : dst_desc.Height;
				D3D11_BOX src_box = {0, 0, 0, copy_width, copy_height, 1};
				c->context->CopySubresourceRegion(c->shared_texture, 0, 0, 0, 0,
				                                   back_buffer, 0, &src_box);
			}
			c->context->Flush();
		}
	}

	// Present with VSync
	xret = comp_d3d11_target_present(c->target, 1);

	// Signal WM_PAINT that the frame is done (unblocks modal drag loop)
	if (c->owns_window && c->own_window != nullptr) {
		comp_d3d11_window_signal_paint_done(c->own_window);
	}

	if (xret != XRT_SUCCESS) {
		U_LOG_E("Failed to present");
		return xret;
	}

	return XRT_SUCCESS;
}

static xrt_result_t
d3d11_compositor_layer_commit_with_semaphore(struct xrt_compositor *xc,
                                              struct xrt_compositor_semaphore *xcsem,
                                              uint64_t value)
{
	// Use the same implementation as layer_commit
	return d3d11_compositor_layer_commit(xc, XRT_GRAPHICS_SYNC_HANDLE_INVALID);
}

static void
d3d11_compositor_destroy(struct xrt_compositor *xc)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);

	U_LOG_I("Destroying D3D11 compositor");

#ifdef XRT_FEATURE_DEBUG_GUI
	// Stop debug GUI first (before destroying resources it may reference)
	if (c->debug != nullptr) {
		comp_d3d11_debug_destroy(&c->debug);
	}
	if (c->debug_gui != nullptr) {
		u_debug_gui_stop(&c->debug_gui);
	}
#endif

	// Destroy shared texture resources
	if (c->shared_rtv != nullptr) {
		c->shared_rtv->Release();
		c->shared_rtv = nullptr;
	}
	if (c->shared_texture != nullptr) {
		c->shared_texture->Release();
		c->shared_texture = nullptr;
	}

	// Destroy HUD
	if (c->hud_texture != nullptr) {
		c->hud_texture->Release();
		c->hud_texture = nullptr;
	}
	u_hud_destroy(&c->hud);

	// Destroy DP input crop texture
	if (c->dp_input_srv != nullptr) {
		c->dp_input_srv->Release();
		c->dp_input_srv = nullptr;
	}
	if (c->dp_input_texture != nullptr) {
		c->dp_input_texture->Release();
		c->dp_input_texture = nullptr;
	}

	// Destroy display processor (handles all vendor cleanup internally)
	xrt_display_processor_d3d11_destroy(&c->display_processor);

	if (c->renderer != nullptr) {
		comp_d3d11_renderer_destroy(&c->renderer);
	}

	if (c->target != nullptr) {
		comp_d3d11_target_destroy(&c->target);
	}

	if (c->dxgi_factory != nullptr) {
		c->dxgi_factory->Release();
	}

	if (c->context != nullptr) {
		c->context->Release();
	}

	if (c->device != nullptr) {
		c->device->Release();
	}

	// Destroy self-created window if we own it
	if (c->owns_window && c->own_window != nullptr) {
		comp_d3d11_window_destroy(&c->own_window);
	}

	// layer_accum doesn't need special cleanup - it's just a struct

	delete c;
}

/*
 *
 * Exported functions
 *
 */

extern "C" xrt_result_t
comp_d3d11_compositor_create(struct xrt_device *xdev,
                             void *hwnd,
                             void *d3d11_device,
                             void *dp_factory_d3d11,
                             void *shared_texture_handle,
                             struct xrt_compositor_native **out_xc)
{
	if (d3d11_device == nullptr) {
		U_LOG_E("D3D11 device is null");
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	U_LOG_I("Creating D3D11 native compositor");

	// Allocate compositor
	comp_d3d11_compositor *c = new comp_d3d11_compositor();
	memset(&c->base, 0, sizeof(c->base));

	c->xdev = xdev;
	c->own_window = nullptr;
	c->owns_window = false;
	c->app_hwnd = nullptr;
	c->canvas = {};
	c->hardware_display_3d = true;
	c->last_3d_mode_index = 1;

	// Handle window: use provided HWND, create our own, or go offscreen (shared texture)
	if (shared_texture_handle != nullptr) {
		// Shared texture mode: compositor doesn't own a swapchain.
		// Store app HWND separately for display processor position tracking.
		// No hidden window — the DP gets the app's real HWND directly.
		c->hwnd = nullptr;
		if (hwnd != nullptr) {
			c->app_hwnd = static_cast<HWND>(hwnd);
			U_LOG_I("Shared texture mode with app HWND for position tracking: %p", hwnd);
		} else {
			U_LOG_I("Shared texture mode (offscreen) — no window");
		}
	} else if (hwnd != nullptr) {
		// App provided window via XR_EXT_win32_window_binding (ext mode)
		c->hwnd = static_cast<HWND>(hwnd);
		U_LOG_I("Using app-provided window handle: %p", hwnd);
	} else {
		// No window provided - create our own at native display resolution
		uint32_t win_w = xdev->hmd->screens[0].w_pixels;
		uint32_t win_h = xdev->hmd->screens[0].h_pixels;
		if (win_w == 0 || win_h == 0) {
			win_w = 1920;
			win_h = 1080;
		}
		U_LOG_I("No window handle provided, creating self-owned window (%ux%u)", win_w, win_h);
		xrt_result_t xret = comp_d3d11_window_create(win_w, win_h, &c->own_window);
		if (xret != XRT_SUCCESS) {
			U_LOG_E("Failed to create self-owned window");
			delete c;
			return xret;
		}
		c->hwnd = static_cast<HWND>(comp_d3d11_window_get_hwnd(c->own_window));
		c->owns_window = true;
		U_LOG_I("Created self-owned window: %p", (void *)c->hwnd);
	}

	// Get D3D11 device - just use the base interface, we don't need Device5 features
	c->device = static_cast<ID3D11Device *>(d3d11_device);
	c->device->AddRef();

	// Get immediate context
	c->device->GetImmediateContext(&c->context);

	// Enable D3D11 multithread protection for cross-thread window/compositor access.
	// The HWND lives on a dedicated window thread while D3D11 rendering happens here.
	HRESULT hr;
	{
		ID3D10Multithread *mt = nullptr;
		hr = c->device->QueryInterface(__uuidof(ID3D10Multithread), (void **)&mt);
		if (SUCCEEDED(hr) && mt != nullptr) {
			mt->SetMultithreadProtected(TRUE);
			mt->Release();
			U_LOG_W("D3D11 multithread protection enabled");
		}
	}

	// Limit DXGI frame queue to 1 to prevent burst/stall frame pacing.
	// The default is 3, which lets the app submit multiple frames before
	// Present(1) blocks, causing micro-stutter in windowed mode.
	{
		IDXGIDevice1 *dxgi_dev1 = nullptr;
		hr = c->device->QueryInterface(__uuidof(IDXGIDevice1), (void **)&dxgi_dev1);
		if (SUCCEEDED(hr) && dxgi_dev1 != nullptr) {
			dxgi_dev1->SetMaximumFrameLatency(1);
			dxgi_dev1->Release();
			U_LOG_W("DXGI maximum frame latency set to 1");
		}
	}

	// Get DXGI factory
	IDXGIDevice *dxgi_device;
	hr = c->device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgi_device));
	if (SUCCEEDED(hr)) {
		IDXGIAdapter *adapter;
		dxgi_device->GetAdapter(&adapter);

		hr = adapter->GetParent(__uuidof(IDXGIFactory4), reinterpret_cast<void **>(&c->dxgi_factory));
		adapter->Release();
		dxgi_device->Release();
	}

	if (c->dxgi_factory == nullptr) {
		U_LOG_E("Failed to get DXGI factory");
		d3d11_compositor_destroy(&c->base.base);
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	// Initialize settings with defaults (simplified for D3D11 native compositor)
	memset(&c->settings, 0, sizeof(c->settings));
	c->settings.preferred.width = xdev->hmd->screens[0].w_pixels;
	c->settings.preferred.height = xdev->hmd->screens[0].h_pixels;
	if (c->settings.preferred.width == 0 || c->settings.preferred.height == 0) {
		c->settings.preferred.width = 1920;
		c->settings.preferred.height = 1080;
	}
	c->settings.nominal_frame_interval_ns = xdev->hmd->screens[0].nominal_frame_interval_ns;
	if (c->settings.nominal_frame_interval_ns == 0) {
		c->settings.nominal_frame_interval_ns = (1000 * 1000 * 1000) / 60; // 60Hz default
	}

	// Get actual window size if we have a window
	if (c->hwnd != nullptr) {
		RECT rect;
		if (GetClientRect(c->hwnd, &rect)) {
			c->settings.preferred.width = rect.right - rect.left;
			c->settings.preferred.height = rect.bottom - rect.top;
		}
	}

	// Open shared texture from app's HANDLE if provided
	c->shared_texture = nullptr;
	c->shared_rtv = nullptr;
	c->has_shared_texture = false;
	if (shared_texture_handle != nullptr) {
		HANDLE h = static_cast<HANDLE>(shared_texture_handle);
		hr = c->device->OpenSharedResource(h, __uuidof(ID3D11Texture2D),
		                                    reinterpret_cast<void **>(&c->shared_texture));
		if (FAILED(hr) || c->shared_texture == nullptr) {
			U_LOG_E("Failed to open shared texture handle %p: 0x%08x", shared_texture_handle, hr);
			d3d11_compositor_destroy(&c->base.base);
			return XRT_ERROR_DEVICE_CREATION_FAILED;
		}

		D3D11_TEXTURE2D_DESC st_desc;
		c->shared_texture->GetDesc(&st_desc);
		U_LOG_W("Opened shared texture: %ux%u format=%u", st_desc.Width, st_desc.Height, st_desc.Format);

		// Use shared texture dimensions for settings when no window
		if (c->hwnd == nullptr) {
			c->settings.preferred.width = st_desc.Width;
			c->settings.preferred.height = st_desc.Height;
		}

		// Create RTV on shared texture
		hr = c->device->CreateRenderTargetView(c->shared_texture, nullptr, &c->shared_rtv);
		if (FAILED(hr)) {
			U_LOG_E("Failed to create RTV on shared texture: 0x%08x", hr);
			d3d11_compositor_destroy(&c->base.base);
			return XRT_ERROR_DEVICE_CREATION_FAILED;
		}
		c->has_shared_texture = true;
	}

	// Create output target (DXGI swapchain) — skip if offscreen (no HWND)
	if (c->hwnd != nullptr) {
		xrt_result_t xret = comp_d3d11_target_create(c, c->hwnd,
		                                              c->settings.preferred.width,
		                                              c->settings.preferred.height,
		                                              &c->target);
		if (xret != XRT_SUCCESS) {
			U_LOG_E("Failed to create D3D11 target");
			d3d11_compositor_destroy(&c->base.base);
			return xret;
		}
	} else {
		c->target = nullptr;
		U_LOG_I("No DXGI target — offscreen shared texture mode");
	}

	// Query display refresh rate from DXGI output
	c->display_refresh_rate = 60.0f; // Default to 60Hz
	IDXGIDevice *refresh_dxgi_device = nullptr;
	hr = c->device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&refresh_dxgi_device));
	if (SUCCEEDED(hr) && refresh_dxgi_device != nullptr) {
		IDXGIAdapter *refresh_adapter = nullptr;
		refresh_dxgi_device->GetAdapter(&refresh_adapter);
		if (refresh_adapter != nullptr) {
			IDXGIOutput *output = nullptr;
			// Try to get the output containing the window
			if (SUCCEEDED(refresh_adapter->EnumOutputs(0, &output)) && output != nullptr) {
				DXGI_OUTPUT_DESC outputDesc;
				if (SUCCEEDED(output->GetDesc(&outputDesc))) {
					// Query the display mode list for the output
					UINT numModes = 0;
					output->GetDisplayModeList(DXGI_FORMAT_R8G8B8A8_UNORM, 0, &numModes, nullptr);
					if (numModes > 0) {
						DXGI_MODE_DESC *modes = new DXGI_MODE_DESC[numModes];
						if (SUCCEEDED(output->GetDisplayModeList(DXGI_FORMAT_R8G8B8A8_UNORM, 0,
						                                          &numModes, modes))) {
							// Find the highest refresh rate mode that matches our resolution
							float best_rate = 0.0f;
							for (UINT m = 0; m < numModes; m++) {
								float rate = static_cast<float>(modes[m].RefreshRate.Numerator) /
								             static_cast<float>(modes[m].RefreshRate.Denominator);
								if (rate > best_rate) {
									best_rate = rate;
								}
							}
							if (best_rate > 0.0f) {
								c->display_refresh_rate = best_rate;
							}
						}
						delete[] modes;
					}
				}
				output->Release();
			}
			refresh_adapter->Release();
		}
		refresh_dxgi_device->Release();
	}
	U_LOG_I("Display refresh rate: %.2f Hz", c->display_refresh_rate);

	// Determine view dimensions for the atlas texture.
	// Default: derive from window size (half width for 2-view atlas)
	uint32_t view_width = c->settings.preferred.width / 2;
	uint32_t view_height = c->settings.preferred.height;

	// Create display processor via factory (set by the target builder at init time).
	if (dp_factory_d3d11 != NULL) {
		auto factory = (xrt_dp_factory_d3d11_fn_t)dp_factory_d3d11;
		// Use app HWND for position tracking in shared texture mode,
		// otherwise use the compositor's own HWND
		HWND dp_hwnd = c->hwnd != nullptr ? c->hwnd : c->app_hwnd;
		xrt_result_t dp_ret = factory(c->device, c->context, dp_hwnd, &c->display_processor);
		if (dp_ret != XRT_SUCCESS) {
			U_LOG_W("D3D11 display processor factory failed (error %d), continuing without",
			        (int)dp_ret);
			c->display_processor = nullptr;
		} else {
			U_LOG_W("D3D11 display processor created via factory");
		}
	} else {
		U_LOG_W("No D3D11 display processor factory provided");
	}

	// If display processor is available, query display pixel info to compute
	// optimal view dimensions (scaled to window size).
	if (c->display_processor != nullptr) {
		uint32_t disp_px_w = 0, disp_px_h = 0;
		int32_t disp_left = 0, disp_top = 0;
		if (xrt_display_processor_d3d11_get_display_pixel_info(
		        c->display_processor, &disp_px_w, &disp_px_h,
		        &disp_left, &disp_top) &&
		    disp_px_w > 0 && disp_px_h > 0) {
			// Use half display width as base view dims
			uint32_t base_vw = disp_px_w / 2;
			uint32_t base_vh = disp_px_h;

			U_LOG_W("Display pixel info: %ux%u, base view dims: %ux%u per eye",
			        disp_px_w, disp_px_h, base_vw, base_vh);

			// Scale by window/display pixel ratio (same as resize path)
			float ratio = fminf(
			    (float)c->settings.preferred.width / (float)disp_px_w,
			    (float)c->settings.preferred.height / (float)disp_px_h);
			if (ratio > 1.0f) {
				ratio = 1.0f;
			}
			view_width = (uint32_t)((float)base_vw * ratio);
			view_height = (uint32_t)((float)base_vh * ratio);
			U_LOG_W("Scaled to window ratio %.3f: %ux%u per eye", ratio, view_width, view_height);
		}
	}

	// Create renderer with the correct view dimensions.
	// When a display processor is present, the atlas texture height must match
	// view_height so the display processor's UV 0..1 maps exactly to the rendered content.
	// Without a display processor, use the window height so the atlas texture is tall enough
	// for mono fallback blitting.  Mono/2D mode uses a GPU stretch blit to fill the full
	// window regardless of atlas texture height.
	// NOTE: the per-frame resize path (renderer_resize) must apply the same guard —
	// see resize_target_h in the mode-switch handler above.
	uint32_t target_height = (c->display_processor != NULL) ? view_height : c->settings.preferred.height;
	xrt_result_t xret = comp_d3d11_renderer_create(c, view_width, view_height, target_height, &c->renderer);
	if (xret != XRT_SUCCESS) {
		U_LOG_E("Failed to create D3D11 renderer");
		d3d11_compositor_destroy(&c->base.base);
		return xret;
	}

#ifdef XRT_FEATURE_DEBUG_GUI
	// Create debug GUI (controlled by XRT_DEBUG_GUI env var)
	struct u_debug_gui_create_info udgci = {};
	udgci.open = U_DEBUG_GUI_OPEN_AUTO;
	strncpy(udgci.window_title, "Monado D3D11 Debug", U_DEBUG_GUI_WINDOW_TITLE_MAX - 1);

	int gui_ret = u_debug_gui_create(&udgci, &c->debug_gui);
	if (gui_ret == 0 && c->debug_gui != nullptr) {
		// Debug GUI was created, now create the readback module
		// Stereo texture is 2x view width
		xrt_result_t xret2 = comp_d3d11_debug_create(c->device, view_width * 2, view_height, &c->debug);
		if (xret2 != XRT_SUCCESS) {
			U_LOG_W("Failed to create D3D11 debug readback, debug GUI preview disabled");
			c->debug = nullptr;
		} else {
			// Add debug variables to u_var
			comp_d3d11_debug_add_vars(c->debug);
		}

		// Note: u_debug_gui_start() is called later when xsysd is available
		// For now, we just create the resources
		U_LOG_I("D3D11 debug GUI created (set XRT_DEBUG_GUI=1 to enable window)");
	}
#endif

	// Initialize layer accumulator - just zero it
	memset(&c->layer_accum, 0, sizeof(c->layer_accum));

	// Create HUD overlay for runtime-owned windows
	c->hud = NULL;
	c->hud_texture = nullptr;
	c->hud_initialized = false;
	c->last_frame_time_ns = 0;
	c->smoothed_frame_time_ms = 16.67f;
	if (c->owns_window) {
		u_hud_create(&c->hud, c->settings.preferred.width);
	}

	// Populate supported swapchain formats (DXGI formats for D3D11)
	// These are the common formats that D3D11 applications can use
	uint32_t format_count = 0;
	c->base.base.info.formats[format_count++] = DXGI_FORMAT_R8G8B8A8_UNORM;
	c->base.base.info.formats[format_count++] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	c->base.base.info.formats[format_count++] = DXGI_FORMAT_B8G8R8A8_UNORM;
	c->base.base.info.formats[format_count++] = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
	c->base.base.info.formats[format_count++] = DXGI_FORMAT_R16G16B16A16_FLOAT;
	c->base.base.info.formats[format_count++] = DXGI_FORMAT_R16G16B16A16_UNORM;
	c->base.base.info.formats[format_count++] = DXGI_FORMAT_D24_UNORM_S8_UINT;
	c->base.base.info.formats[format_count++] = DXGI_FORMAT_D32_FLOAT;
	c->base.base.info.formats[format_count++] = DXGI_FORMAT_D16_UNORM;
	c->base.base.info.format_count = format_count;

	U_LOG_I("D3D11 native compositor supports %u swapchain formats", format_count);

	// Set initial visibility/focus state for session state machine
	// Native in-process compositor is always visible and focused
	c->base.base.info.initial_visible = true;
	c->base.base.info.initial_focused = true;

	// Set up compositor interface
	c->base.base.get_swapchain_create_properties = d3d11_compositor_get_swapchain_create_properties;
	c->base.base.create_swapchain = d3d11_compositor_create_swapchain;
	c->base.base.import_swapchain = d3d11_compositor_import_swapchain;
	c->base.base.import_fence = d3d11_compositor_import_fence;
	c->base.base.create_semaphore = d3d11_compositor_create_semaphore;
	c->base.base.begin_session = d3d11_compositor_begin_session;
	c->base.base.end_session = d3d11_compositor_end_session;
	c->base.base.wait_frame = d3d11_compositor_wait_frame;
	c->base.base.predict_frame = d3d11_compositor_predict_frame;
	c->base.base.mark_frame = d3d11_compositor_mark_frame;
	c->base.base.begin_frame = d3d11_compositor_begin_frame;
	c->base.base.discard_frame = d3d11_compositor_discard_frame;
	c->base.base.layer_begin = d3d11_compositor_layer_begin;
	c->base.base.layer_projection = d3d11_compositor_layer_projection;
	c->base.base.layer_projection_depth = d3d11_compositor_layer_projection_depth;
	c->base.base.layer_quad = d3d11_compositor_layer_quad;
	c->base.base.layer_cube = d3d11_compositor_layer_cube;
	c->base.base.layer_cylinder = d3d11_compositor_layer_cylinder;
	c->base.base.layer_equirect1 = d3d11_compositor_layer_equirect1;
	c->base.base.layer_equirect2 = d3d11_compositor_layer_equirect2;
	c->base.base.layer_passthrough = d3d11_compositor_layer_passthrough;
	c->base.base.layer_window_space = d3d11_compositor_layer_window_space;
	c->base.base.layer_commit = d3d11_compositor_layer_commit;
	c->base.base.layer_commit_with_semaphore = d3d11_compositor_layer_commit_with_semaphore;
	c->base.base.destroy = d3d11_compositor_destroy;

	*out_xc = &c->base;

	U_LOG_IFL_I(U_LOGGING_INFO, "D3D11 native compositor created successfully (%ux%u)",
	            c->settings.preferred.width, c->settings.preferred.height);

	return XRT_SUCCESS;
}

extern "C" void
comp_d3d11_compositor_set_output_rect(struct xrt_compositor *xc,
                                       int32_t x, int32_t y,
                                       uint32_t w, uint32_t h)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);
	c->canvas.valid = true;
	c->canvas.x = x;
	c->canvas.y = y;
	c->canvas.w = w;
	c->canvas.h = h;
}

extern "C" bool
comp_d3d11_compositor_get_predicted_eye_positions(struct xrt_compositor *xc,
                                                  struct xrt_eye_positions *out_eye_pos)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);

	if (c->display_processor != nullptr) {
		if (xrt_display_processor_d3d11_get_predicted_eye_positions(c->display_processor, out_eye_pos) &&
		    out_eye_pos->valid) {
			return true;
		}
	}

	return false;
}

extern "C" bool
comp_d3d11_compositor_get_display_dimensions(struct xrt_compositor *xc,
                                              float *out_width_m,
                                              float *out_height_m)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);

	if (c->display_processor != nullptr) {
		return xrt_display_processor_d3d11_get_display_dimensions(
		    c->display_processor, out_width_m, out_height_m);
	}

	// Default display dimensions (typical laptop display size)
	*out_width_m = 0.3f;
	*out_height_m = 0.2f;

	return false;
}

extern "C" bool
comp_d3d11_compositor_get_window_metrics(struct xrt_compositor *xc,
                                          struct xrt_window_metrics *out_metrics)
{
	if (xc == nullptr || out_metrics == nullptr) {
		if (out_metrics != nullptr) {
			out_metrics->valid = false;
		}
		return false;
	}

	struct comp_d3d11_compositor *c = d3d11_comp(xc);
	memset(out_metrics, 0, sizeof(*out_metrics));

	if (c->display_processor == nullptr || c->hwnd == nullptr) {
		return false;
	}

	// Get display pixel info from display processor
	uint32_t disp_px_w = 0, disp_px_h = 0;
	int32_t disp_left = 0, disp_top = 0;
	if (!xrt_display_processor_d3d11_get_display_pixel_info(
	        c->display_processor, &disp_px_w, &disp_px_h,
	        &disp_left, &disp_top)) {
		return false;
	}

	if (disp_px_w == 0 || disp_px_h == 0) {
		return false;
	}

	// Get physical display dimensions from display processor
	float disp_w_m = 0.0f, disp_h_m = 0.0f;
	if (!xrt_display_processor_d3d11_get_display_dimensions(
	        c->display_processor, &disp_w_m, &disp_h_m)) {
		return false;
	}

	// Get window client rect
	RECT rect;
	if (!GetClientRect(c->hwnd, &rect)) {
		return false;
	}
	uint32_t win_px_w = static_cast<uint32_t>(rect.right - rect.left);
	uint32_t win_px_h = static_cast<uint32_t>(rect.bottom - rect.top);
	if (win_px_w == 0 || win_px_h == 0) {
		return false;
	}

	// Get window screen position
	POINT client_origin = {0, 0};
	ClientToScreen(c->hwnd, &client_origin);

	// Compute pixel size (meters per pixel)
	float pixel_size_x = disp_w_m / (float)disp_px_w;
	float pixel_size_y = disp_h_m / (float)disp_px_h;

	// Window physical size
	float win_w_m = (float)win_px_w * pixel_size_x;
	float win_h_m = (float)win_px_h * pixel_size_y;

	// Window center in pixels (relative to display origin in screen coords)
	float win_center_px_x = (float)(client_origin.x - disp_left) + (float)win_px_w / 2.0f;
	float win_center_px_y = (float)(client_origin.y - disp_top) + (float)win_px_h / 2.0f;

	// Display center in pixels
	float disp_center_px_x = (float)disp_px_w / 2.0f;
	float disp_center_px_y = (float)disp_px_h / 2.0f;

	// Window center offset in meters
	// X: +right (screen coords and eye coords both +right)
	// Y: negated because screen coords Y-down, eye coords Y-up
	float offset_x_m = (win_center_px_x - disp_center_px_x) * pixel_size_x;
	float offset_y_m = -((win_center_px_y - disp_center_px_y) * pixel_size_y);

	// Fill output
	out_metrics->display_width_m = disp_w_m;
	out_metrics->display_height_m = disp_h_m;
	out_metrics->display_pixel_width = disp_px_w;
	out_metrics->display_pixel_height = disp_px_h;
	out_metrics->display_screen_left = disp_left;
	out_metrics->display_screen_top = disp_top;

	out_metrics->window_pixel_width = win_px_w;
	out_metrics->window_pixel_height = win_px_h;
	out_metrics->window_screen_left = static_cast<int32_t>(client_origin.x);
	out_metrics->window_screen_top = static_cast<int32_t>(client_origin.y);

	out_metrics->window_width_m = win_w_m;
	out_metrics->window_height_m = win_h_m;
	out_metrics->window_center_offset_x_m = offset_x_m;
	out_metrics->window_center_offset_y_m = offset_y_m;

	out_metrics->valid = true;

	u_canvas_apply_to_metrics(out_metrics, &c->canvas);

	return true;
}

extern "C" bool
comp_d3d11_compositor_request_display_mode(struct xrt_compositor *xc, bool enable_3d)
{
	if (xc == nullptr) {
		return false;
	}

	struct comp_d3d11_compositor *c = d3d11_comp(xc);

	if (c->display_processor != nullptr) {
		return xrt_display_processor_d3d11_request_display_mode(c->display_processor, enable_3d);
	}

	return false;
}

extern "C" void
comp_d3d11_compositor_set_system_devices(struct xrt_compositor *xc,
                                          struct xrt_system_devices *xsysd)
{
	if (xc == nullptr) {
		return;
	}

	struct comp_d3d11_compositor *c = d3d11_comp(xc);

	c->xsysd = xsysd;

	if (xsysd != nullptr) {
		U_LOG_I("D3D11 compositor: system devices set for qwerty support");
	}

	// Pass xsysd to self-owned window for direct qwerty input from main window
	// This enables WASDQE controls without requiring the SDL debug window
	if (c->owns_window && c->own_window != nullptr) {
		comp_d3d11_window_set_system_devices(c->own_window, xsysd);
	}
}

void
comp_d3d11_compositor_set_legacy_app_tile_scaling(struct xrt_compositor *xc,
                                                   bool legacy,
                                                   float scale_x,
                                                   float scale_y,
                                                   uint32_t view_w,
                                                   uint32_t view_h)
{
	if (xc == nullptr) {
		return;
	}
	struct comp_d3d11_compositor *c = d3d11_comp(xc);
	c->legacy_app_tile_scaling = legacy;
	c->legacy_view_scale_x = scale_x;
	c->legacy_view_scale_y = scale_y;
	if (c->renderer != nullptr) {
		comp_d3d11_renderer_set_legacy_app_tile_scaling(c->renderer, legacy);
	}

	// Fix view dims at the actual recommended size the app was told to render at.
	if (legacy && c->renderer != nullptr && view_w > 0 && view_h > 0) {
		uint32_t target_h = (c->display_processor != nullptr) ? view_h : c->settings.preferred.height;
		comp_d3d11_renderer_resize(c->renderer, view_w, view_h, target_h);
	}
}
