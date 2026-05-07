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
#include "util/u_canvas.h"
#include "util/u_capture_intent.h"
#include "util/u_hud.h"
#include <displayxr_mcp/mcp_capture.h>

// STB_IMAGE_WRITE_STATIC scopes all stbi_write_* to this TU.
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#ifdef XRT_BUILD_DRIVER_QWERTY
#include "qwerty_interface.h"
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <mutex>
#include <cmath>

// Forward declarations for chroma-key post-weave alpha-conversion pass (defined further down)
struct comp_d3d12_compositor;
static bool chroma_key_pass_execute(struct comp_d3d12_compositor *c,
                                     ID3D12GraphicsCommandList *cmd_list,
                                     ID3D12Resource *back_buffer,
                                     D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle,
                                     UINT width, UINT height);
static void chroma_key_destroy(struct comp_d3d12_compositor *c);

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
	//! NULL in shared texture mode — compositor doesn't own a swapchain.
	HWND hwnd;

	//! Optional chroma-key color (0x00BBGGRR / Win32 COLORREF) for the post-weave
	//! alpha-conversion shader pass. Zero disables the pass. Only meaningful when
	//! the compositor was created with transparent_background = true.
	uint32_t chroma_key_color;
	//! Lazy-initialized resources for the chroma-key shader pass; allocated on
	//! the first frame the pass runs. See chroma_key_pass_*() below.
	ID3D12RootSignature *ck_root_signature;
	ID3D12PipelineState *ck_pipeline_state;
	ID3D12Resource      *ck_intermediate;     //!< swap-chain-sized R8G8B8A8_UNORM, COPY_DEST/PIXEL_SHADER_RESOURCE
	ID3D12DescriptorHeap *ck_srv_heap;        //!< Single SRV slot for ck_intermediate
	UINT                 ck_intermediate_w;
	UINT                 ck_intermediate_h;
	D3D12_RESOURCE_STATES ck_intermediate_state;  //!< Current state of ck_intermediate

	//! App HWND for position tracking in shared texture mode.
	//! The display processor uses this for weaver alignment.
	HWND app_hwnd;

	//! Self-created window (NULL if app provided window).
	struct comp_d3d11_window *own_window;

	//! True if we created the window ourselves.
	bool owns_window;

	//! Shared texture resource (opened from app-provided handle).
	ID3D12Resource *shared_texture;

	//! RTV descriptor heap for shared texture (1 descriptor).
	ID3D12DescriptorHeap *shared_texture_rtv_heap;

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

	//! Compromise view scale for legacy apps. Only valid when legacy_app_tile_scaling is true.
	float legacy_view_scale_x;
	float legacy_view_scale_y;

	//! Canvas output rect for shared-texture apps.
	struct u_canvas_rect canvas;

	//! Lazily allocated intermediate resource for cropping atlas to content dims.
	ID3D12Resource *dp_input_resource;

	//! Cached dimensions for lazy reallocation.
	uint32_t dp_input_width, dp_input_height;

	//! HUD overlay.
	struct u_hud *hud;

	//! HUD texture (DEFAULT heap, for GPU copy source).
	ID3D12Resource *hud_texture;

	//! HUD upload buffer (UPLOAD heap, for CPU staging).
	ID3D12Resource *hud_upload_buffer;

	//! HUD upload buffer row pitch (aligned to D3D12_TEXTURE_DATA_PITCH_ALIGNMENT).
	uint32_t hud_upload_pitch;

	//! Whether HUD texture has been created.
	bool hud_initialized;

	//! Frame timing for HUD FPS display.
	uint64_t last_frame_time_ns;
	float smoothed_frame_time_ms;

	//! Thread safety.
	std::mutex mutex;

	//! MCP capture_frame request box (serviced at end of layer_commit).
	//! Mirrors the pattern in comp_metal/gl/d3d11_compositor. See issue #210.
	struct mcp_capture_request mcp_capture;

	//! Per-frame capture intent. See u_capture_intent.h.
	struct u_capture_intent capture_intent;
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

	// Check for window resize — resize immediately to keep backbuffer in sync.
	// The GPU is already idle here: layer_commit() calls gpu_wait_idle() at
	// the end of every frame, so no additional GPU drain is needed.
	// Immediate resize is critical for 3D displays: the weaver outputs
	// pixel-precise interlacing patterns, and any DXGI stretching (from a
	// backbuffer/window size mismatch) destroys the interlacing.
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

					// Resize child window first if fallback is active (no-op otherwise)
					comp_d3d12_target_resize_child_window(c->target, new_width, new_height);

					xrt_result_t xret =
					    comp_d3d12_target_resize(c->target, new_width, new_height);
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

/*!
 * Render the HUD overlay onto the back buffer (D3D12 version).
 *
 * The back buffer must be in D3D12_RESOURCE_STATE_COPY_DEST when this is called.
 */
