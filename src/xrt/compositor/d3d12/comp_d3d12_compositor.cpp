// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Native D3D12 compositor implementation.
 * @author David Fattal
 * @ingroup comp_d3d12
 */

#include "comp_d3d12_compositor.h"
#include "comp_d3d12_swapchain.h"
#include "comp_d3d12_target.h"
#include "comp_d3d12_renderer.h"

#include "d3d11/comp_d3d11_window.h"

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
#include "xrt/xrt_display_processor_d3d12.h"

#include "math/m_api.h"
#include "util/u_tiling.h"

#ifdef XRT_BUILD_DRIVER_QWERTY
#include "qwerty_interface.h"
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <mutex>
#include <cmath>

/*!
 * Minimal settings struct for D3D12 compositor.
 */
struct comp_settings
{
	struct
	{
		uint32_t width;
		uint32_t height;
	} preferred;

	int64_t nominal_frame_interval_ns;
};

/*!
 * The D3D12 native compositor structure.
 */
struct comp_d3d12_compositor
{
	//! Base type - must be first!
	struct xrt_compositor_native base;

	//! The device we are rendering for.
	struct xrt_device *xdev;

	//! D3D12 device (from app's graphics binding, we add a reference).
	ID3D12Device *device;

	//! D3D12 command queue (from app's graphics binding, we add a reference).
	ID3D12CommandQueue *command_queue;

	//! Compositor's own command allocator.
	ID3D12CommandAllocator *cmd_allocator;

	//! Compositor's command list.
	ID3D12GraphicsCommandList *cmd_list;

	//! Fence for GPU synchronization.
	ID3D12Fence *fence;

	//! Current fence value.
	UINT64 fence_value;

	//! Fence event handle.
	HANDLE fence_event;

	//! Output target (DXGI swapchain).
	struct comp_d3d12_target *target;

	//! Renderer for layer compositing.
	struct comp_d3d12_renderer *renderer;

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

	//! Shared texture resource (opened from app-provided handle).
	ID3D12Resource *shared_texture;

	//! True if shared texture mode is active (offscreen rendering).
	bool has_shared_texture;

	//! D3D12 display processor.
	struct xrt_display_processor_d3d12 *display_processor;

	//! SRV descriptor heap for display processor.
	ID3D12DescriptorHeap *dp_srv_heap;

	//! System devices (for qwerty driver keyboard input).
	struct xrt_system_devices *xsysd;

	//! Current frame ID.
	int64_t frame_id;

	//! Display refresh rate in Hz.
	float display_refresh_rate;

	//! Time of the last predicted display time.
	uint64_t last_display_time_ns;

	//! True when display is in 3D mode (weaver active). False = 2D passthrough.
	bool hardware_display_3d;

	//! Last known 3D rendering mode index (for V-key toggle restore).
	uint32_t last_3d_mode_index;

	//! True when a legacy app is using a compromise view scale.
	bool legacy_app_tile_scaling;

	//! Thread safety.
	std::mutex mutex;
};

/*
 *
 * Helper functions
 *
 */

static inline struct comp_d3d12_compositor *
d3d12_comp(struct xrt_compositor *xc)
{
	return reinterpret_cast<struct comp_d3d12_compositor *>(xc);
}

/*!
 * Wait for GPU to finish all submitted work.
 */
static void
gpu_wait_idle(struct comp_d3d12_compositor *c)
{
	c->fence_value++;
	c->command_queue->Signal(c->fence, c->fence_value);

	if (c->fence->GetCompletedValue() < c->fence_value) {
		c->fence->SetEventOnCompletion(c->fence_value, c->fence_event);
		WaitForSingleObject(c->fence_event, INFINITE);
	}
}

/*
 *
 * xrt_compositor member functions
 *
 */

static xrt_result_t
d3d12_compositor_get_swapchain_create_properties(struct xrt_compositor *xc,
                                                  const struct xrt_swapchain_create_info *info,
                                                  struct xrt_swapchain_create_properties *xsccp)
{
	xsccp->image_count = 3;
	xsccp->extra_bits = (enum xrt_swapchain_usage_bits)0;
	return XRT_SUCCESS;
}

static xrt_result_t
d3d12_compositor_create_swapchain(struct xrt_compositor *xc,
                                   const struct xrt_swapchain_create_info *info,
                                   struct xrt_swapchain **out_xsc)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);
	return comp_d3d12_swapchain_create(c, info, out_xsc);
}

static xrt_result_t
d3d12_compositor_import_swapchain(struct xrt_compositor *xc,
                                   const struct xrt_swapchain_create_info *info,
                                   struct xrt_image_native *native_images,
                                   uint32_t image_count,
                                   struct xrt_swapchain **out_xsc)
{
	return XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED;
}

static xrt_result_t
d3d12_compositor_import_fence(struct xrt_compositor *xc,
                               xrt_graphics_sync_handle_t handle,
                               struct xrt_compositor_fence **out_xcf)
{
	return XRT_ERROR_FENCE_CREATE_FAILED;
}

static xrt_result_t
d3d12_compositor_create_semaphore(struct xrt_compositor *xc,
                                   xrt_graphics_sync_handle_t *out_handle,
                                   struct xrt_compositor_semaphore **out_xcsem)
{
	return XRT_ERROR_FENCE_CREATE_FAILED;
}

static xrt_result_t
d3d12_compositor_begin_session(struct xrt_compositor *xc, const struct xrt_begin_session_info *info)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);

	U_LOG_I("D3D12 compositor session begin");

	// Switch display to 3D mode
	if (c->display_processor != nullptr) {
		xrt_display_processor_d3d12_request_display_mode(c->display_processor, true);
	}

	return XRT_SUCCESS;
}

