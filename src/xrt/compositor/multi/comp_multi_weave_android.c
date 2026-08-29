// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_weave on the Android service path (#1036) — the Android
 *         sibling of comp_multi_weave_macos.c (#759).
 * @author David Fattal
 * @ingroup comp_multi
 *
 * A window-bound synchronous weave service for present-owners (the Chromium GPU
 * process of `displayxr-browser`, any app that owns its own SurfaceView and
 * present): the caller hands the runtime pre-weave side-by-side stereo pixels +
 * window-relative rect(s) and composites back a weaved shared buffer. The caller
 * NEVER weaves (ADR-007 / ADR-019) — the interlace is the vendor DP's calibrated
 * per-display shader, behind the plug-in.
 *
 * Android platform mapping (vs the macOS original):
 *
 *  - Input texture   = a caller-allocated AHardwareBuffer. It crosses the IPC as
 *    the buffer itself (`AHardwareBuffer_sendHandleToUnixSocket`,
 *    ipc_message_channel_unix.c) and arrives here as an owned
 *    `AHardwareBuffer *`, imported into a VkImage by the tree's existing
 *    @ref vk_create_image_from_native (VK_ANDROID_external_memory_android_hardware_buffer,
 *    dedicated allocation) — no hand-rolled import like the macOS Metal path
 *    needed.
 *  - Output texture  = an AHardwareBuffer the RUNTIME allocates
 *    (@ref ahardwarebuffer_image_allocate, GPU_SAMPLED | GPU_FRAMEBUFFER) and
 *    imports into its own VkImage, so one code path serves both directions. It
 *    is handed back to the caller once per (re)allocation over the same socket.
 *  - Input-ready sync: no keyed mutex exists here either. The contract is the
 *    macOS one — the caller completes its GPU writes into the input buffer
 *    before calling xrWeaveSubmitDXR. An `acquire` sync_file fd is a documented
 *    follow-up (a latency optimisation, not a correctness requirement).
 *  - Completion sync: SYNCHRONOUS — the service vkWaitForFences before the IPC
 *    reply returns, so xrWeaveSubmitDXR returning IS the completion signal.
 *    weave_get_fence reports no fence; XrWeaveOutputDXR::fence stays NULL and
 *    fenceValue is a plain monotonic counter.
 *  - Window geometry: Android has no HWND, and a SurfaceView's on-screen
 *    position is known only to the app (a pure MOVE raises no resize, so nothing
 *    in the pipeline can observe it). The caller therefore publishes it
 *    explicitly with the spec-v7 XrWeaveWindowGeometryDXR chained onto
 *    xrWeaveBindWindow2DXR, and we forward it to the DP's per-window phase slot
 *    (`set_window_screen_rect`, ADR-036 D6 / #1033) before every weave. Without
 *    it the DP weaves display-scoped — correct only for a full-screen window at
 *    the panel origin.
 *  - DP creation is marshalled onto the service main thread: the Leia CNSDK (and
 *    likely other Android vendor DPs) must have their async init kicked off from
 *    a Looper-bearing thread, and this code runs on an IPC handler thread. Same
 *    hop as multi_compositor's own lazy DP init (#510 M2). The vendor DP itself
 *    is a pure offscreen weaver — it takes no window handle and renders into the
 *    VkImage we give it via set_target_color_view.
 *
 * The weave itself is ONE xrt_display_processor process_atlas per submit — the
 * same one-weave-per-frame batch strategy as Windows and macOS (per-rect weave()
 * calls degrade a vendor weaver's predictor).
 *
 * Per-session state only: everything lives on `multi_compositor`, so the path is
 * identical whether the client runs in the main service or in a satellite
 * compositor process (ADR-036 Architecture C, #1053).
 */

#include "xrt/xrt_compositor.h"
#include "xrt/xrt_display_processor.h"

#include "xrt/xrt_display_processor_vk.h"
#include "xrt/xrt_display_metrics.h"
#include "xrt/xrt_handles.h"

#include "util/u_misc.h"
#include "util/u_logging.h"
#include "util/u_handles.h"
#include "util/u_debug.h"

// Kill-switch for the self-submitting-DP split submission (default ON).
DEBUG_GET_ONCE_BOOL_OPTION(dxr_android_weave_split, "DXR_ANDROID_WEAVE_SPLIT", true)
static inline bool
weave_split_enabled(void)
{
	return debug_get_bool_option_dxr_android_weave_split();
}


#include "vk/vk_helpers.h"
#include "vk/vk_local2d_composite.h"

#include "comp_multi_private.h"

#ifdef XRT_OS_ANDROID

#include "android/android_ahardwarebuffer_allocator.h"
#include "android/android_main_thread.h"
#include "android/android_custom_surface.h"
#include "android/android_globals.h"

#include <android/native_window.h>
#include <sys/system_properties.h>
#include <stdlib.h>

#include <android/hardware_buffer.h>

/*
 *
 * Helpers.
 *
 */

/*!
 * Everything is RGBA8 UNORM end-to-end. AHardwareBuffer has no inherent sRGB
 * (see the note in vk_create_image_from_native), so the runtime-side pipeline
 * stays UNORM and the DP factory gets the same format so its render pass and
 * pipelines match the output framebuffer.
 */
#define WEAVE_VK_FORMAT VK_FORMAT_R8G8B8A8_UNORM

static struct vk_bundle *
weave_get_vk(struct multi_compositor *mc)
{
	if (mc == NULL || mc->msc == NULL || mc->msc->target_service == NULL) {
		return NULL;
	}
	return comp_target_service_get_vk(mc->msc->target_service);
}

//! Lazily create the per-client engine lock (multi_compositor is zero-alloced).
static void
weave_ensure_mutex(struct multi_compositor *mc)
{
	os_mutex_lock(&mc->msc->list_and_timing_lock);
	if (!mc->weave.mutex_initialized) {
		os_mutex_init(&mc->weave.mutex);
		mc->weave.mutex_initialized = true;
	}
	os_mutex_unlock(&mc->msc->list_and_timing_lock);
}

//! The swapchain-create-info shape the AHB helpers speak, for one 2D RGBA8 image.
static struct xrt_swapchain_create_info
weave_sci(uint32_t w, uint32_t h, enum xrt_swapchain_usage_bits bits)
{
	struct xrt_swapchain_create_info info = {
	    .create = 0,
	    .bits = bits,
	    .format = WEAVE_VK_FORMAT,
	    .sample_count = 1,
	    .width = w,
	    .height = h,
	    .face_count = 1,
	    .array_size = 1,
	    .mip_count = 1,
	};
	return info;
}

/*!
 * Import an AHardwareBuffer the caller owns into a VkImage (+ full-image view).
 *
 * @p ahb is BORROWED — we take our own reference for the cache, and hand a
 * second one to @ref vk_create_image_from_native, which consumes it (the Android
 * branch unrefs after the import has added the driver's own reference).
 */
static bool
weave_import_ahb(struct vk_bundle *vk,
                 void *ahb,
                 enum xrt_swapchain_usage_bits bits,
                 VkImage *out_image,
                 VkDeviceMemory *out_memory,
                 VkImageView *out_view,
                 uint32_t *out_w,
                 uint32_t *out_h,
                 const char *what)
{
	AHardwareBuffer_Desc desc = {0};
	AHardwareBuffer_describe((AHardwareBuffer *)ahb, &desc);
	if (desc.width == 0 || desc.height == 0) {
		U_LOG_E("weave(#1036): %s AHardwareBuffer has zero dimensions", what);
		return false;
	}

	struct xrt_swapchain_create_info info = weave_sci(desc.width, desc.height, bits);

	// vk_create_image_from_native consumes the handle it is given (Android:
	// unref after the import took its own reference), so hand it a ref we can
	// afford to lose. The caller's ref stays with the cache.
	xrt_graphics_buffer_handle_t consumed = u_graphics_buffer_ref((xrt_graphics_buffer_handle_t)ahb);
	if (!xrt_graphics_buffer_is_valid(consumed)) {
		U_LOG_E("weave(#1036): %s AHardwareBuffer ref failed", what);
		return false;
	}
	struct xrt_image_native image_native = {
	    .handle = consumed,
	    .size = 0,
	    .use_dedicated_allocation = true,
	};

	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkResult ret = vk_create_image_from_native(vk, &info, &image_native, &image, &memory);
	// On any outcome the helper owns what it was handed; clean up if it did not.
	u_graphics_buffer_unref(&image_native.handle);
	if (ret != VK_SUCCESS) {
		U_LOG_E("weave(#1036): vk_create_image_from_native(%s) failed: %s", what, vk_result_string(ret));
		return false;
	}

	VkImageSubresourceRange range = {
	    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	    .levelCount = 1,
	    .layerCount = 1,
	};
	VkImageView view = VK_NULL_HANDLE;
	ret = vk_create_view(vk, image, VK_IMAGE_VIEW_TYPE_2D, WEAVE_VK_FORMAT, range, &view);
	if (ret != VK_SUCCESS) {
		U_LOG_E("weave(#1036): vk_create_view(%s) failed: %s", what, vk_result_string(ret));
		vk->vkFreeMemory(vk->device, memory, NULL);
		vk->vkDestroyImage(vk->device, image, NULL);
		return false;
	}

	*out_image = image;
	*out_memory = memory;
	*out_view = view;
	*out_w = desc.width;
	*out_h = desc.height;
	return true;
}

static void
weave_release_input(struct vk_bundle *vk, struct multi_compositor *mc)
{
	if (mc->weave.in_view != VK_NULL_HANDLE) {
		vk->vkDestroyImageView(vk->device, mc->weave.in_view, NULL);
		mc->weave.in_view = VK_NULL_HANDLE;
	}
	if (mc->weave.in_image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, mc->weave.in_image, NULL);
		mc->weave.in_image = VK_NULL_HANDLE;
	}
	if (mc->weave.in_memory != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, mc->weave.in_memory, NULL);
		mc->weave.in_memory = VK_NULL_HANDLE;
	}
	if (mc->weave.in_ahb != NULL) {
		xrt_graphics_buffer_handle_t h = (xrt_graphics_buffer_handle_t)mc->weave.in_ahb;
		u_graphics_buffer_unref(&h);
		mc->weave.in_ahb = NULL;
	}
	mc->weave.in_w = 0;
	mc->weave.in_h = 0;
}

static void
weave_release_overlay(struct vk_bundle *vk, struct multi_compositor *mc)
{
	if (mc->weave.overlay_view != VK_NULL_HANDLE) {
		vk->vkDestroyImageView(vk->device, mc->weave.overlay_view, NULL);
		mc->weave.overlay_view = VK_NULL_HANDLE;
	}
	if (mc->weave.overlay_image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, mc->weave.overlay_image, NULL);
		mc->weave.overlay_image = VK_NULL_HANDLE;
	}
	if (mc->weave.overlay_memory != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, mc->weave.overlay_memory, NULL);
		mc->weave.overlay_memory = VK_NULL_HANDLE;
	}
	if (mc->weave.overlay_ahb != NULL) {
		xrt_graphics_buffer_handle_t h = (xrt_graphics_buffer_handle_t)mc->weave.overlay_ahb;
		u_graphics_buffer_unref(&h);
		mc->weave.overlay_ahb = NULL;
	}
	mc->weave.overlay_w = 0;
	mc->weave.overlay_h = 0;
}

