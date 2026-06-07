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
#include "util/u_capture_dims.h"
#include "util/u_image_capture.h"
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
	//! NULL in shared texture mode — compositor doesn't own a swapchain.
	HWND hwnd;

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

	//! 2D surround texture handle (Spec v6).
	struct u_surround_2d_handle surround_2d;

	//! Opened surround resource (ID3D12Device::OpenSharedHandle on first
	//! valid set_surround_2d). NULL when no surround is registered or open
	//! failed. State after open is D3D12_RESOURCE_STATE_COMMON; we
	//! transition COMMON <-> COPY_SOURCE around the per-frame strip blit
	//! and leave it in COMMON at the boundaries.
	ID3D12Resource *surround_texture;
	//! IDXGIKeyedMutex on surround_texture for cross-process sync (key 0
	//! protocol: app writes between Acquire(0)/Release(0), runtime samples
	//! between Acquire(0)/Release(0)). NULL in the spec-v7 fence path.
	IDXGIKeyedMutex *surround_mutex;

	//! Spec v7 fence-sync state. Mutually exclusive with surround_mutex:
	//! when surround_fence is non-NULL we use commandQueue->Wait(fence, value)
	//! instead of AcquireSync(0). The handles below mirror what the app
	//! registered so subsequent calls with the same handles skip re-opens.
	ID3D12Fence *surround_fence;
	void *surround_fence_cached_handle;
	void *surround_texture_cached_handle;
	uint64_t surround_await_fence_value;

	//! Lazily allocated intermediate resource for cropping atlas to content dims.
	ID3D12Resource *dp_input_resource;

	//! Cached dimensions for lazy reallocation.
	uint32_t dp_input_width, dp_input_height;

	//! Active authored zone mask (#439, XR_EXT_local_3d_zone). Set by
	//! comp_d3d12_compositor_zone_mask_submit (sticky, last-submit-wins),
	//! cleared when that mask is destroyed. NOT owned — the oxr handle owns
	//! the mask; lifetime is guaranteed by the destroy hook clearing this.
	struct comp_d3d12_zone_mask *active_zone_mask;

	//! Scratch copies for the masked composite (#439): the window region of
	//! the app 2D surround and of the weave target (RTV-only → the lerp
	//! samples this snapshot). Lazily (re)allocated window-sized (#464);
	//! steady state COMMON. Removed in Phase 3 when the weave lands in an
	//! SRV-capable RT directly.
	ID3D12Resource *surround_scratch;
	ID3D12Resource *weave_scratch;

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

// Spec v6 surround-2D helpers. Defined near the bottom of the file
// alongside comp_d3d12_compositor_set_surround_2d; forward-declared here
// because they're called from d3d12_compositor_layer_commit and
// d3d12_compositor_destroy (defined above the definitions). Matches the
// file's existing helper-before-use pattern for d3d12_crop_atlas_for_dp.
static void d3d12_release_surround(struct comp_d3d12_compositor *c);
static void d3d12_blit_surround_strips(struct comp_d3d12_compositor *c,
                                        ID3D12Resource *dst,
                                        D3D12_RESOURCE_STATES dst_pre_state,
                                        D3D12_RESOURCE_STATES dst_post_state,
                                        uint32_t dst_w, uint32_t dst_h,
                                        int32_t cx, int32_t cy,
                                        uint32_t cw, uint32_t ch);
// #439 authored zone-mask helpers (XR_EXT_local_3d_zone). Defined near the
// bottom of the file alongside the comp_d3d12_compositor_zone_mask_* entry
// points, called from the layer-commit paths + destroy above them.
static bool d3d12_composite_zone_mask(struct comp_d3d12_compositor *c,
                                       ID3D12Resource *dst,
                                       uint64_t dst_rtv,
                                       D3D12_RESOURCE_STATES dst_pre_state,
                                       D3D12_RESOURCE_STATES dst_post_state,
                                       uint32_t dst_w, uint32_t dst_h,
                                       const struct u_canvas_rect *eff_canvas);
static void d3d12_release_zone_state(struct comp_d3d12_compositor *c);
// #439 surround-capture probe (DISPLAYXR_SURROUND_CAPTURE); defined with the
// zone helpers, called after each path's fence wait in layer_commit.
static void d3d12_maybe_capture_surround_target(struct comp_d3d12_compositor *c,
                                                 ID3D12Resource *dst,
                                                 uint32_t dst_w, uint32_t dst_h,
                                                 D3D12_RESOURCE_STATES pre_state);

