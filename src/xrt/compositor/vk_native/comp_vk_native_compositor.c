// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Native Vulkan compositor implementation.
 * @author David Fattal
 * @ingroup comp_vk_native
 *
 * Follows the D3D11 native compositor pattern: direct Vulkan rendering +
 * display processor, no multi-compositor involvement. Uses the app's
 * VkDevice directly via vk_bundle (Monado's Vulkan wrapper).
 */

#include "comp_vk_native_compositor.h"
#include "comp_vk_native_swapchain.h"
#include "comp_vk_native_target.h"
#include "comp_vk_native_renderer.h"

#include "util/comp_layer_accum.h"

#include "xrt/xrt_handles.h"
#include "xrt/xrt_limits.h"
#include "xrt/xrt_vulkan_includes.h"
#include "xrt/xrt_system.h"
#include "xrt/xrt_display_processor.h"

#include "vk/vk_helpers.h"

#include "util/u_logging.h"
#include "util/u_misc.h"
#include "util/u_time.h"
#include "os/os_time.h"

#include "math/m_api.h"

#ifdef XRT_BUILD_DRIVER_QWERTY
#include "qwerty_interface.h"
#endif

#ifdef XRT_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "d3d11/comp_d3d11_window.h"
#endif

#include <string.h>
#include <math.h>

/*!
 * Minimal settings struct for Vulkan compositor.
 */
struct comp_vk_settings
{
	struct
	{
		uint32_t width;
		uint32_t height;
	} preferred;

	int64_t nominal_frame_interval_ns;
};

/*!
 * The Vulkan native compositor structure.
 */
struct comp_vk_native_compositor
{
	//! Base type - must be first!
	struct xrt_compositor_native base;

	//! The device we are rendering for.
	struct xrt_device *xdev;

	//! Vulkan bundle (initialized from app's VkDevice via vk_init_from_given).
	struct vk_bundle vk;

	//! Queue family index.
	uint32_t queue_family_index;

	//! Output target (VkSwapchainKHR).
	struct comp_vk_native_target *target;

	//! Renderer for layer compositing.
	struct comp_vk_native_renderer *renderer;

	//! Accumulated layers for the current frame.
	struct comp_layer_accum layer_accum;

	//! Compositor settings.
	struct comp_vk_settings settings;

#ifdef XRT_OS_WINDOWS
	//! Window handle (either from app or self-created).
	void *hwnd;

	//! Self-created window (NULL if app provided window).
	struct comp_d3d11_window *own_window;

	//! True if we created the window ourselves.
	bool owns_window;
#endif

	//! Shared texture VkImage (imported from HANDLE).
	VkImage shared_image;

	//! Shared texture memory.
	VkDeviceMemory shared_memory;

	//! Shared texture image view.
	VkImageView shared_view;

	//! True if shared texture mode is active.
	bool has_shared_texture;

	//! Shared texture HANDLE (Win32).
	void *shared_texture_handle;

	//! Generic Vulkan display processor (vendor-agnostic weaving).
	struct xrt_display_processor *display_processor;

	//! System devices (for qwerty driver).
	struct xrt_system_devices *xsysd;

	//! Current frame ID.
	int64_t frame_id;

	//! Display refresh rate in Hz.
	float display_refresh_rate;

	//! Time of the last predicted display time.
	uint64_t last_display_time_ns;
};

/*
 *
 * Helper functions
 *
 */

static inline struct comp_vk_native_compositor *
vk_comp(struct xrt_compositor *xc)
{
	return (struct comp_vk_native_compositor *)xc;
}

/*
 *
 * xrt_compositor member functions
 *
 */

static xrt_result_t
vk_compositor_get_swapchain_create_properties(struct xrt_compositor *xc,
                                               const struct xrt_swapchain_create_info *info,
                                               struct xrt_swapchain_create_properties *xsccp)
{
	xsccp->image_count = 3;
	xsccp->extra_bits = (enum xrt_swapchain_usage_bits)0;
	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_create_swapchain(struct xrt_compositor *xc,
                                const struct xrt_swapchain_create_info *info,
                                struct xrt_swapchain **out_xsc)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	return comp_vk_native_swapchain_create(c, info, out_xsc);
}

static xrt_result_t
vk_compositor_import_swapchain(struct xrt_compositor *xc,
                                const struct xrt_swapchain_create_info *info,
                                struct xrt_image_native *native_images,
                                uint32_t image_count,
                                struct xrt_swapchain **out_xsc)
{
	return XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED;
}

static xrt_result_t
vk_compositor_import_fence(struct xrt_compositor *xc,
                            xrt_graphics_sync_handle_t handle,
                            struct xrt_compositor_fence **out_xcf)
{
	return XRT_ERROR_FENCE_CREATE_FAILED;
}

static xrt_result_t
vk_compositor_create_semaphore(struct xrt_compositor *xc,
                                xrt_graphics_sync_handle_t *out_handle,
                                struct xrt_compositor_semaphore **out_xcsem)
{
	return XRT_ERROR_FENCE_CREATE_FAILED;
}

static xrt_result_t
vk_compositor_begin_session(struct xrt_compositor *xc, const struct xrt_begin_session_info *info)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);

	U_LOG_I("VK native compositor session begin - target=%p, renderer=%p",
	        (void *)c->target, (void *)c->renderer);

	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_end_session(struct xrt_compositor *xc)
{
	U_LOG_I("VK native compositor session end");
	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_predict_frame(struct xrt_compositor *xc,
                             int64_t *out_frame_id,
                             int64_t *out_wake_time_ns,
                             int64_t *out_predicted_gpu_time_ns,
                             int64_t *out_predicted_display_time_ns,
                             int64_t *out_predicted_display_period_ns)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);

	c->frame_id++;
	*out_frame_id = c->frame_id;

	int64_t now_ns = (int64_t)os_monotonic_get_ns();
	int64_t period_ns = (int64_t)(U_TIME_1S_IN_NS / c->display_refresh_rate);

	*out_predicted_display_time_ns = now_ns + period_ns * 2;
	*out_predicted_display_period_ns = period_ns;
	*out_wake_time_ns = now_ns;
	*out_predicted_gpu_time_ns = period_ns;

	c->last_display_time_ns = (uint64_t)*out_predicted_display_time_ns;

	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_wait_frame(struct xrt_compositor *xc,
                          int64_t *out_frame_id,
                          int64_t *out_predicted_display_time_ns,
                          int64_t *out_predicted_display_period_ns)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);