//! Plain device-local image + view (the SBS scratch atlas / the v6 crop staging).
static bool
weave_create_local(struct vk_bundle *vk,
                   uint32_t w,
                   uint32_t h,
                   VkImage *out_image,
                   VkDeviceMemory *out_memory,
                   VkImageView *out_view)
{
	VkExtent2D extent = {.width = w, .height = h};
	VkResult ret = vk_create_image_simple(vk, extent, WEAVE_VK_FORMAT,
	                                      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
	                                      out_memory, out_image);
	if (ret != VK_SUCCESS) {
		U_LOG_E("weave(#1036): vk_create_image_simple(%ux%u) failed: %s", w, h, vk_result_string(ret));
		return false;
	}

	VkImageSubresourceRange range = {
	    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	    .levelCount = 1,
	    .layerCount = 1,
	};
	ret = vk_create_view(vk, *out_image, VK_IMAGE_VIEW_TYPE_2D, WEAVE_VK_FORMAT, range, out_view);
	if (ret != VK_SUCCESS) {
		U_LOG_E("weave(#1036): vk_create_view(local) failed: %s", vk_result_string(ret));
		return false;
	}
	return true;
}

static void
weave_release_local(struct vk_bundle *vk, VkImage *image, VkDeviceMemory *memory, VkImageView *view)
{
	if (*view != VK_NULL_HANDLE) {
		vk->vkDestroyImageView(vk->device, *view, NULL);
		*view = VK_NULL_HANDLE;
	}
	if (*image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, *image, NULL);
		*image = VK_NULL_HANDLE;
	}
	if (*memory != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, *memory, NULL);
		*memory = VK_NULL_HANDLE;
	}
}

static void
weave_release_scratch(struct vk_bundle *vk, struct multi_compositor *mc)
{
	weave_release_local(vk, &mc->weave.sbs_image, &mc->weave.sbs_memory, &mc->weave.sbs_view);
	mc->weave.sbs_w = 0;
	mc->weave.sbs_h = 0;
}

static void
weave_release_crop(struct vk_bundle *vk, struct multi_compositor *mc)
{
	weave_release_local(vk, &mc->weave.crop_image, &mc->weave.crop_memory, &mc->weave.crop_view);
	mc->weave.crop_w = 0;
	mc->weave.crop_h = 0;
}

/*!
 * AHardwareBuffer-backed output image + view + framebuffer. The buffer is
 * allocated by the runtime and kept referenced for the client's lifetime; each
 * weave_get_output hands the caller its own reference.
 */
static bool
weave_create_output(struct vk_bundle *vk, struct multi_compositor *mc, uint32_t w, uint32_t h)
{
	struct xrt_swapchain_create_info info =
	    weave_sci(w, h, XRT_SWAPCHAIN_USAGE_COLOR | XRT_SWAPCHAIN_USAGE_SAMPLED);

	xrt_graphics_buffer_handle_t ahb = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
	if (ahardwarebuffer_image_allocate(&info, &ahb) != XRT_SUCCESS) {
		U_LOG_E("weave(#1036): output AHardwareBuffer_allocate(%ux%u) failed", w, h);
		return false;
	}

	if (!weave_import_ahb(vk, (void *)ahb, info.bits, &mc->weave.out_image, &mc->weave.out_memory,
	                      &mc->weave.out_view, &mc->weave.out_w, &mc->weave.out_h, "output")) {
		u_graphics_buffer_unref(&ahb);
		return false;
	}
	mc->weave.out_ahb = (void *)ahb; // keep the runtime's reference

	VkFramebufferCreateInfo fb_ci = {
	    .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
	    .renderPass = mc->weave.render_pass,
	    .attachmentCount = 1,
	    .pAttachments = &mc->weave.out_view,
	    .width = w,
	    .height = h,
	    .layers = 1,
	};
	if (vk->vkCreateFramebuffer(vk->device, &fb_ci, NULL, &mc->weave.out_fb) != VK_SUCCESS) {
		U_LOG_E("weave(#1036): vkCreateFramebuffer(output) failed");
		return false;
	}
	return true;
}

static void
weave_release_output(struct vk_bundle *vk, struct multi_compositor *mc)
{
	if (mc->weave.out_fb != VK_NULL_HANDLE) {
		vk->vkDestroyFramebuffer(vk->device, mc->weave.out_fb, NULL);
		mc->weave.out_fb = VK_NULL_HANDLE;
	}
	if (mc->weave.out_view != VK_NULL_HANDLE) {
		vk->vkDestroyImageView(vk->device, mc->weave.out_view, NULL);
		mc->weave.out_view = VK_NULL_HANDLE;
	}
	if (mc->weave.out_image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, mc->weave.out_image, NULL);
		mc->weave.out_image = VK_NULL_HANDLE;
	}
	if (mc->weave.out_memory != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, mc->weave.out_memory, NULL);
		mc->weave.out_memory = VK_NULL_HANDLE;
	}
	if (mc->weave.out_ahb != NULL) {
		xrt_graphics_buffer_handle_t h = (xrt_graphics_buffer_handle_t)mc->weave.out_ahb;
		u_graphics_buffer_unref(&h);
		mc->weave.out_ahb = NULL;
	}
	mc->weave.out_w = 0;
	mc->weave.out_h = 0;
}

//! Marshalling shim: the vendor DP's async init must start on a Looper thread.
struct weave_dp_factory_ctx
{
	xrt_dp_factory_vk_fn_t factory;
	void *vk_bundle;
	void *cmd_pool;
	struct xrt_display_processor **out_xdp;
	xrt_result_t result;
};

static void
weave_run_dp_factory_on_main_thread(void *data)
{
	struct weave_dp_factory_ctx *c = (struct weave_dp_factory_ctx *)data;
	c->result = c->factory(c->vk_bundle, c->cmd_pool, NULL /* window_handle */, (int32_t)WEAVE_VK_FORMAT,
	                       c->out_xdp);
}

/*!
 * One-time engine bring-up: command pool + buffer, fence, render pass
 * (compatible with the DP's own — same single RGBA8 color attachment), and the
 * DP instance from the plug-in's Vulkan factory.
 */
static bool
weave_ensure_engine(struct vk_bundle *vk, struct multi_compositor *mc)
{
	if (mc->weave.engine_initialized) {
		return true;
	}
	if (mc->weave.engine_failed) {
		return false; // one-shot: don't re-run a hopeless bring-up every frame
	}
	mc->weave.engine_failed = true; // cleared on success below

	VkCommandPoolCreateInfo pool_info = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
	    .queueFamilyIndex = vk->main_queue->family_index,
	    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
	};
	if (vk->vkCreateCommandPool(vk->device, &pool_info, NULL, &mc->weave.cmd_pool) != VK_SUCCESS) {
		U_LOG_E("weave(#1036): vkCreateCommandPool failed");
		return false;
	}
	VkCommandBufferAllocateInfo cb_info = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	    .commandPool = mc->weave.cmd_pool,
	    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
	    .commandBufferCount = 1,
	};
	if (vk->vkAllocateCommandBuffers(vk->device, &cb_info, &mc->weave.cmd_post) != VK_SUCCESS) {
		U_LOG_E("weave(#1036): vkAllocateCommandBuffers (post) failed");
		return false;
	}
	if (vk->vkAllocateCommandBuffers(vk->device, &cb_info, &mc->weave.cmd) != VK_SUCCESS) {
		U_LOG_E("weave(#1036): vkAllocateCommandBuffers failed");
		return false;
	}
	VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	if (vk->vkCreateFence(vk->device, &fence_info, NULL, &mc->weave.fence) != VK_SUCCESS) {
		U_LOG_E("weave(#1036): vkCreateFence failed");
		return false;
	}

	// Render pass the output framebuffer is created against. Compatibility with
	// the DP's internal render pass only needs matching attachment count /
	// format / samples (load-store ops and layouts don't participate).
	VkAttachmentDescription color_attachment = {
	    .format = WEAVE_VK_FORMAT,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
	    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
	    .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
	    .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
	    .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	    .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	};
	VkAttachmentReference color_ref = {.attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
	VkSubpassDescription subpass = {
	    .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
	    .colorAttachmentCount = 1,
	    .pColorAttachments = &color_ref,
	};
	VkRenderPassCreateInfo rp_info = {
	    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
	    .attachmentCount = 1,
	    .pAttachments = &color_attachment,
	    .subpassCount = 1,
	    .pSubpasses = &subpass,
	};
	if (vk->vkCreateRenderPass(vk->device, &rp_info, NULL, &mc->weave.render_pass) != VK_SUCCESS) {
		U_LOG_E("weave(#1036): vkCreateRenderPass failed");
		return false;
	}

	// The DP that weaves. Same Vulkan plug-in factory the per-session Android
	// render path drives; the vendor DP takes no window (pure offscreen weaver)
	// and renders into whatever we hand it via set_target_color_view. Its async
	// init must be kicked off from a Looper-bearing thread, and we are on an IPC
	// handler thread — so hop to the service main thread (#510 M2).
	xrt_dp_factory_vk_fn_t factory = (xrt_dp_factory_vk_fn_t)mc->msc->base.info.dp_factory_vk;
	if (factory == NULL) {
		U_LOG_E("weave(#1036): no Vulkan DP factory — cannot weave");
		return false;
	}
	struct weave_dp_factory_ctx ctx = {
	    .factory = factory,
	    .vk_bundle = vk,
	    .cmd_pool = (void *)(uintptr_t)mc->weave.cmd_pool,
	    .out_xdp = &mc->weave.dp,
	    .result = XRT_SUCCESS,
	};
	android_run_on_main_thread_blocking(weave_run_dp_factory_on_main_thread, &ctx);
	if (ctx.result != XRT_SUCCESS || mc->weave.dp == NULL) {
		U_LOG_E("weave(#1036): Vulkan DP factory failed: %d", ctx.result);
		return false;
	}

	mc->weave.engine_initialized = true;
	mc->weave.engine_failed = false;
	U_LOG_W("weave(#1036): Android weave engine initialized (RGBA8, AHardwareBuffer, synchronous)");
	return true;
}

