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
#include "util/u_pacing.h"
#include "os/os_time.h"

#ifdef XRT_FEATURE_DEBUG_GUI
#include "util/u_debug_gui.h"
#include "comp_d3d11_debug.h"
#endif

#ifdef XRT_HAVE_LEIA_SR_D3D11
#include "leiasr/leiasr_d3d11.h"
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <mutex>

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

#ifdef XRT_FEATURE_DEBUG_GUI
	//! Debug GUI window.
	struct u_debug_gui *debug_gui;

	//! Debug readback module.
	struct comp_d3d11_debug *debug;
#endif

	//! Frame pacing helper.
	struct u_pacing_compositor *upc;

	//! Current frame ID.
	int64_t frame_id;

	//! Display refresh rate in Hz.
	float display_refresh_rate;

	//! Time of the last predicted display time.
	uint64_t last_display_time_ns;

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
	// Pass NULL for xinst and xsysd since the D3D11 compositor doesn't track them
	if (c->debug_gui != nullptr) {
		u_debug_gui_start(c->debug_gui, NULL, NULL);
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

	std::lock_guard<std::mutex> lock(c->mutex);

	c->frame_id++;

	*out_frame_id = c->frame_id;

	// Use queried display refresh rate
	int64_t now_ns = static_cast<int64_t>(os_monotonic_get_ns());
	int64_t period_ns = static_cast<int64_t>(U_TIME_1S_IN_NS / c->display_refresh_rate);

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

	// Check for window resize and handle it
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

	// Render layers to side-by-side stereo texture
	xrt_result_t xret = comp_d3d11_renderer_draw(c->renderer, &c->layer_accum, &left_eye, &right_eye);
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

#ifdef XRT_HAVE_LEIA_SR_D3D11
	if (c->weaver != nullptr && leiasr_d3d11_is_ready(c->weaver)) {
		// Get stereo texture SRV from renderer
		void *stereo_srv = comp_d3d11_renderer_get_stereo_srv(c->renderer);

		// Get view dimensions
		uint32_t view_width, view_height;
		comp_d3d11_renderer_get_view_dimensions(c->renderer, &view_width, &view_height);

		// Set input texture for weaving
		leiasr_d3d11_set_input_texture(c->weaver, stereo_srv, view_width, view_height,
		                                DXGI_FORMAT_R8G8B8A8_UNORM);

		// Get target dimensions for viewport
		uint32_t target_width, target_height;
		comp_d3d11_target_get_dimensions(c->target, &target_width, &target_height);

		// Set viewport for weaving
		D3D11_VIEWPORT viewport = {};
		viewport.TopLeftX = 0.0f;
		viewport.TopLeftY = 0.0f;
		viewport.Width = static_cast<float>(target_width);
		viewport.Height = static_cast<float>(target_height);
		viewport.MinDepth = 0.0f;
		viewport.MaxDepth = 1.0f;
		c->context->RSSetViewports(1, &viewport);

		// Perform weaving
		leiasr_d3d11_weave(c->weaver);
		weaving_done = true;
	}
#endif

	// Fallback: if SR weaving is not available, blit the stereo texture directly to the target
	if (!weaving_done) {
		// Log once at startup that we're using fallback path
		static bool fallback_warned = false;
		if (!fallback_warned) {
			U_LOG_W("SR weaving not available, using fallback stereo blit");
			fallback_warned = true;
		}

		// Get stereo texture from renderer
		ID3D11Texture2D *stereo_texture =
		    static_cast<ID3D11Texture2D *>(comp_d3d11_renderer_get_stereo_texture(c->renderer));

		// Get target back buffer
		ID3D11Texture2D *back_buffer = nullptr;
		ID3D11RenderTargetView *rtv = nullptr;
		c->context->OMGetRenderTargets(1, &rtv, nullptr);
		if (rtv != nullptr) {
			ID3D11Resource *resource = nullptr;
			rtv->GetResource(&resource);
			if (resource != nullptr) {
				resource->QueryInterface(__uuidof(ID3D11Texture2D),
				                          reinterpret_cast<void **>(&back_buffer));
				resource->Release();
			}
			rtv->Release();
		}

		if (back_buffer != nullptr && stereo_texture != nullptr) {
			// Get texture descriptions to check sizes
			D3D11_TEXTURE2D_DESC src_desc, dst_desc;
			stereo_texture->GetDesc(&src_desc);
			back_buffer->GetDesc(&dst_desc);

			if (src_desc.Width == dst_desc.Width && src_desc.Height == dst_desc.Height) {
				// Sizes match - use fast CopyResource
				c->context->CopyResource(back_buffer, stereo_texture);
			} else {
				// Sizes don't match - use CopySubresourceRegion to copy what fits
				UINT copy_width = (src_desc.Width < dst_desc.Width) ? src_desc.Width : dst_desc.Width;
				UINT copy_height =
				    (src_desc.Height < dst_desc.Height) ? src_desc.Height : dst_desc.Height;
				D3D11_BOX src_box = {0, 0, 0, copy_width, copy_height, 1};
				c->context->CopySubresourceRegion(back_buffer, 0, 0, 0, 0, stereo_texture, 0,
				                                   &src_box);
			}
			back_buffer->Release();
		}
	}

	// Present
	xret = comp_d3d11_target_present(c->target, 1); // VSync enabled
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

	if (c->upc != nullptr) {
		u_pc_destroy(&c->upc);
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
		// App provided window via XR_EXT_session_target
		c->hwnd = static_cast<HWND>(hwnd);
		U_LOG_I("Using app-provided window handle: %p", hwnd);
	} else {
		// No window provided - create our own
		U_LOG_I("No window handle provided, creating self-owned window");
		xrt_result_t xret = comp_d3d11_window_create(1920, 1080, &c->own_window);
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

	// Get DXGI factory
	IDXGIDevice *dxgi_device;
	HRESULT hr = c->device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgi_device));
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
	c->settings.preferred.width = 1920;  // Default resolution
	c->settings.preferred.height = 1080;
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

	// Create renderer
	// View size is half of target width for side-by-side stereo
	uint32_t view_width = c->settings.preferred.width / 2;
	uint32_t view_height = c->settings.preferred.height;

	xret = comp_d3d11_renderer_create(c, view_width, view_height, &c->renderer);
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

#ifdef XRT_HAVE_LEIA_SR_D3D11
	// Create SR weaver if available
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
	}
#else
	U_LOG_W("XRT_HAVE_LEIA_SR_D3D11 is NOT defined - SR weaving disabled at compile time");
#endif

	// Initialize layer accumulator - just zero it
	memset(&c->layer_accum, 0, sizeof(c->layer_accum));

	// Create frame pacing
	u_pc_display_timing_create(c->settings.nominal_frame_interval_ns, &U_PC_DISPLAY_TIMING_CONFIG_DEFAULT, &c->upc);

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