static xrt_result_t
d3d12_compositor_end_session(struct xrt_compositor *xc)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);

	U_LOG_I("D3D12 compositor session end");

	// Switch display back to 2D mode
	if (c->display_processor != nullptr) {
		xrt_display_processor_d3d12_request_display_mode(c->display_processor, false);
	}

	return XRT_SUCCESS;
}

static xrt_result_t
d3d12_compositor_predict_frame(struct xrt_compositor *xc,
                                int64_t *out_frame_id,
                                int64_t *out_wake_time_ns,
                                int64_t *out_predicted_gpu_time_ns,
                                int64_t *out_predicted_display_time_ns,
                                int64_t *out_predicted_display_period_ns)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);

	std::lock_guard<std::mutex> lock(c->mutex);

	c->frame_id++;
	*out_frame_id = c->frame_id;

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
d3d12_compositor_wait_frame(struct xrt_compositor *xc,
                             int64_t *out_frame_id,
                             int64_t *out_predicted_display_time_ns,
                             int64_t *out_predicted_display_period_ns)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);

	// Check if window was closed
	if (c->owns_window && c->own_window != nullptr &&
	    !comp_d3d11_window_is_valid(c->own_window)) {
		U_LOG_I("Window closed - signaling session exit");
		return XRT_ERROR_IPC_FAILURE;
	}

	int64_t period_ns = static_cast<int64_t>(U_TIME_1S_IN_NS / c->display_refresh_rate);

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
d3d12_compositor_mark_frame(struct xrt_compositor *xc,
                             int64_t frame_id,
                             enum xrt_compositor_frame_point point,
                             int64_t when_ns)
{
	return XRT_SUCCESS;
}

static xrt_result_t
d3d12_compositor_begin_frame(struct xrt_compositor *xc, int64_t frame_id)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);

	std::lock_guard<std::mutex> lock(c->mutex);

	// Check for window resize (skip in shared texture mode — no window or target)
	if (c->hwnd != nullptr && c->target != nullptr) {
		RECT rect;
		if (GetClientRect(c->hwnd, &rect)) {
			uint32_t new_width = static_cast<uint32_t>(rect.right - rect.left);
			uint32_t new_height = static_cast<uint32_t>(rect.bottom - rect.top);

			if (new_width > 0 && new_height > 0) {
				uint32_t current_width, current_height;
				comp_d3d12_target_get_dimensions(c->target, &current_width, &current_height);

				if (new_width != current_width || new_height != current_height) {
					U_LOG_I("Window resized: %ux%u -> %ux%u",
					        current_width, current_height, new_width, new_height);

					// Wait for GPU idle before resize
					gpu_wait_idle(c);

					xrt_result_t xret = comp_d3d12_target_resize(c->target, new_width, new_height);
					if (xret == XRT_SUCCESS) {
						c->settings.preferred.width = new_width;
						c->settings.preferred.height = new_height;
					}
				}
			}
		}
	}

	// Reset layer accumulator
	c->layer_accum.layer_count = 0;

	return XRT_SUCCESS;
}

static xrt_result_t
d3d12_compositor_discard_frame(struct xrt_compositor *xc, int64_t frame_id)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);

	std::lock_guard<std::mutex> lock(c->mutex);
	c->layer_accum.layer_count = 0;

	return XRT_SUCCESS;
}

static xrt_result_t
d3d12_compositor_layer_begin(struct xrt_compositor *xc, const struct xrt_layer_frame_data *data)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);
	std::lock_guard<std::mutex> lock(c->mutex);
	comp_layer_accum_begin(&c->layer_accum, data);
	return XRT_SUCCESS;
}

static xrt_result_t
d3d12_compositor_layer_projection(struct xrt_compositor *xc,
                                   struct xrt_device *xdev,
                                   struct xrt_swapchain *xsc[XRT_MAX_VIEWS],
                                   const struct xrt_layer_data *data)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);
	std::lock_guard<std::mutex> lock(c->mutex);
	comp_layer_accum_projection(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}

static xrt_result_t
d3d12_compositor_layer_projection_depth(struct xrt_compositor *xc,
                                         struct xrt_device *xdev,
                                         struct xrt_swapchain *xsc[XRT_MAX_VIEWS],
                                         struct xrt_swapchain *d_xsc[XRT_MAX_VIEWS],
                                         const struct xrt_layer_data *data)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);
	std::lock_guard<std::mutex> lock(c->mutex);
	comp_layer_accum_projection_depth(&c->layer_accum, xsc, d_xsc, data);
	return XRT_SUCCESS;
}

static xrt_result_t
d3d12_compositor_layer_quad(struct xrt_compositor *xc,
                             struct xrt_device *xdev,
                             struct xrt_swapchain *xsc,
                             const struct xrt_layer_data *data)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);
	std::lock_guard<std::mutex> lock(c->mutex);
	comp_layer_accum_quad(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}

static xrt_result_t
d3d12_compositor_layer_cube(struct xrt_compositor *xc,
                             struct xrt_device *xdev,
                             struct xrt_swapchain *xsc,
                             const struct xrt_layer_data *data)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);
	std::lock_guard<std::mutex> lock(c->mutex);
	comp_layer_accum_cube(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}

static xrt_result_t
d3d12_compositor_layer_cylinder(struct xrt_compositor *xc,
                                 struct xrt_device *xdev,
                                 struct xrt_swapchain *xsc,
                                 const struct xrt_layer_data *data)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);
	std::lock_guard<std::mutex> lock(c->mutex);
	comp_layer_accum_cylinder(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}