/*
 *
 * Public entry points (called from ipc_server_handler.c).
 *
 */

bool
comp_multi_weave_bind_window(struct xrt_compositor *xc, uint64_t window_id)
{
	struct multi_compositor *mc = multi_compositor(xc);
	if (mc == NULL || mc->msc == NULL) {
		return false;
	}
	weave_ensure_mutex(mc);
	os_mutex_lock(&mc->weave.mutex);
	// Opaque on Android: there is no window handle the runtime can act on. The
	// geometry that actually matters arrives through set_window_geometry.
	mc->weave.window_id = window_id;
	os_mutex_unlock(&mc->weave.mutex);
	U_LOG_W("weave(#1036): bound present-owner window id 0x%" PRIx64, window_id);
	return true;
}

bool
comp_multi_weave_set_window_geometry(struct xrt_compositor *xc,
                                     int32_t origin_x,
                                     int32_t origin_y,
                                     uint32_t client_w,
                                     uint32_t client_h,
                                     int32_t display_id)
{
	struct multi_compositor *mc = multi_compositor(xc);
	if (mc == NULL || mc->msc == NULL || client_w == 0 || client_h == 0) {
		return false;
	}
	weave_ensure_mutex(mc);
	os_mutex_lock(&mc->weave.mutex);
	const bool changed = !mc->weave.have_geometry || mc->weave.win_x != origin_x || mc->weave.win_y != origin_y ||
	                     mc->weave.win_w != client_w || mc->weave.win_h != client_h ||
	                     mc->weave.win_display_id != display_id;
	mc->weave.have_geometry = true;
	mc->weave.win_x = origin_x;
	mc->weave.win_y = origin_y;
	mc->weave.win_w = client_w;
	mc->weave.win_h = client_h;
	mc->weave.win_display_id = display_id;
	mc->weave.geometry_dirty = mc->weave.geometry_dirty || changed;
	os_mutex_unlock(&mc->weave.mutex);
	if (changed) {
		// Lifecycle event (the present-owner's window moved / resized), and the
		// caller is expected to re-publish every frame — so log only on change.
		U_LOG_W("weave(#1036): present-owner window %d,%d %ux%u on display %d", origin_x, origin_y, client_w,
		        client_h, display_id);
	}
	return true;
}


/*
 *
 * Arch-C weave satellite (#1277 P0) — service-presented weave.
 *
 * `debug.dxr.weave_satellite=1` diverts the woven output onto a SERVICE-owned
 * full-panel overlay surface instead of returning it to the caller. The
 * overlay is the #558 machinery (android_custom_surface: an Activity-free
 * TYPE_APPLICATION_OVERLAY MonadoView, translucent with debug.dxr.transparent)
 * — so it is never scaled by the window manager: it IS the physical-pixel
 * canvas, which is the entire point. An OEM freeform "mini window" scales the
 * app's surface AFTER an in-app weave (measured 0.67x on NP02J,
 * browser#173/#186), destroying the interlace; here the weave lands after any
 * such transform, so the scaled window is structurally weavable.
 *
 * The caller needs no changes: spec v7 already allows returning no woven
 * texture (comp_multi_weave_export_output reports none while the satellite is
 * active), so the browser's over-plane draws nothing and the page's own 2D
 * shows under the overlay.
 *
 * Placement: the woven output is blitted at the caller-reported window
 * geometry; `debug.dxr.satellite_scale` (float, default 1.0) maps a logical
 * window size to its physical footprint under an OEM container scale until
 * the platform exposes the real factor (OEM-asks list).
 *
 * Every failure is one-shot latched (sat_failed) and falls back to the
 * return-the-output path bit-for-bit.
 */

static bool
weave_satellite_wanted(struct multi_compositor *mc)
{
	if (!mc->weave.sat_checked) {
		char value[PROP_VALUE_MAX] = {0};
		mc->weave.sat_enabled =
		    __system_property_get("debug.dxr.weave_satellite", value) > 0 && value[0] == '1';
		mc->weave.sat_checked = true;
		if (mc->weave.sat_enabled) {
			U_LOG_W("weave satellite(#1277): ENABLED via debug.dxr.weave_satellite");
		}
	}
	return mc->weave.sat_enabled && !mc->weave.sat_failed;
}

static float
weave_satellite_scale(void)
{
	char value[PROP_VALUE_MAX] = {0};
	if (__system_property_get("debug.dxr.satellite_scale", value) > 0 && value[0] != '\0') {
		float f = (float)atof(value);
		if (f > 0.05f && f <= 4.0f) {
			return f;
		}
	}
	return 1.0f;
}

//! Queue-idles first: swapchain images may be in flight with the present engine.
static void
weave_satellite_release(struct vk_bundle *vk, struct multi_compositor *mc)
{
	if (vk != NULL && vk->device != VK_NULL_HANDLE &&
	    (mc->weave.sat_swapchain != VK_NULL_HANDLE || mc->weave.sat_surface != VK_NULL_HANDLE)) {
		vk_queue_lock(vk->main_queue);
		vk->vkQueueWaitIdle(vk->main_queue->queue);
		vk_queue_unlock(vk->main_queue);
	}
	if (mc->weave.sat_acquire_sem != VK_NULL_HANDLE) {
		vk->vkDestroySemaphore(vk->device, mc->weave.sat_acquire_sem, NULL);
		mc->weave.sat_acquire_sem = VK_NULL_HANDLE;
	}
	if (mc->weave.sat_done_sem != VK_NULL_HANDLE) {
		vk->vkDestroySemaphore(vk->device, mc->weave.sat_done_sem, NULL);
		mc->weave.sat_done_sem = VK_NULL_HANDLE;
	}
	if (mc->weave.sat_fence != VK_NULL_HANDLE) {
		vk->vkDestroyFence(vk->device, mc->weave.sat_fence, NULL);
		mc->weave.sat_fence = VK_NULL_HANDLE;
	}
	if (mc->weave.sat_cmd != VK_NULL_HANDLE && mc->weave.cmd_pool != VK_NULL_HANDLE) {
		vk->vkFreeCommandBuffers(vk->device, mc->weave.cmd_pool, 1, &mc->weave.sat_cmd);
		mc->weave.sat_cmd = VK_NULL_HANDLE;
	}
	if (mc->weave.sat_swapchain != VK_NULL_HANDLE) {
		vk->vkDestroySwapchainKHR(vk->device, mc->weave.sat_swapchain, NULL);
		mc->weave.sat_swapchain = VK_NULL_HANDLE;
	}
	if (mc->weave.sat_surface != VK_NULL_HANDLE) {
		vk->vkDestroySurfaceKHR(vk->instance, mc->weave.sat_surface, NULL);
		mc->weave.sat_surface = VK_NULL_HANDLE;
	}
	if (mc->weave.sat_csurface != NULL) {
		android_custom_surface_destroy(&mc->weave.sat_csurface);
	}
	mc->weave.sat_image_count = 0;
}