// #439 Phase 2: an active zone mask supersedes the canvas output rect —
// the weave region, view dims, Kooima metrics, and composite region all
// become the client-window rect (top-left anchored per #464). With no mask
// this returns c->canvas verbatim, so the no-mask path is unchanged.
// Returning a *valid* window rect (not just "invalid") matters on the
// shared-texture path: the texture is display-sized worst-case, so an
// invalid canvas there would fall back to display dims — the window rect
// keeps the #464 clamp. Callers in the frame path hold c->mutex, which
// zone_mask_submit/destroy also take, so the mask cannot flip mid-frame.
static struct u_canvas_rect
d3d12_effective_canvas(struct comp_d3d12_compositor *c)
{
	if (c->active_zone_mask == nullptr) {
		return c->canvas;
	}
	struct u_canvas_rect win = {};
	HWND wnd = c->hwnd != nullptr ? c->hwnd : c->app_hwnd;
	RECT r;
	if (wnd != nullptr && GetClientRect(wnd, &r) && r.right > 0 && r.bottom > 0) {
		win.valid = true;
		win.x = 0;
		win.y = 0;
		win.w = (uint32_t)r.right;
		win.h = (uint32_t)r.bottom;
		return win;
	}
	return win; // invalid → existing full-target fallbacks
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
 * Local-2D layer (XR_EXT_local_3d_zone v3, #439 Phase 3) — accumulate only;
 * the D3D12 consumer is a Windows follow-up leg
 * (docs/roadmap/unified-2d-3d-phase3-impl.md §7).
 */
static xrt_result_t
d3d12_compositor_layer_local_2d(struct xrt_compositor *xc,
                                struct xrt_device *xdev,
                                struct xrt_swapchain *xsc,
                                const struct xrt_layer_data *data)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);
	std::lock_guard<std::mutex> lock(c->mutex);
	comp_layer_accum_local_2d(&c->layer_accum, xsc, data);
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

// u_capture_dims provider: report the renderer's CURRENT window-scaled per-view
// dims + tile layout so xrCaptureAtlasEXT can fill XrAtlasCaptureResultEXT with
// what the capture actually writes, not the nominal system info (#431).
static bool
d3d12_compositor_capture_dims_provider(void *userdata,
                                       uint32_t *out_view_w,
                                       uint32_t *out_view_h,
                                       uint32_t *out_tile_cols,
                                       uint32_t *out_tile_rows)
{
	struct comp_d3d12_compositor *c = static_cast<struct comp_d3d12_compositor *>(userdata);
	if (c == nullptr || c->renderer == nullptr) {
		return false;
	}
	uint32_t vw = 0, vh = 0, cols = 1, rows = 1;
	comp_d3d12_renderer_get_view_dimensions(c->renderer, &vw, &vh);
	comp_d3d12_renderer_get_tile_layout(c->renderer, &cols, &rows);
	if (vw == 0 || vh == 0) {
		return false;
	}
	*out_view_w = vw;
	*out_view_h = vh;
	*out_tile_cols = cols;
	*out_tile_rows = rows;
	return true;
}

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
			// Swapchain alpha is undefined for display output — force opaque
			// so the PNG doesn't render fully transparent/black (issue #425).
			u_image_force_opaque_rgba8(tight, content_w, content_h, tight_pitch);
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

	// #439 Phase 2: the one canvas authority for this frame. While a zone
	// mask is active this is the client-window rect (the mask supersedes
	// the output rect); otherwise it is c->canvas unchanged. Computed once
	// under c->mutex (held for this whole function) so the weave region,
	// view dims, and composite all see the same rect even if submit/destroy
	// race the frame.
	const struct u_canvas_rect eff_canvas = d3d12_effective_canvas(c);

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
				if (eff_canvas.valid) {
					u_tiling_compute_canvas_view(mode, eff_canvas.w, eff_canvas.h,
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
				        eff_canvas.valid ? eff_canvas.x : -1,
				        eff_canvas.valid ? eff_canvas.y : -1,
				        eff_canvas.valid ? eff_canvas.w : 0,
				        eff_canvas.valid ? eff_canvas.h : 0);
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
			    eff_canvas.valid ? eff_canvas.x : 0,
			    eff_canvas.valid ? eff_canvas.y : 0,
			    eff_canvas.valid ? eff_canvas.w : 0,
			    eff_canvas.valid ? eff_canvas.h : 0);

			// Transition: atlas COMMON→PSR, shared texture RT→COMMON
			barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
			barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
			barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
			barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			c->cmd_list->ResourceBarrier(2, barriers);

			// Spec v6 surround blit: fill non-canvas pixels of the shared
			// texture from the app-supplied 2D surround texture. dst is in
			// COMMON (just transitioned above); leave it in COMMON after.
			// #439: an authored zone mask (XR_EXT_local_3d_zone) replaces
			// the rect-derived region selection entirely — the mask-lerp
			// writes every window pixel, so the strip path must be skipped
			// when it runs (strips would clobber soft edges).
			bool surround_done = d3d12_composite_zone_mask(
			    c, c->shared_texture, st_rtv.ptr,
			    D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON,
			    dp_target_w, dp_target_h, &eff_canvas);
			if (!surround_done) {
				d3d12_blit_surround_strips(
				    c, c->shared_texture,
				    D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON,
				    dp_target_w, dp_target_h,
				    eff_canvas.valid ? eff_canvas.x : 0,
				    eff_canvas.valid ? eff_canvas.y : 0,
				    eff_canvas.valid ? eff_canvas.w : dp_target_w,
				    eff_canvas.valid ? eff_canvas.h : dp_target_h);
			}

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

		// #439 A/B validation probe (no-op unless DISPLAYXR_SURROUND_CAPTURE
		// is set + trigger file exists). Runs post-fence: the probe re-arms
		// the cmd list for its readback, which needs the GPU idle.
		if (c->has_shared_texture && c->shared_texture != nullptr) {
			D3D12_RESOURCE_DESC std_desc = c->shared_texture->GetDesc();
			d3d12_maybe_capture_surround_target(c, c->shared_texture,
			                                    (uint32_t)std_desc.Width, std_desc.Height,
			                                    D3D12_RESOURCE_STATE_COMMON);
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
			    eff_canvas.valid ? eff_canvas.x : 0,
			    eff_canvas.valid ? eff_canvas.y : 0,
			    eff_canvas.valid ? eff_canvas.w : 0,
			    eff_canvas.valid ? eff_canvas.h : 0);

			// Spec v6 surround blit: fill non-canvas pixels of the back
			// buffer from the app-supplied 2D surround texture. Back
			// buffer is still in RENDER_TARGET from the DP; leave it
			// in RENDER_TARGET so HUD's existing RT→COPY_DEST transition
			// (below) proceeds unchanged.
			// #439: an authored zone mask replaces the rect-derived region
			// selection entirely (see the shared-texture path).
			bool surround_done = d3d12_composite_zone_mask(
			    c, back_buffer, rtv_handle.ptr,
			    D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET,
			    tgt_width, tgt_height, &eff_canvas);
			if (!surround_done) {
				d3d12_blit_surround_strips(
				    c, back_buffer,
				    D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET,
				    tgt_width, tgt_height,
				    eff_canvas.valid ? eff_canvas.x : 0,
				    eff_canvas.valid ? eff_canvas.y : 0,
				    eff_canvas.valid ? eff_canvas.w : tgt_width,
				    eff_canvas.valid ? eff_canvas.h : tgt_height);
			}

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

			// Transition back buffer COPY_DEST -> PRESENT. The chroma-key
			// post-weave alpha conversion (when needed) runs inside the Leia
			// D3D12 display processor's process_atlas, which leaves the back
			// buffer in RENDER_TARGET. The D3D12 weaver however leaves the
			// back buffer in RENDER_TARGET pre-HUD; the HUD path puts it in
			// COPY_DEST here. So source state matches the HUD's exit state.
			barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
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

		// #439 A/B validation probe (no-op unless DISPLAYXR_SURROUND_CAPTURE
		// is set + trigger file exists). Must read the back buffer BEFORE
		// Present — flip-model contents are undefined after. When triggered
		// it drains the GPU itself before re-arming the cmd list.
		if (back_buffer != nullptr) {
			d3d12_maybe_capture_surround_target(c, back_buffer, tgt_width, tgt_height,
			                                    D3D12_RESOURCE_STATE_PRESENT);
		}

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


static void
d3d12_compositor_destroy(struct xrt_compositor *xc)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);

	U_LOG_I("Destroying D3D12 compositor");

	// Uninstall MCP capture hook before the GPU resources go away.
	u_capture_dims_set_provider(NULL, c);
	mcp_capture_uninstall();
	mcp_capture_fini(&c->mcp_capture);

	// Wait for GPU idle
	if (c->fence != nullptr && c->command_queue != nullptr) {
		gpu_wait_idle(c);
	}

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

	// Spec v6: release the 2D surround texture + keyed mutex if registered.
	d3d12_release_surround(c);
	c->surround_2d = {};

	// #439: release the zone-mask scratches + detach any active mask (the
	// oxr handle owns the mask object itself).
	d3d12_release_zone_state(c);

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
                             int32_t display_screen_left,
                             int32_t display_screen_top,
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
		xrt_result_t xret = comp_d3d11_window_create(
		    win_w, win_h, display_screen_left, display_screen_top, &c->own_window);
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

			// Forward session-level transparency config. The DP runs the
			// chroma-key fill+strip internally when transparent_background
			// is set; chroma_key_color=0 means the DP picks its own key.
			xrt_display_processor_d3d12_set_chroma_key(
			    c->display_processor, chroma_key_color, transparent_background);
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

	// Expose current window-scaled capture dims to xrCaptureAtlasEXT (#431).
	u_capture_dims_set_provider(d3d12_compositor_capture_dims_provider, c);

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
	c->base.base.layer_local_2d = d3d12_compositor_layer_local_2d;
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
	if (xc == nullptr || out_metrics == nullptr) {
		if (out_metrics != nullptr) {
			out_metrics->valid = false;
		}
		return false;
	}

	struct comp_d3d12_compositor *c = d3d12_comp(xc);

	// Prefer a DP-provided window metrics implementation if one exists.
	bool ok = xrt_display_processor_d3d12_get_window_metrics(c->display_processor, out_metrics);
	if (!ok) {
		// No DP implementation (the in-tree sim_display DP and the Leia
		// plug-in delegate window placement to the runtime). Compute the
		// metrics directly from the HWND — same construction as the d3d11
		// native compositor. Without this, d3d12 handle/texture sessions
		// had NO window metrics and the runtime-side Kooima (rig path, raw
		// channel, legacy-2D fovs) ran display-scoped, so window-relative
		// 3D and the rig's rotation pivot were wrong (#396 W7 dogfood).
		memset(out_metrics, 0, sizeof(*out_metrics));

		// Shared-texture (texture-app) sessions carry the app's window in
		// app_hwnd (c->hwnd stays null); u_canvas_apply_to_metrics below
		// rewrites the result to the canvas sub-rect.
		HWND metrics_hwnd = c->hwnd != nullptr ? c->hwnd : c->app_hwnd;
		if (c->display_processor == nullptr || metrics_hwnd == nullptr) {
			return false;
		}

		uint32_t disp_px_w = 0, disp_px_h = 0;
		int32_t disp_left = 0, disp_top = 0;
		if (!xrt_display_processor_d3d12_get_display_pixel_info(
		        c->display_processor, &disp_px_w, &disp_px_h, &disp_left, &disp_top)) {
			return false;
		}
		if (disp_px_w == 0 || disp_px_h == 0) {
			return false;
		}

		float disp_w_m = 0.0f, disp_h_m = 0.0f;
		if (!xrt_display_processor_d3d12_get_display_dimensions(
		        c->display_processor, &disp_w_m, &disp_h_m)) {
			return false;
		}

		RECT rect;
		if (!GetClientRect(metrics_hwnd, &rect)) {
			return false;
		}
		uint32_t win_px_w = static_cast<uint32_t>(rect.right - rect.left);
		uint32_t win_px_h = static_cast<uint32_t>(rect.bottom - rect.top);
		if (win_px_w == 0 || win_px_h == 0) {
			return false;
		}

		POINT client_origin = {0, 0};
		ClientToScreen(metrics_hwnd, &client_origin);

		float pixel_size_x = disp_w_m / (float)disp_px_w;
		float pixel_size_y = disp_h_m / (float)disp_px_h;

		float win_w_m = (float)win_px_w * pixel_size_x;
		float win_h_m = (float)win_px_h * pixel_size_y;

		float win_center_px_x = (float)(client_origin.x - disp_left) + (float)win_px_w / 2.0f;
		float win_center_px_y = (float)(client_origin.y - disp_top) + (float)win_px_h / 2.0f;
		float disp_center_px_x = (float)disp_px_w / 2.0f;
		float disp_center_px_y = (float)disp_px_h / 2.0f;

		// X: +right (screen and eye coords agree). Y: negated (screen
		// Y-down, eye Y-up).
		float offset_x_m = (win_center_px_x - disp_center_px_x) * pixel_size_x;
		float offset_y_m = -((win_center_px_y - disp_center_px_y) * pixel_size_y);

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
	}

	// #439 Phase 2: the active zone mask supersedes the canvas — the
	// Kooima/adaptive-FOV metrics follow the same authority as the
	// weave region. (This path doesn't take c->mutex; the canvas/mask
	// fields are pointer-sized reads updated under the lock in another
	// function — the pointer check in d3d12_effective_canvas is benign.)
	const struct u_canvas_rect eff_canvas = d3d12_effective_canvas(c);
	u_canvas_apply_to_metrics(out_metrics, &eff_canvas);

	return true;
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