static void
d3d12_render_hud_overlay(struct comp_d3d12_compositor *c,
                         ID3D12GraphicsCommandList *cmd_list,
                         ID3D12Resource *back_buffer,
                         uint32_t win_w, uint32_t win_h,
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
		comp_d3d12_renderer_get_view_dimensions(c->renderer, &render_w, &render_h);
	}

	// Get display physical dimensions from display processor
	float disp_w_m = 0.0f, disp_h_m = 0.0f;
	float nom_x = 0.0f, nom_y = 0.0f, nom_z = 600.0f;
	comp_d3d12_compositor_get_display_dimensions(&c->base.base, &disp_w_m, &disp_h_m);
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

	// Lazy-create HUD texture and upload buffer
	if (!c->hud_initialized) {
		uint32_t hud_w = u_hud_get_width(c->hud);
		uint32_t hud_h = u_hud_get_height(c->hud);

		// Aligned row pitch for D3D12 upload buffer
		uint32_t aligned_pitch = (hud_w * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
		                         ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
		c->hud_upload_pitch = aligned_pitch;

		// Create DEFAULT heap texture (GPU copy source)
		D3D12_RESOURCE_DESC tex_desc = {};
		tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		tex_desc.Width = hud_w;
		tex_desc.Height = hud_h;
		tex_desc.DepthOrArraySize = 1;
		tex_desc.MipLevels = 1;
		tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		tex_desc.SampleDesc.Count = 1;
		tex_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

		D3D12_HEAP_PROPERTIES default_heap = {};
		default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;

		HRESULT hr = c->device->CreateCommittedResource(
		    &default_heap, D3D12_HEAP_FLAG_NONE, &tex_desc,
		    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
		    __uuidof(ID3D12Resource),
		    reinterpret_cast<void **>(&c->hud_texture));
		if (FAILED(hr)) {
			U_LOG_E("Failed to create HUD texture: 0x%08x", hr);
			return;
		}

		// Create UPLOAD heap buffer for CPU staging
		D3D12_RESOURCE_DESC buf_desc = {};
		buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		buf_desc.Width = (uint64_t)aligned_pitch * hud_h;
		buf_desc.Height = 1;
		buf_desc.DepthOrArraySize = 1;
		buf_desc.MipLevels = 1;
		buf_desc.Format = DXGI_FORMAT_UNKNOWN;
		buf_desc.SampleDesc.Count = 1;
		buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

		D3D12_HEAP_PROPERTIES upload_heap = {};
		upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;

		hr = c->device->CreateCommittedResource(
		    &upload_heap, D3D12_HEAP_FLAG_NONE, &buf_desc,
		    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
		    __uuidof(ID3D12Resource),
		    reinterpret_cast<void **>(&c->hud_upload_buffer));
		if (FAILED(hr)) {
			U_LOG_E("Failed to create HUD upload buffer: 0x%08x", hr);
			c->hud_texture->Release();
			c->hud_texture = nullptr;
			return;
		}

		c->hud_initialized = true;
		dirty = true; // Force initial upload
	}

	// Upload pixels to upload buffer if changed
	if (dirty && c->hud_texture != nullptr && c->hud_upload_buffer != nullptr) {
		uint32_t hud_w = u_hud_get_width(c->hud);
		uint32_t hud_h = u_hud_get_height(c->hud);
		const uint8_t *pixels = u_hud_get_pixels(c->hud);

		// Map upload buffer and copy row by row with aligned pitch
		void *mapped = nullptr;
		D3D12_RANGE read_range = {0, 0}; // We won't read from this buffer
		HRESULT hr = c->hud_upload_buffer->Map(0, &read_range, &mapped);
		if (SUCCEEDED(hr)) {
			uint8_t *dst = static_cast<uint8_t *>(mapped);
			for (uint32_t row = 0; row < hud_h; row++) {
				memcpy(dst + row * c->hud_upload_pitch,
				       pixels + row * hud_w * 4,
				       hud_w * 4);
			}
			c->hud_upload_buffer->Unmap(0, nullptr);

			// Copy from upload buffer to hud_texture
			D3D12_TEXTURE_COPY_LOCATION src_loc = {};
			src_loc.pResource = c->hud_upload_buffer;
			src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			src_loc.PlacedFootprint.Offset = 0;
			src_loc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			src_loc.PlacedFootprint.Footprint.Width = hud_w;
			src_loc.PlacedFootprint.Footprint.Height = hud_h;
			src_loc.PlacedFootprint.Footprint.Depth = 1;
			src_loc.PlacedFootprint.Footprint.RowPitch = c->hud_upload_pitch;

			D3D12_TEXTURE_COPY_LOCATION dst_loc = {};
			dst_loc.pResource = c->hud_texture;
			dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			dst_loc.SubresourceIndex = 0;

			cmd_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);
		}
	}

	// Copy hud_texture to back buffer at bottom-left
	if (c->hud_texture != nullptr && back_buffer != nullptr) {
		uint32_t hud_w = u_hud_get_width(c->hud);
		uint32_t hud_h = u_hud_get_height(c->hud);

		// Transition hud_texture: COPY_DEST → COPY_SOURCE
		D3D12_RESOURCE_BARRIER hud_barrier = {};
		hud_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		hud_barrier.Transition.pResource = c->hud_texture;
		hud_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
		hud_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
		hud_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		cmd_list->ResourceBarrier(1, &hud_barrier);

		// Position at bottom-left with 10px margin
		uint32_t dst_x = 10;
		uint32_t dst_y = (win_h > hud_h + 10) ? (win_h - hud_h - 10) : 0;

		D3D12_TEXTURE_COPY_LOCATION src_loc = {};
		src_loc.pResource = c->hud_texture;
		src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		src_loc.SubresourceIndex = 0;

		D3D12_TEXTURE_COPY_LOCATION dst_loc = {};
		dst_loc.pResource = back_buffer;
		dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		dst_loc.SubresourceIndex = 0;

		D3D12_BOX src_box = {0, 0, 0, hud_w, hud_h, 1};
		cmd_list->CopyTextureRegion(&dst_loc, dst_x, dst_y, 0, &src_loc, &src_box);

		// Transition hud_texture back: COPY_SOURCE → COPY_DEST
		hud_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
		hud_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
		cmd_list->ResourceBarrier(1, &hud_barrier);
	}
}

/*!
 * Crop atlas to content dimensions before passing to display processor.
 * Called within an already-recording command list. The atlas is assumed to be
 * in COMMON state (already transitioned by the caller).
 *
 * Returns the resource to pass to process_atlas().
 */
static ID3D12Resource *
d3d12_crop_atlas_for_dp(struct comp_d3d12_compositor *c,
                        ID3D12Resource *atlas_resource,
                        uint32_t content_w,
                        uint32_t content_h)
{
	D3D12_RESOURCE_DESC atlas_desc = atlas_resource->GetDesc();

	if (content_w == (uint32_t)atlas_desc.Width && content_h == atlas_desc.Height) {
		return atlas_resource;
	}

	// Lazily (re)create intermediate resource at content dimensions
	if (c->dp_input_width != content_w || c->dp_input_height != content_h) {
		if (c->dp_input_resource != nullptr) {
			c->dp_input_resource->Release();
			c->dp_input_resource = nullptr;
		}

		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = content_w;
		desc.Height = content_h;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = atlas_desc.Format;
		desc.SampleDesc.Count = 1;
		desc.Flags = D3D12_RESOURCE_FLAG_NONE;

		D3D12_HEAP_PROPERTIES heap = {};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;

		HRESULT hr = c->device->CreateCommittedResource(
		    &heap, D3D12_HEAP_FLAG_NONE, &desc,
		    D3D12_RESOURCE_STATE_COMMON, nullptr,
		    IID_PPV_ARGS(&c->dp_input_resource));
		if (FAILED(hr)) {
			U_LOG_E("Failed to create D3D12 DP input resource %ux%u: 0x%lx",
			        content_w, content_h, hr);
			return atlas_resource;
		}

		c->dp_input_width = content_w;
		c->dp_input_height = content_h;
		U_LOG_I("D3D12 crop: created DP input resource %ux%u (atlas %llux%u)",
		        content_w, content_h,
		        (unsigned long long)atlas_desc.Width, (unsigned)atlas_desc.Height);
	}

	// Transition intermediate: COMMON → COPY_DEST
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = c->dp_input_resource;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	c->cmd_list->ResourceBarrier(1, &barrier);

	// Copy content region from atlas to intermediate
	D3D12_TEXTURE_COPY_LOCATION dst_loc = {};
	dst_loc.pResource = c->dp_input_resource;
	dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	dst_loc.SubresourceIndex = 0;

	D3D12_TEXTURE_COPY_LOCATION src_loc = {};
	src_loc.pResource = atlas_resource;
	src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	src_loc.SubresourceIndex = 0;

	D3D12_BOX src_box = {0, 0, 0, content_w, content_h, 1};
	c->cmd_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, &src_box);

	// Transition intermediate: COPY_DEST → COMMON (for DP)
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
	c->cmd_list->ResourceBarrier(1, &barrier);

	return c->dp_input_resource;
}

/*
 *
 * MCP capture helpers
 *
 */