static xrt_result_t
d3d12_compositor_layer_equirect1(struct xrt_compositor *xc,
                                  struct xrt_device *xdev,
                                  struct xrt_swapchain *xsc,
                                  const struct xrt_layer_data *data)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);
	std::lock_guard<std::mutex> lock(c->mutex);
	comp_layer_accum_equirect1(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}

static xrt_result_t
d3d12_compositor_layer_equirect2(struct xrt_compositor *xc,
                                  struct xrt_device *xdev,
                                  struct xrt_swapchain *xsc,
                                  const struct xrt_layer_data *data)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);
	std::lock_guard<std::mutex> lock(c->mutex);
	comp_layer_accum_equirect2(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}

static xrt_result_t
d3d12_compositor_layer_passthrough(struct xrt_compositor *xc,
                                    struct xrt_device *xdev,
                                    const struct xrt_layer_data *data)
{
	return XRT_SUCCESS;
}

static xrt_result_t
d3d12_compositor_layer_window_space(struct xrt_compositor *xc,
                                     struct xrt_device *xdev,
                                     struct xrt_swapchain *xsc,
                                     const struct xrt_layer_data *data)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);
	std::lock_guard<std::mutex> lock(c->mutex);
	comp_layer_accum_window_space(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}

static xrt_result_t
d3d12_compositor_layer_commit(struct xrt_compositor *xc, xrt_graphics_sync_handle_t sync_handle)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);

	std::lock_guard<std::mutex> lock(c->mutex);

	// Get predicted eye positions
	struct xrt_vec3 left_eye = {-0.032f, 0.0f, 0.6f};
	struct xrt_vec3 right_eye = {0.032f, 0.0f, 0.6f};

	if (c->display_processor != nullptr) {
		struct xrt_eye_positions eyes;
		if (xrt_display_processor_d3d12_get_predicted_eye_positions(c->display_processor, &eyes) &&
		    eyes.valid) {
			left_eye.x = eyes.eyes[0].x;
			left_eye.y = eyes.eyes[0].y;
			left_eye.z = eyes.eyes[0].z;
			right_eye.x = eyes.eyes[1].x;
			right_eye.y = eyes.eyes[1].y;
			right_eye.z = eyes.eyes[1].z;
		}
	}

	// Sync hardware_display_3d and tile layout from device's active rendering mode
	if (c->xdev != NULL && c->xdev->hmd != NULL) {
		uint32_t idx = c->xdev->hmd->active_rendering_mode_index;
		if (idx < c->xdev->rendering_mode_count) {
			const struct xrt_rendering_mode *mode = &c->xdev->rendering_modes[idx];
			c->hardware_display_3d = mode->hardware_display_3d;
			if (mode->tile_columns > 0 && c->renderer != NULL) {
				comp_d3d12_renderer_set_tile_layout(
				    c->renderer, mode->tile_columns, mode->tile_rows);
			}
		}
	}

	// Diagnostic: log layer info for first 5 frames then every ~300 frames
	static uint32_t diag_counter = 0;
	bool diag_log = (diag_counter < 5 || diag_counter % 300 == 0);
	diag_counter++;
	if (diag_log) {
		U_LOG_I("D3D12 layer_commit: layers=%u, 3d=%d, dp=%p, target=%p",
		        c->layer_accum.layer_count, c->hardware_display_3d,
		        (void *)c->display_processor, (void *)c->target);
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
					uint32_t cur = head->hmd->active_rendering_mode_index;
					if (cur < head->rendering_mode_count &&
					    head->rendering_modes[cur].hardware_display_3d) {
						c->last_3d_mode_index = cur;
					}
					head->hmd->active_rendering_mode_index = 0;
				} else {
					head->hmd->active_rendering_mode_index = c->last_3d_mode_index;
				}
			}
			comp_d3d12_compositor_request_display_mode(&c->base.base, !force_2d);
		}

		// Rendering mode change from qwerty 1/2/3 keys (disabled for legacy apps).
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

	// Get target dimensions
	uint32_t tgt_width = c->settings.preferred.width;
	uint32_t tgt_height = c->settings.preferred.height;
	if (c->target != nullptr) {
		comp_d3d12_target_get_dimensions(c->target, &tgt_width, &tgt_height);
	}

	// Zero-copy check: can we pass the app's swapchain directly to the DP?
	bool zero_copy = false;
	void *zc_resource = nullptr;
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
						comp_d3d12_swapchain_get_dimensions(layer->sc_array[0], &sw, &sh);
						int32_t rxs[XRT_MAX_VIEWS], rys[XRT_MAX_VIEWS];
						uint32_t rws[XRT_MAX_VIEWS], rhs_arr[XRT_MAX_VIEWS];
						for (uint32_t v = 0; v < vc; v++) {
							rxs[v] = layer->data.proj.v[v].sub.rect.offset.w;
							rys[v] = layer->data.proj.v[v].sub.rect.offset.h;
							rws[v] = layer->data.proj.v[v].sub.rect.extent.w;
							rhs_arr[v] = layer->data.proj.v[v].sub.rect.extent.h;
						}
						if (u_tiling_can_zero_copy(vc, rxs, rys, rws, rhs_arr, sw, sh, mode)) {
							zc_resource = comp_d3d12_swapchain_get_resource(layer->sc_array[0], img_idx);
							if (zc_resource != nullptr)
								zero_copy = true;
						}
					}
				}
			}
		}
	}

	// Reset command allocator and command list
	c->cmd_allocator->Reset();
	c->cmd_list->Reset(c->cmd_allocator, nullptr);

	// Render layers to SBS stereo texture (skip if zero-copy)
	xrt_result_t xret = XRT_SUCCESS;
	if (!zero_copy) {
		xret = comp_d3d12_renderer_draw(
		    c->renderer, c->cmd_list, &c->layer_accum, &left_eye, &right_eye, tgt_width, tgt_height, c->hardware_display_3d);
		if (xret != XRT_SUCCESS) {
			U_LOG_E("Failed to render layers");
			return xret;
		}
	}

	// Shared texture mode: copy stereo output to shared texture and skip window present
	if (c->has_shared_texture && c->shared_texture != nullptr) {
		ID3D12Resource *atlas_resource = static_cast<ID3D12Resource *>(
		    comp_d3d12_renderer_get_atlas_resource(c->renderer));

		if (atlas_resource != nullptr) {
			// Barrier: shared texture to COPY_DEST, stereo to COPY_SOURCE
			D3D12_RESOURCE_BARRIER barriers[2] = {};
			barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barriers[0].Transition.pResource = c->shared_texture;
			barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
			barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
			barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

			barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barriers[1].Transition.pResource = atlas_resource;
			barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
			barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

			c->cmd_list->ResourceBarrier(2, barriers);

			c->cmd_list->CopyResource(c->shared_texture, atlas_resource);

			// Barrier: back to original states
			barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
			barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
			barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
			barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

			c->cmd_list->ResourceBarrier(2, barriers);
		}

		// Close and execute command list
		c->cmd_list->Close();
		ID3D12CommandList *lists[] = {c->cmd_list};
		c->command_queue->ExecuteCommandLists(1, lists);

		// Signal fence and wait for frame completion
		c->fence_value++;
		c->command_queue->Signal(c->fence, c->fence_value);
		if (c->fence->GetCompletedValue() < c->fence_value) {
			c->fence->SetEventOnCompletion(c->fence_value, c->fence_event);
			WaitForSingleObject(c->fence_event, INFINITE);
		}

		return XRT_SUCCESS;
	}

	// Display processor path: the D3D12 weaver renders to whatever render
	// target is bound on the command list. We bind the swapchain back buffer
	// as RT, call weave, then present.
	if (c->hardware_display_3d && c->display_processor != NULL && c->target != nullptr) {
		static bool dp_logged = false;
		if (!dp_logged) {
			U_LOG_W("D3D12 weaving via display processor (swapchain RT)");
			dp_logged = true;
		}

		// Execute stereo copy so the texture is ready for the weaver
		c->cmd_list->Close();
		ID3D12CommandList *copy_lists[] = {c->cmd_list};
		c->command_queue->ExecuteCommandLists(1, copy_lists);
		gpu_wait_idle(c);

		// Give the weaver a fresh command list
		c->cmd_allocator->Reset();
		c->cmd_list->Reset(c->cmd_allocator, nullptr);

		// Get swapchain back buffer and bind as render target
		uint32_t bb_index = comp_d3d12_target_get_current_index(c->target);
		ID3D12Resource *back_buffer = static_cast<ID3D12Resource *>(
		    comp_d3d12_target_get_back_buffer(c->target, bb_index));
		uint64_t rtv_handle_raw = comp_d3d12_target_get_rtv_handle(c->target, bb_index);
		D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle;
		rtv_handle.ptr = static_cast<SIZE_T>(rtv_handle_raw);

		// Transition back buffer: PRESENT → RENDER_TARGET
		D3D12_RESOURCE_BARRIER barrier = {};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = back_buffer;
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		c->cmd_list->ResourceBarrier(1, &barrier);

		// Bind back buffer as render target
		c->cmd_list->OMSetRenderTargets(1, &rtv_handle, FALSE, nullptr);

		uint32_t view_width, view_height;
		comp_d3d12_renderer_get_view_dimensions(c->renderer, &view_width, &view_height);
		ID3D12Resource *atlas_resource = zero_copy
		    ? static_cast<ID3D12Resource *>(zc_resource)
		    : static_cast<ID3D12Resource *>(comp_d3d12_renderer_get_atlas_resource(c->renderer));

		if (diag_log) {
			U_LOG_W("D3D12 dp path: stereo=%p, view=%ux%u, target=%ux%u, bb=%u, "
			        "back_buffer=%p, rtv=0x%llx",
			        (void *)atlas_resource, view_width, view_height,
			        tgt_width, tgt_height, bb_index,
			        (void *)back_buffer,
			        (unsigned long long)rtv_handle.ptr);
		}

		if (atlas_resource != nullptr) {
			// Transition stereo texture: PIXEL_SHADER_RESOURCE → COMMON
			// The SR D3D12 weaver expects input in COMMON state
			D3D12_RESOURCE_BARRIER atlas_barrier = {};
			atlas_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			atlas_barrier.Transition.pResource = atlas_resource;
			atlas_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			atlas_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
			atlas_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			c->cmd_list->ResourceBarrier(1, &atlas_barrier);

			// Set viewport and scissor on the command list (required by reference impl)
			D3D12_VIEWPORT viewport = {};
			viewport.TopLeftX = 0.0f;
			viewport.TopLeftY = 0.0f;
			viewport.Width = static_cast<float>(tgt_width);
			viewport.Height = static_cast<float>(tgt_height);
			viewport.MinDepth = 0.0f;
			viewport.MaxDepth = 1.0f;
			c->cmd_list->RSSetViewports(1, &viewport);

			D3D12_RECT scissor = {};
			scissor.left = 0;
			scissor.top = 0;
			scissor.right = static_cast<LONG>(tgt_width);
			scissor.bottom = static_cast<LONG>(tgt_height);
			c->cmd_list->RSSetScissorRects(1, &scissor);

			// Call display processor to weave
			uint32_t tile_columns, tile_rows;
			comp_d3d12_renderer_get_tile_layout(c->renderer, &tile_columns, &tile_rows);

			xrt_display_processor_d3d12_process_atlas(
			    c->display_processor,
			    c->cmd_list,
			    atlas_resource,
			    0,  // SRV GPU handle — SR weaver uses setInputViewTexture instead
			    rtv_handle.ptr,
			    view_width, view_height,
			    tile_columns, tile_rows,
			    static_cast<uint32_t>(DXGI_FORMAT_R8G8B8A8_UNORM),
			    tgt_width, tgt_height);

			// Transition stereo back: COMMON → PIXEL_SHADER_RESOURCE
			atlas_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
			atlas_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			c->cmd_list->ResourceBarrier(1, &atlas_barrier);

			// Transition back buffer: RENDER_TARGET → PRESENT
			barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
			barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
			c->cmd_list->ResourceBarrier(1, &barrier);
		} else {
			// No stereo resource — just transition back buffer to PRESENT
			barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
			barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
			c->cmd_list->ResourceBarrier(1, &barrier);
		}

		// Close and execute
		c->cmd_list->Close();
		ID3D12CommandList *weave_lists[] = {c->cmd_list};
		c->command_queue->ExecuteCommandLists(1, weave_lists);

		// Present with VSync
		comp_d3d12_target_present(c->target, 1);

		// Wait for frame completion (frame pacing)
		gpu_wait_idle(c);

		return XRT_SUCCESS;
	}

	// 2D fallback path: display processor exists but hardware is in 2D mode.
	// Stretch-blit the content region of the atlas to the back buffer.
	if (!c->hardware_display_3d && c->display_processor != NULL && c->target != nullptr) {
		uint32_t bb_index = comp_d3d12_target_get_current_index(c->target);
		ID3D12Resource *back_buffer = static_cast<ID3D12Resource *>(
		    comp_d3d12_target_get_back_buffer(c->target, bb_index));

		if (back_buffer != nullptr) {
			static bool fallback_2d_logged = false;
			if (!fallback_2d_logged) {
				U_LOG_W("D3D12 2D mode: stretch-blit atlas content to back buffer");
				fallback_2d_logged = true;
			}

			ID3D12Resource *atlas_resource = static_cast<ID3D12Resource *>(
			    comp_d3d12_renderer_get_atlas_resource(c->renderer));

			if (atlas_resource != nullptr) {
				uint32_t view_width, view_height;
				comp_d3d12_renderer_get_view_dimensions(c->renderer, &view_width, &view_height);
				uint32_t tile_columns, tile_rows;
				comp_d3d12_renderer_get_tile_layout(c->renderer, &tile_columns, &tile_rows);
				uint32_t content_w = tile_columns * view_width;
				uint32_t content_h = tile_rows * view_height;

				// Barriers: back buffer PRESENT → COPY_DEST, atlas PSR → COPY_SOURCE
				D3D12_RESOURCE_BARRIER barriers[2] = {};
				barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				barriers[0].Transition.pResource = back_buffer;
				barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
				barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
				barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

				barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				barriers[1].Transition.pResource = atlas_resource;
				barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
				barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
				barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

				c->cmd_list->ResourceBarrier(2, barriers);

				// Copy content region from atlas to back buffer (stretch via CopyTextureRegion)
				D3D12_TEXTURE_COPY_LOCATION src_loc = {};
				src_loc.pResource = atlas_resource;
				src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
				src_loc.SubresourceIndex = 0;

				D3D12_TEXTURE_COPY_LOCATION dst_loc = {};
				dst_loc.pResource = back_buffer;
				dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
				dst_loc.SubresourceIndex = 0;

				D3D12_BOX src_box = {};
				src_box.left = 0;
				src_box.top = 0;
				src_box.front = 0;
				src_box.right = content_w;
				src_box.bottom = content_h;
				src_box.back = 1;

				c->cmd_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, &src_box);

				// Barriers: back to original states
				barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
				barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
				barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
				barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

				c->cmd_list->ResourceBarrier(2, barriers);
			}
		}

		// Close and execute command list
		c->cmd_list->Close();
		ID3D12CommandList *lists[] = {c->cmd_list};
		c->command_queue->ExecuteCommandLists(1, lists);

		// Present with VSync
		comp_d3d12_target_present(c->target, 1);

		// Signal fence and wait for frame completion
		c->fence_value++;
		c->command_queue->Signal(c->fence, c->fence_value);
		if (c->fence->GetCompletedValue() < c->fence_value) {
			c->fence->SetEventOnCompletion(c->fence_value, c->fence_event);
			WaitForSingleObject(c->fence_event, INFINITE);
		}

		return XRT_SUCCESS;
	}

	// Target path (no display processor, or mono fallback)
	if (c->target != nullptr) {
		uint32_t bb_index = comp_d3d12_target_get_current_index(c->target);
		ID3D12Resource *back_buffer = static_cast<ID3D12Resource *>(
		    comp_d3d12_target_get_back_buffer(c->target, bb_index));

		if (back_buffer != nullptr) {
			static bool fallback_warned = false;
			if (!fallback_warned) {
				U_LOG_W("Display processing not available, using fallback copy (3d=%d)", c->hardware_display_3d);
				fallback_warned = true;
			}

			ID3D12Resource *atlas_resource = static_cast<ID3D12Resource *>(
			    comp_d3d12_renderer_get_atlas_resource(c->renderer));

			if (atlas_resource != nullptr) {
				// Barrier: back buffer PRESENT -> COPY_DEST
				D3D12_RESOURCE_BARRIER barriers[2] = {};
				barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				barriers[0].Transition.pResource = back_buffer;
				barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
				barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
				barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

				barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				barriers[1].Transition.pResource = atlas_resource;
				barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
				barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
				barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

				c->cmd_list->ResourceBarrier(2, barriers);

				c->cmd_list->CopyResource(back_buffer, atlas_resource);

				// Barrier: back to original states
				barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
				barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
				barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
				barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

				c->cmd_list->ResourceBarrier(2, barriers);
			}
		}

		// Close and execute command list
		c->cmd_list->Close();
		ID3D12CommandList *lists[] = {c->cmd_list};
		c->command_queue->ExecuteCommandLists(1, lists);

		// Present with VSync
		xret = comp_d3d12_target_present(c->target, 1);

		// Signal WM_PAINT for modal drag loop
		if (c->owns_window && c->own_window != nullptr) {
			comp_d3d11_window_signal_paint_done(c->own_window);
		}

		// Signal fence and wait for frame completion (frame pacing)
		c->fence_value++;
		c->command_queue->Signal(c->fence, c->fence_value);
		if (c->fence->GetCompletedValue() < c->fence_value) {
			c->fence->SetEventOnCompletion(c->fence_value, c->fence_event);
			WaitForSingleObject(c->fence_event, INFINITE);
		}

		if (xret != XRT_SUCCESS) {
			U_LOG_E("Failed to present");
			return xret;
		}
	}

	return XRT_SUCCESS;
}