static bool
weave_satellite_ensure(struct vk_bundle *vk, struct multi_compositor *mc)
{
	if (mc->weave.sat_swapchain != VK_NULL_HANDLE) {
		return true;
	}

	// 1. The service-owned overlay surface (the #558 machinery, no Activity).
	if (mc->weave.sat_csurface == NULL) {
		struct _JavaVM *vm = android_globals_get_vm();
		void *ctx = android_globals_get_context();
		if (vm == NULL || ctx == NULL) {
			U_LOG_E("weave satellite(#1277): no JavaVM/context; disabled");
			goto fail;
		}
		if (!android_custom_surface_can_draw_overlays(vm, ctx)) {
			U_LOG_E("weave satellite(#1277): SYSTEM_ALERT_WINDOW not granted; disabled");
			goto fail;
		}
		mc->weave.sat_csurface =
		    android_custom_surface_async_start(vm, ctx, /*display_id=*/0, "DisplayXR Weave Satellite", 0);
		if (mc->weave.sat_csurface == NULL) {
			U_LOG_E("weave satellite(#1277): overlay surface start failed; disabled");
			goto fail;
		}
	}
	ANativeWindow *window = android_custom_surface_wait_get_surface(mc->weave.sat_csurface, 2000);
	if (window == NULL) {
		U_LOG_E("weave satellite(#1277): overlay ANativeWindow timeout; disabled");
		goto fail;
	}

	// 2. VkSurface + swapchain on it (comp_window_android's recipe).
	{
		VkAndroidSurfaceCreateInfoKHR surface_info = {
		    .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
		    .window = window,
		};
		VkResult ret = vk->vkCreateAndroidSurfaceKHR(vk->instance, &surface_info, NULL, &mc->weave.sat_surface);
		if (ret == VK_ERROR_NATIVE_WINDOW_IN_USE_KHR) {
			// A just-destroyed swapchain's BufferQueue connection lingers for
			// a few ms after vkDestroySwapchainKHR. Transient: retry on later
			// frames rather than latching a permanent fallback; bound it so a
			// genuinely stuck window still falls back.
			static int in_use_retries = 0;
			if (++in_use_retries <= 120) {
				return false; // not latched — next submit retries
			}
			U_LOG_E("weave satellite(#1277): window stuck IN_USE after %d retries; disabled",
			        in_use_retries);
			goto fail;
		}
		if (ret != VK_SUCCESS) {
			U_LOG_E("weave satellite(#1277): vkCreateAndroidSurfaceKHR: %s; disabled",
			        vk_result_string(ret));
			goto fail;
		}

		VkSurfaceCapabilitiesKHR caps = {0};
		ret = vk->vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk->physical_device, mc->weave.sat_surface,
		                                                    &caps);
		if (ret != VK_SUCCESS) {
			U_LOG_E("weave satellite(#1277): surface caps: %s; disabled", vk_result_string(ret));
			goto fail;
		}

		// Translucency is load-bearing: everything outside the woven rect must
		// show the desktop through. Prefer pre-multiplied, then inherit (the
		// window's own TRANSLUCENT format then decides), and only fall back to
		// opaque with a loud log (a fully-opaque black overlay would "work" but
		// blank the launcher — better visible in the log than mysterious).
		VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
		// debug.dxr.satellite_opaque=1: A/B knob — composite the overlay OPAQUE
		// (alpha ignored; surround is black, page hidden). Diagnostic for the
		// woven-alpha under-blend theory: the DP writes per-pixel alpha in the
		// compose-under model, and a premultiplied overlay then BLENDS the
		// woven rect with whatever the caller drew beneath (its mono scratch),
		// which reads as single-eye crosstalk. Opaque compositing makes that
		// blend impossible; if the crosstalk vanishes, the fix is forcing the
		// woven rect's alpha to 1 in the satellite copy.
		char opq[PROP_VALUE_MAX] = {0};
		const bool force_opaque =
		    __system_property_get("debug.dxr.satellite_opaque", opq) > 0 && opq[0] == '1';
		if (!force_opaque && (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR)) {
			alpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
		} else if (!force_opaque && (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)) {
			alpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
		} else if (force_opaque) {
			U_LOG_W("weave satellite(#1277): OPAQUE composite forced (debug.dxr.satellite_opaque)");
		} else {
			U_LOG_W("weave satellite(#1277): no translucent compositeAlpha; overlay will be OPAQUE");
		}

		// Colorspace: the weave output is CALIBRATED PANEL VALUES (the
		// anti-crosstalk precompensation is baked in), so any compositor
		// color transform after the weave breaks eye separation. The in-app
		// path's buffer rides dataspace UNKNOWN (no CSC); an explicit
		// SRGB_NONLINEAR swapchain may be color-managed by the DPU. Prefer
		// PASS_THROUGH (no CSC, matching the in-app path); fall back to
		// whatever the surface offers, loudly.
		VkColorSpaceKHR colorspace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
		{
			VkSurfaceFormatKHR formats[16];
			uint32_t n_formats = ARRAY_SIZE(formats);
			VkResult fret = vk->vkGetPhysicalDeviceSurfaceFormatsKHR(
			    vk->physical_device, mc->weave.sat_surface, &n_formats, formats);
			if (fret == VK_SUCCESS || fret == VK_INCOMPLETE) {
				bool has_pass_through = false;
				for (uint32_t i = 0; i < n_formats; i++) {
					U_LOG_W("weave satellite(#1277): surface format[%u] vkformat=%d colorspace=%d",
					        i, (int)formats[i].format, (int)formats[i].colorSpace);
					if (formats[i].format == WEAVE_VK_FORMAT &&
					    formats[i].colorSpace == VK_COLOR_SPACE_PASS_THROUGH_EXT) {
						has_pass_through = true;
					}
				}
				if (has_pass_through) {
					colorspace = VK_COLOR_SPACE_PASS_THROUGH_EXT;
					U_LOG_W("weave satellite(#1277): using PASS_THROUGH colorspace (no compositor CSC)");
				}
			}
		}

		uint32_t min_images = caps.minImageCount + 1;
		if (caps.maxImageCount != 0 && min_images > caps.maxImageCount) {
			min_images = caps.maxImageCount;
		}

		VkSwapchainCreateInfoKHR sc_info = {
		    .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
		    .surface = mc->weave.sat_surface,
		    .minImageCount = min_images,
		    .imageFormat = WEAVE_VK_FORMAT,
		    .imageColorSpace = colorspace,
		    .imageExtent = caps.currentExtent,
		    .imageArrayLayers = 1,
		    .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		    .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
		    .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
		    .compositeAlpha = alpha,
		    .presentMode = VK_PRESENT_MODE_FIFO_KHR,
		    .clipped = VK_TRUE,
		};
		ret = vk->vkCreateSwapchainKHR(vk->device, &sc_info, NULL, &mc->weave.sat_swapchain);
		if (ret != VK_SUCCESS) {
			U_LOG_E("weave satellite(#1277): vkCreateSwapchainKHR: %s; disabled",
			        vk_result_string(ret));
			goto fail;
		}
		mc->weave.sat_w = caps.currentExtent.width;
		mc->weave.sat_h = caps.currentExtent.height;

		// Overlay-local vs panel coordinates. The overlay view is inset by the
		// system bars, so window-rect (panel) coordinates must be shifted by
		// the overlay's own origin before blitting. Only the size difference
		// is queryable from here; ASSUME the inset is top/left (the status bar
		// on this hardware — landscape top bar measured 60 px, matching
		// panel_h - extent). Wrong on a bottom-inset device by exactly the
		// inset; revisit with a real view-origin query when the Java side
		// grows one.
		mc->weave.sat_off_x = 0;
		mc->weave.sat_off_y = 0;
		{
			struct xrt_android_display_metrics metrics = {0};
			struct _JavaVM *mvm = android_globals_get_vm();
			void *mctx = android_globals_get_context();
			if (mvm != NULL && mctx != NULL &&
			    android_custom_surface_get_display_metrics(mvm, mctx, &metrics)) {
				int32_t pw = metrics.width_pixels, ph = metrics.height_pixels;
				// Metrics are natural-orientation; match against the
				// current-extent orientation.
				if ((mc->weave.sat_w > mc->weave.sat_h) != (pw > ph)) {
					int32_t t = pw;
					pw = ph;
					ph = t;
				}
				if (ph > (int32_t)mc->weave.sat_h) {
					mc->weave.sat_off_y = ph - (int32_t)mc->weave.sat_h;
				}
				if (pw > (int32_t)mc->weave.sat_w) {
					mc->weave.sat_off_x = pw - (int32_t)mc->weave.sat_w;
				}
				U_LOG_W("weave satellite(#1277): overlay origin on panel: (%d,%d) "
				        "(panel %dx%d, extent %ux%u)",
				        mc->weave.sat_off_x, mc->weave.sat_off_y, pw, ph, mc->weave.sat_w,
				        mc->weave.sat_h);
			}
		}

		mc->weave.sat_image_count = ARRAY_SIZE(mc->weave.sat_images);
		ret = vk->vkGetSwapchainImagesKHR(vk->device, mc->weave.sat_swapchain,
		                                  &mc->weave.sat_image_count, mc->weave.sat_images);
		if (ret != VK_SUCCESS && ret != VK_INCOMPLETE) {
			U_LOG_E("weave satellite(#1277): vkGetSwapchainImagesKHR: %s; disabled",
			        vk_result_string(ret));
			goto fail;
		}
		for (uint32_t i = 0; i < mc->weave.sat_image_count; i++) {
			mc->weave.sat_image_first[i] = true;
		}
	}

	// 3. Sync + command buffer (out of the engine's own pool).
	{
		VkSemaphoreCreateInfo sem_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		if (vk->vkCreateSemaphore(vk->device, &sem_info, NULL, &mc->weave.sat_acquire_sem) != VK_SUCCESS ||
		    vk->vkCreateSemaphore(vk->device, &sem_info, NULL, &mc->weave.sat_done_sem) != VK_SUCCESS ||
		    vk->vkCreateFence(vk->device, &fence_info, NULL, &mc->weave.sat_fence) != VK_SUCCESS) {
			U_LOG_E("weave satellite(#1277): sync object creation failed; disabled");
			goto fail;
		}
		VkCommandBufferAllocateInfo alloc = {
		    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		    .commandPool = mc->weave.cmd_pool,
		    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		    .commandBufferCount = 1,
		};
		if (vk->vkAllocateCommandBuffers(vk->device, &alloc, &mc->weave.sat_cmd) != VK_SUCCESS) {
			U_LOG_E("weave satellite(#1277): command buffer allocation failed; disabled");
			goto fail;
		}
	}

	U_LOG_W("weave satellite(#1277): overlay swapchain live %ux%u images=%u — service-presented weave ACTIVE",
	        mc->weave.sat_w, mc->weave.sat_h, mc->weave.sat_image_count);
	return true;

fail:
	weave_satellite_release(vk, mc);
	mc->weave.sat_failed = true;
	return false;
}

/*!
 * Blit the completed woven output onto the overlay at the window's physical
 * rect and present. Called with the weave mutex held, AFTER the weave fence
 * has been waited (out_image is GPU-complete, COLOR_ATTACHMENT_OPTIMAL).
 * Best-effort: any failure releases the swapchain; the next submit either
 * recreates it (transient, e.g. OUT_OF_DATE) or latched-fails via ensure.
 */