// Copy the content region of the renderer's atlas (tile_columns × view_width
// by tile_rows × view_height — what the app actually wrote, same region the
// compositor crops and sends to the DP) into a READBACK heap buffer, then
// write @p path as PNG. D3D12 renderer uses DXGI_FORMAT_R8G8B8A8_UNORM so no
// channel swap is needed.
//
// Caller must ensure the GPU is idle on entry (gpu_wait_idle has been called
// or the existing layer_commit fence-waits before returning). On exit the
// atlas is left in PIXEL_SHADER_RESOURCE state (matching the renderer's
// expected steady state between frames).
static bool
d3d12_compositor_capture_atlas_to_png(struct comp_d3d12_compositor *c, const char *path)
{
	ID3D12Resource *atlas = static_cast<ID3D12Resource *>(
	    comp_d3d12_renderer_get_atlas_resource(c->renderer));
	if (atlas == nullptr || c->renderer == nullptr) {
		return false;
	}

	uint32_t tile_columns = 1, tile_rows = 1;
	comp_d3d12_renderer_get_tile_layout(c->renderer, &tile_columns, &tile_rows);
	uint32_t view_w = 0, view_h = 0;
	comp_d3d12_renderer_get_view_dimensions(c->renderer, &view_w, &view_h);
	if (tile_columns == 0 || tile_rows == 0 || view_w == 0 || view_h == 0) {
		return false;
	}

	D3D12_RESOURCE_DESC adesc = atlas->GetDesc();
	uint32_t content_w = tile_columns * view_w;
	uint32_t content_h = tile_rows * view_h;
	if (content_w > adesc.Width)  content_w = (uint32_t)adesc.Width;
	if (content_h > adesc.Height) content_h = adesc.Height;

	// D3D12 readback row pitch must be aligned to 256.
	const UINT64 align = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
	UINT64 row_pitch = ((UINT64)content_w * 4 + align - 1) & ~(align - 1);
	UINT64 rb_bytes = row_pitch * content_h;

	// Allocate a transient READBACK buffer. Lifetime = single capture.
	D3D12_HEAP_PROPERTIES heap_props = {};
	heap_props.Type = D3D12_HEAP_TYPE_READBACK;
	D3D12_RESOURCE_DESC rb_desc = {};
	rb_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	rb_desc.Width = rb_bytes;
	rb_desc.Height = 1;
	rb_desc.DepthOrArraySize = 1;
	rb_desc.MipLevels = 1;
	rb_desc.Format = DXGI_FORMAT_UNKNOWN;
	rb_desc.SampleDesc.Count = 1;
	rb_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	rb_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

	ID3D12Resource *readback = nullptr;
	if (FAILED(c->device->CreateCommittedResource(
	        &heap_props, D3D12_HEAP_FLAG_NONE, &rb_desc,
	        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
	        IID_PPV_ARGS(&readback))) || readback == nullptr) {
		return false;
	}

	// Re-arm the cmd_allocator + cmd_list for our private use. GPU is
	// guaranteed idle at this point because layer_commit's existing
	// fence wait runs before we get here.
	c->cmd_allocator->Reset();
	c->cmd_list->Reset(c->cmd_allocator, nullptr);

	// Atlas: PIXEL_SHADER_RESOURCE → COPY_SOURCE.
	D3D12_RESOURCE_BARRIER b = {};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = atlas;
	b.Transition.Subresource = 0;
	b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	c->cmd_list->ResourceBarrier(1, &b);

	D3D12_TEXTURE_COPY_LOCATION src_loc = {};
	src_loc.pResource = atlas;
	src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	src_loc.SubresourceIndex = 0;

	D3D12_TEXTURE_COPY_LOCATION dst_loc = {};
	dst_loc.pResource = readback;
	dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	dst_loc.PlacedFootprint.Offset = 0;
	dst_loc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	dst_loc.PlacedFootprint.Footprint.Width = content_w;
	dst_loc.PlacedFootprint.Footprint.Height = content_h;
	dst_loc.PlacedFootprint.Footprint.Depth = 1;
	dst_loc.PlacedFootprint.Footprint.RowPitch = (UINT)row_pitch;

	D3D12_BOX src_box = {0, 0, 0, content_w, content_h, 1};
	c->cmd_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, &src_box);

	// Atlas: COPY_SOURCE → PIXEL_SHADER_RESOURCE (steady state).
	std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
	c->cmd_list->ResourceBarrier(1, &b);

	c->cmd_list->Close();
	ID3D12CommandList *lists[] = {c->cmd_list};
	c->command_queue->ExecuteCommandLists(1, lists);
	gpu_wait_idle(c);

	// Map readback, repack to tightly-packed rows, encode PNG.
	bool ok = false;
	void *mapped = nullptr;
	D3D12_RANGE read_range = {0, (SIZE_T)rb_bytes};
	if (SUCCEEDED(readback->Map(0, &read_range, &mapped)) && mapped != nullptr) {
		size_t tight_pitch = (size_t)content_w * 4;
		uint8_t *tight = (uint8_t *)malloc(tight_pitch * content_h);
		if (tight != nullptr) {
			const uint8_t *rb_pixels = (const uint8_t *)mapped;
			for (uint32_t y = 0; y < content_h; y++) {
				memcpy(tight + (size_t)y * tight_pitch,
				       rb_pixels + (size_t)y * row_pitch,
				       tight_pitch);
			}
			ok = stbi_write_png(path, (int)content_w, (int)content_h, 4,
			                    tight, (int)tight_pitch) != 0;
			free(tight);
		}
		D3D12_RANGE empty_range = {0, 0};
		readback->Unmap(0, &empty_range);
	}

	readback->Release();
	return ok;
}

// Run the capture readback if the per-frame intent matches @p mode_filter.
static void
d3d12_compositor_dispatch_capture(struct comp_d3d12_compositor *c, uint32_t mode_filter)
{
	if (!u_capture_intent_should_capture(&c->capture_intent, mode_filter)) {
		return;
	}
	bool ok = d3d12_compositor_capture_atlas_to_png(c, c->capture_intent.path);
	if (ok) {
		U_LOG_I("Atlas captured (mode=%u) to %s",
		        c->capture_intent.mode, c->capture_intent.path);
	} else {
		U_LOG_W("Atlas capture failed (mode=%u path=%s)",
		        c->capture_intent.mode, c->capture_intent.path);
	}
	u_capture_intent_complete(&c->capture_intent, &c->mcp_capture, ok);
}


