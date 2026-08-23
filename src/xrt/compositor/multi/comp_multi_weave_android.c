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
                        uint32_t source_rect_count,
                        const struct xrt_weave_source_rect *source_rects,
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

	// v10 (browser#143): source rects are strictly parallel to the batch rects and
	// only meaningful on the batch layout. Normalise the untrusted pair once so
	// every later use is safe without re-checking.
	if (source_rects == NULL || source_rect_count != rect_count || rect_count == 0 ||
	    (layout != NULL && layout->view_count > 0)) {
		source_rect_count = 0;
		source_rects = NULL;
	}

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

		// v10 (browser#143): the one source-rect rule that needs the input.
		// Everything structural was rejected as XR_ERROR_VALIDATION_FAILURE in
		// oxr_weave.c; "inside the input" can only be checked once the input is
		// imported and its size known. A violation REFUSES the whole submit —
		// never a clamp, never a skipped entry: a source rect is an explicit
		// caller input, and quietly weaving something else is how an
		// eye-inconsistency bug hides. Before the command buffer is recorded, so
		// the ordinary `break` failure exit is safe.
		if (source_rects != NULL) {
			bool src_ok = true;
			for (uint32_t i = 0; i < source_rect_count && src_ok; i++) {
				const struct xrt_rect *pair[2] = {&source_rects[i].left, &source_rects[i].right};
				for (uint32_t e = 0; e < 2; e++) {
					// xrt_offset names its fields w/h; they hold x/y here.
					const int64_t sx = pair[e]->offset.w;
					const int64_t sy = pair[e]->offset.h;
					const int64_t sw = pair[e]->extent.w;
					const int64_t sh = pair[e]->extent.h;
					if (sx < 0 || sy < 0 || sw <= 0 || sh <= 0 ||
					    sx + sw > (int64_t)mc->weave.in_w || sy + sh > (int64_t)mc->weave.in_h) {
						U_LOG_E("#143 weave v10: source_rects[%u].%s = %lld,%lld %lldx%lld "
						        "lies outside the %ux%u input — submit REFUSED (not clamped)",
						        i, e == 0 ? "left" : "right", (long long)sx, (long long)sy,
						        (long long)sw, (long long)sh, mc->weave.in_w, mc->weave.in_h);
						src_ok = false;
						break;
					}
				}
			}
			if (!src_ok) {
				break;
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

			// v10 (browser#143): WHERE this entry samples, decoupled from where
			// it draws. Absent source rects the derivation is the pre-v10 one,
			// expression for expression (left = [rx, rx+half), right =
			// [rx+half, rx+rw) at the rect's own position). Bounds were proved
			// against the input before the record began.
			int32_t lx = rx, ly = ry, lw = half, lh = rh;
			int32_t sx = rx + half, sy = ry, sw = rw - half, sh = rh;
			if (source_rects != NULL) {
				lx = source_rects[i].left.offset.w; // xrt_offset fields are named w/h
				ly = source_rects[i].left.offset.h;
				lw = source_rects[i].left.extent.w;
				lh = source_rects[i].left.extent.h;
				sx = source_rects[i].right.offset.w;
				sy = source_rects[i].right.offset.h;
				sw = source_rects[i].right.extent.w;
				sh = source_rects[i].right.extent.h;
			}

			VkImageBlit blits[2] = {
			    // Left eye -> left tile, unsqueezed.
			    {
			        .srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
			        .srcOffsets = {{lx, ly, 0}, {lx + lw, ly + lh, 1}},
			        .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
			        .dstOffsets = {{rx, ry, 0}, {rx + rw, ry + rh, 1}},
			    },
			    // Right eye -> right tile (+out_w).
			    {
			        .srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
			        .srcOffsets = {{sx, sy, 0}, {sx + sw, sy + sh, 1}},
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
	if (mc->weave.out_ahb != NULL && mc->weave.out_w != 0) {
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