static void
weave_satellite_present(struct vk_bundle *vk,
                        struct multi_compositor *mc,
                        uint32_t rect_count,
                        const struct xrt_rect *rects)
{
	if (!weave_satellite_ensure(vk, mc)) {
		return;
	}

	uint32_t idx = 0;
	VkResult ret = vk->vkAcquireNextImageKHR(vk->device, mc->weave.sat_swapchain, 100ULL * 1000ULL * 1000ULL,
	                                         mc->weave.sat_acquire_sem, VK_NULL_HANDLE, &idx);
	if (ret == VK_ERROR_OUT_OF_DATE_KHR) {
		U_LOG_W("weave satellite(#1277): swapchain out of date — recreating next frame");
		weave_satellite_release(vk, mc);
		return;
	}
	// SUBOPTIMAL is Android's steady state on a rotated panel when the
	// swapchain's preTransform is IDENTITY (the compositor applies the
	// rotation). It is NOT a recreate signal — treating it as one is a
	// teardown/recreate thrash that ends in VK_ERROR_NATIVE_WINDOW_IN_USE.
	if (ret != VK_SUCCESS && ret != VK_SUBOPTIMAL_KHR) {
		U_LOG_E("weave satellite(#1277): acquire: %s", vk_result_string(ret));
		weave_satellite_release(vk, mc);
		return;
	}

	VkCommandBuffer cmd = mc->weave.sat_cmd;
	vk->vkResetCommandBuffer(cmd, 0);
	VkCommandBufferBeginInfo begin = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	vk->vkBeginCommandBuffer(cmd, &begin);

	VkImageSubresourceRange range = {
	    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1};

	// Swapchain image -> TRANSFER_DST (UNDEFINED on first use; discard old
	// present contents otherwise — we repaint the whole surface every frame).
	VkImageMemoryBarrier to_dst = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = 0,
	    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .image = mc->weave.sat_images[idx],
	    .subresourceRange = range,
	};
	// Woven output -> TRANSFER_SRC (the weave's render pass left it
	// COLOR_ATTACHMENT_OPTIMAL; its writes are fence-complete).
	VkImageMemoryBarrier out_to_src = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .image = mc->weave.out_image,
	    .subresourceRange = range,
	};
	VkImageMemoryBarrier pre[2] = {to_dst, out_to_src};
	vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
	                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, pre);
	mc->weave.sat_image_first[idx] = false;

	// Everything outside the woven rect is TRANSPARENT — the desktop shows
	// through (pre-multiplied alpha: all-zero = fully clear).
	VkClearColorValue clear = {.float32 = {0.f, 0.f, 0.f, 0.f}};
	vk->vkCmdClearColorImage(cmd, mc->weave.sat_images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1,
	                         &range);

	// Placement: caller-reported window geometry x the container-scale prop,
	// clamped to the panel. No geometry yet -> panel origin at output size.
	const float scale = weave_satellite_scale();
	// Phase-meter nudge (diagnostic): debug.dxr.satellite_shift_x / _y shift the
	// blit destination by whole pixels (may be negative). A left/right-asymmetric
	// crosstalk that a +-1..3 px x-nudge cures IS a sub-lens-pitch displacement,
	// and the value that cures it measures the displacement directly.
	int32_t nudge_x = 0, nudge_y = 0;
	{
		char v[PROP_VALUE_MAX] = {0};
		if (__system_property_get("debug.dxr.satellite_shift_x", v) > 0) {
			nudge_x = atoi(v);
		}
		v[0] = 0;
		if (__system_property_get("debug.dxr.satellite_shift_y", v) > 0) {
			nudge_y = atoi(v);
		}
		static int32_t last_nx = INT32_MIN, last_ny = INT32_MIN;
		if (nudge_x != last_nx || nudge_y != last_ny) {
			last_nx = nudge_x;
			last_ny = nudge_y;
			U_LOG_W("weave satellite(#1277): blit nudge (%d,%d)", nudge_x, nudge_y);
		}
	}
	int32_t dst_x = 0, dst_y = 0;
	int32_t dst_w = (int32_t)mc->weave.out_w, dst_h = (int32_t)mc->weave.out_h;
	if (mc->weave.have_geometry) {
		// Panel coordinates -> overlay-local (see sat_off_* in the header).
		dst_x = mc->weave.win_x - mc->weave.sat_off_x + nudge_x;
		dst_y = mc->weave.win_y - mc->weave.sat_off_y + nudge_y;
		dst_w = (int32_t)((float)mc->weave.win_w * scale + 0.5f);
		dst_h = (int32_t)((float)mc->weave.win_h * scale + 0.5f);
	}
	if (dst_x < 0) dst_x = 0;
	if (dst_y < 0) dst_y = 0;
	if (dst_x + dst_w > (int32_t)mc->weave.sat_w) dst_w = (int32_t)mc->weave.sat_w - dst_x;
	if (dst_y + dst_h > (int32_t)mc->weave.sat_h) dst_h = (int32_t)mc->weave.sat_h - dst_y;

	// Per-RECT blits, not the whole output: the DP's non-tile region carries a
	// low-alpha backdrop (the compose-under transparency model), and copying it
	// panel-wide put a visible dark film over the page (measured round 5). The
	// submitted rects are exactly the woven regions — copy those, leave every
	// other overlay pixel at the transparent clear. This is also occlusion-lite:
	// the overlay only ever owns pixels a weave actually claimed this frame.
	const float sx = (dst_w > 0 && mc->weave.out_w > 0) ? (float)dst_w / (float)mc->weave.out_w : 1.0f;
	const float sy = (dst_h > 0 && mc->weave.out_h > 0) ? (float)dst_h / (float)mc->weave.out_h : 1.0f;
	for (uint32_t i = 0; i < rect_count && rects != NULL; i++) {
		const struct xrt_rect *r = &rects[i];
		if (r->extent.w <= 0 || r->extent.h <= 0) {
			continue;
		}
		int32_t sx0 = r->offset.w, sy0 = r->offset.h;
		int32_t sx1 = sx0 + (int32_t)r->extent.w, sy1 = sy0 + (int32_t)r->extent.h;
		if (sx0 < 0) sx0 = 0;
		if (sy0 < 0) sy0 = 0;
		if (sx1 > (int32_t)mc->weave.out_w) sx1 = (int32_t)mc->weave.out_w;
		if (sy1 > (int32_t)mc->weave.out_h) sy1 = (int32_t)mc->weave.out_h;
		if (sx1 <= sx0 || sy1 <= sy0) {
			continue;
		}
		int32_t dx0 = dst_x + (int32_t)((float)sx0 * sx + 0.5f);
		int32_t dy0 = dst_y + (int32_t)((float)sy0 * sy + 0.5f);
		int32_t dx1 = dst_x + (int32_t)((float)sx1 * sx + 0.5f);
		int32_t dy1 = dst_y + (int32_t)((float)sy1 * sy + 0.5f);
		if (dx1 > (int32_t)mc->weave.sat_w) dx1 = (int32_t)mc->weave.sat_w;
		if (dy1 > (int32_t)mc->weave.sat_h) dy1 = (int32_t)mc->weave.sat_h;
		if (dx1 <= dx0 || dy1 <= dy0) {
			continue;
		}
		VkImageBlit blit = {
		    .srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
		    .srcOffsets = {{sx0, sy0, 0}, {sx1, sy1, 1}},
		    .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
		    .dstOffsets = {{dx0, dy0, 0}, {dx1, dy1, 1}},
		};
		// NEAREST: at 1:1 this is exact-copy semantics with zero filter risk;
		// under a scale the physical-size weave (the pending P0 half) is the
		// real answer, not a nicer filter.
		vk->vkCmdBlitImage(cmd, mc->weave.out_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		                   mc->weave.sat_images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
		                   VK_FILTER_NEAREST);
	}

	// Output back to what the next weave's render pass LOADs; swapchain image
	// to PRESENT.
	VkImageMemoryBarrier out_back = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
	    .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	    .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .image = mc->weave.out_image,
	    .subresourceRange = range,
	};
	VkImageMemoryBarrier sc_to_present = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	    .dstAccessMask = 0,
	    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	    .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
	    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .image = mc->weave.sat_images[idx],
	    .subresourceRange = range,
	};
	VkImageMemoryBarrier post[2] = {out_back, sc_to_present};
	vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
	                         0, 0, NULL, 0, NULL, 2, post);
	vk->vkEndCommandBuffer(cmd);

	VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	VkSubmitInfo submit = {
	    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	    .waitSemaphoreCount = 1,
	    .pWaitSemaphores = &mc->weave.sat_acquire_sem,
	    .pWaitDstStageMask = &wait_stage,
	    .commandBufferCount = 1,
	    .pCommandBuffers = &cmd,
	    .signalSemaphoreCount = 1,
	    .pSignalSemaphores = &mc->weave.sat_done_sem,
	};
	vk_queue_lock(vk->main_queue);
	ret = vk->vkQueueSubmit(vk->main_queue->queue, 1, &submit, mc->weave.sat_fence);
	vk_queue_unlock(vk->main_queue);
	if (ret != VK_SUCCESS) {
		U_LOG_E("weave satellite(#1277): blit submit: %s", vk_result_string(ret));
		weave_satellite_release(vk, mc);
		return;
	}
	// Same bounded synchronous contract as the weave itself.
	ret = vk->vkWaitForFences(vk->device, 1, &mc->weave.sat_fence, VK_TRUE, 1000ULL * 1000ULL * 1000ULL);
	vk->vkResetFences(vk->device, 1, &mc->weave.sat_fence);
	if (ret != VK_SUCCESS) {
		U_LOG_E("weave satellite(#1277): blit did not complete: %s", vk_result_string(ret));
		weave_satellite_release(vk, mc);
		return;
	}

	VkPresentInfoKHR present = {
	    .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
	    .waitSemaphoreCount = 1,
	    .pWaitSemaphores = &mc->weave.sat_done_sem,
	    .swapchainCount = 1,
	    .pSwapchains = &mc->weave.sat_swapchain,
	    .pImageIndices = &idx,
	};
	vk_queue_lock(vk->main_queue);
	ret = vk->vkQueuePresentKHR(vk->main_queue->queue, &present);
	vk_queue_unlock(vk->main_queue);
	if (ret == VK_ERROR_OUT_OF_DATE_KHR) {
		weave_satellite_release(vk, mc);
	} else if (ret != VK_SUCCESS && ret != VK_SUBOPTIMAL_KHR) {
		U_LOG_E("weave satellite(#1277): present: %s", vk_result_string(ret));
		weave_satellite_release(vk, mc);
	}
}