static xrt_result_t
d3d12_compositor_layer_commit(struct xrt_compositor *xc, xrt_graphics_sync_handle_t sync_handle)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);

	std::lock_guard<std::mutex> lock(c->mutex);

	// Capture-intent poll — see u_capture_intent.h. Consumed at the
	// projection-done boundary (PROJECTION_ONLY, once renderer split
	// lands) or end of frame (POST_COMPOSE).
	u_capture_intent_poll(&c->capture_intent, &c->mcp_capture);

	// Get predicted eye positions
	struct xrt_eye_positions eye_pos = {};
	if (c->display_processor != nullptr) {
		xrt_display_processor_d3d12_get_predicted_eye_positions(c->display_processor, &eye_pos);
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
				} else if (!c->owns_window && tgt_width > 0 && tgt_height > 0) {
					// Handle app: window may differ from display size,
					// derive view dims from actual window client area.
					u_tiling_compute_canvas_view(mode, tgt_width, tgt_height,
					                             &new_vw, &new_vh);
				}
				uint32_t cur_vw, cur_vh;
				comp_d3d12_renderer_get_view_dimensions(c->renderer, &cur_vw, &cur_vh);
				if (cur_vw != new_vw || cur_vh != new_vh) {
					uint32_t resize_target_h = (c->display_processor != NULL)
					    ? new_vh : tgt_height;
					comp_d3d12_renderer_resize(
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

	// Verify app renders at the expected resolution (not stretched)
	{
		static int rect_check_log = 0;
		uint32_t expected_vw, expected_vh;
		comp_d3d12_renderer_get_view_dimensions(c->renderer, &expected_vw, &expected_vh);
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

	// Render layers to atlas texture (skip if zero-copy). Split into a
	// projection pass + window-space pass so a projection-only capture
	// can read the atlas in between.
	xrt_result_t xret = XRT_SUCCESS;
	if (!zero_copy) {
		xret = comp_d3d12_renderer_draw_projection_pass(
		    c->renderer, c->cmd_list, &c->layer_accum, &left_eye, &right_eye, tgt_width, tgt_height, c->hardware_display_3d);
		if (xret != XRT_SUCCESS) {
			U_LOG_E("Failed to render projection pass");
			return xret;
		}

		// Projection-only capture point. Atlas is in RENDER_TARGET with
		// uncommitted projection commands in the cmd_list. To read it back
		// we need to commit those commands, transition the atlas to
		// PIXEL_SHADER_RESOURCE, run the capture (which uses the cmd_list
		// for its own copy + barriers), then transition back to
		// RENDER_TARGET so the window-space pass can append draws.
		if (c->capture_intent.pending && c->capture_intent.mode == MCP_CAPTURE_MODE_PROJECTION_ONLY) {
			ID3D12Resource *atlas_res = static_cast<ID3D12Resource *>(
			    comp_d3d12_renderer_get_atlas_resource(c->renderer));

			D3D12_RESOURCE_BARRIER ws_barrier = {};
			ws_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			ws_barrier.Transition.pResource = atlas_res;
			ws_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			ws_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
			ws_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			c->cmd_list->ResourceBarrier(1, &ws_barrier);

			c->cmd_list->Close();
			ID3D12CommandList *flush_lists[] = {c->cmd_list};
			c->command_queue->ExecuteCommandLists(1, flush_lists);
			gpu_wait_idle(c);

			// Capture handles its own cmd_list reset + barriers (PSR↔COPY_SOURCE).
			d3d12_compositor_dispatch_capture(c, MCP_CAPTURE_MODE_PROJECTION_ONLY);

			// Re-arm cmd_list and put atlas back in RENDER_TARGET.
			c->cmd_allocator->Reset();
			c->cmd_list->Reset(c->cmd_allocator, nullptr);
			ws_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			ws_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
			c->cmd_list->ResourceBarrier(1, &ws_barrier);
		}

		xret = comp_d3d12_renderer_draw_window_space_pass(
		    c->renderer, c->cmd_list, &c->layer_accum, tgt_width, tgt_height, c->hardware_display_3d);
		if (xret != XRT_SUCCESS) {
			U_LOG_E("Failed to render window-space pass");
			return xret;
		}
	}

	// Shared texture mode: weave (or copy) atlas into shared texture, skip window present
	if (c->has_shared_texture && c->shared_texture != nullptr) {
		ID3D12Resource *atlas_resource = zero_copy
		    ? static_cast<ID3D12Resource *>(zc_resource)
		    : static_cast<ID3D12Resource *>(comp_d3d12_renderer_get_atlas_resource(c->renderer));

		if (atlas_resource != nullptr && c->display_processor != NULL && c->shared_texture_rtv_heap != nullptr) {
			// DP path: weave atlas directly into shared texture
			static bool st_dp_logged = false;
			if (!st_dp_logged) {
				U_LOG_W("D3D12 shared texture: weaving via display processor");
				st_dp_logged = true;
			}

			// Execute atlas rendering commands first
			c->cmd_list->Close();
			ID3D12CommandList *copy_lists[] = {c->cmd_list};
			c->command_queue->ExecuteCommandLists(1, copy_lists);
			gpu_wait_idle(c);

			// Fresh command list for weaver
			c->cmd_allocator->Reset();
			c->cmd_list->Reset(c->cmd_allocator, nullptr);

			// Transition: shared texture COMMON→RENDER_TARGET, atlas PSR→COMMON
			D3D12_RESOURCE_BARRIER barriers[2] = {};
			barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barriers[0].Transition.pResource = c->shared_texture;
			barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
			barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
			barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

			barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barriers[1].Transition.pResource = atlas_resource;
			barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
			barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

			c->cmd_list->ResourceBarrier(2, barriers);

			// Bind shared texture as render target
			D3D12_CPU_DESCRIPTOR_HANDLE st_rtv = c->shared_texture_rtv_heap->GetCPUDescriptorHandleForHeapStart();
			c->cmd_list->OMSetRenderTargets(1, &st_rtv, FALSE, nullptr);

			uint32_t view_width, view_height;
			comp_d3d12_renderer_get_view_dimensions(c->renderer, &view_width, &view_height);
			uint32_t tile_columns, tile_rows;
			comp_d3d12_renderer_get_tile_layout(c->renderer, &tile_columns, &tile_rows);

			// Crop atlas to content dimensions
			uint32_t content_w = tile_columns * view_width;
			uint32_t content_h = tile_rows * view_height;
			ID3D12Resource *dp_resource = d3d12_crop_atlas_for_dp(c, atlas_resource, content_w, content_h);

			// Pass actual shared texture dimensions to the DP. The DP uses
			// canvas offset/size to set a viewport sub-rect within the shared
			// texture for correct interlacing phase alignment.
			D3D12_RESOURCE_DESC st_desc = c->shared_texture->GetDesc();
			uint32_t dp_target_w = static_cast<uint32_t>(st_desc.Width);
			uint32_t dp_target_h = static_cast<uint32_t>(st_desc.Height);

			static uint32_t pa_log = 0;
			if (pa_log < 5) {
				U_LOG_W("process_atlas: view=%ux%u tiles=%ux%u dp_target=%ux%u "
				        "canvas=(%d,%d %ux%u)",
				        view_width, view_height, tile_columns, tile_rows,
				        dp_target_w, dp_target_h,
				        c->canvas.valid ? c->canvas.x : -1,
				        c->canvas.valid ? c->canvas.y : -1,
				        c->canvas.valid ? c->canvas.w : 0,
				        c->canvas.valid ? c->canvas.h : 0);
				pa_log++;
			}

			xrt_display_processor_d3d12_process_atlas(
			    c->display_processor,
			    c->cmd_list,
			    dp_resource,
			    0,  // SRV GPU handle — SR weaver uses setInputViewTexture instead
			    st_rtv.ptr,
			    c->shared_texture,
			    view_width, view_height,
			    tile_columns, tile_rows,
			    static_cast<uint32_t>(DXGI_FORMAT_R8G8B8A8_UNORM),
			    dp_target_w, dp_target_h,
			    c->canvas.valid ? c->canvas.x : 0,
			    c->canvas.valid ? c->canvas.y : 0,
			    c->canvas.valid ? c->canvas.w : 0,
			    c->canvas.valid ? c->canvas.h : 0);

			// Transition: atlas COMMON→PSR, shared texture RT→COMMON
			barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
			barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
			barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
			barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			c->cmd_list->ResourceBarrier(2, barriers);

		} else if (atlas_resource != nullptr) {
			// No DP: raw copy atlas to shared texture (2D mode fallback)
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
	if (c->display_processor != NULL && c->target != nullptr) {
		static bool dp_logged = false;
		if (!dp_logged) {
			U_LOG_W("D3D12 weaving via display processor (swapchain RT)");
			dp_logged = true;
		}

		// Execute atlas copy so the texture is ready for the weaver
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

		// One-time diagnostic: log back buffer vs viewport dimensions
		static bool dp_dims_logged = false;
		if (!dp_dims_logged) {
			dp_dims_logged = true;
			D3D12_RESOURCE_DESC bb_desc = back_buffer->GetDesc();
			uint32_t vw, vh;
			comp_d3d12_renderer_get_view_dimensions(c->renderer, &vw, &vh);
			uint32_t tc, tr;
			comp_d3d12_renderer_get_tile_layout(c->renderer, &tc, &tr);
			U_LOG_W("D3D12 DP dims: back_buffer=%llux%u, viewport=%ux%u, "
			        "view=%ux%u, atlas=%ux%u (tile %ux%u), zero_copy=%d",
			        (unsigned long long)bb_desc.Width, (unsigned)bb_desc.Height,
			        tgt_width, tgt_height,
			        vw, vh,
			        tc * vw, tr * vh,
			        tc, tr, (int)zero_copy);
		}

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
			D3D12_RESOURCE_DESC atlas_desc = atlas_resource
			    ? atlas_resource->GetDesc() : D3D12_RESOURCE_DESC{};
			U_LOG_W("D3D12 dp path: atlas=%p (%llux%u), view=%ux%u, target=%ux%u, bb=%u, "
			        "back_buffer=%p, rtv=0x%llx, zc=%d",
			        (void *)atlas_resource,
			        (unsigned long long)atlas_desc.Width, (unsigned)atlas_desc.Height,
			        view_width, view_height,
			        tgt_width, tgt_height, bb_index,
			        (void *)back_buffer,
			        (unsigned long long)rtv_handle.ptr,
			        (int)zero_copy);
		}

		if (atlas_resource != nullptr) {
			// Both 3D and 2D modes: DP handles weaving/blit
			D3D12_RESOURCE_BARRIER atlas_barrier = {};
			atlas_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			atlas_barrier.Transition.pResource = atlas_resource;
			atlas_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			atlas_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
			atlas_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			c->cmd_list->ResourceBarrier(1, &atlas_barrier);

			uint32_t tile_columns, tile_rows;
			comp_d3d12_renderer_get_tile_layout(c->renderer, &tile_columns, &tile_rows);

			// Crop atlas to content dimensions before passing to DP
			uint32_t content_w = tile_columns * view_width;
			uint32_t content_h = tile_rows * view_height;
			ID3D12Resource *dp_resource = d3d12_crop_atlas_for_dp(
			    c, atlas_resource, content_w, content_h);

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

			// Pass actual backbuffer dimensions to the DP.
			// Canvas offset and size are passed separately — the DP uses
			// them to set a viewport sub-rect for correct interlacing phase.
			xrt_display_processor_d3d12_process_atlas(
			    c->display_processor,
			    c->cmd_list,
			    dp_resource,
			    0,  // SRV GPU handle — SR weaver uses setInputViewTexture instead
			    rtv_handle.ptr,
			    back_buffer,
			    view_width, view_height,
			    tile_columns, tile_rows,
			    static_cast<uint32_t>(DXGI_FORMAT_R8G8B8A8_UNORM),
			    tgt_width, tgt_height,
			    c->canvas.valid ? c->canvas.x : 0,
			    c->canvas.valid ? c->canvas.y : 0,
			    c->canvas.valid ? c->canvas.w : 0,
			    c->canvas.valid ? c->canvas.h : 0);

			// Transition atlas back: COMMON → PIXEL_SHADER_RESOURCE
			atlas_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
			atlas_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			c->cmd_list->ResourceBarrier(1, &atlas_barrier);

			// Transition back buffer for HUD overlay
			barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
			barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
			c->cmd_list->ResourceBarrier(1, &barrier);

			// HUD overlay
			d3d12_render_hud_overlay(c, c->cmd_list, back_buffer, tgt_width, tgt_height, &eye_pos);

			// Chroma-key post-weave pass (only when transparent + chroma_key_color != 0).
			// Converts pixels matching the chroma key to alpha=0 so DComp passes
			// them through to the desktop. Leaves back buffer in RENDER_TARGET on
			// success; we transition to PRESENT below.
			bool ck_ran = false;
			if (c->chroma_key_color != 0) {
				ck_ran = chroma_key_pass_execute(c, c->cmd_list, back_buffer,
				                                  rtv_handle, tgt_width, tgt_height);
			}

			// Transition back buffer to PRESENT. Source state depends on whether the
			// chroma-key pass ran (RENDER_TARGET) or not (COPY_DEST from HUD).
			barrier.Transition.StateBefore = ck_ran
			    ? D3D12_RESOURCE_STATE_RENDER_TARGET
			    : D3D12_RESOURCE_STATE_COPY_DEST;
			barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
			c->cmd_list->ResourceBarrier(1, &barrier);
		} else {
			// No atlas resource — just transition back buffer to PRESENT
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

		// Post-compose capture (#210) — fully composed atlas as DP saw it.
		// DP path returns early; mirror the fallback path's call site so the
		// capture surface works regardless of which weave path ran.
		d3d12_compositor_dispatch_capture(c, MCP_CAPTURE_MODE_POST_COMPOSE);

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

	// Post-compose capture (#210) — runs after the existing fence wait so
	// the GPU is idle when we reset the compositor's cmd allocator/list
	// for the readback.
	d3d12_compositor_dispatch_capture(c, MCP_CAPTURE_MODE_POST_COMPOSE);

	return XRT_SUCCESS;
}

static xrt_result_t
d3d12_compositor_layer_commit_with_semaphore(struct xrt_compositor *xc,
                                              struct xrt_compositor_semaphore *xcsem,
                                              uint64_t value)
{
	return d3d12_compositor_layer_commit(xc, XRT_GRAPHICS_SYNC_HANDLE_INVALID);
}

/*
 *
 * Chroma-key post-weave alpha-conversion pass (D3D12)
 *
 * The Leia weaver writes opaque RGB into the swapchain back buffer (alpha=1
 * everywhere). With DComp + per-pixel alpha presentation, that means DWM has
 * nothing to make transparent. This pass samples the post-weave back buffer
 * and rewrites it with alpha=0 wherever RGB exactly matches a chroma-key
 * color, alpha=1 otherwise.
 *
 * Inserted between the HUD overlay and the final PRESENT transition.
 *
 */

static const char *kChromaKeyVS = R"(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut main(uint vid : SV_VertexID) {
    VSOut o;
    o.uv = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
)";

static const char *kChromaKeyPS = R"(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
Texture2D<float4> src : register(t0);
SamplerState samp : register(s0);
cbuffer Constants : register(b0) { float3 chroma_rgb; float pad; };
float4 main(VSOut i) : SV_Target {
    float3 c = src.Sample(samp, i.uv).rgb;
    float3 d = abs(c - chroma_rgb);
    bool match = max(max(d.r, d.g), d.b) < (1.0/512.0);
    // Swapchain is DXGI_ALPHA_MODE_PREMULTIPLIED — RGB must already be * alpha.
    // For transparent (alpha=0) pixels RGB MUST be (0,0,0); otherwise DWM's
    // src.rgb + (1-alpha)*dst.rgb blend adds the matched chroma color to the
    // desktop and saturates to white instead of showing through.
    float a = match ? 0.0 : 1.0;
    return float4(c * a, a);
}
)";

/*!
 * Lazy-initialize PSO + root signature for the chroma-key pass.
 * Called once per compositor (resources outlive frames).
 */
static bool
chroma_key_init_pipeline(struct comp_d3d12_compositor *c)
{
	if (c->ck_pipeline_state != nullptr) {
		return true;
	}

	HRESULT hr;

	// Root signature: 32-bit root constants (4 floats — 3 for chroma BGR + pad),
	// descriptor table for SRV (1 entry), static sampler.
	D3D12_ROOT_PARAMETER root_params[2] = {};
	root_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	root_params[0].Constants.ShaderRegister = 0;
	root_params[0].Constants.RegisterSpace = 0;
	root_params[0].Constants.Num32BitValues = 4;
	root_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_DESCRIPTOR_RANGE srv_range = {};
	srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	srv_range.NumDescriptors = 1;
	srv_range.BaseShaderRegister = 0;
	srv_range.RegisterSpace = 0;
	srv_range.OffsetInDescriptorsFromTableStart = 0;
	root_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	root_params[1].DescriptorTable.NumDescriptorRanges = 1;
	root_params[1].DescriptorTable.pDescriptorRanges = &srv_range;
	root_params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_STATIC_SAMPLER_DESC sampler = {};
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
	sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.MaxLOD = D3D12_FLOAT32_MAX;
	sampler.ShaderRegister = 0;
	sampler.RegisterSpace = 0;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_ROOT_SIGNATURE_DESC rs_desc = {};
	rs_desc.NumParameters = 2;
	rs_desc.pParameters = root_params;
	rs_desc.NumStaticSamplers = 1;
	rs_desc.pStaticSamplers = &sampler;
	rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS;

	ID3DBlob *rs_blob = nullptr;
	ID3DBlob *err_blob = nullptr;
	hr = D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1,
	                                  &rs_blob, &err_blob);
	if (FAILED(hr)) {
		U_LOG_E("Chroma-key root sig serialize failed: 0x%08x %s", hr,
		        err_blob ? (const char *)err_blob->GetBufferPointer() : "");
		if (err_blob) err_blob->Release();
		return false;
	}
	if (err_blob) err_blob->Release();

	hr = c->device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
	                                     __uuidof(ID3D12RootSignature),
	                                     reinterpret_cast<void **>(&c->ck_root_signature));
	rs_blob->Release();
	if (FAILED(hr)) {
		U_LOG_E("Chroma-key CreateRootSignature failed: 0x%08x", hr);
		return false;
	}

	// Compile shaders
	ID3DBlob *vs_blob = nullptr;
	hr = D3DCompile(kChromaKeyVS, strlen(kChromaKeyVS), nullptr, nullptr, nullptr,
	                "main", "vs_5_0", 0, 0, &vs_blob, &err_blob);
	if (FAILED(hr)) {
		U_LOG_E("Chroma-key VS compile failed: 0x%08x %s", hr,
		        err_blob ? (const char *)err_blob->GetBufferPointer() : "");
		if (err_blob) err_blob->Release();
		return false;
	}
	if (err_blob) { err_blob->Release(); err_blob = nullptr; }

	ID3DBlob *ps_blob = nullptr;
	hr = D3DCompile(kChromaKeyPS, strlen(kChromaKeyPS), nullptr, nullptr, nullptr,
	                "main", "ps_5_0", 0, 0, &ps_blob, &err_blob);
	if (FAILED(hr)) {
		U_LOG_E("Chroma-key PS compile failed: 0x%08x %s", hr,
		        err_blob ? (const char *)err_blob->GetBufferPointer() : "");
		vs_blob->Release();
		if (err_blob) err_blob->Release();
		return false;
	}
	if (err_blob) err_blob->Release();

	// PSO
	D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
	pso_desc.pRootSignature = c->ck_root_signature;
	pso_desc.VS = {vs_blob->GetBufferPointer(), vs_blob->GetBufferSize()};
	pso_desc.PS = {ps_blob->GetBufferPointer(), ps_blob->GetBufferSize()};
	pso_desc.BlendState.RenderTarget[0].BlendEnable = FALSE;
	pso_desc.BlendState.RenderTarget[0].LogicOpEnable = FALSE;
	pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	pso_desc.SampleMask = UINT_MAX;
	pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	pso_desc.RasterizerState.FrontCounterClockwise = FALSE;
	pso_desc.RasterizerState.DepthClipEnable = TRUE;
	pso_desc.DepthStencilState.DepthEnable = FALSE;
	pso_desc.DepthStencilState.StencilEnable = FALSE;
	pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pso_desc.NumRenderTargets = 1;
	pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	pso_desc.SampleDesc.Count = 1;

	hr = c->device->CreateGraphicsPipelineState(&pso_desc, __uuidof(ID3D12PipelineState),
	                                             reinterpret_cast<void **>(&c->ck_pipeline_state));
	vs_blob->Release();
	ps_blob->Release();
	if (FAILED(hr)) {
		U_LOG_E("Chroma-key CreateGraphicsPipelineState failed: 0x%08x", hr);
		return false;
	}

	// Descriptor heap for the SRV (single shader-visible slot)
	D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
	heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heap_desc.NumDescriptors = 1;
	heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	hr = c->device->CreateDescriptorHeap(&heap_desc, __uuidof(ID3D12DescriptorHeap),
	                                      reinterpret_cast<void **>(&c->ck_srv_heap));
	if (FAILED(hr)) {
		U_LOG_E("Chroma-key CreateDescriptorHeap failed: 0x%08x", hr);
		return false;
	}

	U_LOG_W("Post-weave chroma-key conversion enabled: 0x%08X", c->chroma_key_color);
	return true;
}

/*!
 * Allocate (or reallocate on resize) the intermediate texture sized to the back buffer.
 */
static bool
chroma_key_ensure_intermediate(struct comp_d3d12_compositor *c, UINT width, UINT height)
{
	if (c->ck_intermediate != nullptr &&
	    c->ck_intermediate_w == width && c->ck_intermediate_h == height) {
		return true;
	}
	if (c->ck_intermediate != nullptr) {
		c->ck_intermediate->Release();
		c->ck_intermediate = nullptr;
	}

	D3D12_HEAP_PROPERTIES heap_props = {};
	heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_DESC tex_desc = {};
	tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	tex_desc.Width = width;
	tex_desc.Height = height;
	tex_desc.DepthOrArraySize = 1;
	tex_desc.MipLevels = 1;
	tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	tex_desc.SampleDesc.Count = 1;
	tex_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	tex_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

	HRESULT hr = c->device->CreateCommittedResource(
	    &heap_props, D3D12_HEAP_FLAG_NONE, &tex_desc,
	    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
	    nullptr, __uuidof(ID3D12Resource),
	    reinterpret_cast<void **>(&c->ck_intermediate));
	if (FAILED(hr)) {
		U_LOG_E("Chroma-key intermediate texture create failed: 0x%08x", hr);
		return false;
	}
	c->ck_intermediate_w = width;
	c->ck_intermediate_h = height;
	c->ck_intermediate_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

	// Create SRV at slot 0 of the descriptor heap.
	D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
	srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv_desc.Texture2D.MipLevels = 1;
	D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle = c->ck_srv_heap->GetCPUDescriptorHandleForHeapStart();
	c->device->CreateShaderResourceView(c->ck_intermediate, &srv_desc, cpu_handle);
	return true;
}

/*!
 * Run the post-weave chroma-key pass. Called when the back buffer is in
 * D3D12_RESOURCE_STATE_COPY_DEST (post-HUD). On success, leaves back buffer
 * in D3D12_RESOURCE_STATE_RENDER_TARGET (caller transitions to PRESENT).
 *
 * Returns false on failure; caller should fall back to skipping the pass.
 */
static bool
chroma_key_pass_execute(struct comp_d3d12_compositor *c,
                         ID3D12GraphicsCommandList *cmd_list,
                         ID3D12Resource *back_buffer,
                         D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle,
                         UINT width, UINT height)
{
	if (!chroma_key_init_pipeline(c)) return false;
	if (!chroma_key_ensure_intermediate(c, width, height)) return false;

	// Pack chroma color (0x00BBGGRR) → float3 RGB in [0,1].
	uint32_t k = c->chroma_key_color;
	float chroma_r = ((k >>  0) & 0xFF) / 255.0f;
	float chroma_g = ((k >>  8) & 0xFF) / 255.0f;
	float chroma_b = ((k >> 16) & 0xFF) / 255.0f;
	float root_consts[4] = { chroma_r, chroma_g, chroma_b, 0.0f };

	// Transition back_buffer COPY_DEST → COPY_SOURCE; intermediate (PIXEL_SHADER_RESOURCE) → COPY_DEST.
	D3D12_RESOURCE_BARRIER barriers[2] = {};
	barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barriers[0].Transition.pResource = back_buffer;
	barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barriers[1].Transition.pResource = c->ck_intermediate;
	barriers[1].Transition.StateBefore = c->ck_intermediate_state;
	barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
	barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	cmd_list->ResourceBarrier(2, barriers);
	c->ck_intermediate_state = D3D12_RESOURCE_STATE_COPY_DEST;

	cmd_list->CopyResource(c->ck_intermediate, back_buffer);

	// Transition back_buffer COPY_SOURCE → RENDER_TARGET; intermediate COPY_DEST → PIXEL_SHADER_RESOURCE.
	barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
	barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	cmd_list->ResourceBarrier(2, barriers);
	c->ck_intermediate_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

	// Bind pipeline + draw fullscreen triangle (3 verts via SV_VertexID).
	cmd_list->SetPipelineState(c->ck_pipeline_state);
	cmd_list->SetGraphicsRootSignature(c->ck_root_signature);
	ID3D12DescriptorHeap *heaps[] = { c->ck_srv_heap };
	cmd_list->SetDescriptorHeaps(1, heaps);
	cmd_list->SetGraphicsRoot32BitConstants(0, 4, root_consts, 0);
	cmd_list->SetGraphicsRootDescriptorTable(1, c->ck_srv_heap->GetGPUDescriptorHandleForHeapStart());

	D3D12_VIEWPORT vp = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
	D3D12_RECT scissor = { 0, 0, (LONG)width, (LONG)height };
	cmd_list->RSSetViewports(1, &vp);
	cmd_list->RSSetScissorRects(1, &scissor);
	cmd_list->OMSetRenderTargets(1, &rtv_handle, FALSE, nullptr);
	cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	cmd_list->IASetVertexBuffers(0, 0, nullptr);
	cmd_list->IASetIndexBuffer(nullptr);
	cmd_list->DrawInstanced(3, 1, 0, 0);
	return true;
}

static void
chroma_key_destroy(struct comp_d3d12_compositor *c)
{
	if (c->ck_intermediate)    { c->ck_intermediate->Release();    c->ck_intermediate = nullptr; }
	if (c->ck_srv_heap)        { c->ck_srv_heap->Release();        c->ck_srv_heap = nullptr; }
	if (c->ck_pipeline_state)  { c->ck_pipeline_state->Release();  c->ck_pipeline_state = nullptr; }
	if (c->ck_root_signature)  { c->ck_root_signature->Release();  c->ck_root_signature = nullptr; }
}

static void
d3d12_compositor_destroy(struct xrt_compositor *xc)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);

	U_LOG_I("Destroying D3D12 compositor");

	// Uninstall MCP capture hook before the GPU resources go away.
	mcp_capture_uninstall();
	mcp_capture_fini(&c->mcp_capture);

	// Wait for GPU idle
	if (c->fence != nullptr && c->command_queue != nullptr) {
		gpu_wait_idle(c);
	}

	// Destroy chroma-key pass resources
	chroma_key_destroy(c);

	// Destroy DP input crop resource
	if (c->dp_input_resource != nullptr) {
		c->dp_input_resource->Release();
		c->dp_input_resource = nullptr;
	}

	// Destroy display processor
	xrt_display_processor_d3d12_destroy(&c->display_processor);

	if (c->dp_srv_heap != nullptr) {
		c->dp_srv_heap->Release();
	}

	if (c->shared_texture_rtv_heap != nullptr) {
		c->shared_texture_rtv_heap->Release();
		c->shared_texture_rtv_heap = nullptr;
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

	// Destroy HUD resources
	if (c->hud != NULL) {
		u_hud_destroy(&c->hud);
	}
	if (c->hud_texture != nullptr) {
		c->hud_texture->Release();
	}
	if (c->hud_upload_buffer != nullptr) {
		c->hud_upload_buffer->Release();
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
                             bool transparent_background,
                             uint32_t chroma_key_color,
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
	c->hud = NULL;
	c->hud_texture = nullptr;
	c->hud_upload_buffer = nullptr;
	c->hud_upload_pitch = 0;
	c->hud_initialized = false;
	c->last_frame_time_ns = 0;
	c->smoothed_frame_time_ms = 16.67f;

	// Chroma-key pass state (lazy-allocated on first use)
	c->chroma_key_color = chroma_key_color;
	c->ck_root_signature = nullptr;
	c->ck_pipeline_state = nullptr;
	c->ck_intermediate = nullptr;
	c->ck_srv_heap = nullptr;
	c->ck_intermediate_w = 0;
	c->ck_intermediate_h = 0;

	// Handle window
	c->app_hwnd = nullptr;
	if (shared_texture_handle != nullptr) {
		// Shared texture mode: compositor doesn't own a swapchain.
		// Store app HWND separately for display processor position tracking.
		c->hwnd = nullptr;
		if (hwnd != nullptr) {
			c->app_hwnd = static_cast<HWND>(hwnd);
			U_LOG_I("Shared texture mode with app HWND for position tracking: %p", hwnd);
		} else {
			U_LOG_I("Shared texture mode (offscreen) — no window");
		}
	} else if (hwnd != nullptr) {
		c->hwnd = static_cast<HWND>(hwnd);
		U_LOG_I("Using app-provided window handle: %p", hwnd);
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

	// Create HUD overlay for self-owned windows
	if (c->owns_window) {
		u_hud_create(&c->hud, xdev->hmd->screens[0].w_pixels);
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
	c->shared_texture_rtv_heap = nullptr;
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

		// Create RTV for shared texture so the display processor can weave into it
		D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {};
		rtv_heap_desc.NumDescriptors = 1;
		rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		hr = c->device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&c->shared_texture_rtv_heap));
		if (FAILED(hr)) {
			U_LOG_E("Failed to create shared texture RTV heap: 0x%08x", hr);
			d3d12_compositor_destroy(&c->base.base);
			return XRT_ERROR_D3D;
		}
		D3D12_RENDER_TARGET_VIEW_DESC rtv_desc = {};
		rtv_desc.Format = st_desc.Format;
		rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
		c->device->CreateRenderTargetView(c->shared_texture, &rtv_desc,
		    c->shared_texture_rtv_heap->GetCPUDescriptorHandleForHeapStart());
		U_LOG_I("Created RTV for shared texture (weaver target)");
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
	if (c->has_shared_texture) {
		c->target = nullptr;
		U_LOG_I("Skipping DXGI swapchain (shared texture mode — compositor renders to shared texture)");
	} else if (c->hwnd != nullptr) {
		xret = comp_d3d12_target_create(c, c->hwnd,
		                                              c->settings.preferred.width,
		                                              c->settings.preferred.height,
		                                              transparent_background,
		                                              &c->target);
		if (xret != XRT_SUCCESS) {
			U_LOG_E("Failed to create D3D12 target");
			d3d12_compositor_destroy(&c->base.base);
			return xret;
		}
		if (comp_d3d12_target_has_child_window(c->target)) {
			U_LOG_I("D3D12 target using child window fallback (parent HWND: %p)", (void *)c->hwnd);
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
		HWND dp_hwnd = c->hwnd != nullptr ? c->hwnd : c->app_hwnd;
		xrt_result_t dp_ret = factory(c->device, c->command_queue, dp_hwnd, &c->display_processor);
		if (dp_ret != XRT_SUCCESS) {
			U_LOG_W("D3D12 display processor factory failed (error %d), continuing without", (int)dp_ret);
			c->display_processor = nullptr;
		} else {
			U_LOG_W("D3D12 display processor created via factory");

			// Tell the weaver the output render target format so it can
			// create its internal pipeline state. Without this, the weaver's
			// pipeline state stays null and weave() silently no-ops.
			// Use the shared texture format when available (texture apps),
			// otherwise fall back to the swapchain format (handle apps).
			DXGI_FORMAT output_fmt = c->has_shared_texture
			    ? c->shared_texture->GetDesc().Format
			    : DXGI_FORMAT_R8G8B8A8_UNORM;
			xrt_display_processor_d3d12_set_output_format(
			    c->display_processor,
			    output_fmt);
			U_LOG_W("D3D12 display processor: output format set to %u (target=%p)",
			        (unsigned)output_fmt, (void *)c->target);
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

	// Create renderer — when a DP is present, atlas height must match view height
	// so the DP's UV 0..1 maps exactly to content. The per-frame resize path
	// (resize_target_h above) must apply the same guard.
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

	// Install MCP capture_frame hook + arm the trigger-file path (#210).
	mcp_capture_init(&c->mcp_capture);
	mcp_capture_install(&c->mcp_capture);

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

	bool ok = xrt_display_processor_d3d12_get_window_metrics(c->display_processor, out_metrics);
	if (ok) {
		u_canvas_apply_to_metrics(out_metrics, &c->canvas);
	}
	return ok;
}

extern "C" bool
comp_d3d12_compositor_request_display_mode(struct xrt_compositor *xc, bool enable_3d)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);

	// Ensure GPU is fully idle before switching display mode.
	// The SR SDK's lens_hint enable/disable may interact with the D3D12
	// device internally. If the GPU has pending work (e.g. DXGI Present
	// scan-out), this can cause DXGI_ERROR_DEVICE_REMOVED on some GPUs
	// (observed on Intel Iris Xe with hosted D3D12 apps).
	gpu_wait_idle(c);

	return xrt_display_processor_d3d12_request_display_mode(c->display_processor, enable_3d);
}

extern "C" void
comp_d3d12_compositor_set_system_devices(struct xrt_compositor *xc,
                                          struct xrt_system_devices *xsysd)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);
	c->xsysd = xsysd;

	// Pass xsysd to self-owned window for direct qwerty input (WASD, TAB HUD, V mode toggle)
	if (c->owns_window && c->own_window != nullptr) {
		comp_d3d11_window_set_system_devices(c->own_window, xsysd);
	}
}