// Release any cached surround resources. Idempotent; safe on a freshly
// zeroed compositor. Used by set_surround_2d (re-register / clear) and
// the destroy path. Releases both keyed-mutex (spec v6) and fence (spec
// v7) state regardless of which path the app was using.
static void
d3d12_release_surround(struct comp_d3d12_compositor *c)
{
	if (c->surround_mutex != nullptr) {
		c->surround_mutex->Release();
		c->surround_mutex = nullptr;
	}
	if (c->surround_fence != nullptr) {
		c->surround_fence->Release();
		c->surround_fence = nullptr;
	}
	c->surround_fence_cached_handle = nullptr;
	c->surround_texture_cached_handle = nullptr;
	c->surround_await_fence_value = 0;
	if (c->surround_texture != nullptr) {
		c->surround_texture->Release();
		c->surround_texture = nullptr;
	}
}

// Record a CopyTextureRegion for one strip from surround->dst.
// Skips zero-area strips. Caller has already verified format match,
// surround_texture is in COPY_SOURCE state, and dst is in COPY_DEST state.
static void
d3d12_record_surround_strip(ID3D12GraphicsCommandList *cmd_list,
                             ID3D12Resource *dst, ID3D12Resource *src,
                             uint32_t dst_x, uint32_t dst_y,
                             uint32_t src_x, uint32_t src_y,
                             uint32_t w, uint32_t h)
{
	if (w == 0 || h == 0) return;

	D3D12_TEXTURE_COPY_LOCATION dst_loc = {};
	dst_loc.pResource = dst;
	dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	dst_loc.SubresourceIndex = 0;

	D3D12_TEXTURE_COPY_LOCATION src_loc = {};
	src_loc.pResource = src;
	src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	src_loc.SubresourceIndex = 0;

	D3D12_BOX src_box = {};
	src_box.left = src_x;
	src_box.top = src_y;
	src_box.front = 0;
	src_box.right = src_x + w;
	src_box.bottom = src_y + h;
	src_box.back = 1;

	cmd_list->CopyTextureRegion(&dst_loc, dst_x, dst_y, 0, &src_loc, &src_box);
}