bool
comp_multi_weave_submit(struct xrt_compositor *xc,
                        xrt_graphics_buffer_handle_t in_handle,
                        int32_t rect_x,
                        int32_t rect_y,
                        uint32_t rect_w,
                        uint32_t rect_h,
                        uint32_t rect_count,
                        const struct xrt_rect *rects,
                        xrt_graphics_buffer_handle_t overlay_handle,
                        bool weave_frame_first,
                        const struct xrt_weave_atlas_layout *layout,
                        uint32_t flat_rect_count,
                        const struct xrt_rect *flat_rects,
                        uint32_t *out_width,
                        uint32_t *out_height,
                        uint64_t *out_fence_value,
                        struct xrt_eye_positions *out_eyes)
{
	// v8 (browser#88): accepted and ignored — the per-region hardware wish is
	// published through the D3D11 service's zone-wish channel and has no Android
	// counterpart yet. Ignoring it is CONFORMANT, not a stub: the wish is advisory
	// and hardware-only (ADR-027 D6), so the woven pixels are unaffected and the
	// panel simply stays as 3D as it was pre-v8.
	(void)flat_rect_count;
	(void)flat_rects;

	struct multi_compositor *mc = multi_compositor(xc);
	if (mc == NULL || mc->msc == NULL || in_handle == XRT_GRAPHICS_BUFFER_HANDLE_INVALID) {
		return false;
	}
	struct vk_bundle *vk = weave_get_vk(mc);
	if (vk == NULL) {
		return false;
	}

	// The handler hands us ownership of the AHardwareBuffer references the IPC
	// receive produced; we either adopt them into the caches or release them.
	xrt_graphics_buffer_handle_t input = in_handle;
	xrt_graphics_buffer_handle_t overlay = overlay_handle; // may be INVALID

	weave_ensure_mutex(mc);
	os_mutex_lock(&mc->weave.mutex);

	bool ok = false;
	do {
		if (!weave_ensure_engine(vk, mc)) {
			break;
		}

		// (Re)import the input on identity change. AHardwareBuffer pointers are
		// stable for as long as anyone holds a reference, and we hold one for the
		// cached import — so pointer identity is a sound cache key here.
		if (mc->weave.in_image == VK_NULL_HANDLE || mc->weave.in_ahb != (void *)input) {
			weave_release_input(vk, mc);
			if (!weave_import_ahb(vk, (void *)input,
			                      XRT_SWAPCHAIN_USAGE_SAMPLED | XRT_SWAPCHAIN_USAGE_TRANSFER_SRC,
			                      &mc->weave.in_image, &mc->weave.in_memory, &mc->weave.in_view,
			                      &mc->weave.in_w, &mc->weave.in_h, "input")) {
				break;
			}
			mc->weave.in_ahb = (void *)input; // adopt the reference
			mc->weave.in_first_use = true;
			input = XRT_GRAPHICS_BUFFER_HANDLE_INVALID; // ownership transferred
		}

		// v4 overlay: same, into the SEPARATE overlay cache.
		if (overlay != XRT_GRAPHICS_BUFFER_HANDLE_INVALID &&
		    (mc->weave.overlay_image == VK_NULL_HANDLE || mc->weave.overlay_ahb != (void *)overlay)) {
			weave_release_overlay(vk, mc);
			if (weave_import_ahb(vk, (void *)overlay, XRT_SWAPCHAIN_USAGE_SAMPLED,
			                     &mc->weave.overlay_image, &mc->weave.overlay_memory,
			                     &mc->weave.overlay_view, &mc->weave.overlay_w, &mc->weave.overlay_h,
			                     "overlay")) {
				mc->weave.overlay_ahb = (void *)overlay;
				mc->weave.overlay_first_use = true;
				overlay = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
				U_LOG_W("weave(#1036) v4: overlay import cached (%ux%u)", mc->weave.overlay_w,
				        mc->weave.overlay_h);
			}
		}

		// Spec-v6 N-view atlas (#774): the caller already packed the atlas the way
		// every DisplayXR app does — tiles contiguous from the top-left at
		// (content_view_w, content_view_h) in a worst-case-sized input (ADR-010).
		// No SBS scratch, no per-rect unpack blits, no firstChunk clear: crop the
		// top-left packed region if the worst case is bigger (ADR-030
		// crop-before-DP) and weave once.
		const bool nview = (layout != NULL && layout->view_count > 0);
		uint32_t cvw = 0, cvh = 0, packed_w = 0, packed_h = 0;
		if (nview) {
			cvw = layout->content_view_w;
			cvh = layout->content_view_h;
			packed_w = layout->tile_columns * cvw;
			packed_h = layout->tile_rows * cvh;
			if (packed_w > mc->weave.in_w || packed_h > mc->weave.in_h) {
				U_LOG_E("weave(#1036) v6: packed region %ux%u exceeds input atlas %ux%u "
				        "(views=%u grid=%ux%u content=%ux%u)",
				        packed_w, packed_h, mc->weave.in_w, mc->weave.in_h, layout->view_count,
				        layout->tile_columns, layout->tile_rows, cvw, cvh);
				break;
			}
		}

		// Output dims: v6 = one content view; batch = the (window-client-sized)
		// input; legacy = rect offset+extent.
		uint32_t want_w = 0, want_h = 0;
		if (nview) {
			want_w = cvw;
			want_h = cvh;
		} else if (rect_count > 0) {
			want_w = mc->weave.in_w;
			want_h = mc->weave.in_h;
		} else {
			want_w = (uint32_t)rect_x + rect_w;
			want_h = (uint32_t)rect_y + rect_h;
		}
		if (want_w == 0 || want_h == 0) {
			break;
		}

		// (Re)allocate output (+ SBS scratch on the non-v6 paths) on resize.
		if (mc->weave.out_image == VK_NULL_HANDLE || mc->weave.out_w != want_w || mc->weave.out_h != want_h) {
			// Never yank resources out from under in-flight GPU work.
			vk_queue_lock(vk->main_queue);
			vk->vkQueueWaitIdle(vk->main_queue->queue);
			vk_queue_unlock(vk->main_queue);
			weave_release_output(vk, mc);
			weave_release_scratch(vk, mc);
			if (!weave_create_output(vk, mc, want_w, want_h)) {
				break;
			}
			if (!nview) {
				if (!weave_create_local(vk, want_w * 2, want_h, &mc->weave.sbs_image,
				                        &mc->weave.sbs_memory, &mc->weave.sbs_view)) {
					break;
				}
				mc->weave.sbs_w = want_w * 2;
				mc->weave.sbs_h = want_h;
				mc->weave.sbs_first_use = true;
			}
			U_LOG_W("weave(#1036): output %ux%u (%s layout)", want_w, want_h,
			        nview ? "v6 N-view" : (rect_count > 0 ? "v3 batch" : "legacy"));
		}

		// v6 crop staging: (re)create when the packed region is smaller than the
		// input (the common ADR-010 case). Zero-copy (packed == input) samples the
		// input directly and needs no crop image.
		const bool v6_zero_copy = nview && (packed_w == mc->weave.in_w && packed_h == mc->weave.in_h);
		if (nview && !v6_zero_copy &&
		    (mc->weave.crop_image == VK_NULL_HANDLE || mc->weave.crop_w != packed_w ||
		     mc->weave.crop_h != packed_h)) {
			vk_queue_lock(vk->main_queue);
			vk->vkQueueWaitIdle(vk->main_queue->queue);
			vk_queue_unlock(vk->main_queue);
			weave_release_crop(vk, mc);
			if (!weave_create_local(vk, packed_w, packed_h, &mc->weave.crop_image, &mc->weave.crop_memory,
			                        &mc->weave.crop_view)) {
				break;
			}
			mc->weave.crop_w = packed_w;
			mc->weave.crop_h = packed_h;
			mc->weave.crop_first_use = true;
		}

		// ---- Record ----
		VkCommandBuffer cmd = mc->weave.cmd;
		vk->vkResetCommandBuffer(cmd, 0);
		VkCommandBufferBeginInfo begin = {
		    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		};
		if (vk->vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS) {
			break;
		}

		VkImageSubresourceRange range = {
		    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1};

		VkImage dp_src_image = mc->weave.sbs_image;
		VkImageView dp_src_view = mc->weave.sbs_view;
		uint32_t atlas_view_w = mc->weave.out_w;
		uint32_t atlas_view_h = mc->weave.out_h;
		uint32_t grid_cols = 2, grid_rows = 1;

		if (nview) {
			if (v6_zero_copy) {
				// The packed atlas fills the input exactly — sample it directly.
				VkImageMemoryBarrier in_to_read = {
				    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				    .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
				    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
				    .oldLayout = mc->weave.in_first_use ? VK_IMAGE_LAYOUT_UNDEFINED
				                                        : VK_IMAGE_LAYOUT_GENERAL,
				    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				    .image = mc->weave.in_image,
				    .subresourceRange = range,
				};
				mc->weave.in_first_use = false;
				vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
				                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1,
				                         &in_to_read);
				dp_src_image = mc->weave.in_image;
				dp_src_view = mc->weave.in_view;
			} else {
				// Crop the top-left packed region — tiles are contiguous, so it is
				// a single rectangle: ONE box copy, not a per-tile gather.
				VkImageMemoryBarrier in_to_src = {
				    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				    .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
				    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
				    .oldLayout = mc->weave.in_first_use ? VK_IMAGE_LAYOUT_UNDEFINED
				                                        : VK_IMAGE_LAYOUT_GENERAL,
				    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				    .image = mc->weave.in_image,
				    .subresourceRange = range,
				};
				mc->weave.in_first_use = false;
				vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
				                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
				                         &in_to_src);

				VkImageMemoryBarrier crop_to_dst = {
				    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				    .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
				    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
				    .oldLayout = mc->weave.crop_first_use ? VK_IMAGE_LAYOUT_UNDEFINED
				                                          : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				    .image = mc->weave.crop_image,
				    .subresourceRange = range,
				};
				mc->weave.crop_first_use = false;
				vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
				                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
				                         &crop_to_dst);