#ifdef XRT_OS_WINDOWS
	if (c->owns_window && c->own_window != NULL &&
	    !comp_d3d11_window_is_valid(c->own_window)) {
		U_LOG_I("Window closed - signaling session exit");
		return XRT_ERROR_IPC_FAILURE;
	}
#endif

	int64_t period_ns = (int64_t)(U_TIME_1S_IN_NS / c->display_refresh_rate);

	c->frame_id++;
	*out_frame_id = c->frame_id;

	int64_t now_ns = (int64_t)os_monotonic_get_ns();
	*out_predicted_display_time_ns = now_ns + period_ns * 2;
	*out_predicted_display_period_ns = period_ns;

	c->last_display_time_ns = (uint64_t)*out_predicted_display_time_ns;

	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_mark_frame(struct xrt_compositor *xc,
                          int64_t frame_id,
                          enum xrt_compositor_frame_point point,
                          int64_t when_ns)
{
	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_begin_frame(struct xrt_compositor *xc, int64_t frame_id)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);

#ifdef XRT_OS_WINDOWS
	if (c->hwnd != NULL) {
		RECT rect;
		if (GetClientRect((HWND)c->hwnd, &rect)) {
			uint32_t new_width = (uint32_t)(rect.right - rect.left);
			uint32_t new_height = (uint32_t)(rect.bottom - rect.top);

			if (new_width > 0 && new_height > 0 &&
			    (new_width != c->settings.preferred.width ||
			     new_height != c->settings.preferred.height)) {

				U_LOG_I("Window resized: %ux%u -> %ux%u",
				        c->settings.preferred.width, c->settings.preferred.height,
				        new_width, new_height);

				if (c->target != NULL) {
					comp_vk_native_target_resize(c->target, new_width, new_height);
				}
				c->settings.preferred.width = new_width;
				c->settings.preferred.height = new_height;

				uint32_t new_vw = new_width / 2;
				uint32_t new_vh = new_height;
				comp_vk_native_renderer_resize(c->renderer, new_vw, new_vh, new_height);
			}
		}
	}