// Blit non-canvas pixels of the surround texture into the dst resource.
// Records barriers + 4× CopyTextureRegion + barriers into c->cmd_list,
// bracketed by AcquireSync(0)/ReleaseSync(0) on the surround keyed mutex.
//
// Caller-supplied dst_pre_state/dst_post_state lets us drop the helper
// into either the shared-texture path (COMMON before, COMMON after) or
// the window-DP path (RENDER_TARGET before, RENDER_TARGET after — HUD's
// existing transition handles the move to COPY_DEST).
//
// Surround texture is assumed to be in COMMON state at entry — that's
// the cross-process-shared invariant: both sides leave it in COMMON
// before ReleaseSync, so the next AcquireSync sees it ready for
// transition.
//
// Strip layout matches the D3D11 helper. Skips any zero-area strip
// (canvas flush against an edge). Format mismatch logs once and skips
// the entire blit (no barriers / no mutex AcquireSync) — caller's
// command list is unaffected.
static void
d3d12_blit_surround_strips(struct comp_d3d12_compositor *c,
                            ID3D12Resource *dst,
                            D3D12_RESOURCE_STATES dst_pre_state,
                            D3D12_RESOURCE_STATES dst_post_state,
                            uint32_t dst_w, uint32_t dst_h,
                            int32_t cx, int32_t cy,
                            uint32_t cw, uint32_t ch)
{
	if (!c->surround_2d.valid || c->surround_texture == nullptr) {
		return;
	}
	const bool use_fence = (c->surround_fence != nullptr);
	if (!use_fence && c->surround_mutex == nullptr) {
		return;
	}
	if (dst == nullptr) {
		return;
	}

	if (c->surround_2d.w != dst_w || c->surround_2d.h != dst_h) {
		static bool dims_logged = false;
		if (!dims_logged) {
			U_LOG_W("D3D12 surround 2D: dim mismatch — surround %ux%u, target %ux%u. "
			        "Surround blit skipped. Re-register surround on window resize.",
			        c->surround_2d.w, c->surround_2d.h, dst_w, dst_h);
			dims_logged = true;
		}
		return;
	}

	D3D12_RESOURCE_DESC src_desc = c->surround_texture->GetDesc();
	D3D12_RESOURCE_DESC dst_desc = dst->GetDesc();
	if (src_desc.Format != dst_desc.Format) {
		static bool fmt_logged = false;
		if (!fmt_logged) {
			U_LOG_W("D3D12 surround 2D: format mismatch — surround=%u, target=%u. "
			        "Surround blit skipped. v6 requires matching DXGI formats; "
			        "cross-format SRGB<->UNORM blit not yet supported.",
			        (unsigned)src_desc.Format, (unsigned)dst_desc.Format);
			fmt_logged = true;
		}
		return;
	}

	if (use_fence) {
		// Spec v7: queue a wait on our command queue. The wait gates the
		// next ExecuteCommandLists (the caller's submission of c->cmd_list
		// happens later) on the app's Signal(fence, await_value). Read-only
		// access — no Release counterpart is needed.
		HRESULT hr = c->command_queue->Wait(c->surround_fence, c->surround_await_fence_value);
		if (FAILED(hr)) {
			static bool wait_logged = false;
			if (!wait_logged) {
				U_LOG_W("D3D12 surround 2D: queue->Wait(fence=%p, value=%llu) failed (hr=0x%08x). "
				        "Surround blit skipped.",
				        c->surround_fence,
				        (unsigned long long)c->surround_await_fence_value, hr);
				wait_logged = true;
			}
			return;
		}
	} else {
		HRESULT hr = c->surround_mutex->AcquireSync(0, 16);
		if (FAILED(hr)) {
			// Timeout / abandoned — skip this frame.
			return;
		}
	}

	// Clamp canvas to dst bounds in case the app submitted a degenerate rect.
	uint32_t cx_u = (cx < 0) ? 0u : (uint32_t)cx;
	uint32_t cy_u = (cy < 0) ? 0u : (uint32_t)cy;
	if (cx_u > dst_w) cx_u = dst_w;
	if (cy_u > dst_h) cy_u = dst_h;
	uint32_t cright  = (cx_u + cw > dst_w) ? dst_w : cx_u + cw;
	uint32_t cbottom = (cy_u + ch > dst_h) ? dst_h : cy_u + ch;

	// Enter copy state.
	D3D12_RESOURCE_BARRIER enter[2] = {};
	enter[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	enter[0].Transition.pResource = dst;
	enter[0].Transition.StateBefore = dst_pre_state;
	enter[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
	enter[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

	enter[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	enter[1].Transition.pResource = c->surround_texture;
	enter[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
	enter[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	enter[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

	// If pre-state already equals COPY_DEST, skip the dst transition by
	// collapsing the surround barrier into slot 0.
	uint32_t num_enter = (dst_pre_state == D3D12_RESOURCE_STATE_COPY_DEST) ? 1 : 2;
	if (num_enter == 1) {
		enter[0] = enter[1];
	}
	c->cmd_list->ResourceBarrier(num_enter, enter);

	// Strips. Source and dest coords match (1:1 blit by spec).
	// Top strip: y in [0, cy_u).
	d3d12_record_surround_strip(c->cmd_list, dst, c->surround_texture,
	                             0, 0, 0, 0, dst_w, cy_u);
	// Bottom strip: y in [cbottom, dst_h).
	if (cbottom < dst_h) {
		d3d12_record_surround_strip(c->cmd_list, dst, c->surround_texture,
		                             0, cbottom, 0, cbottom, dst_w, dst_h - cbottom);
	}
	// Left strip: x in [0, cx_u), y in [cy_u, cbottom).
	if (cx_u > 0 && cbottom > cy_u) {
		d3d12_record_surround_strip(c->cmd_list, dst, c->surround_texture,
		                             0, cy_u, 0, cy_u, cx_u, cbottom - cy_u);
	}
	// Right strip: x in [cright, dst_w), y in [cy_u, cbottom).
	if (cright < dst_w && cbottom > cy_u) {
		d3d12_record_surround_strip(c->cmd_list, dst, c->surround_texture,
		                             cright, cy_u, cright, cy_u, dst_w - cright, cbottom - cy_u);
	}

	// Exit copy state.
	D3D12_RESOURCE_BARRIER exit[2] = {};
	exit[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	exit[0].Transition.pResource = dst;
	exit[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	exit[0].Transition.StateAfter = dst_post_state;
	exit[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

	exit[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	exit[1].Transition.pResource = c->surround_texture;
	exit[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
	exit[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
	exit[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

	uint32_t num_exit = (dst_post_state == D3D12_RESOURCE_STATE_COPY_DEST) ? 1 : 2;
	if (num_exit == 1) {
		exit[0] = exit[1];
	}
	c->cmd_list->ResourceBarrier(num_exit, exit);

	if (!use_fence) {
		c->surround_mutex->ReleaseSync(0);
	}
	// Fence path: read-only, no signal-back. The app guarantees the
	// texture is stable until it bumps the fence on the next frame.
}

extern "C" void
comp_d3d12_compositor_set_surround_2d(struct xrt_compositor *xc,
                                       void *shared_handle,
                                       uint32_t w, uint32_t h)
{
	if (xc == nullptr) return;
	struct comp_d3d12_compositor *c = d3d12_comp(xc);

	// Release previous registration (no-op on first call). Also covers
	// the NULL-handle clear path.
	d3d12_release_surround(c);

	if (shared_handle == nullptr) {
		c->surround_2d = {};
		U_LOG_IFL_I(U_LOGGING_INFO, "D3D12 surround 2D cleared");
		return;
	}

	// D3D12::OpenSharedHandle handles NT handles natively (no "1" suffix
	// like D3D11). Spec v6 requires the source texture be created with
	// D3D11_RESOURCE_MISC_SHARED_NTHANDLE | _SHARED_KEYEDMUTEX (or D3D12
	// equivalents) so the IDXGIKeyedMutex QueryInterface succeeds below.
	// Name the local 'nt_handle' (not 'h') to avoid shadowing the
	// formal parameter 'h' (the height) — MSVC's stricter parameter
	// scoping flagged this; clang/gcc let it slide.
	HANDLE nt_handle = static_cast<HANDLE>(shared_handle);
	HRESULT hr = c->device->OpenSharedHandle(nt_handle, __uuidof(ID3D12Resource),
	                                          reinterpret_cast<void **>(&c->surround_texture));
	if (FAILED(hr) || c->surround_texture == nullptr) {
		U_LOG_E("D3D12 surround 2D: OpenSharedHandle failed for handle=%p (hr=0x%08x). "
		        "Ensure the texture was created with NT-handle + keyed-mutex sharing.",
		        shared_handle, hr);
		c->surround_2d = {};
		c->surround_texture = nullptr;
		return;
	}

	// Validate dims against the app's promise. The HWND-equality check
	// happens at frame time when we compare against c->shared_texture's
	// (or back buffer's) dims.
	D3D12_RESOURCE_DESC sd = c->surround_texture->GetDesc();
	if (sd.Width != w || sd.Height != h) {
		U_LOG_E("D3D12 surround 2D: registration dims (%ux%u) do not match opened "
		        "texture dims (%llux%u)", w, h,
		        (unsigned long long)sd.Width, (unsigned)sd.Height);
		d3d12_release_surround(c);
		c->surround_2d = {};
		return;
	}

	hr = c->surround_texture->QueryInterface(__uuidof(IDXGIKeyedMutex),
	                                          reinterpret_cast<void **>(&c->surround_mutex));
	if (FAILED(hr) || c->surround_mutex == nullptr) {
		U_LOG_E("D3D12 surround 2D: opened texture has no IDXGIKeyedMutex "
		        "(hr=0x%08x). Required for cross-process sync.", hr);
		d3d12_release_surround(c);
		c->surround_2d = {};
		return;
	}

	c->surround_2d.valid = true;
	c->surround_2d.shared_handle = shared_handle;
	c->surround_2d.w = w;
	c->surround_2d.h = h;
	c->surround_texture_cached_handle = shared_handle;
	U_LOG_IFL_I(U_LOGGING_INFO, "D3D12 surround 2D registered: handle=%p %llux%u format=%u",
	            shared_handle,
	            (unsigned long long)sd.Width, (unsigned)sd.Height,
	            (unsigned)sd.Format);
}

extern "C" void
comp_d3d12_compositor_set_surround_2d_fence(struct xrt_compositor *xc,
                                              void *shared_texture_handle,
                                              uint32_t w, uint32_t h,
                                              void *shared_fence_handle,
                                              uint64_t await_fence_value)
{
	if (xc == nullptr) return;
	struct comp_d3d12_compositor *c = d3d12_comp(xc);

	// NULL handle = clear (matches the spec v6 fn).
	if (shared_texture_handle == nullptr) {
		d3d12_release_surround(c);
		c->surround_2d = {};
		U_LOG_IFL_I(U_LOGGING_INFO, "D3D12 surround 2D (fence path) cleared");
		return;
	}

	if (shared_fence_handle == nullptr) {
		U_LOG_E("D3D12 surround 2D (fence path): shared_fence_handle is NULL "
		        "but shared_texture_handle is non-NULL — fence handle is required.");
		return;
	}

	// Hot path: same handles as last registration. Just update the await
	// fence value. This is the steady-state per-frame call.
	if (c->surround_texture != nullptr &&
	    c->surround_texture_cached_handle == shared_texture_handle &&
	    c->surround_fence != nullptr &&
	    c->surround_fence_cached_handle == shared_fence_handle) {

		// Spec v7 §3.7: awaitFenceValue must be strictly monotonic in
		// steady-state. The very first non-zero value is accepted from any
		// prior state. We log + ignore (don't queue a backwards wait) but
		// don't return XR_ERROR_VALIDATION_FAILURE from the deeper layer —
		// the state tracker doesn't surface compositor errors today.
		if (await_fence_value < c->surround_await_fence_value) {
			static bool nonmono_logged = false;
			if (!nonmono_logged) {
				U_LOG_W("D3D12 surround 2D (fence path): non-monotonic await value "
				        "%llu < cached %llu — ignoring this frame.",
				        (unsigned long long)await_fence_value,
				        (unsigned long long)c->surround_await_fence_value);
				nonmono_logged = true;
			}
			return;
		}
		c->surround_await_fence_value = await_fence_value;
		return;
	}

	// Cold path: first registration, or handles changed. Reopen both and
	// re-validate. Clears any prior spec-v6 keyed-mutex registration too.
	d3d12_release_surround(c);
	c->surround_2d = {};

	HANDLE tex_nt = static_cast<HANDLE>(shared_texture_handle);
	HRESULT hr = c->device->OpenSharedHandle(tex_nt, __uuidof(ID3D12Resource),
	                                          reinterpret_cast<void **>(&c->surround_texture));
	if (FAILED(hr) || c->surround_texture == nullptr) {
		U_LOG_E("D3D12 surround 2D (fence path): OpenSharedHandle(texture=%p) failed "
		        "(hr=0x%08x).", shared_texture_handle, hr);
		c->surround_texture = nullptr;
		return;
	}

	D3D12_RESOURCE_DESC sd = c->surround_texture->GetDesc();
	if (sd.Width != w || sd.Height != h) {
		U_LOG_E("D3D12 surround 2D (fence path): registration dims (%ux%u) do not "
		        "match opened texture dims (%llux%u)",
		        w, h, (unsigned long long)sd.Width, (unsigned)sd.Height);
		d3d12_release_surround(c);
		return;
	}

	HANDLE fence_nt = static_cast<HANDLE>(shared_fence_handle);
	hr = c->device->OpenSharedHandle(fence_nt, __uuidof(ID3D12Fence),
	                                  reinterpret_cast<void **>(&c->surround_fence));
	if (FAILED(hr) || c->surround_fence == nullptr) {
		U_LOG_E("D3D12 surround 2D (fence path): OpenSharedHandle(fence=%p) failed "
		        "(hr=0x%08x). Ensure the fence was created with shared NT handle.",
		        shared_fence_handle, hr);
		d3d12_release_surround(c);
		return;
	}

	c->surround_2d.valid = true;
	c->surround_2d.shared_handle = shared_texture_handle;
	c->surround_2d.w = w;
	c->surround_2d.h = h;
	c->surround_texture_cached_handle = shared_texture_handle;
	c->surround_fence_cached_handle = shared_fence_handle;
	c->surround_await_fence_value = await_fence_value;
	U_LOG_IFL_I(U_LOGGING_INFO,
	            "D3D12 surround 2D (fence path) registered: tex=%p fence=%p %llux%u "
	            "format=%u initial_await=%llu",
	            shared_texture_handle, shared_fence_handle,
	            (unsigned long long)sd.Width, (unsigned)sd.Height,
	            (unsigned)sd.Format,
	            (unsigned long long)await_fence_value);
}


/*
 *
 * XR_EXT_local_3d_zone — authored 2D/3D mask consumer (#439 cross-API leg).
 *
 * Port of the D3D11 Phase 1+2 consumer (comp_d3d11_compositor.cpp). The oxr
 * handlers (oxr_local_3d_zone.c) forward here. The mask generalizes the
 * surround path's rect-derived 2D region to an arbitrary scalar mask: the
 * masked-composite shader's use_rect_mask = 0 path lerps
 * M·weave + (1−M)·twod per pixel. Authoring happens on the app's thread,
 * consumption inside d3d12_compositor_layer_commit — both serialize on
 * c->mutex (the entry points lock it; layer_commit already holds it), which
 * also makes submit atomic against an in-flight frame (spec §9 Q3).
 *
 * D3D12 specifics vs the D3D11 reference:
 *  - No immediate context: each authoring op re-arms c->cmd_allocator /
 *    c->cmd_list (Reset → record → Close → Execute → gpu_wait_idle) under
 *    c->mutex — the same pattern d3d12_compositor_capture_atlas_to_png uses;
 *    the list is provably idle whenever an entry point holds the mutex
 *    (every layer_commit exit closes + executes + fence-waits).
 *  - Tier 3 hands the app the ID3D12Resource* (descriptor heaps are
 *    app-owned); the resource is in RENDER_TARGET state and must be returned
 *    to RENDER_TARGET before xrSubmitLocal3DZoneEXT. Same device AND queue
 *    (in-process), so submission order is the sync — no fence.
 *  - Tier 2 uses ClearRenderTargetView's native rect array (one call).
 *
 * #464: the mask + 2D layer are window-sized (client-window pixels, matching
 * XrLocal3DZoneMaskCreateInfoEXT); the composite operates on the window rect
 * at the top-left anchor of the worst-case surface, never beyond it.
 *
 */

/*!
 * Compositor-side state for one authored zone mask. Owned by the oxr handle
 * (oxr_local_3d_zone_ext::comp_mask); the compositor only borrows the
 * pointer in active_zone_mask while the mask is submitted.
 */
struct comp_d3d12_zone_mask
{
	//! Authoring texture: R8_UNORM, M in [0,1] (1 = 3D / keep the weave).
	//! Steady state RENDER_TARGET (clears need it; Tier-3 contract returns it).
	ID3D12Resource *tex;
	//! 1-descriptor RTV heap for tex — used for Tier 1/2 fills (Tier 3 apps
	//! create their own RTV on the returned resource).
	ID3D12DescriptorHeap *rtv_heap;
	//! Staged snapshot sampled by the composite (decouples in-progress
	//! authoring from the frame; refreshed by zone_mask_submit). Steady
	//! state PIXEL_SHADER_RESOURCE.
	ID3D12Resource *staged;
	//! Mask dimensions in client-window pixels.
	uint32_t w, h;
	//! True once submitted at least once (an unsubmitted mask is invisible).
	bool submitted;
};

// Release the compositor-owned zone consumables (scratches) and detach any
// active mask (the oxr handle owns the mask object itself). Idempotent;
// called from d3d12_compositor_destroy only — NOT from the surround release
// path, which also runs on surround re-registration.
static void
d3d12_release_zone_state(struct comp_d3d12_compositor *c)
{
	c->active_zone_mask = nullptr;
	if (c->surround_scratch != nullptr) {
		c->surround_scratch->Release();
		c->surround_scratch = nullptr;
	}
	if (c->weave_scratch != nullptr) {
		c->weave_scratch->Release();
		c->weave_scratch = nullptr;
	}
}

// (Re)allocate a DEFAULT-heap committed scratch texture at the given
// dims/format (no-op when it already matches). D3D12 textures are SRV-able
// without bind flags; created in COMMON (the steady state between frames).
// Returns false on allocation failure (with *res released and nulled).
static bool
d3d12_ensure_scratch(struct comp_d3d12_compositor *c,
                     ID3D12Resource **res,
                     uint32_t w,
                     uint32_t h,
                     DXGI_FORMAT fmt,
                     const char *what)
{
	bool need_alloc = *res == nullptr;
	if (!need_alloc) {
		D3D12_RESOURCE_DESC cur = (*res)->GetDesc();
		need_alloc = (cur.Width != w || cur.Height != h || cur.Format != fmt);
	}
	if (!need_alloc) {
		return true;
	}
	if (*res != nullptr) {
		(*res)->Release();
		*res = nullptr;
	}

	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = w;
	desc.Height = h;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = fmt;
	desc.SampleDesc.Count = 1;
	desc.Flags = D3D12_RESOURCE_FLAG_NONE;

	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;

	HRESULT hr = c->device->CreateCommittedResource(
	    &heap, D3D12_HEAP_FLAG_NONE, &desc,
	    D3D12_RESOURCE_STATE_COMMON, nullptr,
	    IID_PPV_ARGS(res));
	if (FAILED(hr) || *res == nullptr) {
		U_LOG_W("%s: scratch alloc (%ux%u fmt=%u) failed: 0x%08x", what, w, h, fmt, hr);
		*res = nullptr;
		return false;
	}
	return true;
}

// Re-arm the compositor's command list for a zone-authoring op. Caller holds
// c->mutex; the list is closed + the GPU idle whenever that's true (see the
// section comment), so the allocator Reset is safe.
static void
d3d12_zone_cmd_begin(struct comp_d3d12_compositor *c)
{
	c->cmd_allocator->Reset();
	c->cmd_list->Reset(c->cmd_allocator, nullptr);
}

// Close + execute the zone-authoring command list and wait for completion,
// restoring the "closed list, idle GPU" invariant before the mutex releases.
// The CPU wait also makes zone_mask_submit's staged copy atomic against the
// next frame (spec §9 Q3).
static void
d3d12_zone_cmd_execute(struct comp_d3d12_compositor *c)
{
	c->cmd_list->Close();
	ID3D12CommandList *lists[] = {c->cmd_list};
	c->command_queue->ExecuteCommandLists(1, lists);
	gpu_wait_idle(c);
}

// #439 — composite the authored zone mask. Records into the OPEN c->cmd_list
// (both call sites are mid-recording in layer_commit). Runs INSTEAD of the
// rect surround path when an active submitted mask exists (the mask-lerp
// writes every window pixel, so the strip path must not also run). Returns
// false → caller falls through to the rect-strip behavior.
//
// dst_pre_state/dst_post_state parameterize the weave target's states the
// same way d3d12_blit_surround_strips does (COMMON/COMMON on the shared-
// texture path, RENDER_TARGET/RENDER_TARGET on the window-DP path).
//
// #464 window clamping: all inputs are window-sized; the pass writes only the
// window region at the top-left anchor of the (worst-case-allocated) dst.
//
// #439 Phase 2: eff_canvas is the caller's per-frame effective canvas
// (d3d12_effective_canvas under c->mutex) — the window rect while the mask
// is active, so the composite region and the weave region share one
// authority.
static bool
d3d12_composite_zone_mask(struct comp_d3d12_compositor *c,
                          ID3D12Resource *dst,
                          uint64_t dst_rtv,
                          D3D12_RESOURCE_STATES dst_pre_state,
                          D3D12_RESOURCE_STATES dst_post_state,
                          uint32_t dst_w,
                          uint32_t dst_h,
                          const struct u_canvas_rect *eff_canvas)
{
	struct comp_d3d12_zone_mask *mask = c->active_zone_mask;
	if (mask == nullptr || !mask->submitted || dst == nullptr || dst_rtv == 0 || c->renderer == nullptr) {
		return false;
	}

	// The window region inside the worst-case surface (#464). No HWND →
	// the dst is the window-sized target already.
	uint32_t region_w = dst_w;
	uint32_t region_h = dst_h;
	HWND wnd = c->hwnd != nullptr ? c->hwnd : c->app_hwnd;
	if (wnd != nullptr) {
		RECT r;
		if (GetClientRect(wnd, &r) && r.right > 0 && r.bottom > 0) {
			region_w = ((uint32_t)r.right < dst_w) ? (uint32_t)r.right : dst_w;
			region_h = ((uint32_t)r.bottom < dst_h) ? (uint32_t)r.bottom : dst_h;
		}
	}

	// The composite is `texture + mask` (impl doc §2): the surround supplies
	// the 2D pixels. Without one there is nothing to composite — full weave.
	// D3D12 accepts either sync flavor: keyed mutex (spec v6) or fence (v7).
	const bool use_fence = (c->surround_fence != nullptr);
	if (!c->surround_2d.valid || c->surround_texture == nullptr ||
	    (!use_fence && c->surround_mutex == nullptr)) {
		static bool no_surround_logged = false;
		if (!no_surround_logged) {
			U_LOG_W("D3D12 zone mask: no 2D surround registered — mask ignored "
			        "(the composite needs xrSetSharedTextureSurround2D(Fence)EXT for the 2D pixels)");
			no_surround_logged = true;
		}
		return false;
	}

	// Relaxed dims contract (#464): accept a window-sized surround or the
	// legacy display-sized one — content is top-left-anchored in both; we
	// copy only the window region.
	if (c->surround_2d.w < region_w || c->surround_2d.h < region_h) {
		static bool dims_logged = false;
		if (!dims_logged) {
			U_LOG_W("D3D12 zone mask: surround %ux%u smaller than window region %ux%u — "
			        "mask ignored. Re-register surround on window resize.",
			        c->surround_2d.w, c->surround_2d.h, region_w, region_h);
			dims_logged = true;
		}
		return false;
	}

	// Format contract: surround must match dst (same rule as the rect path),
	// and dst must be one of the two formats the composite has a PSO for —
	// app-created shared textures are BGRA8 in the wild, DXGI targets RGBA8.
	D3D12_RESOURCE_DESC sd = c->surround_texture->GetDesc();
	D3D12_RESOURCE_DESC dd = dst->GetDesc();
	if (sd.Format != dd.Format ||
	    (dd.Format != DXGI_FORMAT_R8G8B8A8_UNORM && dd.Format != DXGI_FORMAT_B8G8R8A8_UNORM)) {
		static bool fmt_logged = false;
		if (!fmt_logged) {
			U_LOG_W("D3D12 zone mask: format mismatch — surround=%u, target=%u "
			        "(composite PSOs cover R8G8B8A8/B8G8R8A8 UNORM) — mask ignored",
			        (unsigned)sd.Format, (unsigned)dd.Format);
			fmt_logged = true;
		}
		return false;
	}

	// Window-sized scratches (the shader samples uv [0,1] over the window
	// region, so inputs must carry exactly that region).
	if (!d3d12_ensure_scratch(c, &c->surround_scratch, region_w, region_h, sd.Format, "zone_mask surround")) {
		return false;
	}
	if (!d3d12_ensure_scratch(c, &c->weave_scratch, region_w, region_h, dd.Format, "zone_mask weave")) {
		return false;
	}

	// Surround sync — same flavors as d3d12_blit_surround_strips: fence →
	// queue->Wait gates the caller's later ExecuteCommandLists; mutex →
	// AcquireSync(0, 16) bracket around the recording (the shipping v6
	// pattern), short timeout — skip a frame rather than stall.
	if (use_fence) {
		HRESULT hr = c->command_queue->Wait(c->surround_fence, c->surround_await_fence_value);
		if (FAILED(hr)) {
			static bool wait_logged = false;
			if (!wait_logged) {
				U_LOG_W("D3D12 zone mask: queue->Wait(fence=%p, value=%llu) failed (hr=0x%08x). "
				        "Composite skipped.",
				        (void *)c->surround_fence,
				        (unsigned long long)c->surround_await_fence_value, hr);
				wait_logged = true;
			}
			return false;
		}
	} else {
		HRESULT hr = c->surround_mutex->AcquireSync(0, 16);
		if (FAILED(hr)) {
			return false; // timeout/abandoned — previous frame's pixels stay.
		}
	}

	// Copy the window region of the app surround into its scratch.
	D3D12_RESOURCE_BARRIER enter[2] = {};
	enter[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	enter[0].Transition.pResource = c->surround_texture;
	enter[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
	enter[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	enter[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	enter[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	enter[1].Transition.pResource = c->surround_scratch;
	enter[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
	enter[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
	enter[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	c->cmd_list->ResourceBarrier(2, enter);

	D3D12_TEXTURE_COPY_LOCATION dst_loc = {};
	dst_loc.pResource = c->surround_scratch;
	dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	dst_loc.SubresourceIndex = 0;
	D3D12_TEXTURE_COPY_LOCATION src_loc = {};
	src_loc.pResource = c->surround_texture;
	src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	src_loc.SubresourceIndex = 0;
	D3D12_BOX region_box = {0, 0, 0, region_w, region_h, 1};
	c->cmd_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, &region_box);

	// Surround back to COMMON (cross-process invariant), scratch → sampleable.
	D3D12_RESOURCE_BARRIER exit_sr[2] = {};
	exit_sr[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	exit_sr[0].Transition.pResource = c->surround_texture;
	exit_sr[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
	exit_sr[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
	exit_sr[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	exit_sr[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	exit_sr[1].Transition.pResource = c->surround_scratch;
	exit_sr[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	exit_sr[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	exit_sr[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	c->cmd_list->ResourceBarrier(2, exit_sr);

	if (!use_fence) {
		c->surround_mutex->ReleaseSync(0);
	}

	// Snapshot the window region of the weave (the DP wrote dst; the weave
	// target is RTV-only to the shader, so the lerp reads this copy).
	D3D12_RESOURCE_BARRIER weave_enter[2] = {};
	weave_enter[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	weave_enter[0].Transition.pResource = dst;
	weave_enter[0].Transition.StateBefore = dst_pre_state;
	weave_enter[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	weave_enter[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	weave_enter[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	weave_enter[1].Transition.pResource = c->weave_scratch;
	weave_enter[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
	weave_enter[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
	weave_enter[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	c->cmd_list->ResourceBarrier(2, weave_enter);

	dst_loc.pResource = c->weave_scratch;
	src_loc.pResource = dst;
	c->cmd_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, &region_box);

	// Weave scratch → sampleable; dst → RENDER_TARGET for the composite draw.
	D3D12_RESOURCE_BARRIER weave_exit[2] = {};
	weave_exit[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	weave_exit[0].Transition.pResource = dst;
	weave_exit[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
	weave_exit[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	weave_exit[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	weave_exit[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	weave_exit[1].Transition.pResource = c->weave_scratch;
	weave_exit[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	weave_exit[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	weave_exit[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	c->cmd_list->ResourceBarrier(2, weave_exit);

	// Effective canvas rect clamped to the window region (the shader ignores
	// it on the mask path; kept coherent for the constants anyway). Phase 2:
	// this is the window rect while the mask is active.
	int32_t cx = eff_canvas->valid ? eff_canvas->x : 0;
	int32_t cy = eff_canvas->valid ? eff_canvas->y : 0;
	uint32_t cw = eff_canvas->valid ? eff_canvas->w : region_w;
	uint32_t ch = eff_canvas->valid ? eff_canvas->h : region_h;
	uint32_t cx_u = (cx < 0) ? 0u : (uint32_t)cx;
	uint32_t cy_u = (cy < 0) ? 0u : (uint32_t)cy;
	if (cx_u > region_w)
		cx_u = region_w;
	if (cy_u > region_h)
		cy_u = region_h;
	uint32_t cright = (cx_u + cw > region_w) ? region_w : cx_u + cw;
	uint32_t cbottom = (cy_u + ch > region_h) ? region_h : cy_u + ch;

	xrt_result_t xret = comp_d3d12_renderer_composite_2d_masked(
	    c->renderer, c->cmd_list, dst_rtv, static_cast<uint32_t>(dd.Format), c->surround_scratch, mask->staged,
	    c->weave_scratch, region_w, region_h, (int32_t)cx_u, (int32_t)cy_u, cright - cx_u, cbottom - cy_u);

	// Restore steady states: dst → caller's post state, scratches → COMMON.
	D3D12_RESOURCE_BARRIER restore[3] = {};
	uint32_t n = 0;
	if (dst_post_state != D3D12_RESOURCE_STATE_RENDER_TARGET) {
		restore[n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		restore[n].Transition.pResource = dst;
		restore[n].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
		restore[n].Transition.StateAfter = dst_post_state;
		restore[n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		n++;
	}
	restore[n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	restore[n].Transition.pResource = c->surround_scratch;
	restore[n].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	restore[n].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
	restore[n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	n++;
	restore[n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	restore[n].Transition.pResource = c->weave_scratch;
	restore[n].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	restore[n].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
	restore[n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	n++;
	c->cmd_list->ResourceBarrier(n, restore);

	return xret == XRT_SUCCESS;
}

extern "C" xrt_result_t
comp_d3d12_compositor_zone_mask_create(struct xrt_compositor *xc, uint32_t w, uint32_t h, void **out_mask)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);
	std::lock_guard<std::mutex> lock(c->mutex);

	if (out_mask == nullptr) {
		return XRT_ERROR_ALLOCATION;
	}

	// 0 → runtime chooses: the client-window dims (#464 — the mask is
	// window-sized by definition), falling back to the render surface.
	if (w == 0 || h == 0) {
		HWND wnd = c->hwnd != nullptr ? c->hwnd : c->app_hwnd;
		RECT r;
		if (wnd != nullptr && GetClientRect(wnd, &r) && r.right > 0 && r.bottom > 0) {
			w = (uint32_t)r.right;
			h = (uint32_t)r.bottom;
		} else if (c->shared_texture != nullptr) {
			D3D12_RESOURCE_DESC td = c->shared_texture->GetDesc();
			w = (uint32_t)td.Width;
			h = td.Height;
		} else if (c->target != nullptr) {
			comp_d3d12_target_get_dimensions(c->target, &w, &h);
		}
	}
	if (w == 0 || h == 0) {
		U_LOG_E("zone_mask_create: no window/surface to derive mask dims from");
		return XRT_ERROR_ALLOCATION;
	}

	struct comp_d3d12_zone_mask *mask = U_TYPED_CALLOC(struct comp_d3d12_zone_mask);
	if (mask == nullptr) {
		return XRT_ERROR_ALLOCATION;
	}
	mask->w = w;
	mask->h = h;

	// Authoring texture: committed R8_UNORM render target, steady state
	// RENDER_TARGET, optimized clear = all-3D (matches the default fill).
	D3D12_RESOURCE_DESC td = {};
	td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	td.Width = w;
	td.Height = h;
	td.DepthOrArraySize = 1;
	td.MipLevels = 1;
	td.Format = DXGI_FORMAT_R8_UNORM;
	td.SampleDesc.Count = 1;
	td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_CLEAR_VALUE clear = {};
	clear.Format = DXGI_FORMAT_R8_UNORM;
	clear.Color[0] = 1.0f;

	HRESULT hr = c->device->CreateCommittedResource(
	    &heap, D3D12_HEAP_FLAG_NONE, &td,
	    D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
	    IID_PPV_ARGS(&mask->tex));

	if (SUCCEEDED(hr) && mask->tex != nullptr) {
		D3D12_DESCRIPTOR_HEAP_DESC rtv_desc = {};
		rtv_desc.NumDescriptors = 1;
		rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		hr = c->device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&mask->rtv_heap));
	}
	if (SUCCEEDED(hr) && mask->rtv_heap != nullptr) {
		c->device->CreateRenderTargetView(mask->tex, nullptr,
		                                  mask->rtv_heap->GetCPUDescriptorHandleForHeapStart());
		// Staged snapshot: plain texture, steady PIXEL_SHADER_RESOURCE.
		td.Flags = D3D12_RESOURCE_FLAG_NONE;
		hr = c->device->CreateCommittedResource(
		    &heap, D3D12_HEAP_FLAG_NONE, &td,
		    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
		    IID_PPV_ARGS(&mask->staged));
	}
	if (FAILED(hr) || mask->staged == nullptr) {
		U_LOG_E("zone_mask_create: D3D12 resource creation failed: 0x%08x", hr);
		if (mask->rtv_heap != nullptr) {
			mask->rtv_heap->Release();
		}
		if (mask->tex != nullptr) {
			mask->tex->Release();
		}
		free(mask);
		return XRT_ERROR_ALLOCATION;
	}

	// Default to all-3D (M=1): an unauthored-but-submitted mask degrades to
	// the full weave (the no-2D-declared analog), never a blanked canvas.
	// Also prime the staged copy so a create→submit with no authoring is
	// coherent. Recorded + executed via the zone-op re-arm pattern.
	d3d12_zone_cmd_begin(c);
	const float all_3d[4] = {1.0f, 0.0f, 0.0f, 0.0f};
	c->cmd_list->ClearRenderTargetView(mask->rtv_heap->GetCPUDescriptorHandleForHeapStart(), all_3d, 0, nullptr);

	D3D12_RESOURCE_BARRIER to_copy[2] = {};
	to_copy[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	to_copy[0].Transition.pResource = mask->tex;
	to_copy[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	to_copy[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	to_copy[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	to_copy[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	to_copy[1].Transition.pResource = mask->staged;
	to_copy[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	to_copy[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
	to_copy[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	c->cmd_list->ResourceBarrier(2, to_copy);

	c->cmd_list->CopyResource(mask->staged, mask->tex);

	std::swap(to_copy[0].Transition.StateBefore, to_copy[0].Transition.StateAfter);
	std::swap(to_copy[1].Transition.StateBefore, to_copy[1].Transition.StateAfter);
	c->cmd_list->ResourceBarrier(2, to_copy);
	d3d12_zone_cmd_execute(c);

	// One-off lifecycle event (WARN per the debug-logging convention so it
	// survives the hot-path INFO filter).
	U_LOG_W("zone_mask_create: %ux%u (client-window px)", w, h);
	*out_mask = mask;
	return XRT_SUCCESS;
}

extern "C" xrt_result_t
comp_d3d12_compositor_zone_mask_set_whole(struct xrt_compositor *xc, void *mask_ptr, bool enable_3d)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);
	std::lock_guard<std::mutex> lock(c->mutex);

	struct comp_d3d12_zone_mask *mask = static_cast<struct comp_d3d12_zone_mask *>(mask_ptr);
	if (mask == nullptr || mask->rtv_heap == nullptr) {
		return XRT_ERROR_ALLOCATION;
	}

	// Tier 1: one full clear (mask->tex sits in RENDER_TARGET).
	d3d12_zone_cmd_begin(c);
	const float m[4] = {enable_3d ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};
	c->cmd_list->ClearRenderTargetView(mask->rtv_heap->GetCPUDescriptorHandleForHeapStart(), m, 0, nullptr);
	d3d12_zone_cmd_execute(c);
	return XRT_SUCCESS;
}

extern "C" xrt_result_t
comp_d3d12_compositor_zone_mask_set_rects(struct xrt_compositor *xc,
                                          void *mask_ptr,
                                          uint32_t count,
                                          const struct xrt_rect *rects)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);
	std::lock_guard<std::mutex> lock(c->mutex);

	struct comp_d3d12_zone_mask *mask = static_cast<struct comp_d3d12_zone_mask *>(mask_ptr);
	if (mask == nullptr || mask->rtv_heap == nullptr || (count > 0 && rects == nullptr)) {
		return XRT_ERROR_ALLOCATION;
	}

	// Clamp the rects up-front (client-window px); skip fully-outside /
	// degenerate ones. D3D12's ClearRenderTargetView takes the rect array
	// natively — one call, vs D3D11's per-rect ClearView loop.
	D3D12_RECT *drs = nullptr;
	uint32_t n = 0;
	if (count > 0) {
		drs = U_TYPED_ARRAY_CALLOC(D3D12_RECT, count);
		if (drs == nullptr) {
			return XRT_ERROR_ALLOCATION;
		}
		for (uint32_t i = 0; i < count; i++) {
			int32_t left = rects[i].offset.w;
			int32_t top = rects[i].offset.h;
			int32_t right = left + rects[i].extent.w;
			int32_t bottom = top + rects[i].extent.h;
			if (left < 0) {
				left = 0;
			}
			if (top < 0) {
				top = 0;
			}
			if (right > (int32_t)mask->w) {
				right = (int32_t)mask->w;
			}
			if (bottom > (int32_t)mask->h) {
				bottom = (int32_t)mask->h;
			}
			if (right <= left || bottom <= top) {
				continue;
			}
			drs[n].left = left;
			drs[n].top = top;
			drs[n].right = right;
			drs[n].bottom = bottom;
			n++;
		}
	}

	// M=0 everywhere, then M=1 inside the surviving rects.
	d3d12_zone_cmd_begin(c);
	D3D12_CPU_DESCRIPTOR_HANDLE rtv = mask->rtv_heap->GetCPUDescriptorHandleForHeapStart();
	const float all_2d[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	c->cmd_list->ClearRenderTargetView(rtv, all_2d, 0, nullptr);
	if (n > 0) {
		const float all_3d[4] = {1.0f, 0.0f, 0.0f, 0.0f};
		c->cmd_list->ClearRenderTargetView(rtv, all_3d, n, drs);
	}
	d3d12_zone_cmd_execute(c);

	free(drs);
	return XRT_SUCCESS;
}

extern "C" xrt_result_t
comp_d3d12_compositor_zone_mask_acquire_rt(
    struct xrt_compositor *xc, void *mask_ptr, void **out_resource, uint32_t *out_w, uint32_t *out_h)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);
	std::lock_guard<std::mutex> lock(c->mutex);

	struct comp_d3d12_zone_mask *mask = static_cast<struct comp_d3d12_zone_mask *>(mask_ptr);
	if (mask == nullptr || mask->tex == nullptr || out_resource == nullptr || out_w == nullptr ||
	    out_h == nullptr) {
		return XRT_ERROR_ALLOCATION;
	}

	// The runtime retains ownership of the resource (the app must not
	// Release it); valid until the mask handle is destroyed. The compositor
	// device + queue are the app's own in-process, so the app records its
	// own RTV (descriptor heaps are app-owned in D3D12) and draws directly;
	// submission order is the sync. State contract: handed out in
	// RENDER_TARGET, must be back in RENDER_TARGET before submit.
	*out_resource = mask->tex;
	*out_w = mask->w;
	*out_h = mask->h;
	return XRT_SUCCESS;
}

extern "C" xrt_result_t
comp_d3d12_compositor_zone_mask_submit(struct xrt_compositor *xc, void *mask_ptr)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);
	std::lock_guard<std::mutex> lock(c->mutex);

	struct comp_d3d12_zone_mask *mask = static_cast<struct comp_d3d12_zone_mask *>(mask_ptr);
	if (mask == nullptr || mask->staged == nullptr) {
		return XRT_ERROR_ALLOCATION;
	}

	// Snapshot the authoring texture so in-progress Tier-3 drawing can never
	// tear into a frame, and make this the active mask. Sticky
	// last-submit-wins: it stays active across frames until re-submit or
	// destroy (destroy reverts to the rect-surround behavior). The same-queue
	// ExecuteCommandLists + CPU wait below orders the copy after any Tier-3
	// authoring the app already submitted (no fence — same queue).
	d3d12_zone_cmd_begin(c);

	D3D12_RESOURCE_BARRIER to_copy[2] = {};
	to_copy[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	to_copy[0].Transition.pResource = mask->tex;
	to_copy[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	to_copy[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	to_copy[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	to_copy[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	to_copy[1].Transition.pResource = mask->staged;
	to_copy[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	to_copy[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
	to_copy[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	c->cmd_list->ResourceBarrier(2, to_copy);

	c->cmd_list->CopyResource(mask->staged, mask->tex);

	std::swap(to_copy[0].Transition.StateBefore, to_copy[0].Transition.StateAfter);
	std::swap(to_copy[1].Transition.StateBefore, to_copy[1].Transition.StateAfter);
	c->cmd_list->ResourceBarrier(2, to_copy);
	d3d12_zone_cmd_execute(c);

	mask->submitted = true;
	c->active_zone_mask = mask;
	return XRT_SUCCESS;
}

extern "C" void
comp_d3d12_compositor_zone_mask_destroy(struct xrt_compositor *xc, void *mask_ptr)
{
	struct comp_d3d12_compositor *c = d3d12_comp(xc);
	std::lock_guard<std::mutex> lock(c->mutex);

	struct comp_d3d12_zone_mask *mask = static_cast<struct comp_d3d12_zone_mask *>(mask_ptr);
	if (mask == nullptr) {
		return;
	}
	if (c->active_zone_mask == mask) {
		c->active_zone_mask = nullptr; // revert to rect-surround behavior
	}
	// The frame that might still reference these resources has fence-waited
	// before layer_commit returned (the mutex we hold serializes us behind
	// it), so an immediate Release is safe.
	if (mask->staged != nullptr) {
		mask->staged->Release();
	}
	if (mask->rtv_heap != nullptr) {
		mask->rtv_heap->Release();
	}
	if (mask->tex != nullptr) {
		mask->tex->Release();
	}
	free(mask);
}

/*
 *
 * #439 surround-capture probe (DISPLAYXR_SURROUND_CAPTURE).
 *
 */

// Read back a 4-byte-UNORM (RGBA8/BGRA8) resource region and write it as
// PNG. Generalizes the atlas capture above to any resource/state: barrier
// pre_state → COPY_SOURCE, placed-footprint copy into a transient READBACK
// buffer, restore, execute + wait, repack (+ BGRA→RGBA swizzle) +
// stbi_write_png. The placed footprint MUST use the resource's own format —
// app shared textures are BGRA8 in the wild.
//
// Re-arms c->cmd_allocator / c->cmd_list for its private use — caller must
// ensure the list is CLOSED and the GPU idle on entry (call after the frame
// fence wait, mirroring d3d12_compositor_capture_atlas_to_png).
static bool
d3d12_capture_resource_to_png(struct comp_d3d12_compositor *c,
                              ID3D12Resource *res,
                              uint32_t w,
                              uint32_t h,
                              D3D12_RESOURCE_STATES pre_state,
                              const char *path)
{
	if (res == nullptr || w == 0 || h == 0) {
		return false;
	}

	D3D12_RESOURCE_DESC res_desc = res->GetDesc();
	const bool is_bgra = res_desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
	if (!is_bgra && res_desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
		U_LOG_W("d3d12_capture_resource_to_png: unsupported format %u", (unsigned)res_desc.Format);
		return false;
	}

	// D3D12 readback row pitch must be aligned to 256.
	const UINT64 align = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
	UINT64 row_pitch = ((UINT64)w * 4 + align - 1) & ~(align - 1);
	UINT64 rb_bytes = row_pitch * h;

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

	c->cmd_allocator->Reset();
	c->cmd_list->Reset(c->cmd_allocator, nullptr);

	D3D12_RESOURCE_BARRIER b = {};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = res;
	b.Transition.Subresource = 0;
	b.Transition.StateBefore = pre_state;
	b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	c->cmd_list->ResourceBarrier(1, &b);

	D3D12_TEXTURE_COPY_LOCATION src_loc = {};
	src_loc.pResource = res;
	src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	src_loc.SubresourceIndex = 0;

	D3D12_TEXTURE_COPY_LOCATION dst_loc = {};
	dst_loc.pResource = readback;
	dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	dst_loc.PlacedFootprint.Offset = 0;
	dst_loc.PlacedFootprint.Footprint.Format = res_desc.Format;
	dst_loc.PlacedFootprint.Footprint.Width = w;
	dst_loc.PlacedFootprint.Footprint.Height = h;
	dst_loc.PlacedFootprint.Footprint.Depth = 1;
	dst_loc.PlacedFootprint.Footprint.RowPitch = (UINT)row_pitch;

	D3D12_BOX src_box = {0, 0, 0, w, h, 1};
	c->cmd_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, &src_box);

	std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
	c->cmd_list->ResourceBarrier(1, &b);

	c->cmd_list->Close();
	ID3D12CommandList *lists[] = {c->cmd_list};
	c->command_queue->ExecuteCommandLists(1, lists);
	gpu_wait_idle(c);

	bool ok = false;
	void *mapped = nullptr;
	D3D12_RANGE read_range = {0, (SIZE_T)rb_bytes};
	if (SUCCEEDED(readback->Map(0, &read_range, &mapped)) && mapped != nullptr) {
		size_t tight_pitch = (size_t)w * 4;
		uint8_t *tight = (uint8_t *)malloc(tight_pitch * h);
		if (tight != nullptr) {
			const uint8_t *rb_pixels = (const uint8_t *)mapped;
			for (uint32_t y = 0; y < h; y++) {
				memcpy(tight + (size_t)y * tight_pitch,
				       rb_pixels + (size_t)y * row_pitch,
				       tight_pitch);
			}
			// BGRA targets: swizzle to the RGBA byte order stbi expects.
			// Identical for both A and B captures, so the §6 diff is
			// unaffected.
			if (is_bgra) {
				for (size_t i = 0; i < tight_pitch * h; i += 4) {
					uint8_t tmp = tight[i];
					tight[i] = tight[i + 2];
					tight[i + 2] = tmp;
				}
			}
			// Composited-output alpha is undefined for display output —
			// force opaque so the PNG doesn't render transparent (#425).
			u_image_force_opaque_rgba8(tight, w, h, tight_pitch);
			ok = stbi_write_png(path, (int)w, (int)h, 4, tight, (int)tight_pitch) != 0;
			free(tight);
		}
		D3D12_RANGE empty_range = {0, 0};
		readback->Unmap(0, &empty_range);
	}

	readback->Release();
	return ok;
}

// #439 validation probe — env-gated (DISPLAYXR_SURROUND_CAPTURE=1)
// file-trigger dump of the final surround-composited target. The normal
// POST_COMPOSE capture reads the renderer ATLAS, which the surround/zone
// pass never touches, so the §6 A/B pixel-identity diff needs this
// dedicated probe. Trigger: %TEMP%\displayxr_surround_trigger →
// %TEMP%\displayxr_surround.png. Default-off, zero per-frame cost when
// unset. Called AFTER the frame fence wait (the readback re-arms the
// cmd list), unlike the D3D11 mid-recording call site.
static void
d3d12_maybe_capture_surround_target(struct comp_d3d12_compositor *c,
                                    ID3D12Resource *dst,
                                    uint32_t dst_w,
                                    uint32_t dst_h,
                                    D3D12_RESOURCE_STATES pre_state)
{
	static int enabled = -1;
	if (enabled < 0) {
		const char *e = getenv("DISPLAYXR_SURROUND_CAPTURE");
		enabled = (e != nullptr && e[0] != '\0' && e[0] != '0') ? 1 : 0;
	}
	if (!enabled || dst == nullptr) {
		return;
	}
	static char trig[MAX_PATH] = {0};
	static char outp[MAX_PATH] = {0};
	if (trig[0] == '\0') {
		const char *tmp = getenv("TEMP");
		if (tmp == nullptr || tmp[0] == '\0') {
			tmp = "C:\\Temp";
		}
		snprintf(trig, sizeof(trig), "%s\\displayxr_surround_trigger", tmp);
		snprintf(outp, sizeof(outp), "%s\\displayxr_surround.png", tmp);
	}
	if (GetFileAttributesA(trig) == INVALID_FILE_ATTRIBUTES) {
		return;
	}
	DeleteFileA(trig);
	// The frame's commands may still be in flight (the window path calls us
	// between ExecuteCommandLists and Present); drain before the readback
	// re-arms the cmd allocator. Triggered frames only — zero steady cost.
	gpu_wait_idle(c);
	bool ok = d3d12_capture_resource_to_png(c, dst, dst_w, dst_h, pre_state, outp);
	U_LOG_W("Surround composite capture %s -> %s (zone_mask=%d)", ok ? "written" : "FAILED", outp,
	        c->active_zone_mask != nullptr ? 1 : 0);
}