				VkImageCopy copy = {
				    .srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
				    .srcOffset = {0, 0, 0},
				    .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
				    .dstOffset = {0, 0, 0},
				    .extent = {packed_w, packed_h, 1},
				};
				vk->vkCmdCopyImage(cmd, mc->weave.in_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				                   mc->weave.crop_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

				VkImageMemoryBarrier crop_to_read = {
				    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
				    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
				    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				    .image = mc->weave.crop_image,
				    .subresourceRange = range,
				};
				vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
				                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1,
				                         &crop_to_read);
				dp_src_image = mc->weave.crop_image;
				dp_src_view = mc->weave.crop_view;
			}
			atlas_view_w = cvw;
			atlas_view_h = cvh;
			grid_cols = layout->tile_columns;
			grid_rows = layout->tile_rows;
		} else {

		// Input: keep GENERAL across frames (UNDEFINED would discard the caller's
		// pixels); the barrier makes external writes visible.
		VkImageMemoryBarrier in_barrier = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
		    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		    .oldLayout = mc->weave.in_first_use ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
		    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
		    .image = mc->weave.in_image,
		    .subresourceRange = range,
		};
		mc->weave.in_first_use = false;
		vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
		                         NULL, 0, NULL, 1, &in_barrier);

		// Scratch -> TRANSFER_DST (persists across frames: stale regions from
		// closed elements re-weave harmlessly; the caller composites back only its
		// current rects — same contract as the Windows output).
		VkImageMemoryBarrier sbs_to_dst = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
		    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		    .oldLayout = mc->weave.sbs_first_use ? VK_IMAGE_LAYOUT_UNDEFINED
		                                         : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		    .image = mc->weave.sbs_image,
		    .subresourceRange = range,
		};
		mc->weave.sbs_first_use = false;
		vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
		                         0, NULL, 0, NULL, 1, &sbs_to_dst);

		// v5 firstChunk (browser#22): clear the SBS scratch to premultiplied
		// transparent on the first submit of a frame, so regions BETWEEN the woven
		// tiles come out alpha 0 instead of stale.
		if (weave_frame_first) {
			VkClearColorValue sbs_transparent = {.float32 = {0.0f, 0.0f, 0.0f, 0.0f}};
			vk->vkCmdClearColorImage(cmd, mc->weave.sbs_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			                         &sbs_transparent, 1, &range);
			// Order the whole-image clear before the per-rect blits (both TRANSFER
			// writes to overlapping regions — no implicit ordering within a stage).
			VkImageMemoryBarrier clear_to_blit = {
			    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			    .image = mc->weave.sbs_image,
			    .subresourceRange = range,
			};
			vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
			                         0, NULL, 0, NULL, 1, &clear_to_blit);
		}

		// Blit each rect's squeezed-SBS halves into the two atlas tiles: left half
		// -> left tile at the rect's window position (stretched to full rect
		// width), right half -> right tile offset by out_w.
		struct xrt_rect legacy_rect = {
		    .offset = {.w = 0, .h = 0},
		    .extent = {.w = (int)want_w, .h = (int)want_h},
		};
		const struct xrt_rect *blit_rects = rect_count > 0 ? rects : &legacy_rect;
		uint32_t blit_count = rect_count > 0 ? rect_count : 1;

		for (uint32_t i = 0; i < blit_count; i++) {
			// xrt_offset names its fields w/h; they hold x/y here.
			int32_t rx = blit_rects[i].offset.w;
			int32_t ry = blit_rects[i].offset.h;
			int32_t rw = blit_rects[i].extent.w;
			int32_t rh = blit_rects[i].extent.h;
			if (rw <= 0 || rh <= 0) {
				continue;
			}
			// Clamp to the input.
			if (rx < 0 || ry < 0 || (uint32_t)(rx + rw) > mc->weave.in_w ||
			    (uint32_t)(ry + rh) > mc->weave.in_h) {
				continue;
			}
			int32_t half = rw / 2;
			if (half <= 0) {
				continue;
			}

			VkImageBlit blits[2] = {
			    // Left eye: input rect's left half -> left tile, unsqueezed.
			    {
			        .srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
			        .srcOffsets = {{rx, ry, 0}, {rx + half, ry + rh, 1}},
			        .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
			        .dstOffsets = {{rx, ry, 0}, {rx + rw, ry + rh, 1}},
			    },
			    // Right eye: input rect's right half -> right tile (+out_w).
			    {
			        .srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
			        .srcOffsets = {{rx + half, ry, 0}, {rx + rw, ry + rh, 1}},
			        .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
			        .dstOffsets = {{(int32_t)mc->weave.out_w + rx, ry, 0},
			                       {(int32_t)mc->weave.out_w + rx + rw, ry + rh, 1}},
			    },
			};
			vk->vkCmdBlitImage(cmd, mc->weave.in_image, VK_IMAGE_LAYOUT_GENERAL, mc->weave.sbs_image,
			                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, blits, VK_FILTER_LINEAR);
		}

		// Scratch -> SHADER_READ for the DP sample.
		VkImageMemoryBarrier sbs_to_read = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
		    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		    .image = mc->weave.sbs_image,
		    .subresourceRange = range,
		};
		vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
		                         0, NULL, 0, NULL, 1, &sbs_to_read);
		} // end !nview (legacy/batch SBS record)

		// Output -> COLOR_ATTACHMENT (fully re-rendered every submit, so the
		// discard from UNDEFINED is fine).
		VkImageMemoryBarrier out_to_attach = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = 0,
		    .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		    .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		    .image = mc->weave.out_image,
		    .subresourceRange = range,
		};
		vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1,
		                         &out_to_attach);

		// Per-window weave phase (#1033 / ADR-036 D6): the interlace phase is
		// absolute-screen, so hand the DP the present-owner's client rect on the
		// panel before the weave. Sticky on the DP side and deduped there, but
		// re-asserted every submit so a DP created later never misses it. No
		// geometry (or a DP without the slot) = display-scoped, which is correct
		// only for a full-screen window at the panel origin.
		if (mc->weave.have_geometry) {
			bool fed = xrt_display_processor_vk_set_window_screen_rect(
			    (struct xrt_display_processor_vk *)mc->weave.dp, mc->weave.win_x, mc->weave.win_y,
			    mc->weave.win_w, mc->weave.win_h, mc->weave.win_display_id);
			if (mc->weave.geometry_dirty) {
				mc->weave.geometry_dirty = false;
				U_LOG_W("weave(#1036): window rect %s the DP phase slot", fed ? "fed to" : "NOT accepted by");
			}
		}

		// SELF-SUBMITTING DP ORDERING (the one-frame scroll-trail root cause).
		// A self-submitting DP (Leia CNSDK: is_self_submitting == true) submits
		// its interlace batch to the queue DURING process_atlas — i.e. BEFORE the
		// cmd buffer holding THIS frame's input blits into the SBS scratch is
		// submitted (below). Batches execute in submission order, so the
		// interlacer sampled the scratch BEFORE this frame's blits executed and
		// wove LAST frame's tiles at LAST frame's positions — a clean, exactly
		// one-frame positional trail under scroll (invisible at rest because the
		// scratch then holds identical pixels). sim_display records its weave
		// INTO cmd and was never affected. Fix: flush the pre-weave batch to the
		// queue first; same-queue submission order then guarantees the DP's
		// self-submitted batch executes after it. No CPU wait needed here — the
		// existing fence on the final batch still provides the synchronous
		// completion contract for the whole frame.
		// Kill-switch: debug.xrt.DXR_ANDROID_WEAVE_SPLIT=0 restores the old
		// single-submission behaviour.
		bool weave_split = weave_split_enabled() && xrt_display_processor_is_self_submitting(mc->weave.dp);
		if (weave_split) {
			if (vk->vkEndCommandBuffer(cmd) != VK_SUCCESS) {
				U_LOG_E("weave(#1036): vkEndCommandBuffer (pre-weave) failed");
				break;
			}
			VkSubmitInfo pre_submit = {
			    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			    .commandBufferCount = 1,
			    .pCommandBuffers = &cmd,
			};
			vk_queue_lock(vk->main_queue);
			VkResult pre_ret = vk->vkQueueSubmit(vk->main_queue->queue, 1, &pre_submit, VK_NULL_HANDLE);
			vk_queue_unlock(vk->main_queue);
			if (pre_ret != VK_SUCCESS) {
				U_LOG_E("weave(#1036): pre-weave vkQueueSubmit failed: %s", vk_result_string(pre_ret));
				break;
			}
			// Everything after the weave records into the second buffer; the
			// final submit + fence + wait below then retire BOTH batches (same
			// queue, in order), so next frame's reset of either is safe.
			cmd = mc->weave.cmd_post;
			vk->vkResetCommandBuffer(cmd, 0);
			VkCommandBufferBeginInfo post_begin = {
			    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
			};
			if (vk->vkBeginCommandBuffer(cmd, &post_begin) != VK_SUCCESS) {
				U_LOG_E("weave(#1036): vkBeginCommandBuffer (post-weave) failed");
				break;
			}
			static uint64_t split_frames = 0;
			if ((split_frames++ % 300) == 0) {
				U_LOG_W("weave(#1036): SELF-SUBMIT SPLIT active (frame=%llu): pre-weave batch flushed before process_atlas",
				        (unsigned long long)split_frames);
			}
		}

		// ONE process_atlas per submit. Legacy/batch: 2x1 SBS, per-eye dims = the
		// window. v6: the caller's grid at content_view dims.
		xrt_display_processor_set_target_color_view(mc->weave.dp, mc->weave.out_view);
		xrt_display_processor_process_atlas(mc->weave.dp, cmd,                      //
		                                    (VkImage_XDP)dp_src_image, dp_src_view, //
		                                    atlas_view_w, atlas_view_h,             //
		                                    grid_cols, grid_rows,                   //
		                                    (VkFormat_XDP)WEAVE_VK_FORMAT,          //
		                                    mc->weave.out_fb,                       //
		                                    (VkImage_XDP)mc->weave.out_image,       //
		                                    mc->weave.out_w, mc->weave.out_h,       //
		                                    (VkFormat_XDP)WEAVE_VK_FORMAT,          //
		                                    0, 0, 0, 0);

		// v6: restore the input atlas to GENERAL so the next frame's barrier
		// (oldLayout=GENERAL) is correct and the caller's external writes land into
		// a defined layout.
		if (nview) {
			VkImageMemoryBarrier in_restore = {
			    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			    .srcAccessMask = v6_zero_copy ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_TRANSFER_READ_BIT,
			    .dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
			    .oldLayout = v6_zero_copy ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
			                              : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
			    .image = mc->weave.in_image,
			    .subresourceRange = range,
			};
			vk->vkCmdPipelineBarrier(cmd,
			                         v6_zero_copy ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
			                                      : VK_PIPELINE_STAGE_TRANSFER_BIT,
			                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &in_restore);
		}

		// v4 overlay atlas (browser#18): composite the caller's window-sized
		// premultiplied-alpha 2D atlas OVER the woven output with a premul "over"
		// blend, so crisp 2D lands on top of the interlaced 3D at screen depth.
		// The overlay is NOT woven — it is drawn after process_atlas onto the same
		// output attachment.
		if (mc->weave.overlay_image != VK_NULL_HANDLE) {
			bool blend_ready = mc->weave.overlay_blend_initialized;
			if (!blend_ready) {
				blend_ready = vk_local2d_composite_init(&mc->weave.overlay_blend, vk, WEAVE_VK_FORMAT,
				                                        WEAVE_VK_FORMAT);
				mc->weave.overlay_blend_initialized = blend_ready;
				if (blend_ready) {
					U_LOG_W("weave(#1036) v4: premul-over blend pipeline ready");
				} else {
					U_LOG_E("weave(#1036) v4: premul-over blend init failed");
				}
			}
			if (blend_ready) {
				VkImageMemoryBarrier ov_to_read = {
				    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				    .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
				    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
				    .oldLayout = mc->weave.overlay_first_use
				                     ? VK_IMAGE_LAYOUT_UNDEFINED
				                     : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				    .image = mc->weave.overlay_image,
				    .subresourceRange = range,
				};
				mc->weave.overlay_first_use = false;
				vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
				                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1,
				                         &ov_to_read);

				VkImageMemoryBarrier out_weave_to_blend = {
				    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				    .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
				    .dstAccessMask =
				        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
				    .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				    .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				    .image = mc->weave.out_image,
				    .subresourceRange = range,
				};
				vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
				                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0,
				                         NULL, 1, &out_weave_to_blend);

				vk_local2d_composite_begin_frame(&mc->weave.overlay_blend, vk);
				vk_local2d_composite_flatten_draw(&mc->weave.overlay_blend, vk, cmd, mc->weave.out_fb,
				                                  mc->weave.out_w, mc->weave.out_h,
				                                  mc->weave.overlay_view,                 //
				                                  0, 0, mc->weave.out_w, mc->weave.out_h, // dst = full window
				                                  0.0f, 0.0f, 1.0f, 1.0f,                 // src = whole atlas
				                                  /*unpremultiplied*/ false);
			}
		}

		// Output -> GENERAL for the caller's cross-process read.
		VkImageMemoryBarrier out_to_general = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		    .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
		    .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
		    .image = mc->weave.out_image,
		    .subresourceRange = range,
		};
		vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &out_to_general);

		if (vk->vkEndCommandBuffer(cmd) != VK_SUCCESS) {
			break;
		}

		// ---- Submit + synchronous completion (the Android/macOS sync contract) ----
		VkSubmitInfo submit = {
		    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		    .commandBufferCount = 1,
		    .pCommandBuffers = &cmd,
		};
		vk_queue_lock(vk->main_queue);
		VkResult ret = vk->vkQueueSubmit(vk->main_queue->queue, 1, &submit, mc->weave.fence);
		vk_queue_unlock(vk->main_queue);
		if (ret != VK_SUCCESS) {
			U_LOG_E("weave(#1036): vkQueueSubmit failed: %s", vk_result_string(ret));
			break;
		}
		// Bounded wait (#967b style): a wedged vendor weaver must not hang the
		// caller's present thread forever behind a synchronous IPC call.
		ret = vk->vkWaitForFences(vk->device, 1, &mc->weave.fence, VK_TRUE, 1000ULL * 1000ULL * 1000ULL);
		vk->vkResetFences(vk->device, 1, &mc->weave.fence);
		if (ret != VK_SUCCESS) {
			U_LOG_E("weave(#1036): weave did not complete within 1s: %s", vk_result_string(ret));
			break;
		}

		mc->weave.fence_value++;

		// Arch-C satellite (#1277 P0): present the woven output ourselves.
		// Best-effort AFTER the weave is complete — a satellite failure logs,
		// latches sat_failed, and the very same frame still exports normally,
		// so the caller never sees a black gap during a fallback.
		if (weave_satellite_wanted(mc)) {
			weave_satellite_present(vk, mc, rect_count, rects);
		}

		*out_width = mc->weave.out_w;
		*out_height = mc->weave.out_h;
		*out_fence_value = mc->weave.fence_value;

		// Eyes flow OUT (runtime -> caller) for the caller's next off-axis frame;
		// the weave itself reads the tracker DP-internally.
		U_ZERO(out_eyes);
		if (!xrt_display_processor_get_predicted_eye_positions(mc->weave.dp, out_eyes)) {
			U_ZERO(out_eyes);
		}

		ok = true;
	} while (false);

	os_mutex_unlock(&mc->weave.mutex);

	// Release the per-call references that weren't adopted into a cache.
	if (input != XRT_GRAPHICS_BUFFER_HANDLE_INVALID) {
		u_graphics_buffer_unref(&input);
	}
	if (overlay != XRT_GRAPHICS_BUFFER_HANDLE_INVALID) {
		u_graphics_buffer_unref(&overlay);
	}
	return ok;
}

