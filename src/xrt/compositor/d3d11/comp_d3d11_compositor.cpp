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

#include "main/comp_settings.h"
#include "util/comp_layer_accum.h"

#include "xrt/xrt_handles.h"
#include "xrt/xrt_config_have.h"
#include "xrt/xrt_config_build.h"
#include "xrt/xrt_limits.h"

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

#include "sim_display/sim_display_interface.h"

#ifdef XRT_BUILD_DRIVER_QWERTY
#include "qwerty/qwerty_interface.h"
#endif

#ifdef XRT_HAVE_LEIA_SR_D3D11
#include "leia/leia_sr_d3d11.h"
#include "leia/leia_display_processor_d3d11.h"
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

	//! Window handle (either from app or self-created).
	HWND hwnd;

	//! Self-created window (NULL if app provided window).
	struct comp_d3d11_window *own_window;

	//! True if we created the window ourselves.
	bool owns_window;

#ifdef XRT_HAVE_LEIA_SR_D3D11
	//! SR weaver for light field display.
	struct leiasr_d3d11 *weaver;
#endif

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

	// Check if window was closed (user pressed ESC or closed window)
	if (c->owns_window && c->own_window != nullptr &&
	    !comp_d3d11_window_is_valid(c->own_window)) {
		U_LOG_I("Window closed - signaling session exit");
		return XRT_ERROR_IPC_FAILURE;
	}

	// During drag, synchronize with the window thread's WM_PAINT cycle.
	// This ensures the window position is stable between weave() and Present(),
	// so the interlacing pattern matches the actual displayed position.
	if (c->owns_window && c->own_window != nullptr &&
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
	// to keep DXGI in sync, but defer the expensive stereo texture reallocation until
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

#ifdef XRT_HAVE_LEIA_SR_D3D11
						// Scale renderer stereo texture proportionally to window/display ratio.
						// Skip during drag to avoid expensive texture reallocation every pixel.
						// The weaver handles mismatched stereo/target sizes via stretching.
						if (c->weaver != nullptr && !in_size_move) {
							uint32_t sr_w, sr_h, disp_px_w, disp_px_h;
							int32_t disp_left, disp_top;
							float disp_w_m, disp_h_m;

							if (leiasr_d3d11_get_recommended_view_dimensions(
							        c->weaver, &sr_w, &sr_h) &&
							    leiasr_d3d11_get_display_pixel_info(
							        c->weaver, &disp_px_w, &disp_px_h,
							        &disp_left, &disp_top, &disp_w_m, &disp_h_m) &&
							    disp_px_w > 0 && disp_px_h > 0) {

								// Scale recommended view dims by window/display ratio
								float ratio = fminf(
								    (float)new_width / (float)disp_px_w,
								    (float)new_height / (float)disp_px_h);
								if (ratio > 1.0f) {
									ratio = 1.0f;
								}

								uint32_t new_vw = (uint32_t)((float)sr_w * ratio);
								uint32_t new_vh = (uint32_t)((float)sr_h * ratio);

								uint32_t resize_target_h = (c->display_processor != NULL) ? new_vh : new_height;
							comp_d3d11_renderer_resize(c->renderer, new_vw, new_vh, resize_target_h);
							}
						} else
#endif
						// No SR weaver: resize renderer to match new window dimensions
						if (!in_size_move) {
							uint32_t new_vw = new_width / 2;
							uint32_t new_vh = new_height;
							uint32_t resize_target_h = (c->display_processor != NULL) ? new_vh : new_height;
							comp_d3d11_renderer_resize(c->renderer, new_vw, new_vh, resize_target_h);
						}
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
d3d11_render_hud_overlay(struct comp_d3d11_compositor *c, bool is_mono, bool weaving_done,
                         const struct xrt_vec3 *left_eye, const struct xrt_vec3 *right_eye)
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

	// Determine output mode string
	const char *output_mode = "Fallback";
	if (!is_mono && weaving_done) {
		output_mode = (c->display_processor != NULL) ? "Weaved" : "Weaved (direct)";
	} else if (is_mono) {
		output_mode = "2D";
	}

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

	// Get display physical dimensions from SR SDK
	float disp_w_m = 0.0f, disp_h_m = 0.0f;
	float nom_x = 0.0f, nom_y = 0.0f, nom_z = 600.0f;
	comp_d3d11_compositor_get_display_dimensions(&c->base.base, &disp_w_m, &disp_h_m);
	float disp_w_mm = disp_w_m * 1000.0f;
	float disp_h_mm = disp_h_m * 1000.0f;

	// Fill HUD data
	struct u_hud_data data = {};
	data.fps = fps;
	data.frame_time_ms = c->smoothed_frame_time_ms;
	data.mode_3d = !is_mono;
	data.output_mode = output_mode;
	data.render_width = render_w;
	data.render_height = render_h;
	data.window_width = win_w;
	data.window_height = win_h;
	data.display_width_mm = disp_w_mm;
	data.display_height_mm = disp_h_mm;
	data.nominal_x = nom_x;
	data.nominal_y = nom_y;
	data.nominal_z = nom_z;
	data.left_eye_x = left_eye->x * 1000.0f;
	data.left_eye_y = left_eye->y * 1000.0f;
	data.left_eye_z = left_eye->z * 1000.0f;
	data.right_eye_x = right_eye->x * 1000.0f;
	data.right_eye_y = right_eye->y * 1000.0f;
	data.right_eye_z = right_eye->z * 1000.0f;
	data.eye_tracking_active = (left_eye->z != 0.6f || right_eye->z != 0.6f);

#ifdef XRT_BUILD_DRIVER_QWERTY
	if (c->xsysd != nullptr) {
		struct qwerty_stereo_state ss;
		if (qwerty_get_stereo_state(c->xsysd->xdevs, c->xsysd->xdev_count, &ss)) {
			data.camera_mode = ss.camera_mode;
			data.cam_ipd_factor = ss.cam_ipd_factor;
			data.cam_parallax_factor = ss.cam_parallax_factor;
			data.cam_convergence = ss.cam_convergence;
			data.cam_half_tan_vfov = ss.cam_half_tan_vfov;
			data.disp_ipd_factor = ss.disp_ipd_factor;
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

static xrt_result_t
d3d11_compositor_layer_commit(struct xrt_compositor *xc, xrt_graphics_sync_handle_t sync_handle)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);

	std::lock_guard<std::mutex> lock(c->mutex);

	// Get predicted eye positions
	struct xrt_vec3 left_eye = {-0.032f, 0.0f, 0.6f};   // Default: 64mm IPD, 60cm from screen
	struct xrt_vec3 right_eye = {0.032f, 0.0f, 0.6f};

#ifdef XRT_HAVE_LEIA_SR_D3D11
	if (c->weaver != nullptr) {
		float left[3], right[3];
		if (leiasr_d3d11_get_predicted_eye_positions(c->weaver, left, right)) {
			left_eye.x = left[0];
			left_eye.y = left[1];
			left_eye.z = left[2];
			right_eye.x = right[0];
			right_eye.y = right[1];
			right_eye.z = right[2];
		}
	}
#endif

	// Detect mono submission from first projection layer
	bool is_mono = false;
	for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
		if (c->layer_accum.layers[i].data.type == XRT_LAYER_PROJECTION ||
		    c->layer_accum.layers[i].data.type == XRT_LAYER_PROJECTION_DEPTH) {
			is_mono = (c->layer_accum.layers[i].data.view_count == 1);
			break;
		}
	}

	// Runtime-side 2D/3D toggle from qwerty V key
#ifdef XRT_BUILD_DRIVER_QWERTY
	if (c->xsysd != nullptr) {
		bool force_2d = false;
		bool toggled = qwerty_check_display_mode_toggle(c->xsysd->xdevs, c->xsysd->xdev_count, &force_2d);
		if (toggled) {
			comp_d3d11_compositor_request_display_mode(&c->base.base, !force_2d);
		}
		if (force_2d) {
			is_mono = true;
		}
	}
#endif

	// Get target (window) dimensions for mono viewport sizing
	uint32_t tgt_width = c->settings.preferred.width;
	uint32_t tgt_height = c->settings.preferred.height;
	if (c->target != nullptr) {
		comp_d3d11_target_get_dimensions(c->target, &tgt_width, &tgt_height);
	}

	// Render layers to side-by-side stereo texture (or full-width mono).
	// Pass is_mono so the renderer forces 1-view rendering even if the app submitted 2 views
	// (e.g. runtime-side V toggle where the app doesn't know about the 2D switch).
	xrt_result_t xret = comp_d3d11_renderer_draw(
	    c->renderer, &c->layer_accum, &left_eye, &right_eye, tgt_width, tgt_height, is_mono);
	if (xret != XRT_SUCCESS) {
		U_LOG_E("Failed to render layers");
		return xret;
	}

#ifdef XRT_FEATURE_DEBUG_GUI
	// Update debug GUI preview with the rendered stereo texture
	if (comp_d3d11_debug_is_active(c->debug)) {
		ID3D11Texture2D *stereo_texture =
		    static_cast<ID3D11Texture2D *>(comp_d3d11_renderer_get_stereo_texture(c->renderer));
		comp_d3d11_debug_update_preview(c->debug, c->context, stereo_texture);
	}
#endif

	// Acquire target image
	uint32_t target_index;
	xret = comp_d3d11_target_acquire(c->target, &target_index);
	if (xret != XRT_SUCCESS) {
		U_LOG_E("Failed to acquire target");
		return xret;
	}

	bool weaving_done = false;

	// Use generic display processor for weaving (vendor-agnostic path)
	// TODO: Same crop-blit fix as Vulkan comp_multi (ensure_session_dp_crop_images).
	// If the app renders to a sub-region of the swapchain (imageRect.extent < swapchain),
	// the SRV here covers the full texture but view_width/view_height reflect the sub-region.
	// The display processor samples UVs 0..1 on the full SRV, reading uninitialized texels.
	// Fix: crop-copy into a correctly-sized intermediate D3D11 texture before passing to
	// the display processor, matching the Vulkan dp_crop_images approach.
	if (!is_mono && c->display_processor != NULL) {
		static bool dp_logged = false;
		if (!dp_logged) {
			U_LOG_W("D3D11 weaving via display processor interface");
			dp_logged = true;
		}

		void *stereo_srv = comp_d3d11_renderer_get_stereo_srv(c->renderer);

		uint32_t view_width, view_height;
		comp_d3d11_renderer_get_view_dimensions(c->renderer, &view_width, &view_height);

		uint32_t target_width, target_height;
		comp_d3d11_target_get_dimensions(c->target, &target_width, &target_height);

		xrt_display_processor_d3d11_process_stereo(
		    c->display_processor, c->context, stereo_srv, view_width, view_height,
		    DXGI_FORMAT_R8G8B8A8_UNORM, target_width, target_height);
		weaving_done = true;
	}
#ifdef XRT_HAVE_LEIA_SR_D3D11
	// Fallback: direct SR weaver call if display processor not available
	else if (!is_mono && c->weaver != nullptr && leiasr_d3d11_is_ready(c->weaver)) {
		static bool fallback_dp_logged = false;
		if (!fallback_dp_logged) {
			U_LOG_W("D3D11 weaving via direct SR call (display processor unavailable)");
			fallback_dp_logged = true;
		}
		void *stereo_srv = comp_d3d11_renderer_get_stereo_srv(c->renderer);

		uint32_t view_width, view_height;
		comp_d3d11_renderer_get_view_dimensions(c->renderer, &view_width, &view_height);

		uint32_t target_width, target_height;
		comp_d3d11_target_get_dimensions(c->target, &target_width, &target_height);

		leiasr_d3d11_set_input_texture(c->weaver, stereo_srv, view_width, view_height,
		                                DXGI_FORMAT_R8G8B8A8_UNORM);

		D3D11_VIEWPORT viewport = {};
		viewport.TopLeftX = 0.0f;
		viewport.TopLeftY = 0.0f;
		viewport.Width = static_cast<float>(target_width);
		viewport.Height = static_cast<float>(target_height);
		viewport.MinDepth = 0.0f;
		viewport.MaxDepth = 1.0f;
		c->context->RSSetViewports(1, &viewport);

		leiasr_d3d11_weave(c->weaver);
		weaving_done = true;
	}
#endif

	// Fallback: if SR weaving is not available, blit the stereo texture directly to the target
	if (!weaving_done) {
		// Log once at startup that we're using fallback path
		static bool fallback_warned = false;
		if (!fallback_warned) {
			U_LOG_W("SR weaving not available, using fallback blit (mono=%d)", is_mono);
			fallback_warned = true;
		}

		// Get target back buffer directly (not from OMGetRenderTargets, which
		// returns the renderer's stereo RTV after comp_d3d11_renderer_draw)
		ID3D11Texture2D *back_buffer = static_cast<ID3D11Texture2D *>(
		    comp_d3d11_target_get_back_buffer(c->target));

		if (back_buffer != nullptr) {
			if (is_mono) {
				// MONO: GPU stretch blit from stereo texture to back buffer.
				// The stereo texture may be shorter than the window (when a
				// display processor is present, texture_height = view_height).
				// A pixel-copy would only fill the top portion; the stretch
				// blit renders a fullscreen quad that scales to fill the window.
				comp_d3d11_renderer_blit_stretch(c->renderer, back_buffer,
				                                 tgt_width, tgt_height);
			} else {
				// STEREO fallback: pixel-copy from stereo texture to back buffer
				ID3D11Texture2D *stereo_texture = static_cast<ID3D11Texture2D *>(
				    comp_d3d11_renderer_get_stereo_texture(c->renderer));
				if (stereo_texture != nullptr) {
					D3D11_TEXTURE2D_DESC src_desc, dst_desc;
					stereo_texture->GetDesc(&src_desc);
					back_buffer->GetDesc(&dst_desc);

					if (src_desc.Width == dst_desc.Width &&
					    src_desc.Height == dst_desc.Height) {
						c->context->CopyResource(back_buffer, stereo_texture);
					} else {
						UINT copy_width = (src_desc.Width < dst_desc.Width)
						                      ? src_desc.Width
						                      : dst_desc.Width;
						UINT copy_height = (src_desc.Height < dst_desc.Height)
						                       ? src_desc.Height
						                       : dst_desc.Height;
						D3D11_BOX src_box = {0, 0, 0, copy_width, copy_height, 1};
						c->context->CopySubresourceRegion(
						    back_buffer, 0, 0, 0, 0, stereo_texture, 0, &src_box);
					}
				}
			}
			// No Release needed: back_buffer is a borrowed pointer from the target
		}
	}

	// HUD overlay (post-weave, always readable)
	d3d11_render_hud_overlay(c, is_mono, weaving_done, &left_eye, &right_eye);

	// Present with VSync. sync_interval=1 ensures DWM composites the frame
	// at the same vsync boundary the weaver targeted, keeping the interlacing
	// pattern aligned with the lenticular display's phase-snapped position.
	// Present(0) was tried during drag but broke 3D: without vsync sync,
	// DWM composites the frame at a window position that differs from where
	// the weaver computed the interlacing, destroying the 3D effect.
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

	// Destroy HUD
	if (c->hud_texture != nullptr) {
		c->hud_texture->Release();
		c->hud_texture = nullptr;
	}
	u_hud_destroy(&c->hud);

	// Destroy display processor before underlying weaver
	xrt_display_processor_d3d11_destroy(&c->display_processor);

#ifdef XRT_HAVE_LEIA_SR_D3D11
	if (c->weaver != nullptr) {
		leiasr_d3d11_destroy(&c->weaver);
	}
#endif

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

	// Handle window: use provided HWND or create our own
	if (hwnd != nullptr) {
		// App provided window via XR_EXT_win32_window_binding
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

	// Create output target
	xrt_result_t xret = comp_d3d11_target_create(c, c->hwnd,
	                                              c->settings.preferred.width,
	                                              c->settings.preferred.height,
	                                              &c->target);
	if (xret != XRT_SUCCESS) {
		U_LOG_E("Failed to create D3D11 target");
		d3d11_compositor_destroy(&c->base.base);
		return xret;
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

	// Determine view dimensions for the stereo texture.
	// Default: derive from window size (half width for side-by-side)
	uint32_t view_width = c->settings.preferred.width / 2;
	uint32_t view_height = c->settings.preferred.height;

#ifdef XRT_HAVE_LEIA_SR_D3D11
	// Create SR weaver FIRST so we can query recommended dimensions.
	// The sr_cube_native app uses getRecommendedViewsTextureWidth/Height from the
	// SR display to size the stereo texture - we must do the same for correct weaving.
	U_LOG_W("Attempting to create SR D3D11 weaver (XRT_HAVE_LEIA_SR_D3D11 is defined)");
	xret = leiasr_d3d11_create(5.0, // 5 second timeout
	                           c->device,
	                           c->context,
	                           c->hwnd,
	                           view_width,
	                           view_height,
	                           &c->weaver);
	if (xret != XRT_SUCCESS) {
		U_LOG_W("Failed to create SR weaver (error %d), continuing without interlacing", (int)xret);
		c->weaver = nullptr;
	} else {
		U_LOG_W("SR D3D11 weaver created successfully");

		// Query SR recommended view dimensions - these must be used for the stereo
		// texture to match what the weaver expects (same as sr_cube_native app)
		uint32_t sr_width = 0, sr_height = 0;
		if (leiasr_d3d11_get_recommended_view_dimensions(c->weaver, &sr_width, &sr_height)) {
			U_LOG_W("Using SR recommended view dimensions: %ux%u per eye (was %ux%u from window)",
			        sr_width, sr_height, view_width, view_height);
			view_width = sr_width;
			view_height = sr_height;
		} else {
			U_LOG_W("Could not get SR recommended dimensions, using window-derived: %ux%u",
			        view_width, view_height);
		}

		// Wrap the SR D3D11 weaver in a generic display processor
		{
			xrt_result_t dp_ret = leia_display_processor_d3d11_create(
			    c->weaver, &c->display_processor);
			if (dp_ret != XRT_SUCCESS) {
				U_LOG_W("Failed to create D3D11 display processor, continuing without");
				c->display_processor = NULL;
			}
		}
	}
#else
	U_LOG_W("XRT_HAVE_LEIA_SR_D3D11 is NOT defined - SR weaving disabled at compile time");
#endif

	// sim_display takes priority over SR display processor (if enabled)
	{
		const char *sim_enable = getenv("SIM_DISPLAY_ENABLE");
		if (sim_enable != NULL && strcmp(sim_enable, "1") == 0) {
			// Parse SIM_DISPLAY_OUTPUT mode
			enum sim_display_output_mode mode = SIM_DISPLAY_OUTPUT_SBS;
			const char *mode_str = getenv("SIM_DISPLAY_OUTPUT");
			if (mode_str != NULL) {
				if (strcmp(mode_str, "anaglyph") == 0) {
					mode = SIM_DISPLAY_OUTPUT_ANAGLYPH;
				} else if (strcmp(mode_str, "blend") == 0) {
					mode = SIM_DISPLAY_OUTPUT_BLEND;
				}
			}

			// Replace any existing display processor with sim_display
			xrt_display_processor_d3d11_destroy(&c->display_processor);
			xrt_result_t dp_ret = sim_display_processor_d3d11_create(
			    mode, c->device, &c->display_processor);
			if (dp_ret != XRT_SUCCESS) {
				U_LOG_W("Failed to create sim display D3D11 processor, continuing without");
				c->display_processor = NULL;
			}
		}
	}

	// Create renderer with the correct view dimensions.
	// When a display processor (weaver) is present, the stereo texture height must match
	// view_height so the display processor's UV 0..1 maps exactly to the rendered content.
	// Without a display processor, use the window height so the stereo texture is tall enough
	// for mono fallback blitting.  Mono/2D mode uses a GPU stretch blit to fill the full
	// window regardless of stereo texture height.
	uint32_t target_height = (c->display_processor != NULL) ? view_height : c->settings.preferred.height;
	xret = comp_d3d11_renderer_create(c, view_width, view_height, target_height, &c->renderer);
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
		xret = comp_d3d11_debug_create(c->device, view_width * 2, view_height, &c->debug);
		if (xret != XRT_SUCCESS) {
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

extern "C" bool
comp_d3d11_compositor_get_predicted_eye_positions(struct xrt_compositor *xc,
                                                  struct xrt_vec3 *out_left_eye,
                                                  struct xrt_vec3 *out_right_eye)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);

	// Debug: log entry (throttled)
	static int call_count = 0;
	bool should_log = (++call_count % 60) == 1;

#ifdef XRT_HAVE_LEIA_SR_D3D11
	if (should_log) {
		U_LOG_D("D3D11 get_predicted_eye_positions: weaver=%p", (void *)c->weaver);
	}
	if (c->weaver != nullptr) {
		float left[3], right[3];
		if (leiasr_d3d11_get_predicted_eye_positions(c->weaver, left, right)) {
			out_left_eye->x = left[0];
			out_left_eye->y = left[1];
			out_left_eye->z = left[2];
			out_right_eye->x = right[0];
			out_right_eye->y = right[1];
			out_right_eye->z = right[2];
			if (should_log) {
				U_LOG_D("D3D11 eye positions from SR: left=(%.3f,%.3f,%.3f) right=(%.3f,%.3f,%.3f)",
				        left[0], left[1], left[2], right[0], right[1], right[2]);
			}
			return true;
		} else if (should_log) {
			U_LOG_W("D3D11 leiasr_d3d11_get_predicted_eye_positions failed!");
		}
	}
#else
	if (should_log) {
		U_LOG_D("D3D11 get_predicted_eye_positions: XRT_HAVE_LEIA_SR_D3D11 not defined");
	}
#endif

	// Default eye positions
	out_left_eye->x = -0.032f;
	out_left_eye->y = 0.0f;
	out_left_eye->z = 0.6f;
	out_right_eye->x = 0.032f;
	out_right_eye->y = 0.0f;
	out_right_eye->z = 0.6f;

	if (should_log) {
		U_LOG_D("D3D11 using default eye positions");
	}

	return false;
}

extern "C" bool
comp_d3d11_compositor_get_display_dimensions(struct xrt_compositor *xc,
                                              float *out_width_m,
                                              float *out_height_m)
{
	struct comp_d3d11_compositor *c = d3d11_comp(xc);

#ifdef XRT_HAVE_LEIA_SR_D3D11
	if (c->weaver != nullptr) {
		struct leiasr_display_dimensions dims;
		if (leiasr_d3d11_get_display_dimensions(c->weaver, &dims) && dims.valid) {
			*out_width_m = dims.width_m;
			*out_height_m = dims.height_m;
			return true;
		}
	}
#endif

	// Default display dimensions (typical laptop display size)
	*out_width_m = 0.3f;
	*out_height_m = 0.2f;

	return false;
}

extern "C" bool
comp_d3d11_compositor_get_window_metrics(struct xrt_compositor *xc,
                                          struct leiasr_window_metrics *out_metrics)
{
	if (xc == nullptr || out_metrics == nullptr) {
		if (out_metrics != nullptr) {
			out_metrics->valid = false;
		}
		return false;
	}

	struct comp_d3d11_compositor *c = d3d11_comp(xc);
	memset(out_metrics, 0, sizeof(*out_metrics));

#ifdef XRT_HAVE_LEIA_SR_D3D11
	if (c->weaver == nullptr || c->hwnd == nullptr) {
		return false;
	}

	// Get display pixel info from weaver
	uint32_t disp_px_w, disp_px_h;
	int32_t disp_left, disp_top;
	float disp_w_m, disp_h_m;
	if (!leiasr_d3d11_get_display_pixel_info(c->weaver, &disp_px_w, &disp_px_h,
	                                          &disp_left, &disp_top, &disp_w_m, &disp_h_m)) {
		return false;
	}

	if (disp_px_w == 0 || disp_px_h == 0) {
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

	return true;
#else
	return false;
#endif
}

extern "C" bool
comp_d3d11_compositor_request_display_mode(struct xrt_compositor *xc, bool enable_3d)
{
	if (xc == nullptr) {
		return false;
	}

	struct comp_d3d11_compositor *c = d3d11_comp(xc);

#ifdef XRT_HAVE_LEIA_SR_D3D11
	if (c->weaver != nullptr) {
		return leiasr_d3d11_request_display_mode(c->weaver, enable_3d);
	}
#endif

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