static xrt_result_t
d3d12_compositor_layer_commit_with_semaphore(struct xrt_compositor *xc,
                                              struct xrt_compositor_semaphore *xcsem,
                                              uint64_t value)
{
	return d3d12_compositor_layer_commit(xc, XRT_GRAPHICS_SYNC_HANDLE_INVALID);
}

static void
d3d12_compositor_destroy(struct xrt_compositor *xc)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);

	U_LOG_I("Destroying D3D12 compositor");

	// Wait for GPU idle
	if (c->fence != nullptr && c->command_queue != nullptr) {
		gpu_wait_idle(c);
	}

	// Destroy display processor
	xrt_display_processor_d3d12_destroy(&c->display_processor);

	if (c->dp_srv_heap != nullptr) {
		c->dp_srv_heap->Release();
	}

	if (c->shared_texture != nullptr) {
		c->shared_texture->Release();
		c->shared_texture = nullptr;
	}

	if (c->renderer != nullptr) {
		comp_d3d12_renderer_destroy(&c->renderer);
	}

	if (c->target != nullptr) {
		comp_d3d12_target_destroy(&c->target);
	}

	if (c->fence_event != nullptr) {
		CloseHandle(c->fence_event);
	}
	if (c->fence != nullptr) {
		c->fence->Release();
	}
	if (c->cmd_list != nullptr) {
		c->cmd_list->Release();
	}
	if (c->cmd_allocator != nullptr) {
		c->cmd_allocator->Release();
	}

	if (c->command_queue != nullptr) {
		c->command_queue->Release();
	}
	if (c->device != nullptr) {
		c->device->Release();
	}

	// Destroy self-created window
	if (c->owns_window && c->own_window != nullptr) {
		comp_d3d11_window_destroy(&c->own_window);
	}

	delete c;
}