bool
comp_multi_weave_export_output(struct xrt_compositor *xc,
                               xrt_graphics_buffer_handle_t *out_handle,
                               uint32_t *out_width,
                               uint32_t *out_height)
{
	struct multi_compositor *mc = multi_compositor(xc);
	if (mc == NULL || !mc->weave.mutex_initialized) {
		return false;
	}
	os_mutex_lock(&mc->weave.mutex);
	bool ok = false;
	// Arch-C satellite (#1277): while the service presents the weave itself,
	// report no output — the caller's over-plane then draws nothing and its
	// page-owned 2D shows under the overlay (spec v7 permits absent output).
	const bool satellite_live = mc->weave.sat_enabled && !mc->weave.sat_failed &&
	                            mc->weave.sat_swapchain != VK_NULL_HANDLE;
	if (!satellite_live && mc->weave.out_ahb != NULL && mc->weave.out_w != 0) {
		// Hand out the CACHED handle without taking a reference. The generated
		// out_handles send path only READS the handle — it never releases it
		// (proto.py) — and AHardwareBuffer_sendHandleToUnixSocket does not
		// consume one either; the receiving process gets its own reference from
		// AHardwareBuffer_recvHandleFromUnixSocket. Adding a reference here
		// would therefore strand the whole buffer (~15 MB at panel size) on
		// every re-allocation, since nothing on this side would ever drop it.
		// Ownership stays with the cache, which releases on resize / teardown.
		*out_handle = (xrt_graphics_buffer_handle_t)mc->weave.out_ahb;
		*out_width = mc->weave.out_w;
		*out_height = mc->weave.out_h;
		ok = true;
	}
	os_mutex_unlock(&mc->weave.mutex);
	return ok;
}

bool
comp_multi_weave_export_fence(struct xrt_compositor *xc, xrt_graphics_sync_handle_t *out_handle)
{
	// No cross-process GPU fence on Android — completion is synchronous
	// (xrWeaveSubmitDXR returns after the weave finished on the GPU). A
	// sync_file-fd variant is a documented follow-up, not a correctness gap.
	(void)xc;
	(void)out_handle;
	return false;
}

bool
comp_multi_weave_snap_window_rect(struct xrt_compositor *xc,
                                  int32_t origin_x,
                                  int32_t origin_y,
                                  int32_t target_x,
                                  int32_t target_y,
                                  int32_t *out_snapped_x,
                                  int32_t *out_snapped_y)
{
	// Identity snap: on Android the app does not drag its own window pixel by
	// pixel (the window manager does), and the VK DP vtable carries no snap slot
	// — the per-window phase contract is set_window_screen_rect instead, which
	// reports geometry and leaves ALL phase to the weaver (ADR-033).
	(void)xc;
	(void)origin_x;
	(void)origin_y;
	(void)target_x;
	(void)target_y;
	(void)out_snapped_x;
	(void)out_snapped_y;
	return false;
}

void
comp_multi_weave_fini(struct multi_compositor *mc)
{
	if (mc == NULL || !mc->weave.mutex_initialized) {
		return;
	}
	struct vk_bundle *vk = weave_get_vk(mc);
	os_mutex_lock(&mc->weave.mutex);
	if (vk != NULL) {
		if (mc->weave.engine_initialized) {
			// Nothing may be in flight (submits are synchronous), but a
			// belt-and-braces idle keeps teardown safe if that changes.
			vk_queue_lock(vk->main_queue);
			vk->vkQueueWaitIdle(vk->main_queue->queue);
			vk_queue_unlock(vk->main_queue);
		}
		weave_satellite_release(vk, mc);
		weave_release_input(vk, mc);
		weave_release_overlay(vk, mc);
		weave_release_scratch(vk, mc);
		weave_release_crop(vk, mc);
		weave_release_output(vk, mc);
		if (mc->weave.overlay_blend_initialized) {
			vk_local2d_composite_fini(&mc->weave.overlay_blend, vk);
			mc->weave.overlay_blend_initialized = false;
		}
		if (mc->weave.dp != NULL) {
			xrt_display_processor_destroy(&mc->weave.dp);
		}
		if (mc->weave.render_pass != VK_NULL_HANDLE) {
			vk->vkDestroyRenderPass(vk->device, mc->weave.render_pass, NULL);
			mc->weave.render_pass = VK_NULL_HANDLE;
		}
		if (mc->weave.fence != VK_NULL_HANDLE) {
			vk->vkDestroyFence(vk->device, mc->weave.fence, NULL);
			mc->weave.fence = VK_NULL_HANDLE;
		}
		if (mc->weave.cmd_pool != VK_NULL_HANDLE) {
			vk->vkDestroyCommandPool(vk->device, mc->weave.cmd_pool, NULL);
			mc->weave.cmd_pool = VK_NULL_HANDLE;
		}
	}
	mc->weave.engine_initialized = false;
	os_mutex_unlock(&mc->weave.mutex);
	os_mutex_destroy(&mc->weave.mutex);
	mc->weave.mutex_initialized = false;
}

#endif // XRT_OS_ANDROID