#endif

	c->layer_accum.layer_count = 0;
	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_discard_frame(struct xrt_compositor *xc, int64_t frame_id)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	c->layer_accum.layer_count = 0;
	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_layer_begin(struct xrt_compositor *xc, const struct xrt_layer_frame_data *data)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	comp_layer_accum_begin(&c->layer_accum, data);
	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_layer_projection(struct xrt_compositor *xc,
                                struct xrt_device *xdev,
                                struct xrt_swapchain *xsc[XRT_MAX_VIEWS],
                                const struct xrt_layer_data *data)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	comp_layer_accum_projection(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_layer_projection_depth(struct xrt_compositor *xc,
                                      struct xrt_device *xdev,
                                      struct xrt_swapchain *xsc[XRT_MAX_VIEWS],
                                      struct xrt_swapchain *d_xsc[XRT_MAX_VIEWS],
                                      const struct xrt_layer_data *data)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	comp_layer_accum_projection_depth(&c->layer_accum, xsc, d_xsc, data);
	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_layer_quad(struct xrt_compositor *xc,
                          struct xrt_device *xdev,
                          struct xrt_swapchain *xsc,
                          const struct xrt_layer_data *data)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	comp_layer_accum_quad(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_layer_cube(struct xrt_compositor *xc,
                          struct xrt_device *xdev,
                          struct xrt_swapchain *xsc,
                          const struct xrt_layer_data *data)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	comp_layer_accum_cube(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_layer_cylinder(struct xrt_compositor *xc,
                              struct xrt_device *xdev,
                              struct xrt_swapchain *xsc,
                              const struct xrt_layer_data *data)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	comp_layer_accum_cylinder(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_layer_equirect1(struct xrt_compositor *xc,
                               struct xrt_device *xdev,
                               struct xrt_swapchain *xsc,
                               const struct xrt_layer_data *data)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	comp_layer_accum_equirect1(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_layer_equirect2(struct xrt_compositor *xc,
                               struct xrt_device *xdev,
                               struct xrt_swapchain *xsc,
                               const struct xrt_layer_data *data)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	comp_layer_accum_equirect2(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_layer_passthrough(struct xrt_compositor *xc,
                                 struct xrt_device *xdev,
                                 const struct xrt_layer_data *data)
{
	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_layer_window_space(struct xrt_compositor *xc,
                                  struct xrt_device *xdev,
                                  struct xrt_swapchain *xsc,
                                  const struct xrt_layer_data *data)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	comp_layer_accum_window_space(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_layer_commit(struct xrt_compositor *xc, xrt_graphics_sync_handle_t sync_handle)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	struct vk_bundle *vk = &c->vk;

	// Get predicted eye positions
	struct xrt_vec3 left_eye = {-0.032f, 0.0f, 0.6f};
	struct xrt_vec3 right_eye = {0.032f, 0.0f, 0.6f};

	if (c->display_processor != NULL) {
		struct xrt_eye_pair eyes;
		if (xrt_display_processor_get_predicted_eye_positions(c->display_processor, &eyes) &&
		    eyes.valid) {
			left_eye.x = eyes.left.x;
			left_eye.y = eyes.left.y;
			left_eye.z = eyes.left.z;
			right_eye.x = eyes.right.x;
			right_eye.y = eyes.right.y;
			right_eye.z = eyes.right.z;
		}
	}

	// Detect mono submission
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
	if (c->xsysd != NULL) {
		bool force_2d = false;
		bool toggled = qwerty_check_display_mode_toggle(c->xsysd->xdevs, c->xsysd->xdev_count, &force_2d);
		if (toggled) {
			comp_vk_native_compositor_request_display_mode(&c->base.base, !force_2d);
		}
		if (force_2d) {
			is_mono = true;
		}
	}
#endif

	// Get target dimensions
	uint32_t tgt_width = c->settings.preferred.width;
	uint32_t tgt_height = c->settings.preferred.height;
	if (c->target != NULL) {
		comp_vk_native_target_get_dimensions(c->target, &tgt_width, &tgt_height);
	}

	// Render layers to stereo texture
	xrt_result_t xret = comp_vk_native_renderer_draw(
	    c->renderer, &c->layer_accum, &left_eye, &right_eye, tgt_width, tgt_height, is_mono);
	if (xret != XRT_SUCCESS) {
		U_LOG_E("Failed to render layers");
		return xret;
	}

	// If we have a target (window), present to it
	if (c->target != NULL) {
		uint32_t target_index;
		xret = comp_vk_native_target_acquire(c->target, &target_index);
		if (xret != XRT_SUCCESS) {
			U_LOG_E("Failed to acquire target");
			return xret;
		}

		bool weaving_done = false;

		// Use display processor for weaving
		if (!is_mono && c->display_processor != NULL) {
			static bool dp_logged = false;
			if (!dp_logged) {
				U_LOG_W("VK weaving via display processor interface");
				dp_logged = true;
			}

			uint64_t left_view, right_view;
			comp_vk_native_renderer_get_stereo_views(c->renderer, &left_view, &right_view);

			uint32_t view_width, view_height;
			comp_vk_native_renderer_get_view_dimensions(c->renderer, &view_width, &view_height);

			int32_t view_format = comp_vk_native_renderer_get_format(c->renderer);

			uint64_t target_image, target_view;
			comp_vk_native_target_get_current_image(c->target, &target_image, &target_view);

			VkCommandPool cmd_pool = (VkCommandPool)(uintptr_t)
			    comp_vk_native_renderer_get_cmd_pool(c->renderer);

			VkCommandBufferAllocateInfo alloc_info = {
			    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			    .commandPool = cmd_pool,
			    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			    .commandBufferCount = 1,
			};

			VkCommandBuffer cmd;
			VkResult res = vk->vkAllocateCommandBuffers(vk->device, &alloc_info, &cmd);
			if (res == VK_SUCCESS) {
				VkCommandBufferBeginInfo begin_info = {
				    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
				    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
				};
				vk->vkBeginCommandBuffer(cmd, &begin_info);

				VkFramebuffer target_fb = VK_NULL_HANDLE;
				// TODO: Create proper framebuffer when display processor render pass is available

				xrt_display_processor_process_views(
				    c->display_processor,
				    cmd,
				    (VkImageView)(uintptr_t)left_view,
				    (VkImageView)(uintptr_t)right_view,
				    view_width, view_height,
				    (VkFormat_XDP)view_format,
				    target_fb,
				    tgt_width, tgt_height,
				    (VkFormat_XDP)view_format);

				vk->vkEndCommandBuffer(cmd);

				VkSubmitInfo submit_info = {
				    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
				    .commandBufferCount = 1,
				    .pCommandBuffers = &cmd,
				};

				res = vk->vkQueueSubmit(vk->main_queue->queue, 1, &submit_info, VK_NULL_HANDLE);
				if (res == VK_SUCCESS) {
					vk->vkQueueWaitIdle(vk->main_queue->queue);
					weaving_done = true;
				}

				vk->vkFreeCommandBuffers(vk->device, cmd_pool, 1, &cmd);
			}
		}

		// Fallback: blit stereo texture to target
		if (!weaving_done) {
			static bool fallback_warned = false;
			if (!fallback_warned) {
				U_LOG_W("Display processing not available, using fallback blit (mono=%d)", is_mono);
				fallback_warned = true;
			}

			VkCommandPool cmd_pool = (VkCommandPool)(uintptr_t)
			    comp_vk_native_renderer_get_cmd_pool(c->renderer);

			VkCommandBufferAllocateInfo alloc_info = {
			    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			    .commandPool = cmd_pool,
			    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			    .commandBufferCount = 1,
			};

			VkCommandBuffer cmd;
			VkResult res = vk->vkAllocateCommandBuffers(vk->device, &alloc_info, &cmd);
			if (res == VK_SUCCESS) {
				VkCommandBufferBeginInfo begin_info = {
				    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
				    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
				};
				vk->vkBeginCommandBuffer(cmd, &begin_info);

				uint64_t target_image, target_view;
				comp_vk_native_target_get_current_image(c->target, &target_image, &target_view);

				comp_vk_native_renderer_blit_to_target(c->renderer, cmd,
				                                        target_image, tgt_width, tgt_height);

				vk->vkEndCommandBuffer(cmd);

				VkSubmitInfo submit_info = {
				    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
				    .commandBufferCount = 1,
				    .pCommandBuffers = &cmd,
				};

				res = vk->vkQueueSubmit(vk->main_queue->queue, 1, &submit_info, VK_NULL_HANDLE);
				if (res == VK_SUCCESS) {
					vk->vkQueueWaitIdle(vk->main_queue->queue);
				}

				vk->vkFreeCommandBuffers(vk->device, cmd_pool, 1, &cmd);
			}
		}

		// Present
		xret = comp_vk_native_target_present(c->target);

#ifdef XRT_OS_WINDOWS
		if (c->owns_window && c->own_window != NULL) {
			comp_d3d11_window_signal_paint_done(c->own_window);
		}
#endif

		if (xret != XRT_SUCCESS) {
			U_LOG_E("Failed to present");
			return xret;
		}
	}

	return XRT_SUCCESS;
}

static xrt_result_t
vk_compositor_layer_commit_with_semaphore(struct xrt_compositor *xc,
                                           struct xrt_compositor_semaphore *xcsem,
                                           uint64_t value)
{
	return vk_compositor_layer_commit(xc, XRT_GRAPHICS_SYNC_HANDLE_INVALID);
}

static void
vk_compositor_destroy(struct xrt_compositor *xc)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);
	struct vk_bundle *vk = &c->vk;

	U_LOG_I("Destroying VK native compositor");

	vk->vkDeviceWaitIdle(vk->device);

	// Destroy display processor
	xrt_display_processor_destroy(&c->display_processor);

	// Destroy shared texture resources
	if (c->shared_view != VK_NULL_HANDLE) {
		vk->vkDestroyImageView(vk->device, c->shared_view, NULL);
	}
	if (c->shared_image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, c->shared_image, NULL);
	}
	if (c->shared_memory != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, c->shared_memory, NULL);
	}

	if (c->renderer != NULL) {
		comp_vk_native_renderer_destroy(&c->renderer);
	}

	if (c->target != NULL) {
		comp_vk_native_target_destroy(&c->target);
	}

#ifdef XRT_OS_WINDOWS
	if (c->owns_window && c->own_window != NULL) {
		comp_d3d11_window_destroy(&c->own_window);
	}
#endif

	// Note: we do NOT destroy the VkDevice — it belongs to the app.
	// vk_bundle cleanup is minimal (just mutexes).

	free(c);
}

/*
 *
 * Exported functions
 *
 */

xrt_result_t
comp_vk_native_compositor_create(struct xrt_device *xdev,
                                 void *hwnd,
                                 void *vk_instance,
                                 void *vk_physical_device,
                                 void *vk_device,
                                 uint32_t queue_family_index,
                                 uint32_t queue_index,
                                 void *dp_factory_vk,
                                 void *shared_texture_handle,
                                 struct xrt_compositor_native **out_xc)
{
	if (vk_device == NULL) {
		U_LOG_E("VkDevice is null");
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	U_LOG_I("Creating VK native compositor");

	struct comp_vk_native_compositor *c = U_TYPED_CALLOC(struct comp_vk_native_compositor);
	if (c == NULL) {
		return XRT_ERROR_ALLOCATION;
	}

	c->xdev = xdev;
	c->queue_family_index = queue_family_index;
	c->shared_texture_handle = shared_texture_handle;

	// Initialize vk_bundle from the app's existing VkDevice
	VkResult vk_ret = vk_init_from_given(
	    &c->vk,
	    vkGetInstanceProcAddr,
	    (VkInstance)vk_instance,
	    (VkPhysicalDevice)vk_physical_device,
	    (VkDevice)vk_device,
	    queue_family_index,
	    queue_index,
	    false,  // external_fence_fd_enabled
	    false,  // external_semaphore_fd_enabled
	    false,  // timeline_semaphore_enabled
	    false,  // image_format_list_enabled
	    false,  // debug_utils_enabled
	    U_LOGGING_INFO);
	if (vk_ret != VK_SUCCESS) {
		U_LOG_E("Failed to initialize vk_bundle from app device: %d", vk_ret);
		free(c);
		return XRT_ERROR_VULKAN;
	}

#ifdef XRT_OS_WINDOWS
	// Handle window
	if (hwnd != NULL) {
		c->hwnd = hwnd;
		U_LOG_I("Using app-provided window handle: %p", hwnd);
	} else if (shared_texture_handle != NULL) {
		c->hwnd = NULL;
		U_LOG_I("Offscreen mode — no window (shared texture handle: %p)", shared_texture_handle);
	} else {
		uint32_t win_w = xdev->hmd->screens[0].w_pixels;
		uint32_t win_h = xdev->hmd->screens[0].h_pixels;
		if (win_w == 0 || win_h == 0) {
			win_w = 1920;
			win_h = 1080;
		}
		U_LOG_I("Creating self-owned window (%ux%u)", win_w, win_h);
		xrt_result_t xret = comp_d3d11_window_create(win_w, win_h, &c->own_window);
		if (xret != XRT_SUCCESS) {
			U_LOG_E("Failed to create self-owned window");
			free(c);
			return xret;
		}
		c->hwnd = comp_d3d11_window_get_hwnd(c->own_window);
		c->owns_window = true;
		U_LOG_I("Created self-owned window: %p", c->hwnd);
	}
#endif

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

#ifdef XRT_OS_WINDOWS
	if (c->hwnd != NULL) {
		RECT rect;
		if (GetClientRect((HWND)c->hwnd, &rect)) {
			c->settings.preferred.width = (uint32_t)(rect.right - rect.left);
			c->settings.preferred.height = (uint32_t)(rect.bottom - rect.top);
		}
	}
#endif

	// Create output target (VkSwapchainKHR) if we have a window
	if (hwnd != NULL
#ifdef XRT_OS_WINDOWS
	    || c->owns_window
#endif
	) {
		void *target_hwnd = hwnd;
#ifdef XRT_OS_WINDOWS
		if (target_hwnd == NULL) target_hwnd = c->hwnd;
#endif
		xrt_result_t xret = comp_vk_native_target_create(c, target_hwnd,
		                                                  c->settings.preferred.width,
		                                                  c->settings.preferred.height,
		                                                  &c->target);
		if (xret != XRT_SUCCESS) {
			U_LOG_E("Failed to create VK target");
			vk_compositor_destroy(&c->base.base);
			return xret;
		}
	} else {
		c->target = NULL;
		U_LOG_I("No VK target — offscreen shared texture mode");
	}

	// Default refresh rate
	c->display_refresh_rate = 60.0f;

	// Determine view dimensions
	uint32_t view_width = c->settings.preferred.width / 2;
	uint32_t view_height = c->settings.preferred.height;

	// Create display processor via factory
	if (dp_factory_vk != NULL) {
		xrt_dp_factory_vk_fn_t factory = (xrt_dp_factory_vk_fn_t)dp_factory_vk;

		xrt_result_t dp_ret = factory(&c->vk, NULL,
#ifdef XRT_OS_WINDOWS
		                               c->hwnd,
#else
		                               NULL,
#endif
		                               (int32_t)VK_FORMAT_R8G8B8A8_UNORM,
		                               &c->display_processor);
		if (dp_ret != XRT_SUCCESS) {
			U_LOG_W("VK display processor factory failed (error %d), continuing without",
			        (int)dp_ret);
			c->display_processor = NULL;
		} else {
			U_LOG_W("VK display processor created via factory");
		}
	} else {
		U_LOG_W("No VK display processor factory provided");
	}

	// If display processor is available, query display info for view dimensions
	if (c->display_processor != NULL) {
		uint32_t disp_px_w = 0, disp_px_h = 0;
		int32_t disp_left = 0, disp_top = 0;
		if (xrt_display_processor_get_display_pixel_info(
		        c->display_processor, &disp_px_w, &disp_px_h,
		        &disp_left, &disp_top) &&
		    disp_px_w > 0 && disp_px_h > 0) {
			uint32_t base_vw = disp_px_w / 2;
			uint32_t base_vh = disp_px_h;

			float ratio = fminf(
			    (float)c->settings.preferred.width / (float)disp_px_w,
			    (float)c->settings.preferred.height / (float)disp_px_h);
			if (ratio > 1.0f) ratio = 1.0f;

			view_width = (uint32_t)((float)base_vw * ratio);
			view_height = (uint32_t)((float)base_vh * ratio);
		}
	}

	// Create renderer
	uint32_t target_height = (c->display_processor != NULL) ? view_height : c->settings.preferred.height;
	xrt_result_t xret = comp_vk_native_renderer_create(c, view_width, view_height, target_height, &c->renderer);
	if (xret != XRT_SUCCESS) {
		U_LOG_E("Failed to create VK renderer");
		vk_compositor_destroy(&c->base.base);
		return xret;
	}

	// Initialize layer accumulator
	memset(&c->layer_accum, 0, sizeof(c->layer_accum));

	// Populate supported swapchain formats (Vulkan formats)
	uint32_t format_count = 0;
	c->base.base.info.formats[format_count++] = VK_FORMAT_R8G8B8A8_UNORM;
	c->base.base.info.formats[format_count++] = VK_FORMAT_R8G8B8A8_SRGB;
	c->base.base.info.formats[format_count++] = VK_FORMAT_B8G8R8A8_UNORM;
	c->base.base.info.formats[format_count++] = VK_FORMAT_B8G8R8A8_SRGB;
	c->base.base.info.formats[format_count++] = VK_FORMAT_R16G16B16A16_SFLOAT;
	c->base.base.info.formats[format_count++] = VK_FORMAT_R16G16B16A16_UNORM;
	c->base.base.info.formats[format_count++] = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
	c->base.base.info.formats[format_count++] = VK_FORMAT_D16_UNORM;
	c->base.base.info.formats[format_count++] = VK_FORMAT_D32_SFLOAT;
	c->base.base.info.formats[format_count++] = VK_FORMAT_D24_UNORM_S8_UINT;
	c->base.base.info.format_count = format_count;

	// Native compositor is always visible and focused
	c->base.base.info.initial_visible = true;
	c->base.base.info.initial_focused = true;

	// Set up compositor interface
	c->base.base.get_swapchain_create_properties = vk_compositor_get_swapchain_create_properties;
	c->base.base.create_swapchain = vk_compositor_create_swapchain;
	c->base.base.import_swapchain = vk_compositor_import_swapchain;
	c->base.base.import_fence = vk_compositor_import_fence;
	c->base.base.create_semaphore = vk_compositor_create_semaphore;
	c->base.base.begin_session = vk_compositor_begin_session;
	c->base.base.end_session = vk_compositor_end_session;
	c->base.base.wait_frame = vk_compositor_wait_frame;
	c->base.base.predict_frame = vk_compositor_predict_frame;
	c->base.base.mark_frame = vk_compositor_mark_frame;
	c->base.base.begin_frame = vk_compositor_begin_frame;
	c->base.base.discard_frame = vk_compositor_discard_frame;
	c->base.base.layer_begin = vk_compositor_layer_begin;
	c->base.base.layer_projection = vk_compositor_layer_projection;
	c->base.base.layer_projection_depth = vk_compositor_layer_projection_depth;
	c->base.base.layer_quad = vk_compositor_layer_quad;
	c->base.base.layer_cube = vk_compositor_layer_cube;
	c->base.base.layer_cylinder = vk_compositor_layer_cylinder;
	c->base.base.layer_equirect1 = vk_compositor_layer_equirect1;
	c->base.base.layer_equirect2 = vk_compositor_layer_equirect2;
	c->base.base.layer_passthrough = vk_compositor_layer_passthrough;
	c->base.base.layer_window_space = vk_compositor_layer_window_space;
	c->base.base.layer_commit = vk_compositor_layer_commit;
	c->base.base.layer_commit_with_semaphore = vk_compositor_layer_commit_with_semaphore;
	c->base.base.destroy = vk_compositor_destroy;

	*out_xc = &c->base;

	U_LOG_I("VK native compositor created successfully (%ux%u)",
	        c->settings.preferred.width, c->settings.preferred.height);

	return XRT_SUCCESS;
}

bool
comp_vk_native_compositor_get_predicted_eye_positions(struct xrt_compositor *xc,
                                                      struct xrt_vec3 *out_left_eye,
                                                      struct xrt_vec3 *out_right_eye)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);

	if (c->display_processor != NULL) {
		struct xrt_eye_pair eyes;
		if (xrt_display_processor_get_predicted_eye_positions(c->display_processor, &eyes) &&
		    eyes.valid) {
			out_left_eye->x = eyes.left.x;
			out_left_eye->y = eyes.left.y;
			out_left_eye->z = eyes.left.z;
			out_right_eye->x = eyes.right.x;
			out_right_eye->y = eyes.right.y;
			out_right_eye->z = eyes.right.z;
			return true;
		}
	}

	out_left_eye->x = -0.032f;
	out_left_eye->y = 0.0f;
	out_left_eye->z = 0.6f;
	out_right_eye->x = 0.032f;
	out_right_eye->y = 0.0f;
	out_right_eye->z = 0.6f;

	return false;
}

bool
comp_vk_native_compositor_get_display_dimensions(struct xrt_compositor *xc,
                                                  float *out_width_m,
                                                  float *out_height_m)
{
	struct comp_vk_native_compositor *c = vk_comp(xc);

	if (c->display_processor != NULL) {
		return xrt_display_processor_get_display_dimensions(
		    c->display_processor, out_width_m, out_height_m);
	}

	*out_width_m = 0.3f;
	*out_height_m = 0.2f;
	return false;
}

bool
comp_vk_native_compositor_get_window_metrics(struct xrt_compositor *xc,
                                              struct xrt_window_metrics *out_metrics)
{
	if (xc == NULL || out_metrics == NULL) {
		if (out_metrics != NULL) out_metrics->valid = false;
		return false;
	}

	struct comp_vk_native_compositor *c = vk_comp(xc);
	memset(out_metrics, 0, sizeof(*out_metrics));

#ifdef XRT_OS_WINDOWS
	if (c->display_processor == NULL || c->hwnd == NULL) {
		return false;
	}

	uint32_t disp_px_w = 0, disp_px_h = 0;
	int32_t disp_left = 0, disp_top = 0;
	if (!xrt_display_processor_get_display_pixel_info(
	        c->display_processor, &disp_px_w, &disp_px_h,
	        &disp_left, &disp_top)) {
		return false;
	}
	if (disp_px_w == 0 || disp_px_h == 0) return false;

	float disp_w_m = 0.0f, disp_h_m = 0.0f;
	if (!xrt_display_processor_get_display_dimensions(
	        c->display_processor, &disp_w_m, &disp_h_m)) {
		return false;
	}

	RECT rect;
	if (!GetClientRect((HWND)c->hwnd, &rect)) return false;
	uint32_t win_px_w = (uint32_t)(rect.right - rect.left);
	uint32_t win_px_h = (uint32_t)(rect.bottom - rect.top);
	if (win_px_w == 0 || win_px_h == 0) return false;

	POINT client_origin = {0, 0};
	ClientToScreen((HWND)c->hwnd, &client_origin);

	float pixel_size_x = disp_w_m / (float)disp_px_w;
	float pixel_size_y = disp_h_m / (float)disp_px_h;

	out_metrics->display_width_m = disp_w_m;
	out_metrics->display_height_m = disp_h_m;
	out_metrics->display_pixel_width = disp_px_w;
	out_metrics->display_pixel_height = disp_px_h;
	out_metrics->display_screen_left = disp_left;
	out_metrics->display_screen_top = disp_top;

	out_metrics->window_pixel_width = win_px_w;
	out_metrics->window_pixel_height = win_px_h;
	out_metrics->window_screen_left = (int32_t)client_origin.x;
	out_metrics->window_screen_top = (int32_t)client_origin.y;

	out_metrics->window_width_m = (float)win_px_w * pixel_size_x;
	out_metrics->window_height_m = (float)win_px_h * pixel_size_y;

	float win_center_px_x = (float)(client_origin.x - disp_left) + (float)win_px_w / 2.0f;
	float win_center_px_y = (float)(client_origin.y - disp_top) + (float)win_px_h / 2.0f;
	float disp_center_px_x = (float)disp_px_w / 2.0f;
	float disp_center_px_y = (float)disp_px_h / 2.0f;

	out_metrics->window_center_offset_x_m = (win_center_px_x - disp_center_px_x) * pixel_size_x;
	out_metrics->window_center_offset_y_m = -((win_center_px_y - disp_center_px_y) * pixel_size_y);

	out_metrics->valid = true;
	return true;
#else
	(void)c;
	return false;
#endif
}

bool
comp_vk_native_compositor_request_display_mode(struct xrt_compositor *xc, bool enable_3d)
{
	if (xc == NULL) return false;
	struct comp_vk_native_compositor *c = vk_comp(xc);

	if (c->display_processor != NULL) {
		return xrt_display_processor_request_display_mode(c->display_processor, enable_3d);
	}
	return false;
}

void
comp_vk_native_compositor_set_system_devices(struct xrt_compositor *xc,
                                              struct xrt_system_devices *xsysd)
{
	if (xc == NULL) return;
	struct comp_vk_native_compositor *c = vk_comp(xc);

	c->xsysd = xsysd;

	if (xsysd != NULL) {
		U_LOG_I("VK native compositor: system devices set");
	}

#ifdef XRT_OS_WINDOWS
	if (c->owns_window && c->own_window != NULL) {
		comp_d3d11_window_set_system_devices(c->own_window, xsysd);
	}
#endif
}

struct vk_bundle *
comp_vk_native_compositor_get_vk(struct comp_vk_native_compositor *c)
{
	return &c->vk;
}

uint32_t
comp_vk_native_compositor_get_queue_family(struct comp_vk_native_compositor *c)
{
	return c->queue_family_index;
}