void
comp_d3d12_compositor_set_legacy_app_tile_scaling(struct xrt_compositor *xc,
                                                   bool legacy,
                                                   float scale_x,
                                                   float scale_y,
                                                   uint32_t view_w,
                                                   uint32_t view_h)
{
	if (xc == nullptr) {
		return;
	}
	struct comp_d3d12_compositor *c = d3d12_comp(xc);
	c->legacy_app_tile_scaling = legacy;
	c->legacy_view_scale_x = scale_x;
	c->legacy_view_scale_y = scale_y;
	if (c->renderer != nullptr) {
		comp_d3d12_renderer_set_legacy_app_tile_scaling(c->renderer, legacy);
	}

	// Fix view dims at the actual recommended size the app was told to render at.
	if (legacy && c->renderer != nullptr && view_w > 0 && view_h > 0) {
		uint32_t target_h = (c->display_processor != nullptr) ? view_h : c->settings.preferred.height;
		comp_d3d12_renderer_resize(c->renderer, view_w, view_h, target_h);
	}
}

extern "C" void
comp_d3d12_compositor_set_output_rect(struct xrt_compositor *xc,
                                       int32_t x, int32_t y,
                                       uint32_t w, uint32_t h)
{
	if (xc == nullptr) return;
	struct comp_d3d12_compositor *c = d3d12_comp(xc);
	struct u_canvas_rect rect = {true, x, y, w, h};
	c->canvas = rect;
}