/*
 *
 * Exported functions
 *
 */

extern "C" xrt_result_t
comp_d3d12_compositor_create(struct xrt_device *xdev,
                             void *hwnd,
                             void *shared_texture_handle,
                             void *d3d12_device,
                             void *d3d12_command_queue,
                             void *dp_factory_d3d12,
                             struct xrt_compositor_native **out_xc)
{
	if (d3d12_device == nullptr) {
		U_LOG_E("D3D12 device is null");
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	if (d3d12_command_queue == nullptr) {
		U_LOG_E("D3D12 command queue is null");
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	U_LOG_I("Creating D3D12 native compositor");

	comp_d3d12_compositor *c = new comp_d3d12_compositor();
	memset(&c->base, 0, sizeof(c->base));

	c->xdev = xdev;
	c->own_window = nullptr;
	c->owns_window = false;
	c->hardware_display_3d = true;
	c->last_3d_mode_index = 1;

	// Handle window
	if (hwnd != nullptr) {
		c->hwnd = static_cast<HWND>(hwnd);
		U_LOG_I("Using app-provided window handle: %p", hwnd);
	} else if (shared_texture_handle != nullptr) {
		// Offscreen shared texture mode — no window needed
		c->hwnd = nullptr;
		U_LOG_I("Shared texture mode (offscreen) — no window");
	} else {
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

	// Get D3D12 device and command queue
	c->device = static_cast<ID3D12Device *>(d3d12_device);
	c->device->AddRef();

	c->command_queue = static_cast<ID3D12CommandQueue *>(d3d12_command_queue);
	c->command_queue->AddRef();

	// Create command allocator and command list
	HRESULT hr = c->device->CreateCommandAllocator(
	    D3D12_COMMAND_LIST_TYPE_DIRECT,
	    __uuidof(ID3D12CommandAllocator),
	    reinterpret_cast<void **>(&c->cmd_allocator));
	if (FAILED(hr)) {
		U_LOG_E("Failed to create command allocator: 0x%08x", hr);
		d3d12_compositor_destroy(&c->base.base);
		return XRT_ERROR_D3D;
	}

	hr = c->device->CreateCommandList(
	    0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->cmd_allocator, nullptr,
	    __uuidof(ID3D12GraphicsCommandList),
	    reinterpret_cast<void **>(&c->cmd_list));
	if (FAILED(hr)) {
		U_LOG_E("Failed to create command list: 0x%08x", hr);
		d3d12_compositor_destroy(&c->base.base);
		return XRT_ERROR_D3D;
	}
	// Command list is created in recording state, close it
	c->cmd_list->Close();

	// Create fence
	hr = c->device->CreateFence(
	    0, D3D12_FENCE_FLAG_NONE,
	    __uuidof(ID3D12Fence),
	    reinterpret_cast<void **>(&c->fence));
	if (FAILED(hr)) {
		U_LOG_E("Failed to create fence: 0x%08x", hr);
		d3d12_compositor_destroy(&c->base.base);
		return XRT_ERROR_D3D;
	}
	c->fence_value = 0;
	c->fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);

	// Open shared texture if handle provided
	c->shared_texture = nullptr;
	c->has_shared_texture = false;
	if (shared_texture_handle != nullptr) {
		HANDLE st_handle = static_cast<HANDLE>(shared_texture_handle);
		hr = c->device->OpenSharedHandle(
		    st_handle, __uuidof(ID3D12Resource),
		    reinterpret_cast<void **>(&c->shared_texture));
		if (FAILED(hr)) {
			U_LOG_E("Failed to open shared texture handle: 0x%08x", hr);
			d3d12_compositor_destroy(&c->base.base);
			return XRT_ERROR_D3D;
		}
		c->has_shared_texture = true;

		// Query shared texture dimensions
		D3D12_RESOURCE_DESC st_desc = c->shared_texture->GetDesc();
		U_LOG_W("Opened shared texture handle: %p -> resource %p (%llux%llu)",
		        shared_texture_handle, (void *)c->shared_texture,
		        (unsigned long long)st_desc.Width, (unsigned long long)st_desc.Height);
	}

	// Initialize settings
	memset(&c->settings, 0, sizeof(c->settings));
	c->settings.preferred.width = xdev->hmd->screens[0].w_pixels;
	c->settings.preferred.height = xdev->hmd->screens[0].h_pixels;
	if (c->settings.preferred.width == 0 || c->settings.preferred.height == 0) {
		c->settings.preferred.width = 1920;
		c->settings.preferred.height = 1080;
	}
	c->settings.nominal_frame_interval_ns = xdev->hmd->screens[0].nominal_frame_interval_ns;
	if (c->settings.nominal_frame_interval_ns == 0) {
		c->settings.nominal_frame_interval_ns = (1000 * 1000 * 1000) / 60;
	}

	// Get actual dimensions — from window or shared texture
	if (c->has_shared_texture && c->shared_texture != nullptr) {
		D3D12_RESOURCE_DESC st_desc = c->shared_texture->GetDesc();
		c->settings.preferred.width = static_cast<uint32_t>(st_desc.Width);
		c->settings.preferred.height = static_cast<uint32_t>(st_desc.Height);
	} else if (c->hwnd != nullptr) {
		RECT rect;
		if (GetClientRect(c->hwnd, &rect)) {
			c->settings.preferred.width = rect.right - rect.left;
			c->settings.preferred.height = rect.bottom - rect.top;
		}
	}

	// Create output target (DXGI swapchain).
	// The D3D12 weaver renders to whatever render target is bound on the
	// command list — it does NOT create its own swapchain. So we always
	// need a swapchain when we have a window, even with a display processor.
	// Skip only for shared texture offscreen mode (no window to present to).
	xrt_result_t xret;
	if (c->has_shared_texture && c->hwnd == nullptr) {
		c->target = nullptr;
		U_LOG_I("Skipping DXGI swapchain (shared texture offscreen mode, no window)");
	} else if (c->hwnd != nullptr) {
		xret = comp_d3d12_target_create(c, c->hwnd,
		                                              c->settings.preferred.width,
		                                              c->settings.preferred.height,
		                                              &c->target);
		if (xret != XRT_SUCCESS) {
			U_LOG_E("Failed to create D3D12 target");
			d3d12_compositor_destroy(&c->base.base);
			return xret;
		}
	} else {
		c->target = nullptr;
		U_LOG_I("No window — skipping DXGI swapchain");
	}

	// Query display refresh rate
	c->display_refresh_rate = 60.0f;

	// Determine view dimensions
	uint32_t view_width = c->settings.preferred.width / 2;
	uint32_t view_height = c->settings.preferred.height;

	// Create display processor via factory
	if (dp_factory_d3d12 != NULL) {
		auto factory = (xrt_dp_factory_d3d12_fn_t)dp_factory_d3d12;
		xrt_result_t dp_ret = factory(c->device, c->command_queue, c->hwnd, &c->display_processor);
		if (dp_ret != XRT_SUCCESS) {
			U_LOG_W("D3D12 display processor factory failed (error %d), continuing without", (int)dp_ret);
			c->display_processor = nullptr;
		} else {
			U_LOG_W("D3D12 display processor created via factory");

			// Tell the weaver the output render target format so it can
			// create its internal pipeline state. Without this, the weaver's
			// pipeline state stays null and weave() silently no-ops.
			xrt_display_processor_d3d12_set_output_format(
			    c->display_processor,
			    DXGI_FORMAT_R8G8B8A8_UNORM);
			U_LOG_W("D3D12 display processor: output format set to RGBA8_UNORM (target=%p)",
			        (void *)c->target);
		}
	} else {
		U_LOG_W("No D3D12 display processor factory provided");
	}

	// If display processor is available, query display pixel info to compute
	// optimal view dimensions (scaled to window size, matching D3D11 model).
	// Do NOT resize the app's window — _ext apps own their window.
	if (c->display_processor != nullptr) {
		uint32_t disp_px_w = 0, disp_px_h = 0;
		int32_t disp_left = 0, disp_top = 0;
		if (xrt_display_processor_d3d12_get_display_pixel_info(
		        c->display_processor, &disp_px_w, &disp_px_h, &disp_left, &disp_top) &&
		    disp_px_w > 0 && disp_px_h > 0) {
			// Use half display width as base view dims
			uint32_t base_vw = disp_px_w / 2;
			uint32_t base_vh = disp_px_h;

			U_LOG_W("Display pixel info: %ux%u, base view dims: %ux%u per eye",
			        disp_px_w, disp_px_h, base_vw, base_vh);

			// Scale by window/display pixel ratio (same as D3D11 resize path)
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

	// Create SRV descriptor heap for display processor (shader-visible, reuses renderer's SRV)
	{
		D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
		heap_desc.NumDescriptors = 1;
		heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		hr = c->device->CreateDescriptorHeap(
		    &heap_desc, __uuidof(ID3D12DescriptorHeap),
		    reinterpret_cast<void **>(&c->dp_srv_heap));
		if (FAILED(hr)) {
			U_LOG_W("Failed to create DP SRV heap: 0x%08x", hr);
		}
	}

	// Create renderer
	uint32_t target_height = (c->display_processor != NULL) ? view_height : c->settings.preferred.height;
	xret = comp_d3d12_renderer_create(c, view_width, view_height, target_height, &c->renderer);
	if (xret != XRT_SUCCESS) {
		U_LOG_E("Failed to create D3D12 renderer");
		d3d12_compositor_destroy(&c->base.base);
		return xret;
	}

	// Initialize layer accumulator
	memset(&c->layer_accum, 0, sizeof(c->layer_accum));

	// Populate supported swapchain formats
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

	c->base.base.info.initial_visible = true;
	c->base.base.info.initial_focused = true;

	// Set up compositor interface
	c->base.base.get_swapchain_create_properties = d3d12_compositor_get_swapchain_create_properties;
	c->base.base.create_swapchain = d3d12_compositor_create_swapchain;
	c->base.base.import_swapchain = d3d12_compositor_import_swapchain;
	c->base.base.import_fence = d3d12_compositor_import_fence;
	c->base.base.create_semaphore = d3d12_compositor_create_semaphore;
	c->base.base.begin_session = d3d12_compositor_begin_session;
	c->base.base.end_session = d3d12_compositor_end_session;
	c->base.base.wait_frame = d3d12_compositor_wait_frame;
	c->base.base.predict_frame = d3d12_compositor_predict_frame;
	c->base.base.mark_frame = d3d12_compositor_mark_frame;
	c->base.base.begin_frame = d3d12_compositor_begin_frame;
	c->base.base.discard_frame = d3d12_compositor_discard_frame;
	c->base.base.layer_begin = d3d12_compositor_layer_begin;
	c->base.base.layer_projection = d3d12_compositor_layer_projection;
	c->base.base.layer_projection_depth = d3d12_compositor_layer_projection_depth;
	c->base.base.layer_quad = d3d12_compositor_layer_quad;
	c->base.base.layer_cube = d3d12_compositor_layer_cube;
	c->base.base.layer_cylinder = d3d12_compositor_layer_cylinder;
	c->base.base.layer_equirect1 = d3d12_compositor_layer_equirect1;
	c->base.base.layer_equirect2 = d3d12_compositor_layer_equirect2;
	c->base.base.layer_passthrough = d3d12_compositor_layer_passthrough;
	c->base.base.layer_window_space = d3d12_compositor_layer_window_space;
	c->base.base.layer_commit = d3d12_compositor_layer_commit;
	c->base.base.layer_commit_with_semaphore = d3d12_compositor_layer_commit_with_semaphore;
	c->base.base.destroy = d3d12_compositor_destroy;

	*out_xc = &c->base;

	U_LOG_IFL_I(U_LOGGING_INFO, "D3D12 native compositor created successfully (%ux%u)",
	            c->settings.preferred.width, c->settings.preferred.height);

	return XRT_SUCCESS;
}

extern "C" bool
comp_d3d12_compositor_get_predicted_eye_positions(struct xrt_compositor *xc,
                                                  struct xrt_eye_positions *out_eye_pos)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);

	if (c->display_processor != nullptr) {
		if (xrt_display_processor_d3d12_get_predicted_eye_positions(c->display_processor, out_eye_pos) &&
		    out_eye_pos->valid) {
			return true;
		}
	}

	return false;
}

extern "C" bool
comp_d3d12_compositor_get_display_dimensions(struct xrt_compositor *xc,
                                              float *out_width_m,
                                              float *out_height_m)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);

	return xrt_display_processor_d3d12_get_display_dimensions(
	    c->display_processor, out_width_m, out_height_m);
}

extern "C" bool
comp_d3d12_compositor_get_window_metrics(struct xrt_compositor *xc,
                                          struct xrt_window_metrics *out_metrics)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);

	return xrt_display_processor_d3d12_get_window_metrics(c->display_processor, out_metrics);
}

extern "C" bool
comp_d3d12_compositor_request_display_mode(struct xrt_compositor *xc, bool enable_3d)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);

	return xrt_display_processor_d3d12_request_display_mode(c->display_processor, enable_3d);
}

extern "C" void
comp_d3d12_compositor_set_system_devices(struct xrt_compositor *xc,
                                          struct xrt_system_devices *xsysd)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);
	c->xsysd = xsysd;
}

void
comp_d3d12_compositor_set_legacy_app_tile_scaling(struct xrt_compositor *xc, bool legacy)
{
	if (xc == nullptr) {
		return;
	}
	struct comp_d3d12_compositor *c = d3d12_comp(xc);
	c->legacy_app_tile_scaling = legacy;
}
