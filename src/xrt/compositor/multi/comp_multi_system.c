// Copyright 2019-2024, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Multi client wrapper compositor.
 * @author Pete Black <pblack@collabora.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Korcan Hussein <korcan.hussein@collabora.com>
 * @ingroup comp_multi
 *
 * @note DisplayXR-specific: this is the Monado-legacy multi-client orchestrator.
 * DisplayXR's workspace mode uses a separate per-client compositor
 * (`d3d11_service_compositor`) and its own multi-client orchestration
 * (`d3d11_multi_compositor`) inside `compositor/d3d11_service/comp_d3d11_service.cpp`.
 * The code in this file is reachable only via `compositor/null/null_compositor.c`
 * (headless testing). Modifying it does NOT affect workspace-mode performance
 * or behavior.
 */

#include "xrt/xrt_config_os.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_handles.h"
#include "xrt/xrt_session.h"
#include "xrt/xrt_display_metrics.h"

#include "os/os_time.h"
#include "os/os_threading.h"

#include "util/u_var.h"
#include "util/u_misc.h"
#include "util/u_time.h"
#include "util/u_wait.h"
#include "util/u_debug.h"
#include "util/u_trace_marker.h"
#include "util/u_distortion_mesh.h"

#ifdef XRT_OS_LINUX
#include "util/u_linux.h"
#endif

#include "multi/comp_multi_private.h"
#include "multi/comp_multi_interface.h"
#include "multi/comp_multi_workspace.h"
#include "main/comp_target.h"

// Per-session rendering support (Phase 4)
#include "util/comp_swapchain.h"
#include "util/comp_render_helpers.h"
#include "util/u_tiling.h"

// Vulkan helpers needed for Y-flip atlas blit (not Leia-specific)
#include "vk/vk_helpers.h"
#include "vk/vk_hud_blend.h"

#include "xrt/xrt_system.h"
#include "math/m_api.h"

#include "render/render_interface.h"

#ifdef XRT_OS_WINDOWS
#include "comp_d3d11_window.h"
#endif

#ifdef XRT_OS_ANDROID
#include "android/android_globals.h"
#endif

#ifdef XRT_OS_MACOS
#include "main/comp_window_macos.h" // per-session NSWindow reposition/resize (#59)
#endif

#ifdef XRT_BUILD_DRIVER_QWERTY
#include "qwerty/qwerty_interface.h"
#include "xrt/xrt_system.h"
#endif

#include <math.h>
#include <stdio.h>

#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#ifdef XRT_GRAPHICS_SYNC_HANDLE_IS_FD
#include <unistd.h>
#endif



/*
 *
 * Render thread.
 *
 */

static void
do_projection_layer(struct xrt_compositor *xc, struct multi_compositor *mc, struct multi_layer_entry *layer, uint32_t i)
{
	struct xrt_device *xdev = layer->xdev;

	// Cast away
	struct xrt_layer_data *data = (struct xrt_layer_data *)&layer->data;

	// Do not need to copy the reference, but should verify the pointers for consistency
	for (uint32_t j = 0; j < data->view_count; j++) {
		if (layer->xscs[j] == NULL) {
			U_LOG_E("Invalid swap chain for projection layer #%u!", i);
			return;
		}
	}

	if (xdev == NULL) {
		U_LOG_E("Invalid xdev for projection layer #%u!", i);
		return;
	}

	xrt_comp_layer_projection(xc, xdev, layer->xscs, data);
}

static void
do_projection_layer_depth(struct xrt_compositor *xc,
                          struct multi_compositor *mc,
                          struct multi_layer_entry *layer,
                          uint32_t i)
{
	struct xrt_device *xdev = layer->xdev;

	struct xrt_swapchain *xsc[XRT_MAX_VIEWS];
	struct xrt_swapchain *d_xsc[XRT_MAX_VIEWS];
	// Cast away
	struct xrt_layer_data *data = (struct xrt_layer_data *)&layer->data;

	for (uint32_t j = 0; j < data->view_count; j++) {
		xsc[j] = layer->xscs[j];
		d_xsc[j] = layer->xscs[j + data->view_count];

		if (xsc[j] == NULL || d_xsc[j] == NULL) {
			U_LOG_E("Invalid swap chain for projection layer #%u!", i);
			return;
		}
	}

	if (xdev == NULL) {
		U_LOG_E("Invalid xdev for projection layer #%u!", i);
		return;
	}


	xrt_comp_layer_projection_depth(xc, xdev, xsc, d_xsc, data);
}

static void
do_zone_3d_layer(struct xrt_compositor *xc, struct multi_compositor *mc, struct multi_layer_entry *layer, uint32_t i)
{
	// XR_EXT_display_zones P5: the target compositor may not consume zone
	// layers (e.g. the null compositor) — drop with a one-shot WARN,
	// never an error (layers are advisory compositing).
	if (xc->layer_zone_3d == NULL) {
		static bool warned = false;
		if (!warned) {
			warned = true;
			U_LOG_W("Zone-3D layers are not consumed by the target compositor — "
			        "dropping (one-time warning)");
		}
		return;
	}

	struct xrt_device *xdev = layer->xdev;

	// Cast away
	struct xrt_layer_data *data = (struct xrt_layer_data *)&layer->data;

	// Do not need to copy the reference, but should verify the pointers for consistency
	for (uint32_t j = 0; j < data->view_count; j++) {
		if (layer->xscs[j] == NULL) {
			U_LOG_E("Invalid swap chain for zone-3D layer #%u!", i);
			return;
		}
	}

	if (xdev == NULL) {
		U_LOG_E("Invalid xdev for zone-3D layer #%u!", i);
		return;
	}

	xrt_comp_layer_zone_3d(xc, xdev, layer->xscs, data);
}

static bool
do_single(struct xrt_compositor *xc,
          struct multi_compositor *mc,
          struct multi_layer_entry *layer,
          uint32_t i,
          const char *name,
          struct xrt_device **out_xdev,
          struct xrt_swapchain **out_xcs,
          struct xrt_layer_data **out_data)
{
	struct xrt_device *xdev = layer->xdev;
	struct xrt_swapchain *xcs = layer->xscs[0];

	if (xcs == NULL) {
		U_LOG_E("Invalid swapchain for layer #%u '%s'!", i, name);
		return false;
	}

	if (xdev == NULL) {
		U_LOG_E("Invalid xdev for layer #%u '%s'!", i, name);
		return false;
	}

	// Cast away
	struct xrt_layer_data *data = (struct xrt_layer_data *)&layer->data;

	*out_xdev = xdev;
	*out_xcs = xcs;
	*out_data = data;

	return true;
}

static void
do_quad_layer(struct xrt_compositor *xc, struct multi_compositor *mc, struct multi_layer_entry *layer, uint32_t i)
{
	struct xrt_device *xdev = NULL;
	struct xrt_swapchain *xcs = NULL;
	struct xrt_layer_data *data = NULL;

	if (!do_single(xc, mc, layer, i, "quad", &xdev, &xcs, &data)) {
		return;
	}

	xrt_comp_layer_quad(xc, xdev, xcs, data);
}

static void
do_cube_layer(struct xrt_compositor *xc, struct multi_compositor *mc, struct multi_layer_entry *layer, uint32_t i)
{
	struct xrt_device *xdev = NULL;
	struct xrt_swapchain *xcs = NULL;
	struct xrt_layer_data *data = NULL;

	if (!do_single(xc, mc, layer, i, "cube", &xdev, &xcs, &data)) {
		return;
	}

	xrt_comp_layer_cube(xc, xdev, xcs, data);
}

static void
do_cylinder_layer(struct xrt_compositor *xc, struct multi_compositor *mc, struct multi_layer_entry *layer, uint32_t i)
{
	struct xrt_device *xdev = NULL;
	struct xrt_swapchain *xcs = NULL;
	struct xrt_layer_data *data = NULL;

	if (!do_single(xc, mc, layer, i, "cylinder", &xdev, &xcs, &data)) {
		return;
	}

	xrt_comp_layer_cylinder(xc, xdev, xcs, data);
}

static void
do_equirect1_layer(struct xrt_compositor *xc, struct multi_compositor *mc, struct multi_layer_entry *layer, uint32_t i)
{
	struct xrt_device *xdev = NULL;
	struct xrt_swapchain *xcs = NULL;
	struct xrt_layer_data *data = NULL;

	if (!do_single(xc, mc, layer, i, "equirect1", &xdev, &xcs, &data)) {
		return;
	}

	xrt_comp_layer_equirect1(xc, xdev, xcs, data);
}

static void
do_equirect2_layer(struct xrt_compositor *xc, struct multi_compositor *mc, struct multi_layer_entry *layer, uint32_t i)
{
	struct xrt_device *xdev = NULL;
	struct xrt_swapchain *xcs = NULL;
	struct xrt_layer_data *data = NULL;

	if (!do_single(xc, mc, layer, i, "equirect2", &xdev, &xcs, &data)) {
		return;
	}

	xrt_comp_layer_equirect2(xc, xdev, xcs, data);
}

static int
overlay_sort_func(const void *a, const void *b)
{
	struct multi_compositor *mc_a = *(struct multi_compositor **)a;
	struct multi_compositor *mc_b = *(struct multi_compositor **)b;

	if (mc_a->state.z_order < mc_b->state.z_order) {
		return -1;
	}

	if (mc_a->state.z_order > mc_b->state.z_order) {
		return 1;
	}

	return 0;
}

static enum xrt_blend_mode
find_active_blend_mode(struct multi_compositor **overlay_sorted_clients, size_t size)
{
	if (overlay_sorted_clients == NULL)
		return XRT_BLEND_MODE_OPAQUE;

	const struct multi_compositor *first_visible = NULL;
	for (size_t k = 0; k < size; ++k) {
		const struct multi_compositor *mc = overlay_sorted_clients[k];
		assert(mc != NULL);

		// if a focused client is found just return, "first_visible" has lower priority and can be ignored.
		if (mc->state.focused) {
			assert(mc->state.visible);
			return mc->delivered.data.env_blend_mode;
		}

		if (first_visible == NULL && mc->state.visible) {
			first_visible = mc;
		}
	}
	if (first_visible != NULL)
		return first_visible->delivered.data.env_blend_mode;
	return XRT_BLEND_MODE_OPAQUE;
}


/*
 *
 * Per-session rendering (Phase 4)
 *
 */

/*!
 * Extract VkImageView and dimensions from a multi_layer_entry for a specific view.
 * Similar to getLayerInfo() in comp_renderer.c but adapted for multi_layer_entry.
 *
 * @param layer The layer entry to extract from
 * @param view_index 0 for left eye, 1 for right eye
 * @param[out] out_width Image width
 * @param[out] out_height Image height
 * @param[out] out_format Image format
 * @param[out] out_image_view The VkImageView for rendering
 * @return true if extraction successful
 */
// Legacy per-session helper (see the #ifndef block below) — guarded out on macOS,
// which composites every client into the one shared surface instead.
#ifndef XRT_OS_MACOS
/*!
 * Composite LOCAL_2D layers (XR_EXT_local_3d_zone, e.g. the avatar speech
 * bubble, #568) into the final per-session target, post-weave, by scale-blitting
 * each layer image into its destination rect. The target's 2D regions (outside
 * any 3D zone) were cleared transparent by the DP, and the layer carries its own
 * alpha, so the blit drops the panel in with transparent margins → the live
 * screen shows through. Recorded into @p cmd after process_atlas; the target
 * enters and leaves in COLOR_ATTACHMENT_OPTIMAL so the existing post-weave →
 * PRESENT barrier is unaffected.
 *
 * Layers blit in submission order (alpha-replace, not alpha-over): correct for a
 * single bubble or non-overlapping 2D layers; overlapping layers would need a
 * blend pass (deferred — not required by the avatar, which composites its panel
 * and text into one layer).
 *
 * @return true if at least one LOCAL_2D layer was blitted.
 */
static bool
composite_local_2d_layers(struct multi_compositor *mc,
                          struct vk_bundle *vk,
                          VkCommandBuffer cmd,
                          VkImage target_image,
                          uint32_t fb_width,
                          uint32_t fb_height)
{
	bool any = false;
	for (uint32_t i = 0; i < mc->delivered.layer_count; i++) {
		struct multi_layer_entry *layer = &mc->delivered.layers[i];
		if (layer->data.type != XRT_LAYER_LOCAL_2D) {
			continue;
		}
		const struct xrt_layer_local_2d_data *l2d = &layer->data.local_2d;
		struct xrt_swapchain *xsc = layer->xscs[0];
		if (xsc == NULL) {
			continue;
		}
		struct comp_swapchain *sc = comp_swapchain(xsc);
		uint32_t img_idx = l2d->sub.image_index;
		VkImage src_image = sc->vkic.images[img_idx].handle;
		if (src_image == VK_NULL_HANDLE) {
			continue;
		}

		// Source sub-rect within the layer image; honor flip_y by swapping
		// the src Y bounds (same convention as the atlas blit above).
		int src_x0 = l2d->sub.rect.offset.w;
		int src_y0 = l2d->sub.rect.offset.h;
		int src_w = l2d->sub.rect.extent.w;
		int src_h = l2d->sub.rect.extent.h;
		int src_top = layer->data.flip_y ? (src_y0 + src_h) : src_y0;
		int src_bot = layer->data.flip_y ? src_y0 : (src_y0 + src_h);

		// Destination rect — window/canvas px (== panel px on Android OOP),
		// clamped to the target.
		int dx0 = l2d->rect.offset.w, dy0 = l2d->rect.offset.h;
		int dx1 = dx0 + l2d->rect.extent.w, dy1 = dy0 + l2d->rect.extent.h;
		if (dx0 < 0) dx0 = 0;
		if (dy0 < 0) dy0 = 0;
		if (dx1 > (int)fb_width) dx1 = (int)fb_width;
		if (dy1 > (int)fb_height) dy1 = (int)fb_height;
		if (dx1 <= dx0 || dy1 <= dy0) {
			continue;
		}

		uint32_t arr = l2d->sub.array_index;

		// target COLOR_ATTACHMENT → TRANSFER_DST; src GENERAL → TRANSFER_SRC
		// (overlay/shared images are kept in GENERAL, matching the window-space path).
		VkImageMemoryBarrier pre[2] = {
		    {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		     .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		     .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		     .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		     .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		     .image = target_image,
		     .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
		    {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		     .srcAccessMask = 0,
		     .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		     .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
		     .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		     .image = src_image,
		     .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, arr, 1}},
		};
		vk->vkCmdPipelineBarrier(cmd,
		                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, pre);

		VkImageBlit blit = {
		    .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, arr, 1},
		    .srcOffsets = {{src_x0, src_top, 0}, {src_x0 + src_w, src_bot, 1}},
		    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
		    .dstOffsets = {{dx0, dy0, 0}, {dx1, dy1, 1}},
		};
		vk->vkCmdBlitImage(cmd, src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target_image,
		                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

		// restore: target → COLOR_ATTACHMENT, src → GENERAL
		VkImageMemoryBarrier post[2] = {
		    {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		     .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		     .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		     .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		     .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		     .image = target_image,
		     .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
		    {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		     .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		     .dstAccessMask = 0,
		     .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		     .newLayout = VK_IMAGE_LAYOUT_GENERAL,
		     .image = src_image,
		     .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, arr, 1}},
		};
		vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
		                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 2, post);
		any = true;
	}
	return any;
}
#endif // !XRT_OS_MACOS

static bool
get_session_layer_view(struct multi_layer_entry *layer,
                       int view_index,
                       int *out_width,
                       int *out_height,
                       VkFormat *out_format,
                       VkImageView *out_image_view,
                       VkImage *out_image,
                       uint32_t *out_array_index)
{
	const struct xrt_layer_data *layer_data = &layer->data;

	// Projection layers for SR weaving, plus zone-3D layers (XR_EXT_display_zones,
	// #568): xrt_layer_zone_3d_data embeds xrt_layer_projection_data as its first
	// member, and data.proj / data.zone_3d.proj alias the union's first member —
	// so the per-view proj/sub reads below are byte-identical for ZONE_3D.
	if (layer_data->type != XRT_LAYER_PROJECTION && layer_data->type != XRT_LAYER_PROJECTION_DEPTH &&
	    layer_data->type != XRT_LAYER_ZONE_3D) {
		return false;
	}

	// Get the swapchain for this view (xscs[] is indexed by view index)
	struct xrt_swapchain *xsc = layer->xscs[view_index];
	if (xsc == NULL) {
		return false;
	}

	// Cast to comp_swapchain to access Vulkan resources
	struct comp_swapchain *sc = comp_swapchain(xsc);

	// Get the projection view data
	const struct xrt_layer_projection_view_data *vd = &layer_data->proj.v[view_index];
	const uint32_t array_index = vd->sub.array_index;
	const struct comp_swapchain_image *image = &sc->images[vd->sub.image_index];

	// Extract dimensions
	*out_width = vd->sub.rect.extent.w;
	*out_height = vd->sub.rect.extent.h;
	*out_format = (VkFormat)sc->vkic.info.format;
	*out_image_view = get_image_view(image, layer_data->flags, array_index);
	*out_image = sc->vkic.images[vd->sub.image_index].handle;
	*out_array_index = array_index;

	return (*out_image_view != VK_NULL_HANDLE);
}

// The per-session compositor helpers below (composite-into-atlas, swapchain
// recreate, HUD, workspace chrome/overlay/cursor, and render_session_to_own_target)
// implement the legacy one-NSWindow-per-client path. macOS now composites every
// client into ONE shared full-screen surface (see render_shared_surface_locked),
// so this whole path is compiled only for the non-macOS OOP service (Android,
// and Windows where it is unused in favor of the D3D11 monolith). Guarding it out
// on macOS keeps the macOS binary free of the dead legacy path.
#ifndef XRT_OS_MACOS
/*!
 * Initialize intermediate composite resources for pre-display-processing layer compositing.
 * Creates a tiled atlas image, per-eye views, render pass, framebuffers,
 * pipeline, and descriptor resources.
 *
 * @param mc    The multi_compositor with per-session rendering already initialized
 * @param vk    The Vulkan bundle
 * @param width Single eye width
 * @param height Eye height
 * @param format Image format (should match swapchain format)
 * @return true on success
 */
static bool
init_composite_resources(struct multi_compositor *mc, struct vk_bundle *vk, uint32_t width, uint32_t height, VkFormat format)
{
	VkResult ret;

	if (mc->session_render.composite_initialized) {
		return true;
	}

	mc->session_render.composite_width = width;
	mc->session_render.composite_height = height;

	U_LOG_W("[composite] Initializing composite resources: %ux%u per eye, format=%d", width, height, format);

	// Create per-eye composite images (separate images for clean display processor input)
	// TRANSFER_SRC needed for potential vkCmdBlitImage operations.
	VkExtent2D eye_extent = {.width = width, .height = height};
	VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
	                          VK_IMAGE_USAGE_SAMPLED_BIT |
	                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

	VkImageSubresourceRange eye_range = {
	    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	    .baseMipLevel = 0,
	    .levelCount = 1,
	    .baseArrayLayer = 0,
	    .layerCount = 1,
	};

	for (int eye = 0; eye < 2; eye++) {
		ret = vk_create_image_simple(vk, eye_extent, format, usage,
		                             &mc->session_render.composite_memories[eye],
		                             &mc->session_render.composite_images[eye]);
		if (ret != VK_SUCCESS) {
			U_LOG_E("[composite] Failed to create composite image %d: %d", eye, ret);
			goto err_images;
		}

		ret = vk_create_view(vk, mc->session_render.composite_images[eye],
		                     VK_IMAGE_VIEW_TYPE_2D, format, eye_range,
		                     &mc->session_render.composite_eye_views[eye]);
		if (ret != VK_SUCCESS) {
			U_LOG_E("[composite] Failed to create eye view %d: %d", eye, ret);
			goto err_images;
		}
	}

	// Create pre-blit local copies of shared projection images (Intel CCS workaround).
	// vkCmdBlitImage works for cross-device shared images on Intel Iris Xe; fragment
	// shader sampling does not. We blit shared images into these local copies, then
	// sample the local copies in the compositing render pass.
	{
		VkImageUsageFlags preblit_usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

		for (int eye = 0; eye < 2; eye++) {
			ret = vk_create_image_simple(vk, eye_extent, format, preblit_usage,
			                             &mc->session_render.preblit_memories[eye],
			                             &mc->session_render.preblit_images[eye]);
			if (ret != VK_SUCCESS) {
				U_LOG_E("[composite] Failed to create preblit image %d: %d", eye, ret);
				goto err_images;
			}

			ret = vk_create_view(vk, mc->session_render.preblit_images[eye],
			                     VK_IMAGE_VIEW_TYPE_2D, format, eye_range,
			                     &mc->session_render.preblit_views[eye]);
			if (ret != VK_SUCCESS) {
				U_LOG_E("[composite] Failed to create preblit view %d: %d", eye, ret);
				goto err_images;
			}
		}
	}

	// Create render pass with LOAD_OP_CLEAR - projection layer is drawn first as fullscreen quad
	VkAttachmentDescription attachment = {
	    .format = format,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
	    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
	    .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
	    .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
	    .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	    .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	};

	VkAttachmentReference color_ref = {
	    .attachment = 0,
	    .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	};

	VkSubpassDescription subpass = {
	    .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
	    .colorAttachmentCount = 1,
	    .pColorAttachments = &color_ref,
	};

	VkRenderPassCreateInfo rp_info = {
	    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
	    .attachmentCount = 1,
	    .pAttachments = &attachment,
	    .subpassCount = 1,
	    .pSubpasses = &subpass,
	};

	ret = vk->vkCreateRenderPass(vk->device, &rp_info, NULL,
	                             &mc->session_render.composite_render_pass);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[composite] Failed to create render pass: %d", ret);
		goto err_images;
	}

	// Create framebuffers - one per eye, each using its own image view
	for (int eye = 0; eye < 2; eye++) {
		VkImageView attachments[1] = {mc->session_render.composite_eye_views[eye]};
		VkFramebufferCreateInfo fb_info = {
		    .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		    .renderPass = mc->session_render.composite_render_pass,
		    .attachmentCount = 1,
		    .pAttachments = attachments,
		    .width = width,
		    .height = height,
		    .layers = 1,
		};
		ret = vk->vkCreateFramebuffer(vk->device, &fb_info, NULL,
		                              &mc->session_render.composite_framebuffers[eye]);
		if (ret != VK_SUCCESS) {
			U_LOG_E("[composite] Failed to create framebuffer %d: %d", eye, ret);
			goto err_framebuffers;
		}
	}

	// Create descriptor set layout (UBO + combined image sampler, same as render_resources)
	VkDescriptorSetLayoutBinding bindings[2] = {
	    {
	        .binding = 0, // UBO
	        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
	        .descriptorCount = 1,
	        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
	    },
	    {
	        .binding = 1, // Combined image sampler
	        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	        .descriptorCount = 1,
	        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
	    },
	};

	VkDescriptorSetLayoutCreateInfo dsl_info = {
	    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
	    .bindingCount = 2,
	    .pBindings = bindings,
	};

	ret = vk->vkCreateDescriptorSetLayout(vk->device, &dsl_info, NULL,
	                                      &mc->session_render.composite_desc_layout);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[composite] Failed to create descriptor set layout: %d", ret);
		goto err_framebuffers;
	}

	// Create pipeline layout
	ret = vk_create_pipeline_layout(vk, mc->session_render.composite_desc_layout,
	                                &mc->session_render.composite_pipe_layout);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[composite] Failed to create pipeline layout: %d", ret);
		goto err_desc_layout;
	}

	// Load per-session shaders on demand (avoids invalid comp_compositor cast)
	if (!mc->session_render.shaders_loaded) {
		if (!render_shaders_load(&mc->session_render.shaders, vk)) {
			U_LOG_E("[composite] Failed to load shaders");
			goto err_pipe_layout;
		}
		mc->session_render.shaders_loaded = true;
	}

	// Create per-session pipeline cache on demand
	if (mc->session_render.pipeline_cache == VK_NULL_HANDLE) {
		ret = vk_create_pipeline_cache(vk, &mc->session_render.pipeline_cache);
		if (ret != VK_SUCCESS) {
			U_LOG_E("[composite] Failed to create pipeline cache: %d", ret);
			goto err_pipe_layout;
		}
	}

	// Build a pipeline compatible with our render pass
	// Triangle strip, no vertex input, dynamic viewport/scissor, alpha blending
	VkPipelineInputAssemblyStateCreateInfo ia = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
	    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
	};

	VkPipelineVertexInputStateCreateInfo vi = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
	};

	VkPipelineViewportStateCreateInfo vp = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
	    .viewportCount = 1,
	    .scissorCount = 1,
	};

	VkPipelineRasterizationStateCreateInfo rs = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
	    .polygonMode = VK_POLYGON_MODE_FILL,
	    .cullMode = VK_CULL_MODE_NONE, // 2D compositing — no culling (shader Y-flip reverses winding)
	    .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
	    .lineWidth = 1.0f,
	};

	VkPipelineMultisampleStateCreateInfo ms = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
	    .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
	};

	// Porter-Duff "over" for both color and alpha so dst.a is preserved
	// through layered composition (see issue #225). The previous
	// (srcA=ONE_MINUS_SRC_ALPHA, dstA=ONE) was quadratic in src.a and
	// clobbered opaque destinations when a semi-transparent layer landed
	// on top — breaking compose-under-bg downstream.
	VkPipelineColorBlendAttachmentState blend_att = {
	    .blendEnable = VK_TRUE,
	    .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
	                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
	    .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
	    .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
	    .colorBlendOp = VK_BLEND_OP_ADD,
	    .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
	    .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
	    .alphaBlendOp = VK_BLEND_OP_ADD,
	};

	VkPipelineColorBlendStateCreateInfo cb = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
	    .attachmentCount = 1,
	    .pAttachments = &blend_att,
	};

	VkPipelineDepthStencilStateCreateInfo ds = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
	};

	VkDynamicState dyn_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dyn = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
	    .dynamicStateCount = 2,
	    .pDynamicStates = dyn_states,
	};

	VkPipelineShaderStageCreateInfo stages[2] = {
	    {
	        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	        .stage = VK_SHADER_STAGE_VERTEX_BIT,
	        .module = mc->session_render.shaders.layer_quad_vert,
	        .pName = "main",
	    },
	    {
	        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	        .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
	        .module = mc->session_render.shaders.layer_shared_frag,
	        .pName = "main",
	    },
	};

	VkGraphicsPipelineCreateInfo pipe_info = {
	    .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
	    .stageCount = 2,
	    .pStages = stages,
	    .pVertexInputState = &vi,
	    .pInputAssemblyState = &ia,
	    .pViewportState = &vp,
	    .pRasterizationState = &rs,
	    .pMultisampleState = &ms,
	    .pDepthStencilState = &ds,
	    .pColorBlendState = &cb,
	    .pDynamicState = &dyn,
	    .layout = mc->session_render.composite_pipe_layout,
	    .renderPass = mc->session_render.composite_render_pass,
	    .subpass = 0,
	};

	ret = vk->vkCreateGraphicsPipelines(vk->device, mc->session_render.pipeline_cache, 1,
	                                    &pipe_info, NULL,
	                                    &mc->session_render.composite_pipeline);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[composite] Failed to create composite pipeline: %d", ret);
		goto err_pipe_layout;
	}

	// Create sampler
	ret = vk_create_sampler(vk, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	                        &mc->session_render.composite_sampler);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[composite] Failed to create sampler: %d", ret);
		goto err_pipeline;
	}

	// Create descriptor pool (enough for XRT_MAX_LAYERS descriptors)
	struct vk_descriptor_pool_info pool_info = {
	    .uniform_per_descriptor_count = 1,
	    .sampler_per_descriptor_count = 1,
	    .descriptor_count = XRT_MAX_LAYERS,
	    .freeable = false,
	};
	ret = vk_create_descriptor_pool(vk, &pool_info,
	                                &mc->session_render.composite_desc_pool);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[composite] Failed to create descriptor pool: %d", ret);
		goto err_sampler;
	}

	// Pre-allocate descriptor sets
	for (uint32_t i = 0; i < XRT_MAX_LAYERS; i++) {
		ret = vk_create_descriptor_set(vk, mc->session_render.composite_desc_pool,
		                               mc->session_render.composite_desc_layout,
		                               &mc->session_render.composite_desc_sets[i]);
		if (ret != VK_SUCCESS) {
			U_LOG_E("[composite] Failed to create descriptor set %u: %d", i, ret);
			goto err_desc_pool;
		}
	}

	// Create persistent UBO buffer for window-space layer data
	// Size: per-eye quad UBO (post_transform + mvp = 80 bytes) × 2 eyes × XRT_MAX_LAYERS
	VkDeviceSize ubo_size = sizeof(struct xrt_normalized_rect) + sizeof(struct xrt_matrix_4x4);
	VkDeviceSize total_ubo_size = ubo_size * 2 * XRT_MAX_LAYERS;
	{
		VkBufferCreateInfo buf_ci = {
		    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		    .size = total_ubo_size,
		    .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
		};
		ret = vk->vkCreateBuffer(vk->device, &buf_ci, NULL, &mc->session_render.composite_ubo_buffer);
		if (ret != VK_SUCCESS) {
			U_LOG_E("[composite] Failed to create UBO buffer: %d", ret);
			goto err_desc_pool;
		}

		VkMemoryRequirements mem_reqs;
		vk->vkGetBufferMemoryRequirements(vk->device, mc->session_render.composite_ubo_buffer, &mem_reqs);

		VkPhysicalDeviceMemoryProperties mem_props;
		vk->vkGetPhysicalDeviceMemoryProperties(vk->physical_device, &mem_props);
		uint32_t mem_type = 0;
		VkMemoryPropertyFlags desired = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		for (uint32_t mi = 0; mi < mem_props.memoryTypeCount; mi++) {
			if ((mem_reqs.memoryTypeBits & (1u << mi)) &&
			    (mem_props.memoryTypes[mi].propertyFlags & desired) == desired) {
				mem_type = mi;
				break;
			}
		}

		VkMemoryAllocateInfo alloc_ci = {
		    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		    .allocationSize = mem_reqs.size,
		    .memoryTypeIndex = mem_type,
		};
		ret = vk->vkAllocateMemory(vk->device, &alloc_ci, NULL, &mc->session_render.composite_ubo_memory);
		if (ret != VK_SUCCESS) {
			U_LOG_E("[composite] Failed to allocate UBO memory: %d", ret);
			vk->vkDestroyBuffer(vk->device, mc->session_render.composite_ubo_buffer, NULL);
			mc->session_render.composite_ubo_buffer = VK_NULL_HANDLE;
			goto err_desc_pool;
		}

		vk->vkBindBufferMemory(vk->device, mc->session_render.composite_ubo_buffer,
		                       mc->session_render.composite_ubo_memory, 0);

		ret = vk->vkMapMemory(vk->device, mc->session_render.composite_ubo_memory,
		                      0, total_ubo_size, 0, &mc->session_render.composite_ubo_mapped);
		if (ret != VK_SUCCESS) {
			U_LOG_E("[composite] Failed to map UBO memory: %d", ret);
			vk->vkDestroyBuffer(vk->device, mc->session_render.composite_ubo_buffer, NULL);
			vk->vkFreeMemory(vk->device, mc->session_render.composite_ubo_memory, NULL);
			mc->session_render.composite_ubo_buffer = VK_NULL_HANDLE;
			mc->session_render.composite_ubo_memory = VK_NULL_HANDLE;
			goto err_desc_pool;
		}
	}

	mc->session_render.composite_initialized = true;
	U_LOG_W("[composite] Composite resources initialized: %ux%u per eye", width, height);
	return true;

	// Error cleanup (reverse order)
err_desc_pool:
	vk->vkDestroyDescriptorPool(vk->device, mc->session_render.composite_desc_pool, NULL);
	mc->session_render.composite_desc_pool = VK_NULL_HANDLE;
err_sampler:
	vk->vkDestroySampler(vk->device, mc->session_render.composite_sampler, NULL);
	mc->session_render.composite_sampler = VK_NULL_HANDLE;
err_pipeline:
	vk->vkDestroyPipeline(vk->device, mc->session_render.composite_pipeline, NULL);
	mc->session_render.composite_pipeline = VK_NULL_HANDLE;
err_pipe_layout:
	vk->vkDestroyPipelineLayout(vk->device, mc->session_render.composite_pipe_layout, NULL);
	mc->session_render.composite_pipe_layout = VK_NULL_HANDLE;
err_desc_layout:
	vk->vkDestroyDescriptorSetLayout(vk->device, mc->session_render.composite_desc_layout, NULL);
	mc->session_render.composite_desc_layout = VK_NULL_HANDLE;
err_framebuffers:
	for (int i = 0; i < 2; i++) {
		if (mc->session_render.composite_framebuffers[i] != VK_NULL_HANDLE) {
			vk->vkDestroyFramebuffer(vk->device, mc->session_render.composite_framebuffers[i], NULL);
			mc->session_render.composite_framebuffers[i] = VK_NULL_HANDLE;
		}
	}
	vk->vkDestroyRenderPass(vk->device, mc->session_render.composite_render_pass, NULL);
	mc->session_render.composite_render_pass = VK_NULL_HANDLE;
err_images:
	for (int i = 0; i < 2; i++) {
		if (mc->session_render.preblit_views[i] != VK_NULL_HANDLE) {
			vk->vkDestroyImageView(vk->device, mc->session_render.preblit_views[i], NULL);
			mc->session_render.preblit_views[i] = VK_NULL_HANDLE;
		}
		if (mc->session_render.preblit_images[i] != VK_NULL_HANDLE) {
			vk->vkDestroyImage(vk->device, mc->session_render.preblit_images[i], NULL);
			mc->session_render.preblit_images[i] = VK_NULL_HANDLE;
		}
		if (mc->session_render.preblit_memories[i] != VK_NULL_HANDLE) {
			vk->vkFreeMemory(vk->device, mc->session_render.preblit_memories[i], NULL);
			mc->session_render.preblit_memories[i] = VK_NULL_HANDLE;
		}
		if (mc->session_render.composite_eye_views[i] != VK_NULL_HANDLE) {
			vk->vkDestroyImageView(vk->device, mc->session_render.composite_eye_views[i], NULL);
			mc->session_render.composite_eye_views[i] = VK_NULL_HANDLE;
		}
		if (mc->session_render.composite_images[i] != VK_NULL_HANDLE) {
			vk->vkDestroyImage(vk->device, mc->session_render.composite_images[i], NULL);
			mc->session_render.composite_images[i] = VK_NULL_HANDLE;
		}
		if (mc->session_render.composite_memories[i] != VK_NULL_HANDLE) {
			vk->vkFreeMemory(vk->device, mc->session_render.composite_memories[i], NULL);
			mc->session_render.composite_memories[i] = VK_NULL_HANDLE;
		}
	}
	return false;
}

/*!
 * Destroy intermediate composite resources.
 */
static void
fini_composite_resources(struct multi_compositor *mc, struct vk_bundle *vk)
{
	if (!mc->session_render.composite_initialized) {
		return;
	}

	// Destroy UBO buffer and memory
	if (mc->session_render.composite_ubo_buffer != VK_NULL_HANDLE) {
		vk->vkDestroyBuffer(vk->device, mc->session_render.composite_ubo_buffer, NULL);
		mc->session_render.composite_ubo_buffer = VK_NULL_HANDLE;
	}
	if (mc->session_render.composite_ubo_memory != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, mc->session_render.composite_ubo_memory, NULL);
		mc->session_render.composite_ubo_memory = VK_NULL_HANDLE;
	}
	mc->session_render.composite_ubo_mapped = NULL;

	vk->vkDestroyDescriptorPool(vk->device, mc->session_render.composite_desc_pool, NULL);
	mc->session_render.composite_desc_pool = VK_NULL_HANDLE;

	vk->vkDestroySampler(vk->device, mc->session_render.composite_sampler, NULL);
	mc->session_render.composite_sampler = VK_NULL_HANDLE;

	vk->vkDestroyPipeline(vk->device, mc->session_render.composite_pipeline, NULL);
	mc->session_render.composite_pipeline = VK_NULL_HANDLE;

	vk->vkDestroyPipelineLayout(vk->device, mc->session_render.composite_pipe_layout, NULL);
	mc->session_render.composite_pipe_layout = VK_NULL_HANDLE;

	vk->vkDestroyDescriptorSetLayout(vk->device, mc->session_render.composite_desc_layout, NULL);
	mc->session_render.composite_desc_layout = VK_NULL_HANDLE;

	for (int i = 0; i < 2; i++) {
		if (mc->session_render.composite_framebuffers[i] != VK_NULL_HANDLE) {
			vk->vkDestroyFramebuffer(vk->device, mc->session_render.composite_framebuffers[i], NULL);
			mc->session_render.composite_framebuffers[i] = VK_NULL_HANDLE;
		}
	}

	vk->vkDestroyRenderPass(vk->device, mc->session_render.composite_render_pass, NULL);
	mc->session_render.composite_render_pass = VK_NULL_HANDLE;

	for (int i = 0; i < 2; i++) {
		if (mc->session_render.preblit_views[i] != VK_NULL_HANDLE) {
			vk->vkDestroyImageView(vk->device, mc->session_render.preblit_views[i], NULL);
			mc->session_render.preblit_views[i] = VK_NULL_HANDLE;
		}
		if (mc->session_render.preblit_images[i] != VK_NULL_HANDLE) {
			vk->vkDestroyImage(vk->device, mc->session_render.preblit_images[i], NULL);
			mc->session_render.preblit_images[i] = VK_NULL_HANDLE;
		}
		if (mc->session_render.preblit_memories[i] != VK_NULL_HANDLE) {
			vk->vkFreeMemory(vk->device, mc->session_render.preblit_memories[i], NULL);
			mc->session_render.preblit_memories[i] = VK_NULL_HANDLE;
		}
		if (mc->session_render.composite_eye_views[i] != VK_NULL_HANDLE) {
			vk->vkDestroyImageView(vk->device, mc->session_render.composite_eye_views[i], NULL);
			mc->session_render.composite_eye_views[i] = VK_NULL_HANDLE;
		}
		if (mc->session_render.composite_images[i] != VK_NULL_HANDLE) {
			vk->vkDestroyImage(vk->device, mc->session_render.composite_images[i], NULL);
			mc->session_render.composite_images[i] = VK_NULL_HANDLE;
		}
		if (mc->session_render.composite_memories[i] != VK_NULL_HANDLE) {
			vk->vkFreeMemory(vk->device, mc->session_render.composite_memories[i], NULL);
			mc->session_render.composite_memories[i] = VK_NULL_HANDLE;
		}
	}

	// Destroy per-session shaders and pipeline cache
	if (mc->session_render.shaders_loaded) {
		render_shaders_fini(&mc->session_render.shaders, vk);
		mc->session_render.shaders_loaded = false;
	}
	if (mc->session_render.pipeline_cache != VK_NULL_HANDLE) {
		vk->vkDestroyPipelineCache(vk->device, mc->session_render.pipeline_cache, NULL);
		mc->session_render.pipeline_cache = VK_NULL_HANDLE;
	}

	mc->session_render.composite_initialized = false;
}

/*!
 * Check if any window-space layers exist in the delivered frame.
 */
static bool
has_window_space_layers(struct multi_compositor *mc)
{
	for (uint32_t i = 0; i < mc->delivered.layer_count; i++) {
		if (mc->delivered.layers[i].data.type == XRT_LAYER_WINDOW_SPACE) {
			return true;
		}
	}
	return false;
}

/*!
 * Composite all layers (projection + window-space) into the intermediate atlas
 * targets before display processing. This is the pre-display-processing compositing step.
 *
 * Intel CCS workaround (Intel Iris Xe / Gen12 iGPU):
 * On Intel, fragment shader sampling of cross-device shared images produces a
 * black right eye due to CCS (Color Control Surface) metadata not being resolved.
 * Fix: pre-blit shared projection images into compositor-owned local copies via
 * vkCmdBlitImage (which works on Intel), then sample the local copies in the
 * compositing render pass. This avoids shader reads of cross-device images entirely.
 * Cost: 2 extra same-size blits per frame (~microseconds on modern GPUs).
 * On NVIDIA this is a harmless no-op — NVIDIA does not use CCS compression.
 *
 * @param mc  The multi_compositor
 * @param vk  The Vulkan bundle
 * @param cmd The command buffer to record into
 * @param out_left_view  Output: left eye view of composited result
 * @param out_right_view Output: right eye view of composited result
 * @return true if compositing was performed
 */
static bool
composite_layers_to_intermediate(struct multi_compositor *mc,
                                 struct vk_bundle *vk,
                                 VkCommandBuffer cmd,
                                 VkImageView *out_left_view,
                                 VkImageView *out_right_view)
{
	// Find the projection (or zone-3D) layer first. Zone-3D carries an
	// embedded projection (XR_EXT_display_zones, #568) and composites the
	// same way; only the window-space layer path reaches here today.
	struct multi_layer_entry *proj_layer = NULL;
	for (uint32_t i = 0; i < mc->delivered.layer_count; i++) {
		enum xrt_layer_type type = mc->delivered.layers[i].data.type;
		if (type == XRT_LAYER_PROJECTION || type == XRT_LAYER_PROJECTION_DEPTH ||
		    type == XRT_LAYER_ZONE_3D) {
			proj_layer = &mc->delivered.layers[i];
			break;
		}
	}

	if (proj_layer == NULL) {
		U_LOG_W("[composite] No projection layer found");
		return false;
	}

	// Get projection layer image info
	int imgW = 0, imgH = 0;
	VkFormat imgFmt = VK_FORMAT_UNDEFINED;
	VkImageView leftProjView = VK_NULL_HANDLE, rightProjView = VK_NULL_HANDLE;
	VkImage leftProjImage = VK_NULL_HANDLE, rightProjImage = VK_NULL_HANDLE;
	uint32_t leftProjArray = 0, rightProjArray = 0;

	if (!get_session_layer_view(proj_layer, 0, &imgW, &imgH, &imgFmt, &leftProjView, &leftProjImage,
	                            &leftProjArray)) {
		U_LOG_W("[composite] Could not extract left projection view");
		return false;
	}

	// Mono mode (viewCount=1): duplicate left view for right eye so the
	// compositing pipeline can run unchanged.  The right composite output
	// is identical to the left and is simply discarded by the caller.
	bool mono_composite = (proj_layer->data.view_count < 2);
	if (mono_composite) {
		rightProjView = leftProjView;
		rightProjImage = leftProjImage;
		rightProjArray = leftProjArray;
	} else if (!get_session_layer_view(proj_layer, 1, &imgW, &imgH, &imgFmt, &rightProjView,
	                                    &rightProjImage, &rightProjArray)) {
		U_LOG_W("[composite] Could not extract right projection view");
		return false;
	}

	// Recreate composite resources if projection size changed (window resize)
	if (mc->session_render.composite_initialized &&
	    ((uint32_t)imgW != mc->session_render.composite_width ||
	     (uint32_t)imgH != mc->session_render.composite_height)) {
		U_LOG_W("[composite] Projection size changed %ux%u -> %ux%u, recreating",
		        mc->session_render.composite_width, mc->session_render.composite_height,
		        (uint32_t)imgW, (uint32_t)imgH);
		fini_composite_resources(mc, vk);
	}

	// Lazily init composite resources if needed
	if (!mc->session_render.composite_initialized) {
		if (!init_composite_resources(mc, vk, (uint32_t)imgW, (uint32_t)imgH, imgFmt)) {
			return false;
		}
	}

	uint32_t cw = mc->session_render.composite_width;
	uint32_t ch = mc->session_render.composite_height;

	// Step 1: Transition both composite images UNDEFINED → COLOR_ATTACHMENT
	// (compositor-owned images, safe to transition; LOAD_OP_CLEAR will initialize them)
	VkImageMemoryBarrier barriers_to_attach[2];
	for (int eye = 0; eye < 2; eye++) {
		barriers_to_attach[eye] = (VkImageMemoryBarrier){
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = 0,
		    .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		    .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		    .image = mc->session_render.composite_images[eye],
		    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		};
	}
	vk->vkCmdPipelineBarrier(cmd,
	                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	                         0, 0, NULL, 0, NULL, 2, barriers_to_attach);

	// Pre-blit shared projection images into compositor-owned local copies (Intel CCS workaround).
	// On Intel Iris Xe (Gen12), fragment shader sampling of cross-device shared images produces
	// a black right eye due to CCS (Color Control Surface) metadata not being resolved for shader
	// reads. vkCmdBlitImage (transfer read) works correctly on Intel, so we blit shared images
	// into local preblit copies, then sample those in the compositing render pass.
	// On NVIDIA this is a harmless extra copy (~microseconds for same-size same-format blit).

	{
		VkImage shared_imgs[2] = {leftProjImage, rightProjImage};
		uint32_t shared_layers[2] = {leftProjArray, rightProjArray};
		bool same_source = (shared_imgs[0] == shared_imgs[1] &&
		                    shared_layers[0] == shared_layers[1]);

		// Per-eye source offsets for single-swapchain SBS apps.
		// Mono mode: right eye duplicates left eye offsets.
		int src_off_x[2], src_off_y[2];
		src_off_x[0] = proj_layer->data.proj.v[0].sub.rect.offset.w;
		src_off_y[0] = proj_layer->data.proj.v[0].sub.rect.offset.h;
		src_off_x[1] = mono_composite ? src_off_x[0] : proj_layer->data.proj.v[1].sub.rect.offset.w;
		src_off_y[1] = mono_composite ? src_off_y[0] : proj_layer->data.proj.v[1].sub.rect.offset.h;

		// Pre-barriers: shared images GENERAL->TRANSFER_SRC, preblit UNDEFINED->TRANSFER_DST
		// Deduplicate source barrier when both eyes share the same image.
		VkImageMemoryBarrier pre[4];
		uint32_t pre_count = 0;
		pre[pre_count++] = (VkImageMemoryBarrier){
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = 0,
		    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		    .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
		    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		    .image = shared_imgs[0],
		    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, shared_layers[0], 1},
		};
		if (!same_source) {
			pre[pre_count++] = (VkImageMemoryBarrier){
			    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			    .srcAccessMask = 0,
			    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
			    .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
			    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			    .image = shared_imgs[1],
			    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, shared_layers[1], 1},
			};
		}
		pre[pre_count++] = (VkImageMemoryBarrier){
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = 0,
		    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		    .image = mc->session_render.preblit_images[0],
		    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		};
		pre[pre_count++] = (VkImageMemoryBarrier){
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = 0,
		    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		    .image = mc->session_render.preblit_images[1],
		    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		};
		vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
		                         pre_count, pre);

		// Blit shared images into preblit copies with per-eye source offsets
		for (int eye = 0; eye < 2; eye++) {
			VkImageBlit region = {
			    .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, shared_layers[eye], 1},
			    .srcOffsets = {{src_off_x[eye], src_off_y[eye], 0},
			                  {src_off_x[eye] + (int32_t)cw, src_off_y[eye] + (int32_t)ch, 1}},
			    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
			    .dstOffsets = {{0, 0, 0}, {(int32_t)cw, (int32_t)ch, 1}},
			};
			vk->vkCmdBlitImage(cmd, shared_imgs[eye], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			                   mc->session_render.preblit_images[eye],
			                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
			                   VK_FILTER_NEAREST);
		}

		// Post-barriers: shared images TRANSFER_SRC->GENERAL (restore for next frame),
		// preblit TRANSFER_DST->SHADER_READ_ONLY (ready for sampling in render pass)
		// Deduplicate source barrier when both eyes share the same image.
		VkImageMemoryBarrier post[4];
		uint32_t post_count = 0;
		post[post_count++] = (VkImageMemoryBarrier){
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		    .dstAccessMask = 0,
		    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
		    .image = shared_imgs[0],
		    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, shared_layers[0], 1},
		};
		if (!same_source) {
			post[post_count++] = (VkImageMemoryBarrier){
			    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			    .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
			    .dstAccessMask = 0,
			    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
			    .image = shared_imgs[1],
			    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, shared_layers[1], 1},
			};
		}
		post[post_count++] = (VkImageMemoryBarrier){
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
		    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		    .image = mc->session_render.preblit_images[0],
		    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		};
		post[post_count++] = (VkImageMemoryBarrier){
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
		    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		    .image = mc->session_render.preblit_images[1],
		    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		};
		vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
		                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
		                         0, 0, NULL, 0, NULL, post_count, post);
	}

	// UBO stride for sub-allocation from the persistent UBO buffer
	VkDeviceSize ubo_stride = sizeof(struct xrt_normalized_rect) + sizeof(struct xrt_matrix_4x4);

	// Projection layer image views: use preblit copies (compositor-owned, safe for shader sampling)
	VkImageView proj_views[2] = {
	    mc->session_render.preblit_views[0],
	    mc->session_render.preblit_views[1],
	};

	// Step 2: For each eye — begin render pass, draw projection quad, draw overlays, end render pass.
	// Preblit copies are in SHADER_READ_ONLY_OPTIMAL (transitioned above).
	//
	// Descriptor set allocation: ds[0]=eye0 projection, ds[1]=eye1 projection,
	// ds[2..N]=overlay draws (unique per eye×overlay to avoid aliasing, since
	// vkUpdateDescriptorSets is immediate and GPU reads final host state).
	uint32_t ws_desc_index = 2; // overlays start after both projection descriptor sets
	for (int eye = 0; eye < 2; eye++) {
		// --- Projection layer: fullscreen opaque quad (descriptor set 0) ---
		{
			// Fullscreen MVP: scale the [-0.5, 0.5] unit quad to [-1, 1] NDC
			struct xrt_matrix_4x4 mvp;
			// clang-format off
			mvp.v[0]  = 2.0f; mvp.v[1]  = 0.0f; mvp.v[2]  = 0.0f; mvp.v[3]  = 0.0f;
			mvp.v[4]  = 0.0f; mvp.v[5]  = -2.0f; mvp.v[6]  = 0.0f; mvp.v[7]  = 0.0f;
			mvp.v[8]  = 0.0f; mvp.v[9]  = 0.0f; mvp.v[10] = 1.0f; mvp.v[11] = 0.0f;
			mvp.v[12] = 0.0f; mvp.v[13] = 0.0f; mvp.v[14] = 0.5f; mvp.v[15] = 1.0f;
			// clang-format on

			struct
			{
				struct xrt_normalized_rect post_transform;
				struct xrt_matrix_4x4 mvp;
			} ubo_data;

			// UV post_transform: identity {0,0,1,1} because the preblit
			// images already contain the correctly cropped per-eye content.
			// The original norm_rect maps into the full SBS swapchain UV
			// space, but the preblit step (above) extracted each eye's
			// sub-region into a full-extent local image. Using norm_rect
			// here would double-crop (e.g. sampling only UVs [0,0.5] of
			// an already-cropped 960px image → showing only 480px).
			ubo_data.post_transform.x = 0.0f;
			ubo_data.post_transform.y = 0.0f;
			ubo_data.post_transform.w = 1.0f;
			ubo_data.post_transform.h = 1.0f;
			if (proj_layer->data.flip_y) {
				// GL textures are Y-flipped: remap UV y from [0,1]→[1,0]
				// to flip the preblit content. Same pattern as overlay layers.
				ubo_data.post_transform.y += ubo_data.post_transform.h;
				ubo_data.post_transform.h = -ubo_data.post_transform.h;
			}
			ubo_data.mvp = mvp;

			// Write UBO data — projection uses first slot per eye
			uint32_t ubo_index = eye;
			VkDeviceSize ubo_offset = ubo_index * ubo_stride;
			memcpy((uint8_t *)mc->session_render.composite_ubo_mapped + ubo_offset,
			       &ubo_data, sizeof(ubo_data));

			// Update descriptor set for this eye with projection image
			VkDescriptorSet ds_set = mc->session_render.composite_desc_sets[eye];

			VkDescriptorBufferInfo buf_desc = {
			    .buffer = mc->session_render.composite_ubo_buffer,
			    .offset = ubo_offset,
			    .range = sizeof(ubo_data),
			};

			VkDescriptorImageInfo img_desc = {
			    .sampler = mc->session_render.composite_sampler,
			    .imageView = proj_views[eye],
			    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			};

			VkWriteDescriptorSet writes[2] = {
			    {
			        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet = ds_set,
			        .dstBinding = 0,
			        .descriptorCount = 1,
			        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			        .pBufferInfo = &buf_desc,
			    },
			    {
			        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet = ds_set,
			        .dstBinding = 1,
			        .descriptorCount = 1,
			        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .pImageInfo = &img_desc,
			    },
			};
			vk->vkUpdateDescriptorSets(vk->device, 2, writes, 0, NULL);
		}

		// Begin render pass (LOAD_OP_CLEAR clears to transparent black)
		VkClearValue clear_value = {.color = {{0.0f, 0.0f, 0.0f, 0.0f}}};
		VkRenderPassBeginInfo rp_begin = {
		    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		    .renderPass = mc->session_render.composite_render_pass,
		    .framebuffer = mc->session_render.composite_framebuffers[eye],
		    .renderArea = {
		        .offset = {0, 0},
		        .extent = {cw, ch},
		    },
		    .clearValueCount = 1,
		    .pClearValues = &clear_value,
		};
		vk->vkCmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

		// Set viewport and scissor
		VkViewport eye_viewport = {
		    .x = 0.0f,
		    .y = 0.0f,
		    .width = (float)cw,
		    .height = (float)ch,
		    .minDepth = 0.0f,
		    .maxDepth = 1.0f,
		};
		vk->vkCmdSetViewport(cmd, 0, 1, &eye_viewport);

		VkRect2D scissor = {
		    .offset = {0, 0},
		    .extent = {cw, ch},
		};
		vk->vkCmdSetScissor(cmd, 0, 1, &scissor);

		// Draw projection as fullscreen opaque quad
		VkDescriptorSet proj_ds = mc->session_render.composite_desc_sets[eye];
		vk->vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
		                      mc->session_render.composite_pipeline);
		vk->vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
		                            mc->session_render.composite_pipe_layout,
		                            0, 1, &proj_ds, 0, NULL);
		vk->vkCmdDraw(cmd, 4, 1, 0, 0);

		// --- Overlay layers: alpha-blended quads (descriptor sets 2+) ---
		for (uint32_t li = 0; li < mc->delivered.layer_count; li++) {
			struct multi_layer_entry *layer = &mc->delivered.layers[li];
			if (layer->data.type != XRT_LAYER_WINDOW_SPACE) {
				continue;
			}
			if (ws_desc_index >= XRT_MAX_LAYERS) {
				break;
			}

			const struct xrt_layer_window_space_data *ws = &layer->data.window_space;
			struct xrt_swapchain *xsc = layer->xscs[0];
			if (xsc == NULL) {
				continue;
			}

			struct comp_swapchain *sc = comp_swapchain(xsc);
			uint32_t img_idx = ws->sub.image_index;
			const struct comp_swapchain_image *ws_image = &sc->images[img_idx];
			VkImageView ws_view = get_image_view(ws_image, layer->data.flags, ws->sub.array_index);
			if (ws_view == VK_NULL_HANDLE) {
				continue;
			}

			// Compute per-eye disparity offset
			float half_disp = ws->disparity / 2.0f;
			float eye_shift = (eye == 0) ? -half_disp : half_disp;

			// Window-space fractional coords → Vulkan NDC [-1, 1]
			// Vulkan NDC: Y=-1 is top, Y=+1 is bottom.
			// Shader flips Y (pos.y = -pos.y), so ndc_sy must be negative
			// to map shader top (+0.5) to Vulkan top (NDC -1).
			float frac_cx = ws->x + ws->width / 2.0f + eye_shift;
			float frac_cy = ws->y + ws->height / 2.0f;
			float ndc_cx = frac_cx * 2.0f - 1.0f;
			float ndc_cy = frac_cy * 2.0f - 1.0f;
			float ndc_sx = ws->width * 2.0f;
			float ndc_sy = -(ws->height * 2.0f);

			// Build orthographic MVP (quad vert shader uses [-0.5, 0.5] unit quad)
			struct xrt_matrix_4x4 mvp;
			// clang-format off
			mvp.v[0]  = ndc_sx; mvp.v[1]  = 0.0f;   mvp.v[2]  = 0.0f; mvp.v[3]  = 0.0f;
			mvp.v[4]  = 0.0f;   mvp.v[5]  = ndc_sy;  mvp.v[6]  = 0.0f; mvp.v[7]  = 0.0f;
			mvp.v[8]  = 0.0f;   mvp.v[9]  = 0.0f;   mvp.v[10] = 1.0f; mvp.v[11] = 0.0f;
			mvp.v[12] = ndc_cx; mvp.v[13] = ndc_cy;  mvp.v[14] = 0.5f; mvp.v[15] = 1.0f;
			// clang-format on

			struct
			{
				struct xrt_normalized_rect post_transform;
				struct xrt_matrix_4x4 mvp;
			} ubo_data;

			ubo_data.post_transform.x = ws->sub.norm_rect.x;
			ubo_data.post_transform.y = ws->sub.norm_rect.y;
			ubo_data.post_transform.w = ws->sub.norm_rect.w;
			ubo_data.post_transform.h = ws->sub.norm_rect.h;

			if (layer->data.flip_y) {
				ubo_data.post_transform.y += ubo_data.post_transform.h;
				ubo_data.post_transform.h = -ubo_data.post_transform.h;
			}

			ubo_data.mvp = mvp;

			// Write UBO data — each draw has a unique UBO slot matching its descriptor set index
			uint32_t ubo_index = ws_desc_index;
			VkDeviceSize ubo_offset = ubo_index * ubo_stride;
			memcpy((uint8_t *)mc->session_render.composite_ubo_mapped + ubo_offset,
			       &ubo_data, sizeof(ubo_data));

			// Update descriptor set
			VkDescriptorSet ds_set = mc->session_render.composite_desc_sets[ws_desc_index];

			VkDescriptorBufferInfo buf_desc = {
			    .buffer = mc->session_render.composite_ubo_buffer,
			    .offset = ubo_offset,
			    .range = sizeof(ubo_data),
			};

			VkDescriptorImageInfo img_desc = {
			    .sampler = mc->session_render.composite_sampler,
			    .imageView = ws_view,
			    .imageLayout = VK_IMAGE_LAYOUT_GENERAL, // Overlay shared images stay in GENERAL
			};

			VkWriteDescriptorSet writes[2] = {
			    {
			        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet = ds_set,
			        .dstBinding = 0,
			        .descriptorCount = 1,
			        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			        .pBufferInfo = &buf_desc,
			    },
			    {
			        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet = ds_set,
			        .dstBinding = 1,
			        .descriptorCount = 1,
			        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .pImageInfo = &img_desc,
			    },
			};
			vk->vkUpdateDescriptorSets(vk->device, 2, writes, 0, NULL);

			// Draw overlay quad (pipeline already bound)
			vk->vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
			                            mc->session_render.composite_pipe_layout,
			                            0, 1, &ds_set, 0, NULL);
			vk->vkCmdDraw(cmd, 4, 1, 0, 0);

			ws_desc_index++;
		}

		vk->vkCmdEndRenderPass(cmd);
	}

	// Step 3: Transition both composite images to SHADER_READ_ONLY for weaver input
	VkImageMemoryBarrier barriers_to_read[2];
	for (int eye = 0; eye < 2; eye++) {
		barriers_to_read[eye] = (VkImageMemoryBarrier){
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
		    .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		    .image = mc->session_render.composite_images[eye],
		    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		};
	}
	vk->vkCmdPipelineBarrier(cmd,
	                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	                         0, 0, NULL, 0, NULL, 2, barriers_to_read);

	// Output the per-eye views
	*out_left_view = mc->session_render.composite_eye_views[0];
	*out_right_view = mc->session_render.composite_eye_views[1];

	return true;
}

/*!
 * Recreate the per-session swapchain after a window resize.
 * Waits for all GPU work, recreates swapchain images, destroys old framebuffers,
 * creates new framebuffers, and reallocates command buffers/fences if image count changed.
 *
 * @param mc The multi_compositor with per-session rendering
 * @param vk The Vulkan bundle
 */
static void
recreate_session_swapchain(struct multi_compositor *mc, struct vk_bundle *vk)
{
	struct comp_target *ct = mc->session_render.target;
	if (ct == NULL) {
		return;
	}

	U_LOG_W("[per-session] Recreating swapchain (window resized)...");

	// 1. Wait for ALL pending GPU work to complete
	if (mc->session_render.fenced_buffer >= 0) {
		vk->vkWaitForFences(vk->device, 1,
		                    &mc->session_render.fences[mc->session_render.fenced_buffer],
		                    VK_TRUE, UINT64_MAX);
		mc->session_render.fenced_buffer = -1;
	}
	// Also wait for all fences to ensure no in-flight commands reference old swapchain
	for (uint32_t i = 0; i < mc->session_render.buffer_count; i++) {
		if (mc->session_render.fences[i] != VK_NULL_HANDLE) {
			vk->vkWaitForFences(vk->device, 1, &mc->session_render.fences[i], VK_TRUE, UINT64_MAX);
		}
	}
	// Fences only cover render work; a present (vkQueuePresentKHR) can still be in
	// flight holding a BufferQueue slot. Drain the whole device so destroying the
	// old swapchain releases every buffer back to the queue — otherwise the new
	// swapchain's first vkAcquireNextImageKHR can block forever on Android (#510).
	vk->vkDeviceWaitIdle(vk->device);

	uint32_t old_image_count = mc->session_render.buffer_count;

	// 2. Recreate swapchain images (queries new surface extent internally).
	// Request the SAME format the target already settled on at init
	// (ct->format) rather than a hardcoded one: the per-session render pass +
	// framebuffers were built for that format, and a resize/rotation must not
	// change it. Hardcoding VK_FORMAT_B8G8R8A8_SRGB broke live rotation on
	// Android (#510) — that surface only exposes R8G8B8A8_*, so the init's
	// 4-format fallback (null_compositor.c) picks R8G8B8A8_UNORM, but the
	// recreate then failed find_surface_format → VK_ERROR_INITIALIZATION_FAILED
	// → every acquire failed → compositor froze on the last frame. Fall back to
	// the init's preferred only if the target has no format yet (never on the
	// recreate path, but keeps the request well-formed).
	VkFormat keep_format = (ct->format != VK_FORMAT_UNDEFINED) ? ct->format : VK_FORMAT_B8G8R8A8_SRGB;
	struct comp_target_create_images_info info = {
	    .image_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
	    .format_count = 1,
	    .formats = {keep_format},
	    .extent = {ct->width, ct->height}, // Will be overridden by surface caps
	    .color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
	    .present_mode = VK_PRESENT_MODE_FIFO_KHR,
	};
	comp_target_create_images(ct, &info);

	if (!comp_target_has_images(ct)) {
		U_LOG_E("[per-session] Failed to recreate swapchain images");
		mc->session_render.swapchain_needs_recreate = false;
		return;
	}

	uint32_t new_image_count = ct->image_count;

	// 3. Destroy old framebuffers
	if (mc->session_render.framebuffers != NULL) {
		for (uint32_t i = 0; i < old_image_count; i++) {
			if (mc->session_render.framebuffers[i] != VK_NULL_HANDLE) {
				vk->vkDestroyFramebuffer(vk->device, mc->session_render.framebuffers[i], NULL);
			}
		}
	}

	// 4. Handle image_count change - reallocate arrays if needed
	if (new_image_count != old_image_count) {
		U_LOG_W("[per-session] Image count changed: %u -> %u", old_image_count, new_image_count);

		// Free old command buffers from the pool
		vk->vkFreeCommandBuffers(vk->device, mc->session_render.cmd_pool,
		                         old_image_count, mc->session_render.cmd_buffers);

		// Destroy old fences
		for (uint32_t i = 0; i < old_image_count; i++) {
			if (mc->session_render.fences[i] != VK_NULL_HANDLE) {
				vk->vkDestroyFence(vk->device, mc->session_render.fences[i], NULL);
			}
		}

		// Reallocate arrays
		free(mc->session_render.cmd_buffers);
		free(mc->session_render.fences);
		free(mc->session_render.framebuffers);

		mc->session_render.cmd_buffers = U_TYPED_ARRAY_CALLOC(VkCommandBuffer, new_image_count);
		mc->session_render.fences = U_TYPED_ARRAY_CALLOC(VkFence, new_image_count);
		mc->session_render.framebuffers = U_TYPED_ARRAY_CALLOC(VkFramebuffer, new_image_count);

		if (!mc->session_render.cmd_buffers || !mc->session_render.fences || !mc->session_render.framebuffers) {
			U_LOG_E("[per-session] Failed to allocate new arrays after resize");
			mc->session_render.swapchain_needs_recreate = false;
			return;
		}

		// Allocate new command buffers
		VkCommandBufferAllocateInfo cb_alloc = {
		    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		    .commandPool = mc->session_render.cmd_pool,
		    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		    .commandBufferCount = new_image_count,
		};
		VkResult vk_ret = vk->vkAllocateCommandBuffers(vk->device, &cb_alloc, mc->session_render.cmd_buffers);
		if (vk_ret != VK_SUCCESS) {
			U_LOG_E("[per-session] Failed to allocate new command buffers: %s", vk_result_string(vk_ret));
			mc->session_render.swapchain_needs_recreate = false;
			return;
		}

		// Create new fences (signaled)
		VkFenceCreateInfo fence_info = {
		    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
		    .flags = VK_FENCE_CREATE_SIGNALED_BIT,
		};
		for (uint32_t i = 0; i < new_image_count; i++) {
			vk_ret = vk->vkCreateFence(vk->device, &fence_info, NULL, &mc->session_render.fences[i]);
			if (vk_ret != VK_SUCCESS) {
				U_LOG_E("[per-session] Failed to create fence %u: %s", i, vk_result_string(vk_ret));
			}
		}
	} else {
		// Same image count - just reallocate framebuffer array for new images
		free(mc->session_render.framebuffers);
		mc->session_render.framebuffers = U_TYPED_ARRAY_CALLOC(VkFramebuffer, new_image_count);
	}

	// 5. Create new framebuffers bound to new swapchain images
	if (mc->session_render.framebuffers != NULL && mc->session_render.render_pass != VK_NULL_HANDLE) {
		for (uint32_t i = 0; i < new_image_count; i++) {
			VkFramebufferCreateInfo fb_info = {
			    .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
			    .renderPass = mc->session_render.render_pass,
			    .attachmentCount = 1,
			    .pAttachments = &ct->images[i].view,
			    .width = ct->width,
			    .height = ct->height,
			    .layers = 1,
			};
			VkResult vk_ret = vk->vkCreateFramebuffer(vk->device, &fb_info, NULL,
			                                           &mc->session_render.framebuffers[i]);
			if (vk_ret != VK_SUCCESS) {
				U_LOG_E("[per-session] Failed to create framebuffer %u: %s", i, vk_result_string(vk_ret));
				mc->session_render.framebuffers[i] = VK_NULL_HANDLE;
			}
		}
	}

	// 6. Update state
	mc->session_render.buffer_count = new_image_count;
	mc->session_render.fenced_buffer = -1;
	mc->session_render.swapchain_needs_recreate = false;

	U_LOG_W("[per-session] Swapchain recreated: %ux%u, %u images", ct->width, ct->height, new_image_count);

}


/*!
 * Ensure the atlas intermediate image exists for display processors
 * that prefer packed atlas input. Creates a single image at
 * (tile_columns * per_eye_width x tile_rows * height) so all views can be
 * blitted into a tiled atlas before passing to the display processor.
 * Reuses the flip_sbs_* fields in session_render.
 * Recreates if size or format changed.
 */
static bool
ensure_session_atlas_image(struct multi_compositor *mc, struct vk_bundle *vk, int per_eye_width, int height,
                         uint32_t tile_columns, uint32_t tile_rows, VkFormat format)
{
	// Cache on the ATLAS dimensions, not the per-tile dims: different grids can
	// share a per-tile size but need a different atlas (e.g. Anaglyph 2x1 →
	// 2*W x H vs Cropped SBS 1x2 → W x 2*H, both per-tile WxH). Keying on per-tile
	// W/H alone kept the stale atlas across such a mode switch → mistiled output
	// after cycling V (#48).
	uint32_t atlas_width = tile_columns * (uint32_t)per_eye_width;
	uint32_t atlas_height = tile_rows * (uint32_t)height;
	if (mc->session_render.flip_initialized && mc->session_render.flip_width == (int)atlas_width &&
	    mc->session_render.flip_height == (int)atlas_height && mc->session_render.flip_format == format) {
		return true;
	}

	// Destroy old if resizing
	if (mc->session_render.flip_initialized) {
		if (mc->session_render.flip_sbs_view != VK_NULL_HANDLE)
			vk->vkDestroyImageView(vk->device, mc->session_render.flip_sbs_view, NULL);
		if (mc->session_render.flip_sbs_image != VK_NULL_HANDLE)
			vk->vkDestroyImage(vk->device, mc->session_render.flip_sbs_image, NULL);
		if (mc->session_render.flip_sbs_memory != VK_NULL_HANDLE)
			vk->vkFreeMemory(vk->device, mc->session_render.flip_sbs_memory, NULL);
		mc->session_render.flip_initialized = false;
	}

	VkExtent2D extent = {atlas_width, atlas_height};
	VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	VkImageSubresourceRange range = {
	    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	    .levelCount = 1,
	    .layerCount = 1,
	};

	VkResult ret = vk_create_image_simple(vk, extent, format, usage,
	                                      &mc->session_render.flip_sbs_memory,
	                                      &mc->session_render.flip_sbs_image);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[per-session] Failed to create atlas image: %s", vk_result_string(ret));
		return false;
	}

	ret = vk_create_view(vk, mc->session_render.flip_sbs_image, VK_IMAGE_VIEW_TYPE_2D, format, range,
	                     &mc->session_render.flip_sbs_view);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[per-session] Failed to create atlas view: %s", vk_result_string(ret));
		return false;
	}

	mc->session_render.flip_width = (int)atlas_width;
	mc->session_render.flip_height = (int)atlas_height;
	mc->session_render.flip_format = format;
	mc->session_render.flip_initialized = true;

	U_LOG_W("[per-session] Created atlas image: %ux%u (per-eye %dx%d, tiles %ux%u) format=%d",
	        atlas_width, atlas_height, per_eye_width, height, tile_columns, tile_rows, format);
	return true;
}

/*!
 * Initialize Vulkan GPU resources for the HUD overlay (image + staging buffer).
 * Called lazily on first HUD render.
 */
static bool
session_render_hud_init_gpu(struct multi_compositor *mc, struct vk_bundle *vk)
{
	if (mc->session_render.hud_gpu_initialized) {
		return true;
	}

	uint32_t hud_w = u_hud_get_width(mc->session_render.hud);
	uint32_t hud_h = u_hud_get_height(mc->session_render.hud);
	uint32_t pixel_size = hud_w * hud_h * 4;
	VkResult ret;

	// Create HUD image (TRANSFER_DST for upload, SAMPLED for blend pipeline)
	VkImageCreateInfo image_info = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .imageType = VK_IMAGE_TYPE_2D,
	    .format = VK_FORMAT_R8G8B8A8_UNORM,
	    .extent = {hud_w, hud_h, 1},
	    .mipLevels = 1,
	    .arrayLayers = 1,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .tiling = VK_IMAGE_TILING_OPTIMAL,
	    .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	ret = vk->vkCreateImage(vk->device, &image_info, NULL, &mc->session_render.hud_image);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[HUD] Failed to create image: %s", vk_result_string(ret));
		return false;
	}

	// Allocate image memory
	VkMemoryRequirements mem_reqs;
	vk->vkGetImageMemoryRequirements(vk->device, mc->session_render.hud_image, &mem_reqs);

	VkMemoryAllocateInfo alloc_info = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .allocationSize = mem_reqs.size,
	};
	if (!vk_get_memory_type(vk, mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
	                        &alloc_info.memoryTypeIndex)) {
		U_LOG_E("[HUD] Failed to find device-local memory type");
		vk->vkDestroyImage(vk->device, mc->session_render.hud_image, NULL);
		mc->session_render.hud_image = VK_NULL_HANDLE;
		return false;
	}

	ret = vk->vkAllocateMemory(vk->device, &alloc_info, NULL, &mc->session_render.hud_memory);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[HUD] Failed to allocate image memory");
		vk->vkDestroyImage(vk->device, mc->session_render.hud_image, NULL);
		mc->session_render.hud_image = VK_NULL_HANDLE;
		return false;
	}
	vk->vkBindImageMemory(vk->device, mc->session_render.hud_image, mc->session_render.hud_memory, 0);

	// Create staging buffer
	VkBufferCreateInfo buf_info = {
	    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
	    .size = pixel_size,
	    .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	};
	ret = vk->vkCreateBuffer(vk->device, &buf_info, NULL, &mc->session_render.hud_staging_buffer);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[HUD] Failed to create staging buffer");
		return false;
	}

	VkMemoryRequirements buf_reqs;
	vk->vkGetBufferMemoryRequirements(vk->device, mc->session_render.hud_staging_buffer, &buf_reqs);

	VkMemoryAllocateInfo buf_alloc = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .allocationSize = buf_reqs.size,
	};
	if (!vk_get_memory_type(vk, buf_reqs.memoryTypeBits,
	                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	                        &buf_alloc.memoryTypeIndex)) {
		U_LOG_E("[HUD] Failed to find host-visible memory type");
		return false;
	}

	ret = vk->vkAllocateMemory(vk->device, &buf_alloc, NULL, &mc->session_render.hud_staging_memory);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[HUD] Failed to allocate staging memory");
		return false;
	}
	vk->vkBindBufferMemory(vk->device, mc->session_render.hud_staging_buffer,
	                       mc->session_render.hud_staging_memory, 0);

	// Persistently map staging buffer
	ret = vk->vkMapMemory(vk->device, mc->session_render.hud_staging_memory, 0, pixel_size, 0,
	                      &mc->session_render.hud_staging_mapped);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[HUD] Failed to map staging buffer");
		return false;
	}

	// Init alpha-blend pipeline for semi-transparent HUD overlay (image is
	// passed per-draw now so the helper can be reused with rotating swapchain
	// images and per-layer destination rects).
	if (!vk_hud_blend_init(&mc->session_render.hud_blend, vk, mc->session_render.target->format)) {
		U_LOG_E("[HUD] Failed to init alpha-blend pipeline");
		return false;
	}

	mc->session_render.hud_gpu_initialized = true;
	U_LOG_W("[HUD] Vulkan GPU resources initialized (%ux%u)", hud_w, hud_h);
	return true;
}

/*!
 * Alpha-blend HUD overlay onto swapchain image (post-weave).
 * Transitions: PRESENT_SRC -> COLOR_ATTACHMENT -> PRESENT_SRC
 */
static void
session_render_hud_overlay(struct multi_compositor *mc,
                           struct vk_bundle *vk,
                           VkCommandBuffer cmd,
                           VkImage swapchain_image,
                           VkImageView swapchain_view,
                           uint32_t fb_width,
                           uint32_t fb_height)
{
	if (mc->session_render.hud == NULL || !u_hud_is_visible()) {
		return;
	}

	// Compute FPS
	uint64_t now_ns = os_monotonic_get_ns();
	if (mc->session_render.hud_last_frame_time_ns != 0) {
		float dt_ms = (float)(now_ns - mc->session_render.hud_last_frame_time_ns) / 1e6f;
		mc->session_render.hud_smoothed_frame_time_ms =
		    mc->session_render.hud_smoothed_frame_time_ms * 0.9f + dt_ms * 0.1f;
	}
	mc->session_render.hud_last_frame_time_ns = now_ns;

	float fps = 0.0f;
	if (mc->session_render.hud_smoothed_frame_time_ms > 0.0f) {
		fps = 1000.0f / mc->session_render.hud_smoothed_frame_time_ms;
	}

	// Get eye positions and display dims via display processor vtable
	struct xrt_eye_positions eye_pos = {0};
	float disp_w_mm = 0.0f, disp_h_mm = 0.0f;
	float nom_x = 0.0f, nom_y = 0.0f, nom_z = 600.0f;

	if (mc->session_render.display_processor != NULL) {
		if (xrt_display_processor_get_predicted_eye_positions(
		        mc->session_render.display_processor, &eye_pos) && eye_pos.valid) {
			// eye_pos populated with N-view positions
		}

		float dim_w = 0.0f, dim_h = 0.0f;
		if (xrt_display_processor_get_display_dimensions(
		        mc->session_render.display_processor, &dim_w, &dim_h)) {
			disp_w_mm = dim_w * 1000.0f;
			disp_h_mm = dim_h * 1000.0f;
		}
	}

	// Get head device for device name and forward vector
	struct xrt_device *xdev = (mc->xsysd != NULL) ? mc->xsysd->static_roles.head : NULL;

	// Fallback: display info from xrt_system_compositor_info (populated at init)
	float zoom_scale = 0.0f;
	if (disp_w_mm == 0.0f && disp_h_mm == 0.0f) {
		const struct xrt_system_compositor_info *info = &mc->msc->base.info;
		if (info->display_width_m > 0.0f) {
			disp_w_mm = info->display_width_m * 1000.0f;
			disp_h_mm = info->display_height_m * 1000.0f;
			nom_y = info->nominal_viewer_y_m * 1000.0f;
			nom_z = info->nominal_viewer_z_m * 1000.0f;
		}
	}

	// Get render dimensions from last delivered layer
	uint32_t render_w = 0, render_h = 0;
	for (uint32_t i = 0; i < mc->delivered.layer_count; i++) {
		enum xrt_layer_type type = mc->delivered.layers[i].data.type;
		if (type == XRT_LAYER_PROJECTION || type == XRT_LAYER_PROJECTION_DEPTH) {
			render_w = mc->delivered.layers[i].data.proj.v[0].sub.rect.extent.w;
			render_h = mc->delivered.layers[i].data.proj.v[0].sub.rect.extent.h;
			break;
		}
	}

	// Virtual display position + forward vector from qwerty device pose.
	float vdisp_x = 0.0f, vdisp_y = 0.0f, vdisp_z = 0.0f;
	float fwd_x = 0.0f, fwd_y = 0.0f, fwd_z = -1.0f;
#ifdef XRT_BUILD_DRIVER_QWERTY
	if (mc->xsysd != NULL) {
		struct xrt_pose qwerty_pose;
		if (qwerty_get_hmd_pose(mc->xsysd->xdevs, mc->xsysd->xdev_count, &qwerty_pose)) {
			vdisp_x = qwerty_pose.position.x;
			vdisp_y = qwerty_pose.position.y;
			vdisp_z = qwerty_pose.position.z;
			struct xrt_vec3 fwd_in = {0, 0, -1};
			struct xrt_vec3 fwd_out;
			math_quat_rotate_vec3(&qwerty_pose.orientation, &fwd_in, &fwd_out);
			fwd_x = fwd_out.x;
			fwd_y = fwd_out.y;
			fwd_z = fwd_out.z;
		}
	}
#endif

	// Device name includes mode suffix (set by device's set_property handler)
	const char *device_name = (xdev != NULL) ? xdev->str : NULL;

	struct u_hud_data data = {0};
	data.device_name = device_name;
	data.fps = fps;
	data.frame_time_ms = mc->session_render.hud_smoothed_frame_time_ms;
	data.mode_3d = mc->hardware_display_3d;
	if (xdev != NULL && xdev->hmd != NULL) {
		uint32_t idx = xdev->hmd->active_rendering_mode_index;
		if (idx < xdev->rendering_mode_count) {
			data.rendering_mode_name = xdev->rendering_modes[idx].mode_name;
		}
	}
	data.render_width = render_w;
	data.render_height = render_h;
	if (xdev != NULL && xdev->rendering_mode_count > 0) {
		u_tiling_compute_system_atlas(xdev->rendering_modes, xdev->rendering_mode_count,
		                              &data.swapchain_width, &data.swapchain_height);
	}
	data.window_width = fb_width;
	data.window_height = fb_height;
	data.display_width_mm = disp_w_mm;
	data.display_height_mm = disp_h_mm;
	data.nominal_x = nom_x;
	data.nominal_y = nom_y;
	data.nominal_z = nom_z;
	if (!eye_pos.valid) {
		// Fallback: nominal stereo (in mm)
		eye_pos.count = 2;
		eye_pos.eyes[0] = (struct xrt_eye_position){-0.032f, nom_y / 1000.0f, nom_z / 1000.0f};
		eye_pos.eyes[1] = (struct xrt_eye_position){ 0.032f, nom_y / 1000.0f, nom_z / 1000.0f};
	}
	data.eye_count = eye_pos.count;
	for (uint32_t e = 0; e < eye_pos.count && e < 8; e++) {
		data.eyes[e].x = eye_pos.eyes[e].x * 1000.0f;
		data.eyes[e].y = eye_pos.eyes[e].y * 1000.0f;
		data.eyes[e].z = eye_pos.eyes[e].z * 1000.0f;
	}
	data.eye_tracking_active = eye_pos.is_tracking;

	data.zoom_scale = zoom_scale;
	data.vdisp_x = vdisp_x;
	data.vdisp_y = vdisp_y;
	data.vdisp_z = vdisp_z;
	data.forward_x = fwd_x;
	data.forward_y = fwd_y;
	data.forward_z = fwd_z;

#ifdef XRT_BUILD_DRIVER_QWERTY
	if (mc->xsysd != NULL) {
		struct qwerty_view_state ss;
		if (qwerty_get_view_state(mc->xsysd->xdevs, mc->xsysd->xdev_count, &ss)) {
			data.camera_mode = ss.camera_mode;
			data.ipd_factor = ss.ipd_factor;
			data.parallax_factor = ss.parallax_factor;
			data.inv_convergence_distance = ss.inv_convergence_distance;
			data.half_tan_vfov = ss.half_tan_vfov;
			data.m2v = ss.m2v;
			data.virtual_display_height = ss.virtual_display_height;
			data.perspective_factor = ss.perspective_factor;
			data.nominal_viewer_z = ss.nominal_viewer_z;
			data.screen_height_m = ss.screen_height_m;
		}
	}
#endif

	bool dirty = u_hud_update(mc->session_render.hud, &data);

	// Lazy-init GPU resources
	if (!session_render_hud_init_gpu(mc, vk)) {
		return;
	}

	uint32_t hud_w = u_hud_get_width(mc->session_render.hud);
	uint32_t hud_h = u_hud_get_height(mc->session_render.hud);

	// Upload pixels to staging buffer if changed
	if (dirty) {
		memcpy(mc->session_render.hud_staging_mapped,
		       u_hud_get_pixels(mc->session_render.hud),
		       hud_w * hud_h * 4);
	}

	// Transition HUD image: UNDEFINED -> TRANSFER_DST (for copy from staging)
	VkImageMemoryBarrier hud_to_dst = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = 0,
	    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	    .image = mc->session_render.hud_image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &hud_to_dst);

	// Copy staging buffer -> HUD image
	VkBufferImageCopy copy_region = {
	    .bufferOffset = 0,
	    .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	    .imageExtent = {hud_w, hud_h, 1},
	};
	vk->vkCmdCopyBufferToImage(cmd, mc->session_render.hud_staging_buffer,
	                           mc->session_render.hud_image,
	                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                           1, &copy_region);

	// Transition HUD image: TRANSFER_DST -> SHADER_READ_ONLY (for sampling in blend pipeline)
	VkImageMemoryBarrier hud_to_src = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	    .image = mc->session_render.hud_image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &hud_to_src);

	// Alpha-blend HUD onto swapchain (PRESENT_SRC -> COLOR_ATTACHMENT -> PRESENT_SRC)
	static bool mono_hud_blend_logged = false;
	if (!mc->hardware_display_3d && !mono_hud_blend_logged) {
		U_LOG_W("[HUD] Mono (2D) mode: blend draw fb=%ux%u hud=%ux%u init=%d",
		        fb_width, fb_height, hud_w, hud_h,
		        mc->session_render.hud_blend.initialized);
		mono_hud_blend_logged = true;
	}
	// Bottom-left placement with a 10px margin (matches the previous behavior
	// when destination rect was hardcoded inside vk_hud_blend_draw).
	int32_t hud_dst_x = 10;
	int32_t hud_dst_y = (fb_height > hud_h + 10u) ? (int32_t)(fb_height - hud_h - 10u) : 0;
	vk_hud_blend_draw(&mc->session_render.hud_blend, vk, cmd, swapchain_view, swapchain_image, fb_width,
	                   fb_height, mc->session_render.hud_image, hud_w, hud_h,
	                   hud_dst_x, hud_dst_y, hud_w, hud_h);
}

/*!
 * Lazy-init the shared workspace alpha-blend pipeline. Chrome, overlay and
 * cursor all blend RGBA over the same target format, so they reuse one
 * @ref vk_hud_blend instance (the field is named chrome_blend for historical
 * reasons; it backs all three). (#48)
 */
static bool
ensure_workspace_blend(struct multi_compositor *mc, struct vk_bundle *vk)
{
	if (mc->session_render.chrome_blend_initialized) {
		return true;
	}
	if (!vk_hud_blend_init(&mc->session_render.chrome_blend, vk, mc->session_render.target->format)) {
		U_LOG_E("[workspace] Failed to init alpha-blend pipeline");
		return false;
	}
	mc->session_render.chrome_blend_initialized = true;
	U_LOG_W("[workspace] Chrome/overlay/cursor blend pipeline initialized");
	return true;
}

/*!
 * Alpha-blend one controller-submitted workspace layer (chrome / overlay /
 * cursor) at an axis-aligned destination rect, post-weave (#48). Mirrors the HUD
 * path: the shared cross-process source image goes GENERAL → SHADER_READ_ONLY →
 * GENERAL (images rest in GENERAL between uses, see @ref composite_local_2d_layers);
 * the target is taken/left in PRESENT_SRC by @ref vk_hud_blend_draw.
 */
static void
workspace_blend_layer(struct multi_compositor *mc,
                      struct vk_bundle *vk,
                      VkCommandBuffer cmd,
                      VkImage swapchain_image,
                      VkImageView swapchain_view,
                      uint32_t fb_width,
                      uint32_t fb_height,
                      VkImage src_image,
                      uint32_t src_w,
                      uint32_t src_h,
                      int32_t dst_x,
                      int32_t dst_y,
                      uint32_t dst_w,
                      uint32_t dst_h)
{
	if (src_image == VK_NULL_HANDLE || dst_w == 0 || dst_h == 0) {
		return;
	}

	VkImageMemoryBarrier to_src = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = 0,
	    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
	    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	    .image = src_image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
	                         NULL, 0, NULL, 1, &to_src);

	vk_hud_blend_draw(&mc->session_render.chrome_blend, vk, cmd, swapchain_view, swapchain_image, fb_width,
	                   fb_height, src_image, src_w, src_h, dst_x, dst_y, dst_w, dst_h);

	VkImageMemoryBarrier to_general = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
	    .dstAccessMask = 0,
	    .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
	    .image = src_image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
	                         NULL, 0, NULL, 1, &to_general);
}
#endif // !XRT_OS_MACOS

//! Display dims (meters) for this session, DP-first with system-info fallback
//! (matches the HUD). Returns false if neither yields valid dims.
static bool
session_display_dims_m(struct multi_compositor *mc, float *out_w_m, float *out_h_m)
{
	float w = 0.0f, h = 0.0f;
	if (mc->session_render.display_processor != NULL) {
		(void)xrt_display_processor_get_display_dimensions(mc->session_render.display_processor, &w, &h);
	}
	if (w <= 0.0f || h <= 0.0f) {
		const struct xrt_system_compositor_info *info = &mc->msc->base.info;
		w = info->display_width_m;
		h = info->display_height_m;
	}
	if (w <= 0.0f || h <= 0.0f) {
		return false;
	}
	*out_w_m = w;
	*out_h_m = h;
	return true;
}

#ifndef XRT_OS_MACOS
/*!
 * Physical size, in meters, of THIS client's window surface (Tier-2, #59). The
 * display's pixel density is uniform, so window_meters = window_px ÷ display
 * px-per-meter. The Tier-1 workspace composites used the full display meters with
 * the window's framebuffer px, which over-sized chrome/overlays/cursor once a
 * window tiled to a sub-rect (px_per_m came out as window_px ÷ display_m instead
 * of the true display density). Falls back to the full display dims when px info
 * is missing (a full-display Tier-1 window then reads identical to before).
 */
static bool
session_window_dims_m(struct multi_compositor *mc, uint32_t fb_w, uint32_t fb_h, float *out_w_m, float *out_h_m)
{
	float disp_w_m = 0.0f, disp_h_m = 0.0f;
	if (!session_display_dims_m(mc, &disp_w_m, &disp_h_m)) {
		return false;
	}
	const struct xrt_system_compositor_info *info = &mc->msc->base.info;
	uint32_t disp_px_w = info->display_pixel_width;
	uint32_t disp_px_h = info->display_pixel_height;
	if (disp_px_w == 0 || disp_px_h == 0 || fb_w == 0 || fb_h == 0) {
		*out_w_m = disp_w_m;
		*out_h_m = disp_h_m;
		return true;
	}
	float px_per_m_x = (float)disp_px_w / disp_w_m;
	float px_per_m_y = (float)disp_px_h / disp_h_m;
	*out_w_m = (float)fb_w / px_per_m_x;
	*out_h_m = (float)fb_h / px_per_m_y;
	return true;
}
#endif // !XRT_OS_MACOS

/*!
 * Public window-dims query for the workspace IPC layer (macOS get_window_pose).
 *
 * In the macOS OOP single-app model the content client fills the display, so
 * its "window" IS the display — and the chrome composite
 * (@ref session_render_chrome_overlay) places chrome against the same display
 * dims. Returning those dims here keeps the controller's pill sizing
 * (win_w * fraction) consistent with where the runtime composites it. The pose
 * is identity at the origin (the composite ignores per-client window pose in
 * the single-app model; true multi-window placement is Tier-2, #59).
 */
bool
comp_multi_workspace_get_client_window_dims(struct xrt_compositor *xc,
                                            struct xrt_pose *out_pose,
                                            float *out_w_m,
                                            float *out_h_m)
{
	struct multi_compositor *mc = multi_compositor(xc);
	if (mc == NULL) {
		return false;
	}
	// Tier-2 (#59): if the controller has placed this client, echo the stored
	// pose + window meters so its chrome sizing and hit-testing match the actual
	// tiled NSWindow. Until then, fall back to the Tier-1 full-display window.
	if (comp_multi_workspace_load_window_pose(xc, out_pose, out_w_m, out_h_m)) {
		return true;
	}
	if (out_pose != NULL) {
		struct xrt_pose ident = XRT_POSE_IDENTITY;
		*out_pose = ident;
	}
	return session_display_dims_m(mc, out_w_m, out_h_m);
}

/*!
 * Tier-2 window placement (#59): reposition + resize this client's NSWindow to a
 * display sub-rect. See the header for the contract. Lives here (not
 * comp_multi_workspace.c) because it needs session_render + the macOS comp_target.
 */
bool
comp_multi_workspace_set_client_window_pose(struct xrt_compositor *xc,
                                            const struct xrt_pose *pose,
                                            float width_m,
                                            float height_m)
{
#ifdef XRT_OS_MACOS
	struct multi_compositor *mc = multi_compositor(xc);
	if (mc == NULL || pose == NULL) {
		return false;
	}
	// In the shared-surface model (#59) there is no per-client NSWindow; the stored
	// px-rect drives WHERE this client composites into the combined atlas. The
	// shared surface is the only macOS path now, so there is never a per-client
	// target — store the pose below without any NSWindow reposition.

	// Clamp to a sane minimum so a degenerate pose can't make a zero-size window.
	if (width_m < 0.02f) {
		width_m = 0.02f;
	}
	if (height_m < 0.02f) {
		height_m = 0.02f;
	}

	// Display geometry from the system compositor info (same fallback the D3D11
	// service's slot_pose_to_pixel_rect uses).
	const struct xrt_system_compositor_info *info = &mc->msc->base.info;
	float disp_w_m = info->display_width_m;
	float disp_h_m = info->display_height_m;
	int32_t disp_px_w = (int32_t)info->display_pixel_width;
	int32_t disp_px_h = (int32_t)info->display_pixel_height;
	if (disp_w_m <= 0.0f || disp_h_m <= 0.0f || disp_px_w <= 0 || disp_px_h <= 0) {
		disp_px_w = 3024;
		disp_px_h = 1964;
		disp_w_m = 0.301f;
		disp_h_m = 0.196f;
	}

	float px_per_m_x = (float)disp_px_w / disp_w_m;
	float px_per_m_y = (float)disp_px_h / disp_h_m;

	float w_px_f = width_m * px_per_m_x;
	float h_px_f = height_m * px_per_m_y;

	// Honor the controller's requested WIDTH and HEIGHT verbatim. Window sizing and
	// aspect policy are the workspace controller's look-and-feel job, not the
	// runtime's (which only provides the infra). The shell sizes windows to fill
	// most of the display (a 1×2 grid cell is ~0.81×display-height tall); the old
	// code instead forced height = width / display_aspect, which HALVED that and
	// made vertical resize a no-op (the requested height was discarded). Width is
	// still taken as-requested, so the chrome texture (sized to the window width)
	// is never squished — only the bogus height override is gone.
	int32_t w_px = (int32_t)(w_px_f + 0.5f);
	int32_t h_px = (int32_t)(h_px_f + 0.5f);
	// Keep the window centered on the controller's requested center (top-left-origin
	// display pixels; matches slot_pose_to_pixel_rect, +y up).
	float center_px_x = (float)disp_px_w * 0.5f + pose->position.x * px_per_m_x;
	float center_px_y = (float)disp_px_h * 0.5f - pose->position.y * px_per_m_y;
	int32_t x_px = (int32_t)(center_px_x - (float)w_px * 0.5f + 0.5f);
	int32_t y_px = (int32_t)(center_px_y - (float)h_px * 0.5f + 0.5f);

	// Store the CLAMPED dims (meters derived back from the clamped px) so the
	// controller's chrome sizing + hit-test match the actual window.
	float clamped_w_m = (float)w_px / px_per_m_x;
	float clamped_h_m = (float)h_px / px_per_m_y;
	comp_multi_workspace_store_window_pose(xc, pose, clamped_w_m, clamped_h_m, x_px, y_px, w_px, h_px);

	// In the shared-surface model the stored px-rect alone drives where this client
	// composites into the combined atlas — there is no per-client NSWindow to
	// reposition/resize, so no deferred-resize bookkeeping is needed here.
	return true;
#else
	(void)xc;
	(void)pose;
	(void)width_m;
	(void)height_m;
	return false;
#endif
}

/*!
 * Ask a managed content client to exit (#59). Mirrors the macOS window-close path
 * in multi_compositor_wait_frame: push XRT_SESSION_EVENT_EXIT_REQUEST to the
 * client's per-session compositor so its xrPollEvent transitions to EXITING and
 * the app leaves its loop cleanly.
 */
bool
comp_multi_workspace_request_client_exit(struct xrt_compositor *target_xc)
{
	struct multi_compositor *mc = multi_compositor(target_xc);
	if (mc == NULL) {
		return false;
	}
	union xrt_session_event xse = XRT_STRUCT_INIT;
	xse.type = XRT_SESSION_EVENT_EXIT_REQUEST;
	xrt_result_t r = multi_compositor_push_event(mc, &xse);
	U_LOG_W("[#59] request_client_exit → push EXIT_REQUEST (ret=%d)", (int)r);
	return r == XRT_SUCCESS;
}

#ifndef XRT_OS_MACOS
/*!
 * Composite the workspace controller's chrome (e.g. a title pill) over this
 * client's woven content, post-weave, as a flat alpha-blended 2D overlay (#48).
 * The chrome swapchain is registered by the controller through the workspace IPC
 * handlers and looked up by this client's compositor pointer (== ics->xc). v1 is
 * axis-aligned and zero-disparity (flat at the screen plane); per-eye /
 * perspective chrome is the deferred Tier-2 work the D3D11 monolith does.
 */
static void
session_render_chrome_overlay(struct multi_compositor *mc,
                              struct vk_bundle *vk,
                              VkCommandBuffer cmd,
                              VkImage swapchain_image,
                              VkImageView swapchain_view,
                              uint32_t fb_width,
                              uint32_t fb_height)
{
	struct xrt_swapchain *chrome_xsc = NULL;
	struct comp_multi_chrome_layout layout;
	if (!comp_multi_workspace_chrome_get(&mc->base.base, &chrome_xsc, &layout)) {
		return; // No chrome for this client — the common case.
	}

	struct comp_swapchain *sc = comp_swapchain(chrome_xsc);
	if (sc == NULL || sc->vkic.image_count == 0) {
		return;
	}
	// Single-image chrome contract (matches the D3D11 service, samples images[0]).
	VkImage chrome_image = sc->vkic.images[0].handle;
	uint32_t chrome_w = sc->vkic.info.width;
	uint32_t chrome_h = sc->vkic.info.height;
	if (chrome_image == VK_NULL_HANDLE || chrome_w == 0 || chrome_h == 0) {
		return;
	}

	if (!ensure_workspace_blend(mc, vk)) {
		return;
	}

	// Window-relative dims (#59): chrome is sized/positioned in window-local meters,
	// so use THIS window's physical size (derived from its framebuffer px), not the
	// full display. For a full-display Tier-1 window these are identical.
	float disp_w_m = 0.0f, disp_h_m = 0.0f;
	if (!session_window_dims_m(mc, fb_width, fb_height, &disp_w_m, &disp_h_m)) {
		return;
	}
	float px_per_m_x = (float)fb_width / disp_w_m;
	float px_per_m_y = (float)fb_height / disp_h_m;

	// Chrome extent in meters (fraction-of-window width if the controller asked).
	float chrome_w_m =
	    (layout.width_as_fraction_of_window > 0.0f) ? disp_w_m * layout.width_as_fraction_of_window : layout.size_w_m;
	float chrome_h_m = layout.size_h_m;
	if (chrome_w_m <= 0.0f || chrome_h_m <= 0.0f) {
		return;
	}

	// Center in window-local meters. Top-edge anchor: pose.y is an offset from
	// the window's top edge (matches the D3D11 service convention).
	float cx_m = layout.pose_in_client.position.x;
	float cy_m = layout.anchor_to_window_top_edge ? (disp_h_m * 0.5f + layout.pose_in_client.position.y)
	                                               : layout.pose_in_client.position.y;

	// Window-local meters → target pixels (origin at center; +y up → -y down).
	float w_px = chrome_w_m * px_per_m_x;
	float h_px = chrome_h_m * px_per_m_y;
	int32_t dst_x = (int32_t)((float)fb_width * 0.5f + cx_m * px_per_m_x - w_px * 0.5f);
	int32_t dst_y = (int32_t)((float)fb_height * 0.5f - cy_m * px_per_m_y - h_px * 0.5f);

	workspace_blend_layer(mc, vk, cmd, swapchain_image, swapchain_view, fb_width, fb_height, chrome_image,
	                      chrome_w, chrome_h, dst_x, dst_y, (uint32_t)(w_px + 0.5f), (uint32_t)(h_px + 0.5f));
}

/*!
 * Composite the session-global controller overlays (taskbar / toast / launcher)
 * at z = 0 — zero disparity, docked at a normalized display anchor (#48). z=0 is
 * the overlay's design depth, so this is fully correct (not a v1 approximation).
 * stereo_sbs overlays draw their full source in v1 (no per-eye half-sampling).
 */
static void
session_render_workspace_overlays(struct multi_compositor *mc,
                                  struct vk_bundle *vk,
                                  VkCommandBuffer cmd,
                                  VkImage swapchain_image,
                                  VkImageView swapchain_view,
                                  uint32_t fb_width,
                                  uint32_t fb_height)
{
	struct comp_multi_overlay_state states[COMP_MULTI_WORKSPACE_MAX_OVERLAYS];
	struct xrt_swapchain *xscs[COMP_MULTI_WORKSPACE_MAX_OVERLAYS];
	uint32_t n = comp_multi_workspace_copy_overlays(states, xscs, COMP_MULTI_WORKSPACE_MAX_OVERLAYS);
	if (n == 0) {
		return;
	}

	// Window-relative dims (#59) so overlay sizes use the true display density even
	// when this window is a tiled sub-rect; docked at the window's normalized anchor.
	float disp_w_m = 0.0f, disp_h_m = 0.0f;
	if (!session_window_dims_m(mc, fb_width, fb_height, &disp_w_m, &disp_h_m) || !ensure_workspace_blend(mc, vk)) {
		return;
	}
	float px_per_m_x = (float)fb_width / disp_w_m;
	float px_per_m_y = (float)fb_height / disp_h_m;

	for (uint32_t i = 0; i < n; i++) {
		struct comp_swapchain *sc = comp_swapchain(xscs[i]);
		if (sc == NULL || sc->vkic.image_count == 0) {
			continue;
		}
		VkImage img = sc->vkic.images[0].handle;
		uint32_t sw = sc->vkic.info.width, sh = sc->vkic.info.height;
		if (img == VK_NULL_HANDLE || sw == 0 || sh == 0) {
			continue;
		}
		float ow_px = states[i].size_w_m * px_per_m_x;
		float oh_px = states[i].size_h_m * px_per_m_y;
		if (ow_px < 1.0f || oh_px < 1.0f) {
			continue;
		}
		// Dock: the overlay's pivot UV lands on the normalized display anchor
		// (anchor/pivot are top-left-origin normalized, matching pixel space).
		int32_t dst_x = (int32_t)(states[i].anchor_x * (float)fb_width - states[i].pivot_x * ow_px);
		int32_t dst_y = (int32_t)(states[i].anchor_y * (float)fb_height - states[i].pivot_y * oh_px);
		workspace_blend_layer(mc, vk, cmd, swapchain_image, swapchain_view, fb_width, fb_height, img, sw, sh,
		                      dst_x, dst_y, (uint32_t)(ow_px + 0.5f), (uint32_t)(oh_px + 0.5f));
	}
}

/*!
 * Composite the session-global cursor sprite, topmost, at the tracked pointer
 * position (#48). v1 is flat at the screen plane (zero disparity); the hit-depth
 * disparity + over-window dim are the deferred Tier-2 work (the depth state is
 * stored but not yet applied). The AppKit pump feeds the pointer position in
 * target pixels via comp_multi_workspace_set_pointer_px.
 */
static void
session_render_workspace_cursor(struct multi_compositor *mc,
                                struct vk_bundle *vk,
                                VkCommandBuffer cmd,
                                VkImage swapchain_image,
                                VkImageView swapchain_view,
                                uint32_t fb_width,
                                uint32_t fb_height)
{
	struct xrt_swapchain *cur_xsc = NULL;
	struct comp_multi_cursor_state cur;
	if (!comp_multi_workspace_get_cursor(&cur_xsc, &cur)) {
		return;
	}

	struct comp_swapchain *sc = comp_swapchain(cur_xsc);
	if (sc == NULL || sc->vkic.image_count == 0) {
		return;
	}
	VkImage img = sc->vkic.images[0].handle;
	uint32_t sw = sc->vkic.info.width, sh = sc->vkic.info.height;
	if (img == VK_NULL_HANDLE || sw == 0 || sh == 0) {
		return;
	}

	float disp_w_m = 0.0f, disp_h_m = 0.0f;
	if (!session_window_dims_m(mc, fb_width, fb_height, &disp_w_m, &disp_h_m) || !ensure_workspace_blend(mc, vk)) {
		return;
	}
	float px_per_m_x = (float)fb_width / disp_w_m;
	float size_px = cur.size_meters * px_per_m_x; // square sprite
	if (size_px < 1.0f) {
		return;
	}

	int32_t px = 0, py = 0;
	comp_multi_workspace_get_pointer_px(&px, &py);
	// The pointer is in GLOBAL display px; this window's surface is a sub-rect at
	// (win_x,win_y) px (#59). Convert to window-local px. No stored rect (Tier-1
	// full-display) → offset 0, identical to before.
	int32_t win_x = 0, win_y = 0;
	(void)comp_multi_workspace_load_window_px_rect(&mc->base.base, &win_x, &win_y, NULL, NULL);
	px -= win_x;
	py -= win_y;
	// CLIP: only the window the pointer is actually over draws the sprite. Without
	// this every tiled window blits a sprite at its own offset (the blend clamps to
	// the framebuffer), so each window shows a stray "ghost" cursor — and with the
	// OS cursor hidden the user ends up aiming at a ghost in the wrong window (#59).
	if (px < 0 || py < 0 || px >= (int32_t)fb_width || py >= (int32_t)fb_height) {
		return;
	}
	// Hotspot: the sprite's hot point sits at the pointer.
	int32_t dst_x = px - (int32_t)(cur.hot_x * size_px);
	int32_t dst_y = py - (int32_t)(cur.hot_y * size_px);
	workspace_blend_layer(mc, vk, cmd, swapchain_image, swapchain_view, fb_width, fb_height, img, sw, sh, dst_x,
	                      dst_y, (uint32_t)(size_px + 0.5f), (uint32_t)(size_px + 0.5f));
}

/*!
 * Render a single per-session client to its own comp_target using display processing.
 *
 * @param mc The multi_compositor with per-session rendering
 * @param vk The Vulkan bundle
 * @param display_time_ns The display timestamp
 * @param force_black When true the client is workspace-minimized (#61): skip all
 *        content/view/atlas work and present a black desktop canvas, keeping only
 *        the session-global overlays/cursor (taskbar) at the submit_and_present pass.
 */
static void
render_session_to_own_target(struct multi_compositor *mc,
                             struct vk_bundle *vk,
                             int64_t display_time_ns,
                             bool force_black)
{
	struct comp_target *ct = mc->session_render.target;

	if (ct == NULL) {
		U_LOG_E("[per-session] Per-session target not initialized");
		return;
	}

	// Recreate swapchain if flagged (from previous frame's VK_SUBOPTIMAL_KHR)
#ifdef XRT_OS_WINDOWS
	if (mc->session_render.swapchain_needs_recreate &&
	    mc->session_render.owns_window && mc->session_render.own_window != NULL &&
	    comp_d3d11_window_is_in_size_move(mc->session_render.own_window)) {
		// Defer recreation until drag ends — avoids stutter from texture reallocation
	} else
#endif
	if (mc->session_render.swapchain_needs_recreate) {
		// Never recreate against a torn-down target (Android surface loss,
		// #528): create_images on a VK_NULL_HANDLE surface is invalid usage.
		// The acquire-time surface sync rebuilds the target when a new
		// surface arrives.
		if (!comp_target_check_ready(ct)) {
			mc->session_render.swapchain_needs_recreate = false;
			return;
		}
		recreate_session_swapchain(mc, vk);
		// Re-read ct since create_images updates it in place
		ct = mc->session_render.target;
	}

	// Workspace-minimized client (#61): skip all content/view/atlas work and jump
	// straight to acquiring + clearing the target to black. The overlays/cursor are
	// composited at submit_and_present so the taskbar remains visible.
	if (force_black) {
		goto black_canvas;
	}

	// Must have at least one layer
	if (mc->delivered.layer_count == 0) {
		U_LOG_W("[per-session] No layers delivered, skipping");
		return;
	}

	// Get the first projection (or zone-3D) layer. A zone-3D layer
	// (XR_EXT_display_zones, #568) carries an embedded projection plus a
	// placement rect; it renders exactly like a projection here, then the
	// rect is handed to the DP as the canvas sub-rect (see process_atlas below).
	struct multi_layer_entry *layer = NULL;
	for (uint32_t i = 0; i < mc->delivered.layer_count; i++) {
		enum xrt_layer_type type = mc->delivered.layers[i].data.type;
		if (type == XRT_LAYER_PROJECTION || type == XRT_LAYER_PROJECTION_DEPTH ||
		    type == XRT_LAYER_ZONE_3D) {
			layer = &mc->delivered.layers[i];
			break;
		}
	}

	// One-shot WARN (per layer-type change) naming what the per-session path
	// selected and, for a zone (XR_EXT_display_zones / #568), its placement
	// rect — a lifecycle breadcrumb at the same level as the atlas/DP WARNs
	// below, so zone-on-OOP framing is greppable without the INFO hot path.
	{
		static enum xrt_layer_type last_logged = (enum xrt_layer_type)-1;
		if (layer != NULL && layer->data.type != last_logged) {
			last_logged = layer->data.type;
			if (layer->data.type == XRT_LAYER_ZONE_3D) {
				const struct xrt_rect *zr = &layer->data.zone_3d.rect;
				U_LOG_W("[per-session] #568 render layer = ZONE_3D id=%u rect=%d,%d %dx%d",
				        layer->data.zone_3d.zone_id, zr->offset.w, zr->offset.h,
				        zr->extent.w, zr->extent.h);
			} else {
				U_LOG_W("[per-session] #568 render layer = type %d (not a zone)",
				        (int)layer->data.type);
			}
		}
	}

	if (layer == NULL) {
		U_LOG_W("[per-session] No projection layer found, skipping");
		return;
	}

	// HARDWARE 2D/3D (the weave-state) is set by multi_compositor_request_display_mode
	// — the app's xrRequestDisplayRenderingModeEXT, which now crosses IPC out-of-process
	// (#533). It is DECOUPLED from the per-frame CONTENT (how many tiles the app
	// submitted): content drives the atlas, mc->hardware_display_3d drives the DP weave.
	// In-process the device's active rendering mode is the same authority, so sync from
	// it when the head device is available (no-op out-of-process: head is NULL there, so
	// the request-set value stands).
	struct xrt_device *xdev_head = (mc->xsysd != NULL) ? mc->xsysd->static_roles.head : NULL;
	if (xdev_head != NULL && xdev_head->hmd != NULL) {
		uint32_t idx = xdev_head->hmd->active_rendering_mode_index;
		if (idx < xdev_head->rendering_mode_count) {
			mc->hardware_display_3d = xdev_head->rendering_modes[idx].hardware_display_3d;
		}
	}

	// Runtime-side 2D/3D toggle from qwerty V key
#ifdef XRT_BUILD_DRIVER_QWERTY
	if (mc->xsysd != NULL) {
		bool force_2d = false;
		bool toggled =
		    qwerty_check_display_mode_toggle(mc->xsysd->xdevs, mc->xsysd->xdev_count, &force_2d);
		if (toggled) {
			struct xrt_device *head = mc->xsysd->static_roles.head;
			if (head != NULL && head->hmd != NULL) {
				if (force_2d) {
					// Save current 3D mode index before switching to 2D
					mc->last_3d_mode_index = head->hmd->active_rendering_mode_index;
					// Find first 2D mode
					for (uint32_t i = 0; i < head->rendering_mode_count; i++) {
						if (!head->rendering_modes[i].hardware_display_3d) {
							head->hmd->active_rendering_mode_index = i;
							break;
						}
					}
				} else {
					// Restore last known 3D mode index
					head->hmd->active_rendering_mode_index = mc->last_3d_mode_index;
				}
				mc->hardware_display_3d = !force_2d;
			}
			multi_compositor_request_display_mode(mc, !force_2d);
		}

		// Rendering mode change from qwerty 1/2/3 keys (disabled for legacy apps).
		if (!mc->msc->base.info.legacy_app_tile_scaling) {
			int render_mode = -1;
			if (qwerty_check_rendering_mode_change(mc->xsysd->xdevs, mc->xsysd->xdev_count, &render_mode)) {
				struct xrt_device *head = mc->xsysd->static_roles.head;
				if (head != NULL) {
					xrt_device_set_property(head, XRT_DEVICE_PROPERTY_OUTPUT_MODE, render_mode);
				}
			}
		}
	}
#endif

	// CONTENT: the atlas recipe is the active MODE's (ADR-028, #553) — the
	// submission clamps DOWN to it, mirroring the in-process compositors
	// (comp_d3d11_renderer_compute_effective_layout). In-process the head
	// device is the mode authority, so sync the grid from it here (after the
	// qwerty block, to pick up same-frame V/1-2-3 changes); out-of-process
	// head is NULL and the mode arrives via compositor_request_rendering_mode
	// over IPC. Orthogonal to mc->hardware_display_3d (the DP weave-state).
	if (xdev_head != NULL && xdev_head->hmd != NULL) {
		uint32_t idx = xdev_head->hmd->active_rendering_mode_index;
		if (idx < xdev_head->rendering_mode_count) {
			multi_compositor_set_rendering_mode(mc, idx,
			                                    xdev_head->rendering_modes[idx].tile_columns,
			                                    xdev_head->rendering_modes[idx].tile_rows);
		}
	}

	uint32_t submitted_views = layer->data.view_count;
	if (submitted_views < 1)
		submitted_views = 1;
	if (submitted_views > XRT_MAX_VIEWS)
		submitted_views = XRT_MAX_VIEWS;

	// Fallback (no mode has arrived yet): the legacy submission-derived strip.
	uint32_t mode_cols = mc->active_mode_valid ? mc->mode_tile_columns : submitted_views;
	uint32_t mode_rows = mc->active_mode_valid ? mc->mode_tile_rows : 1;
	uint32_t mode_tiles = mode_cols * mode_rows;

	// Clamp: always-stereo apps submit the xrLocateViews max view count even
	// in mono modes — without the clamp that builds an oversized atlas (the
	// recurring left-shift bug class ADR-028 documents). The min() also kills
	// the old 2D⇄3D transition hazard: during the one-frame skew between the
	// mode flip and the content adapting, the atlas never exceeds what was
	// actually submitted.
	uint32_t view_count = submitted_views < mode_tiles ? submitted_views : mode_tiles;

	// Mono → one tile spanning the full content region; otherwise the MODE
	// grid (an under-submitting app paints the first view_count tiles).
	uint32_t tile_columns = (view_count == 1) ? 1 : mode_cols;
	uint32_t tile_rows = (view_count == 1) ? 1 : mode_rows;

	int imageWidth = 0, imageHeight = 0;
	VkFormat imageFormat = VK_FORMAT_UNDEFINED;
	VkImageView viewImageViews[XRT_MAX_VIEWS];
	VkImage viewImages[XRT_MAX_VIEWS];
	uint32_t viewArrayIndices[XRT_MAX_VIEWS];
	int viewOffsetX[XRT_MAX_VIEWS], viewOffsetY[XRT_MAX_VIEWS];

	bool views_ok = true;
	for (uint32_t v = 0; v < view_count; v++) {
		viewImageViews[v] = VK_NULL_HANDLE;
		viewImages[v] = VK_NULL_HANDLE;
		viewArrayIndices[v] = 0;
		if (!get_session_layer_view(layer, v, &imageWidth, &imageHeight, &imageFormat,
		                            &viewImageViews[v], &viewImages[v], &viewArrayIndices[v])) {
			views_ok = false;
		}
		viewOffsetX[v] = layer->data.proj.v[v].sub.rect.offset.w;
		viewOffsetY[v] = layer->data.proj.v[v].sub.rect.offset.h;
	}

	if (!views_ok) {
		U_LOG_W("[per-session] Could not extract views for per-session rendering (3d=%d, count=%u)",
		        mc->hardware_display_3d, view_count);
		return;
	}

	// Wait for pending fence if exists (from previous frame using same buffer)
	if (mc->session_render.fenced_buffer >= 0) {
		VkResult fence_ret = vk->vkWaitForFences(vk->device, 1,
		                                         &mc->session_render.fences[mc->session_render.fenced_buffer],
		                                         VK_TRUE, UINT64_MAX);
		if (fence_ret != VK_SUCCESS) {
			U_LOG_E("[per-session] Failed to wait for fence: %s", vk_result_string(fence_ret));
		}
		mc->session_render.fenced_buffer = -1;
	}

#ifdef XRT_OS_WINDOWS
	// During drag of self-owned window, synchronize with WM_PAINT cycle.
	if (mc->session_render.owns_window && mc->session_render.own_window != NULL &&
	    comp_d3d11_window_is_in_size_move(mc->session_render.own_window)) {
		comp_d3d11_window_wait_for_paint(mc->session_render.own_window);
	}
#endif

black_canvas:; // force_black (minimized) jumps here, skipping all content/view setup

	// Establish frame pacing so the per-session comp_target_swapchain has a
	// valid current_frame_id before acquire/present. render_session_to_own_target
	// is the only consumer of this target, so it must drive pacing itself —
	// without this, comp_target_swapchain_present asserts current_frame_id > 0.
	// (#510: first path to actually exercise comp_window_android present.)
	{
#ifdef XRT_OS_ANDROID
		// Feed the target's pacer the AChoreographer vsync timing before
		// predicting (closed-loop pacing, #510). In-process compositors call
		// update_timings every frame; this OOP per-session path omitted it, so
		// the real vblank never reached the fake pacer and its present phase
		// drifted → 30 Hz lock. Android-only: it exists solely to drive the
		// Choreographer vblank feed; Windows/macOS self-owned-window targets
		// have no such source and never called this here before.
		comp_target_update_timings(ct);
#endif

		int64_t frame_id = -1;
		int64_t wake_up_ns = 0, desired_present_ns = 0, present_slop_ns = 0, predicted_display_ns = 0;
		comp_target_calc_frame_pacing(ct, &frame_id, &wake_up_ns, &desired_present_ns, &present_slop_ns,
		                              &predicted_display_ns);
		int64_t now_ns = os_monotonic_get_ns();
		comp_target_mark_wake_up(ct, frame_id, now_ns);
		comp_target_mark_begin(ct, frame_id, now_ns);
	}

	// Acquire the next swapchain image from the per-session target
	uint32_t buffer_index = 0;
	VkResult ret = comp_target_acquire(ct, &buffer_index);
	if (ret == VK_ERROR_OUT_OF_DATE_KHR) {
		U_LOG_W("[per-session] Swapchain out of date, recreating now");
		recreate_session_swapchain(mc, vk);
		ct = mc->session_render.target;
		mc->session_render.swapchain_needs_recreate = false;

		// Retry acquire after recreation.
		// Accept VK_SUBOPTIMAL_KHR — the image IS acquired, it's just not
		// optimal (e.g. overlay window still resizing asynchronously).
		ret = comp_target_acquire(ct, &buffer_index);
		if (ret == VK_ERROR_SURFACE_LOST_KHR || ret == VK_ERROR_OUT_OF_DATE_KHR) {
			// Surface gone / still settling (Android background, #528):
			// skip this frame quietly, the target logs the transition.
			return;
		}
		if (ret != VK_SUCCESS && ret != VK_SUBOPTIMAL_KHR) {
			U_LOG_E("[per-session] Failed to acquire after swapchain recreation: %s",
			        vk_result_string(ret));
			return;
		}
	} else if (ret == VK_ERROR_SURFACE_LOST_KHR) {
		// No live output surface (Android client backgrounded, #528): skip
		// quietly. The delivered frame is still retired by the caller, so
		// client frame pacing keeps flowing and xrWaitFrame never stalls.
		return;
	} else if (ret != VK_SUCCESS && ret != VK_SUBOPTIMAL_KHR) {
		U_LOG_E("[per-session] Failed to acquire per-session target image: %s", vk_result_string(ret));
		return;
	}

	// Validate buffer_index is in range
	if (buffer_index >= mc->session_render.buffer_count) {
		U_LOG_E("[per-session] buffer_index %u out of range (max %u)", buffer_index, mc->session_render.buffer_count);
		return;
	}

	// Reset fence for current buffer
	ret = vk->vkResetFences(vk->device, 1, &mc->session_render.fences[buffer_index]);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[per-session] Failed to reset fence: %s", vk_result_string(ret));
		return;
	}

	// Get target framebuffer info
	uint32_t framebufferWidth = ct->width;
	uint32_t framebufferHeight = ct->height;
	VkFormat framebufferFormat = ct->format;

	// Set up viewport (fullscreen)
	VkRect2D viewport = {0};
	viewport.offset.x = 0;
	viewport.offset.y = 0;
	viewport.extent.width = framebufferWidth;
	viewport.extent.height = framebufferHeight;

	// Use pre-allocated command buffer for this swapchain image
	VkCommandBuffer cmd = mc->session_render.cmd_buffers[buffer_index];
	ret = vk->vkResetCommandBuffer(cmd, 0);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[per-session] Failed to reset command buffer: %s", vk_result_string(ret));
		return;
	}

	// Begin command buffer
	VkCommandBufferBeginInfo begin_info = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};

	ret = vk->vkBeginCommandBuffer(cmd, &begin_info);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[per-session] Failed to begin command buffer: %s", vk_result_string(ret));
		return;
	}

	// Workspace-minimized desktop canvas (#61): clear the swapchain image to black,
	// leave it in PRESENT_SRC_KHR (the layout submit_and_present's overlay composite
	// and present expect), then composite only the session-global overlays/cursor.
	if (force_black) {
		VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		VkImageMemoryBarrier to_dst = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = 0,
		    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		    .image = ct->images[buffer_index].handle,
		    .subresourceRange = range,
		};
		vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		                         0, 0, NULL, 0, NULL, 1, &to_dst);

		VkClearColorValue black = {.float32 = {0.0f, 0.0f, 0.0f, 1.0f}};
		vk->vkCmdClearColorImage(cmd, ct->images[buffer_index].handle,
		                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);

		VkImageMemoryBarrier to_present = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		    .dstAccessMask = 0,
		    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		    .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		    .image = ct->images[buffer_index].handle,
		    .subresourceRange = range,
		};
		vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
		                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1,
		                         &to_present);
		goto submit_and_present;
	}

	// #533: 2D is no longer a special-cased mono blit. Both 2D and 3D flow through
	// the unified atlas → display-processor path below. The app submits the active
	// mode's tiles (1 for 2D, 2 for 3D) with per-view imageRects; the runtime packs
	// them into a matching atlas (view_count×1) and hands it to the DP, which weaves
	// (hardware 3D) or shows the single tile flat (hardware 2D, mode_3d=false). The
	// window-space HUD is composited in the atlas path too (composite_layers_to_-
	// intermediate below), so it survives the unification. Content (tile count) and
	// hardware (mode_3d) are now fully decoupled — the old mono-blit branch is gone.

	// Unified display-processor path

	// Get the framebuffer for the current swapchain image
	VkFramebuffer framebuffer = VK_NULL_HANDLE;
	if (mc->session_render.framebuffers != NULL) {
		framebuffer = mc->session_render.framebuffers[buffer_index];
	}

	// Display processor path: blit source sub-regions into a tiled atlas,
	// then pass to the display processor for final output.
	// All display processors now accept atlas input with explicit tile params.
	if (mc->session_render.display_processor != NULL) {
		static bool dp_logged = false;
		if (!dp_logged) {
			U_LOG_W("[per-session] Vulkan display processor: input=%dx%d fmt=%d, fb=%ux%u fmt=%d",
			        imageWidth, imageHeight, imageFormat,
			        framebufferWidth, framebufferHeight, framebufferFormat);
			dp_logged = true;
		}

		// ================================================================
		// ATLAS INPUT PATH: blit eyes into tiled atlas image, then weave
		// with explicit pre/post layout barriers on the target image.
		// Tile layout comes from the active rendering mode (default 2x1
		// for stereo).
		// ================================================================
		{
			// The atlas grid (tile_columns × tile_rows) is the MODE's, clamped
			// to the submission — computed above (ADR-028, #553).

			// Determine blit sources — either composited overlay images or
			// direct swapchain sub-regions.
			VkImage blit_sources[XRT_MAX_VIEWS];
			uint32_t blit_arrays[XRT_MAX_VIEWS];
			int blit_off_x[XRT_MAX_VIEWS], blit_off_y[XRT_MAX_VIEWS];
			int blit_eye_w = imageWidth, blit_eye_h = imageHeight;
			bool blit_flip_y = layer->data.flip_y;
			VkImageLayout blit_src_old_layout = VK_IMAGE_LAYOUT_GENERAL;

			for (uint32_t v = 0; v < view_count; v++) {
				blit_sources[v] = viewImages[v];
				blit_arrays[v] = viewArrayIndices[v];
				blit_off_x[v] = viewOffsetX[v];
				blit_off_y[v] = viewOffsetY[v];
			}

			if (has_window_space_layers(mc)) {
				VkImageView comp_left = VK_NULL_HANDLE, comp_right = VK_NULL_HANDLE;
				if (composite_layers_to_intermediate(mc, vk, cmd, &comp_left, &comp_right)) {
					// Composite path currently supports 2 views
					uint32_t comp_count = view_count < 2 ? view_count : 2;
					for (uint32_t v = 0; v < comp_count; v++) {
						blit_sources[v] = mc->session_render.composite_images[v];
						blit_arrays[v] = 0;
						blit_off_x[v] = 0;
						blit_off_y[v] = 0;
					}
					blit_eye_w = (int)mc->session_render.composite_width;
					blit_eye_h = (int)mc->session_render.composite_height;
					blit_flip_y = false;
					blit_src_old_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				}
			}

			// Ensure atlas intermediate image exists (tile_columns*eye_w x tile_rows*eye_h)
			if (!ensure_session_atlas_image(mc, vk, blit_eye_w, blit_eye_h,
			                              tile_columns, tile_rows, imageFormat)) {
				U_LOG_E("[per-session] Failed to ensure atlas image");
				goto submit_and_present;
			}

			// Collect unique source images for barriers
			VkImage unique_sources[XRT_MAX_VIEWS];
			uint32_t unique_arrays[XRT_MAX_VIEWS];
			uint32_t unique_count = 0;
			for (uint32_t v = 0; v < view_count; v++) {
				bool found = false;
				for (uint32_t u = 0; u < unique_count; u++) {
					if (unique_sources[u] == blit_sources[v] &&
					    unique_arrays[u] == blit_arrays[v]) {
						found = true;
						break;
					}
				}
				if (!found) {
					unique_sources[unique_count] = blit_sources[v];
					unique_arrays[unique_count] = blit_arrays[v];
					unique_count++;
				}
			}

			// Pre-barriers: unique sources → TRANSFER_SRC, atlas → TRANSFER_DST
			VkImageMemoryBarrier pre_barriers[XRT_MAX_VIEWS + 1];
			uint32_t pre_count = 0;
			for (uint32_t u = 0; u < unique_count; u++) {
				pre_barriers[pre_count++] = (VkImageMemoryBarrier){
				    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				    .srcAccessMask = 0,
				    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
				    .oldLayout = blit_src_old_layout,
				    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				    .image = unique_sources[u],
				    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, unique_arrays[u], 1},
				};
			}
			pre_barriers[pre_count++] = (VkImageMemoryBarrier){
			    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			    .srcAccessMask = 0,
			    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			    .image = mc->session_render.flip_sbs_image,
			    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
			};
			vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
			                         pre_count, pre_barriers);

			// Clear the atlas when the submission doesn't cover every tile (#48):
			// on a mode switch the grid can grow (e.g. → Quad 2x2) a frame before
			// the app catches up and submits all N views, leaving the surplus tiles
			// UNDEFINED → a one-frame magenta flash. Clear to opaque black so the
			// uncovered tiles are benign until the app fills them. Skipped in the
			// common fully-covered case to avoid a per-frame clear.
			if (view_count < tile_columns * tile_rows) {
				VkClearColorValue clear = {.float32 = {0.0f, 0.0f, 0.0f, 1.0f}};
				VkImageSubresourceRange clear_range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
				vk->vkCmdClearColorImage(cmd, mc->session_render.flip_sbs_image,
				                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &clear_range);
				// Order the clear before the per-tile blits (both TRANSFER writes to
				// the same image, overlapping regions).
				VkImageMemoryBarrier clear_barrier = {
				    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
				    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
				    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				    .image = mc->session_render.flip_sbs_image,
				    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
				};
				vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
				                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
				                         &clear_barrier);
			}

			// Blit each view into its tile position in the atlas
			for (uint32_t v = 0; v < view_count; v++) {
				uint32_t tile_x, tile_y;
				u_tiling_view_origin(v, tile_columns, (uint32_t)blit_eye_w, (uint32_t)blit_eye_h,
				                     &tile_x, &tile_y);
				int src_top = blit_off_y[v] + (blit_flip_y ? blit_eye_h : 0);
				int src_bot = blit_off_y[v] + (blit_flip_y ? 0 : blit_eye_h);
				VkImageBlit blit = {
				    .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, blit_arrays[v], 1},
				    .srcOffsets = {{blit_off_x[v], src_top, 0},
				                   {blit_off_x[v] + blit_eye_w, src_bot, 1}},
				    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
				    .dstOffsets = {{(int32_t)tile_x, (int32_t)tile_y, 0},
				                   {(int32_t)(tile_x + blit_eye_w), (int32_t)(tile_y + blit_eye_h), 1}},
				};
				vk->vkCmdBlitImage(cmd, blit_sources[v], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				                   mc->session_render.flip_sbs_image,
				                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				                   1, &blit, VK_FILTER_NEAREST);
			}

			// Post-barriers: unique sources → original layout, atlas → SHADER_READ_ONLY
			VkImageMemoryBarrier post_barriers[XRT_MAX_VIEWS + 1];
			uint32_t post_count = 0;
			for (uint32_t u = 0; u < unique_count; u++) {
				post_barriers[post_count++] = (VkImageMemoryBarrier){
				    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				    .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
				    .dstAccessMask = 0,
				    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				    .newLayout = blit_src_old_layout,
				    .image = unique_sources[u],
				    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, unique_arrays[u], 1},
				};
			}
			post_barriers[post_count++] = (VkImageMemoryBarrier){
			    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
			    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			    .image = mc->session_render.flip_sbs_image,
			    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
			};
			vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
			                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL,
			                         post_count, post_barriers);

			// Pre-weave barrier: target → COLOR_ATTACHMENT_OPTIMAL
			VkImageMemoryBarrier pre_weave = {
			    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			    .srcAccessMask = 0,
			    .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			    .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			    .image = ct->images[buffer_index].handle,
			    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
			};
			vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			                         0, 0, NULL, 0, NULL, 1, &pre_weave);

			// Hand the DP the target swapchain image view BEFORE process_atlas.
			// A self-submitting DP (Leia CNSDK) builds its own destination
			// framebuffer and needs this view; without it get_or_create_weave_fb
			// bails ("no target view from compositor") and the weave is skipped
			// every frame → black. The in-process compositor already does this
			// (comp_vk_native_compositor.c); the OOP per-session path was missing
			// it — which is why in-process Leia 3D works but OOP was black. (#510 M2)
			xrt_display_processor_set_target_color_view(mc->session_render.display_processor,
			                                            ct->images[buffer_index].view);

			// Canvas sub-rect: a zone-3D layer (XR_EXT_display_zones, #568)
			// confines its woven output to a placement rect (e.g. the avatar's
			// bottom-75% tiger zone); the DP weaves into that band and clears
			// outside it transparent. A plain projection layer passes 0,0,0,0
			// → the DP fills the full target (unchanged for every other app).
			int32_t canvas_x = 0, canvas_y = 0;
			uint32_t canvas_w = 0, canvas_h = 0;
			if (layer->data.type == XRT_LAYER_ZONE_3D) {
				const struct xrt_rect *zr = &layer->data.zone_3d.rect;
				canvas_x = zr->offset.w;
				canvas_y = zr->offset.h;
				canvas_w = (uint32_t)zr->extent.w;
				canvas_h = (uint32_t)zr->extent.h;
			}

			// Call display processor with atlas input
			xrt_display_processor_process_atlas(
			    mc->session_render.display_processor, cmd,
			    (VkImage_XDP)mc->session_render.flip_sbs_image, // atlas image (for copy/blit)
			    mc->session_render.flip_sbs_view,  // atlas view (tiled views)
			    (uint32_t)blit_eye_w,              // per-view width
			    (uint32_t)blit_eye_h,              // per-view height
			    tile_columns,                       // tile layout columns
			    tile_rows,                          // tile layout rows
			    (VkFormat_XDP)imageFormat,
			    framebuffer,
			    (VkImage_XDP)ct->images[buffer_index].handle,
			    framebufferWidth, framebufferHeight,
			    (VkFormat_XDP)framebufferFormat,
			    canvas_x, canvas_y, canvas_w, canvas_h);

			// Composite LOCAL_2D layers (e.g. the avatar speech bubble in the
			// top-25% 2D band, #568) into the woven target. Post-weave so they
			// land in the 2D regions the DP left transparent; the target stays
			// in COLOR_ATTACHMENT for the post-weave barrier below.
			composite_local_2d_layers(mc, vk, cmd, ct->images[buffer_index].handle,
			                          framebufferWidth, framebufferHeight);

			// Post-weave barrier: target → PRESENT_SRC_KHR (for HUD overlay + present)
			VkImageMemoryBarrier post_weave = {
			    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			    .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			    .dstAccessMask = 0,
			    .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			    .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
			    .image = ct->images[buffer_index].handle,
			    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
			};
			vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			                         0, 0, NULL, 0, NULL, 1, &post_weave);

			goto submit_and_present;
		}

	}

submit_and_present:
	// HUD overlay (post-weave, always readable).
	// Render for all per-session windows (self-owned and app-provided),
	// matching the D3D11 compositor which renders HUD for all sessions.
	// A minimized client (#61) shows neither the app HUD nor its own title pill
	// over the black desktop canvas — only the session-global overlays/cursor.
	if (!force_black) {
		session_render_hud_overlay(mc, vk, cmd, ct->images[buffer_index].handle,
		                           ct->images[buffer_index].view,
		                           framebufferWidth, framebufferHeight);

		// Workspace chrome (per-client title pill), composited post-weave over the
		// HUD, flat (#48). A no-op when no chrome is registered for this client.
		session_render_chrome_overlay(mc, vk, cmd, ct->images[buffer_index].handle,
		                              ct->images[buffer_index].view,
		                              framebufferWidth, framebufferHeight);
	}

	// Session-global overlays (taskbar/launcher z=0) and the cursor (topmost) —
	// these stay visible even for a minimized client so the taskbar persists.
	session_render_workspace_overlays(mc, vk, cmd, ct->images[buffer_index].handle,
	                                  ct->images[buffer_index].view,
	                                  framebufferWidth, framebufferHeight);
	session_render_workspace_cursor(mc, vk, cmd, ct->images[buffer_index].handle,
	                                ct->images[buffer_index].view,
	                                framebufferWidth, framebufferHeight);

	// End command buffer
	ret = vk->vkEndCommandBuffer(cmd);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[per-session] Failed to end command buffer: %s", vk_result_string(ret));
		return;
	}

	// Submit command buffer with fence for async completion.
	// Wait on present_complete (signaled by vkAcquireNextImageKHR) before writing
	// to the swapchain image, and signal render_complete for comp_target_present.
	VkSemaphore wait_sem = ct->semaphores.present_complete;
	VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkSemaphore signal_sem = ct->semaphores.render_complete;
	VkSubmitInfo submit_info = {
	    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	    .waitSemaphoreCount = (wait_sem != VK_NULL_HANDLE) ? 1 : 0,
	    .pWaitSemaphores = (wait_sem != VK_NULL_HANDLE) ? &wait_sem : NULL,
	    .pWaitDstStageMask = (wait_sem != VK_NULL_HANDLE) ? &wait_stage : NULL,
	    .commandBufferCount = 1,
	    .pCommandBuffers = &cmd,
	    .signalSemaphoreCount = (signal_sem != VK_NULL_HANDLE) ? 1 : 0,
	    .pSignalSemaphores = (signal_sem != VK_NULL_HANDLE) ? &signal_sem : NULL,
	};

	ret = vk->vkQueueSubmit(vk->main_queue->queue, 1, &submit_info, mc->session_render.fences[buffer_index]);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[per-session] Failed to submit per-session render: %s", vk_result_string(ret));
		return;
	}

	// CRITICAL: Wait for GPU work to complete before returning.
	// With cross-device external memory sharing (null compositor + VK app),
	// there is no GPU-level synchronization between Device A (compositor) and
	// Device B (app). Without this wait, the compositor's GPU read of shared
	// images may still be in-flight when the app starts writing to the same
	// images for the next frame, causing VK_ERROR_DEVICE_LOST on Intel.
	// GL apps don't hit this because GL has implicit driver-level sync.
	ret = vk->vkWaitForFences(vk->device, 1, &mc->session_render.fences[buffer_index], VK_TRUE, UINT64_MAX);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[per-session] Failed to wait for render fence: %s", vk_result_string(ret));
	}
	mc->session_render.fenced_buffer = -1; // Fence already waited, no deferred wait needed

	// Present the image (GPU work is complete, semaphore already signaled)
	ret = comp_target_present(ct, vk->main_queue->queue, buffer_index, 0, display_time_ns, 0);
	if (ret == VK_ERROR_OUT_OF_DATE_KHR) {
		U_LOG_W("[per-session] Present returned OUT_OF_DATE, flagging for recreation");
		mc->session_render.swapchain_needs_recreate = true;
	} else if (ret == VK_ERROR_SURFACE_LOST_KHR) {
		// Surface died mid-frame (Android background, #528). Do NOT flag a
		// recreate — the swapchain can't be rebuilt on a dead surface; the
		// next acquire's surface sync tears the target down and pauses
		// presents until the client passes a replacement surface.
		U_LOG_W("[per-session] Present returned SURFACE_LOST, waiting for a new surface");
	} else if (ret == VK_SUBOPTIMAL_KHR) {
		// Presentation succeeded — swapchain size differs from surface but content is shown.
	} else if (ret != VK_SUCCESS) {
		U_LOG_E("[per-session] Failed to present per-session target: %s", vk_result_string(ret));
	}
#ifdef XRT_OS_WINDOWS
	// Signal WM_PAINT handler that frame is done
	if (mc->session_render.owns_window && mc->session_render.own_window != NULL) {
		comp_d3d11_window_signal_paint_done(mc->session_render.own_window);
	}
#endif

}
#endif // !XRT_OS_MACOS

#ifdef XRT_OS_MACOS
/*
 * ============================================================================
 * Shared spatial surface (#59) — the macOS analogue of the Windows D3D11
 * monolith (comp_d3d11_service.cpp::multi_compositor_render).
 *
 * Instead of one NSWindow per client, the service owns ONE full-screen window
 * and composites every client app into ONE combined stereo atlas at its 3D
 * pose, then performs ONE display-processor weave and ONE present. M1: flat
 * (window at panel depth, each client's internal stereo preserved). M2 adds
 * per-eye parallax; M3 adds chrome/cursor/overlays floating above each window.
 * ============================================================================
 */

/*!
 * Lazily create the one shared full-screen target, its render rings, and the
 * one display processor. Mirrors multi_compositor_init_session_render but the
 * resources live on the multi_system_compositor (not a client).
 */
static bool
shared_surface_init(struct multi_system_compositor *msc, struct vk_bundle *vk)
{
	if (msc->shared_surface_initialized) {
		return true;
	}
	if (msc->target_service == NULL) {
		U_LOG_E("[#59] no target service for shared surface");
		return false;
	}

	// macOS: the service creates + owns the full-screen NSWindow; the handle arg
	// is ignored (null_target_service_create_from_window_macos).
	xrt_result_t ret = comp_target_service_create(msc->target_service, NULL, &msc->shared_target);
	if (ret != XRT_SUCCESS || msc->shared_target == NULL) {
		U_LOG_E("[#59] failed to create shared full-screen target: %d", ret);
		return false;
	}
	struct comp_target *ct = msc->shared_target;
	U_LOG_W("[#59] created shared full-screen target %ux%u fmt=%d (%u images)", ct->width, ct->height,
	        ct->format, ct->image_count);

	uint32_t image_count = ct->image_count;

	// Command pool + per-image command buffers + fences (signaled).
	VkResult vr;
	VkCommandPoolCreateInfo pool_info = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
	    .queueFamilyIndex = vk->main_queue->family_index,
	    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
	};
	vr = vk->vkCreateCommandPool(vk->device, &pool_info, NULL, &msc->shared_cmd_pool);
	if (vr != VK_SUCCESS) {
		U_LOG_E("[#59] failed to create shared command pool: %d", vr);
		return false;
	}

	msc->shared_cmd_buffers = U_TYPED_ARRAY_CALLOC(VkCommandBuffer, image_count);
	msc->shared_fences = U_TYPED_ARRAY_CALLOC(VkFence, image_count);
	if (msc->shared_cmd_buffers == NULL || msc->shared_fences == NULL) {
		U_LOG_E("[#59] failed to allocate shared cmd/fence arrays");
		return false;
	}

	VkCommandBufferAllocateInfo cb_alloc_info = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	    .commandPool = msc->shared_cmd_pool,
	    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
	    .commandBufferCount = image_count,
	};
	vr = vk->vkAllocateCommandBuffers(vk->device, &cb_alloc_info, msc->shared_cmd_buffers);
	if (vr != VK_SUCCESS) {
		U_LOG_E("[#59] failed to allocate shared command buffers: %d", vr);
		return false;
	}

	VkFenceCreateInfo fence_info = {
	    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
	    .flags = VK_FENCE_CREATE_SIGNALED_BIT,
	};
	for (uint32_t i = 0; i < image_count; i++) {
		vr = vk->vkCreateFence(vk->device, &fence_info, NULL, &msc->shared_fences[i]);
		if (vr != VK_SUCCESS) {
			U_LOG_E("[#59] failed to create shared fence %u: %d", i, vr);
			return false;
		}
	}
	msc->shared_buffer_count = image_count;
	msc->shared_fenced_buffer = -1;

	// Render pass (single color attachment) + framebuffers — used by a
	// self-submitting DP that targets the swapchain image (matches per-session).
	VkAttachmentDescription color_attachment = {
	    .format = ct->format,
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
	vr = vk->vkCreateRenderPass(vk->device, &rp_info, NULL, &msc->shared_render_pass);
	if (vr == VK_SUCCESS) {
		msc->shared_framebuffers = U_TYPED_ARRAY_CALLOC(VkFramebuffer, image_count);
		if (msc->shared_framebuffers != NULL) {
			for (uint32_t i = 0; i < image_count; i++) {
				VkFramebufferCreateInfo fb_info = {
				    .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
				    .renderPass = msc->shared_render_pass,
				    .attachmentCount = 1,
				    .pAttachments = &ct->images[i].view,
				    .width = ct->width,
				    .height = ct->height,
				    .layers = 1,
				};
				if (vk->vkCreateFramebuffer(vk->device, &fb_info, NULL,
				                            &msc->shared_framebuffers[i]) != VK_SUCCESS) {
					msc->shared_framebuffers[i] = VK_NULL_HANDLE;
				}
			}
		}
	}

	// The one display processor that weaves the combined atlas.
	xrt_dp_factory_vk_fn_t factory = (xrt_dp_factory_vk_fn_t)msc->base.info.dp_factory_vk;
	if (factory != NULL) {
		xrt_result_t dp_ret = factory(vk,                                         // vk_bundle
		                              (void *)(uintptr_t)msc->shared_cmd_pool,    // cmd_pool
		                              NULL,                                        // window_handle (service-owned)
		                              (int32_t)ct->format,                         // target_format
		                              &msc->shared_dp);
		if (dp_ret != XRT_SUCCESS) {
			U_LOG_W("[#59] shared DP factory failed: %d (no weave)", dp_ret);
			msc->shared_dp = NULL;
		} else {
			U_LOG_W("[#59] created shared display processor");
		}
	}

	msc->shared_surface_initialized = true;
	U_LOG_W("[#59] shared surface initialized");
	return true;
}

/*!
 * Tear down everything shared_surface_init / shared_ensure_atlas /
 * shared_ensure_chrome_blend created. Called from system_compositor_destroy once
 * the render thread has stopped, while the Vulkan device (owned by the native
 * compositor) and the target service are both still alive. Mirrors the per-session
 * fini ordering in multi_compositor_destroy. Idempotent: every handle is checked
 * and cleared so a second call is a no-op.
 */
static void
shared_surface_fini(struct multi_system_compositor *msc)
{
	struct vk_bundle *vk = (msc->target_service != NULL) ? comp_target_service_get_vk(msc->target_service) : NULL;

	// The render thread is stopped, but the last present's GPU work may still be
	// in flight; wait before destroying anything it could reference.
	if (vk != NULL) {
		vk->vkDeviceWaitIdle(vk->device);
	}

	// Display processor first (owns any vendor SDK handles); NULLs the pointer.
	if (msc->shared_dp != NULL) {
		xrt_display_processor_destroy(&msc->shared_dp);
	}

	// M3 decorations: the atlas-wide framebuffer references the blend's render
	// pass, so destroy the framebuffer before the blend pipeline.
	if (vk != NULL && msc->shared_atlas_fb != VK_NULL_HANDLE) {
		vk->vkDestroyFramebuffer(vk->device, msc->shared_atlas_fb, NULL);
		msc->shared_atlas_fb = VK_NULL_HANDLE;
		msc->shared_atlas_fb_view = VK_NULL_HANDLE;
	}
	if (vk != NULL && msc->shared_chrome_blend_initialized) {
		vk_hud_blend_fini(&msc->shared_chrome_blend, vk);
		msc->shared_chrome_blend_initialized = false;
	}
	// Task 9 content composite: owns its own render pass + pipeline (no fb).
	if (vk != NULL && msc->shared_content_blend_initialized) {
		comp_multi_content_blend_fini(&msc->shared_content_blend, vk);
		msc->shared_content_blend_initialized = false;
	}

	// The one combined stereo atlas (view + image + memory).
	if (vk != NULL && msc->shared_atlas_initialized) {
		if (msc->shared_atlas_view != VK_NULL_HANDLE)
			vk->vkDestroyImageView(vk->device, msc->shared_atlas_view, NULL);
		if (msc->shared_atlas_image != VK_NULL_HANDLE)
			vk->vkDestroyImage(vk->device, msc->shared_atlas_image, NULL);
		if (msc->shared_atlas_memory != VK_NULL_HANDLE)
			vk->vkFreeMemory(vk->device, msc->shared_atlas_memory, NULL);
		msc->shared_atlas_view = VK_NULL_HANDLE;
		msc->shared_atlas_image = VK_NULL_HANDLE;
		msc->shared_atlas_memory = VK_NULL_HANDLE;
		msc->shared_atlas_initialized = false;
	}

	// Per-image render rings: fences, framebuffers, render pass, command pool.
	if (vk != NULL && msc->shared_fences != NULL) {
		for (uint32_t i = 0; i < msc->shared_buffer_count; i++) {
			if (msc->shared_fences[i] != VK_NULL_HANDLE)
				vk->vkDestroyFence(vk->device, msc->shared_fences[i], NULL);
		}
	}
	free(msc->shared_fences);
	msc->shared_fences = NULL;

	// Command buffers are freed when the pool is destroyed below; just drop the array.
	free(msc->shared_cmd_buffers);
	msc->shared_cmd_buffers = NULL;

	if (vk != NULL && msc->shared_framebuffers != NULL) {
		for (uint32_t i = 0; i < msc->shared_buffer_count; i++) {
			if (msc->shared_framebuffers[i] != VK_NULL_HANDLE)
				vk->vkDestroyFramebuffer(vk->device, msc->shared_framebuffers[i], NULL);
		}
	}
	free(msc->shared_framebuffers);
	msc->shared_framebuffers = NULL;

	if (vk != NULL && msc->shared_render_pass != VK_NULL_HANDLE) {
		vk->vkDestroyRenderPass(vk->device, msc->shared_render_pass, NULL);
		msc->shared_render_pass = VK_NULL_HANDLE;
	}
	if (vk != NULL && msc->shared_cmd_pool != VK_NULL_HANDLE) {
		vk->vkDestroyCommandPool(vk->device, msc->shared_cmd_pool, NULL);
		msc->shared_cmd_pool = VK_NULL_HANDLE;
	}
	msc->shared_buffer_count = 0;
	msc->shared_fenced_buffer = -1;

	// The one full-screen NSWindow target (created + owned via the target service).
	if (msc->shared_target != NULL && msc->target_service != NULL) {
		comp_target_service_destroy(msc->target_service, &msc->shared_target);
	}

	msc->shared_surface_initialized = false;
}

/*!
 * Ensure the one combined stereo atlas exists at (tile_columns*eye_w) × eye_h.
 */
static bool
shared_ensure_atlas(struct multi_system_compositor *msc,
                    struct vk_bundle *vk,
                    int eye_w,
                    int eye_h,
                    uint32_t tile_columns,
                    VkFormat format)
{
	int atlas_w = (int)tile_columns * eye_w;
	int atlas_h = eye_h;
	if (msc->shared_atlas_initialized && msc->shared_atlas_w == atlas_w && msc->shared_atlas_h == atlas_h &&
	    msc->shared_atlas_format == format) {
		return true;
	}
	if (msc->shared_atlas_initialized) {
		if (msc->shared_atlas_view != VK_NULL_HANDLE)
			vk->vkDestroyImageView(vk->device, msc->shared_atlas_view, NULL);
		if (msc->shared_atlas_image != VK_NULL_HANDLE)
			vk->vkDestroyImage(vk->device, msc->shared_atlas_image, NULL);
		if (msc->shared_atlas_memory != VK_NULL_HANDLE)
			vk->vkFreeMemory(vk->device, msc->shared_atlas_memory, NULL);
		msc->shared_atlas_initialized = false;
	}

	VkExtent2D extent = {(uint32_t)atlas_w, (uint32_t)atlas_h};
	VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	VkResult ret =
	    vk_create_image_simple(vk, extent, format, usage, &msc->shared_atlas_memory, &msc->shared_atlas_image);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[#59] failed to create combined atlas: %s", vk_result_string(ret));
		return false;
	}
	VkImageSubresourceRange range = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1};
	ret = vk_create_view(vk, msc->shared_atlas_image, VK_IMAGE_VIEW_TYPE_2D, format, range, &msc->shared_atlas_view);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[#59] failed to create combined atlas view: %s", vk_result_string(ret));
		return false;
	}
	msc->shared_atlas_w = atlas_w;
	msc->shared_atlas_h = atlas_h;
	msc->shared_eye_w = eye_w;
	msc->shared_eye_h = eye_h;
	msc->shared_atlas_format = format;
	msc->shared_atlas_initialized = true;
	U_LOG_W("[#59] combined atlas %dx%d (per-eye %dx%d, %u tiles) fmt=%d", atlas_w, atlas_h, eye_w, eye_h,
	        tile_columns, format);
	return true;
}

//! One client's resolved placement + content for the combined-atlas pass.
struct shared_client_render
{
	struct multi_compositor *mc;
	struct multi_layer_entry *layer;
	uint32_t view_count;
	int slot; // stable tie-break (msc->clients index) so equal-z order is deterministic
	bool placed;                     // true if the controller has set a window pose
	float pose_x, pose_y, pose_z;    // window center in meters (display-center origin, +y up, +z toward viewer)
	struct xrt_quat pose_q;          // window orientation (shell rotate gesture: yaw+pitch); identity = axis-aligned
	bool rotated;                    // true if pose_q is non-identity → project as a tilted quad (Task 10)
	float win_w_m, win_h_m;          // window size in meters (aspect-clamped)
	int win_x, win_y, win_w, win_h;  // M1 / unplaced fallback: top-left display px
};

/*!
 * Project a window's center pose (meters, display-center origin) through one eye
 * onto the display plane (Z=0), returning a top-left display-px rect. M2 port of
 * the D3D11 slot_pose_to_pixel_rect_for_eye: a window at z>0 (toward the viewer)
 * scales up and parallax-shifts per eye so it floats in front of the panel; z=0
 * gives both eyes the same rect (window on the panel). px_per_m_{x,y} convert
 * meters→display px; disp_px_{w,h} are the per-eye display dims.
 */
static void
shared_project_rect_for_eye(float pose_x,
                            float pose_y,
                            float pose_z,
                            float win_w_m,
                            float win_h_m,
                            float eye_x,
                            float eye_y,
                            float eye_z,
                            int disp_px_w,
                            int disp_px_h,
                            float px_per_m_x,
                            float px_per_m_y,
                            int *out_x,
                            int *out_y,
                            int *out_w,
                            int *out_h)
{
	float wx = pose_x, wy = pose_y, w_m = win_w_m, h_m = win_h_m;
	if (fabsf(pose_z) > 0.0001f && eye_z > 0.01f) {
		float denom = eye_z - pose_z;
		if (fabsf(denom) < 0.001f) {
			denom = (denom >= 0.0f) ? 0.001f : -0.001f;
		}
		float scale = eye_z / denom;
		wx = eye_x + scale * (pose_x - eye_x);
		wy = eye_y + scale * (pose_y - eye_y);
		w_m *= scale;
		h_m *= scale;
	}
	int w_px = (int)(w_m * px_per_m_x + 0.5f);
	int h_px = (int)(h_m * px_per_m_y + 0.5f);
	float center_px_x = (float)disp_px_w * 0.5f + wx * px_per_m_x;
	float center_px_y = (float)disp_px_h * 0.5f - wy * px_per_m_y; // +y up → -y down
	*out_x = (int)(center_px_x - (float)w_px * 0.5f + 0.5f);
	*out_y = (int)(center_px_y - (float)h_px * 0.5f + 0.5f);
	*out_w = w_px;
	*out_h = h_px;
}

//! True when @p q is (approximately) the identity quaternion — the axis-aligned
//! fast path applies. Same 1e-4 tolerance as the D3D11 service's quat_is_identity.
static inline bool
shared_quat_is_identity(const struct xrt_quat *q)
{
	return fabsf(q->x) < 0.0001f && fabsf(q->y) < 0.0001f && fabsf(q->z) < 0.0001f &&
	       fabsf(q->w - 1.0f) < 0.0001f;
}

/*!
 * Project one 3D world point through @p eye onto the panel plane (Z=0), returning
 * display pixel coords. Mirror of the D3D11 service project_point_for_eye.
 */
static inline void
shared_project_point_for_eye(float px,
                             float py,
                             float pz,
                             float eye_x,
                             float eye_y,
                             float eye_z,
                             float disp_px_w,
                             float disp_px_h,
                             float px_per_m_x,
                             float px_per_m_y,
                             float *out_px_x,
                             float *out_px_y)
{
	if (fabsf(pz) > 0.0001f && eye_z > 0.01f) {
		float denom = eye_z - pz;
		if (fabsf(denom) < 0.001f) {
			denom = (denom >= 0.0f) ? 0.001f : -0.001f;
		}
		float scale = eye_z / denom;
		px = eye_x + scale * (px - eye_x);
		py = eye_y + scale * (py - eye_y);
	}
	*out_px_x = disp_px_w * 0.5f + px * px_per_m_x;
	*out_px_y = disp_px_h * 0.5f - py * px_per_m_y; // +y up → -y down
}

/*!
 * Project the 4 corners of a rotated window (orientation @p q about its center
 * pose, size win_w_m × win_h_m) through @p eye onto the panel, and emit them as
 * clip-space NDC (against the FULL atlas) + per-corner W for perspective-correct
 * interpolation. Corner order TL, BL, TR, BR matches the quad shader's win-local
 * UV LUT. @p tile_x0 offsets the eye tile within the combined atlas (col 0 = left,
 * col 1 = right). Faithful port of the D3D11 compute_projected_quad_corners.
 *
 * The window-local corners are rotated by @p q, translated to the window center,
 * projected per-eye, converted from atlas px → NDC. W = (eye_z - corner_world_z)
 * so a tilted window foreshortens correctly. out_corners is laid out as the quad
 * push-constant's corners[4][4] = {ndc.x, ndc.y, depth_ndc(0), W}.
 */
static void
shared_project_quad_for_eye(float pose_x,
                            float pose_y,
                            float pose_z,
                            const struct xrt_quat *q,
                            float win_w_m,
                            float win_h_m,
                            float eye_x,
                            float eye_y,
                            float eye_z,
                            int tile_x0,
                            int disp_px_w,
                            int disp_px_h,
                            int atlas_w,
                            int atlas_h,
                            float px_per_m_x,
                            float px_per_m_y,
                            float out_corners[4][4])
{
	float hw = win_w_m * 0.5f;
	float hh = win_h_m * 0.5f;
	// Window-local corners: TL, BL, TR, BR (matches quad.vert win_uv LUT).
	struct xrt_vec3 local[4] = {
	    {-hw, +hh, 0.0f}, // TL
	    {-hw, -hh, 0.0f}, // BL
	    {+hw, +hh, 0.0f}, // TR
	    {+hw, -hh, 0.0f}, // BR
	};
	for (int i = 0; i < 4; i++) {
		struct xrt_vec3 world;
		math_quat_rotate_vec3(q, &local[i], &world);
		world.x += pose_x;
		world.y += pose_y;
		world.z += pose_z;

		float w = eye_z - world.z; // perspective W (>0 in front of the eye)
		if (w < 0.01f) {
			w = 0.01f;
		}

		float dpx = 0.0f, dpy = 0.0f;
		shared_project_point_for_eye(world.x, world.y, world.z, eye_x, eye_y, eye_z, (float)disp_px_w,
		                             (float)disp_px_h, px_per_m_x, px_per_m_y, &dpx, &dpy);
		// Atlas px (eye tile) → NDC against the full atlas. Vulkan NDC y=-1 is the
		// viewport top, and dpy grows downward, so no extra y-flip is needed.
		float atlas_px_x = (float)tile_x0 + dpx;
		float atlas_px_y = dpy;
		out_corners[i][0] = atlas_px_x / (float)atlas_w * 2.0f - 1.0f;
		out_corners[i][1] = atlas_px_y / (float)atlas_h * 2.0f - 1.0f;
		out_corners[i][2] = 0.0f; // depth_ndc (no hardware depth test here)
		out_corners[i][3] = w;
	}
}

//! Painter's order: far (larger z) first so nearer windows overwrite. Equal z
//! tie-breaks on the stable client slot — otherwise qsort can swap two
//! overlapping same-z windows frame to frame, which reads as a blink.
static int
shared_client_sort(const void *a, const void *b)
{
	const struct shared_client_render *ca = a;
	const struct shared_client_render *cb = b;
	if (ca->pose_z > cb->pose_z)
		return -1;
	if (ca->pose_z < cb->pose_z)
		return 1;
	return (ca->slot < cb->slot) ? -1 : (ca->slot > cb->slot) ? 1 : 0;
}

//! Chrome depth offset (meters, toward the viewer) relative to its window. 0 =
//! coplanar with the window: the pill carries exactly the window's depth (no
//! extra disparity of its own), so its detailed text/buttons don't fringe in
//! front of the flat pill background. A small positive value would float it
//! slightly ahead, but that reads as odd anaglyph fringing on the chrome detail.
#define SHARED_CHROME_DEPTH_BIAS_M 0.0f

/*!
 * Ensure the M3 chrome/overlay/cursor blend pipeline and the atlas-wide
 * framebuffer exist (the framebuffer is rebuilt when the atlas view changes).
 */
static bool
shared_ensure_chrome_blend(struct multi_system_compositor *msc, struct vk_bundle *vk)
{
	if (!msc->shared_chrome_blend_initialized) {
		if (!vk_hud_blend_init(&msc->shared_chrome_blend, vk, msc->shared_atlas_format)) {
			U_LOG_E("[#59] failed to init shared chrome blend");
			return false;
		}
		msc->shared_chrome_blend_initialized = true;
	}
	if (msc->shared_atlas_fb == VK_NULL_HANDLE || msc->shared_atlas_fb_view != msc->shared_atlas_view) {
		if (msc->shared_atlas_fb != VK_NULL_HANDLE) {
			vk->vkDestroyFramebuffer(vk->device, msc->shared_atlas_fb, NULL);
			msc->shared_atlas_fb = VK_NULL_HANDLE;
		}
		VkFramebufferCreateInfo fb_info = {
		    .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		    .renderPass = msc->shared_chrome_blend.render_pass,
		    .attachmentCount = 1,
		    .pAttachments = &msc->shared_atlas_view,
		    .width = (uint32_t)msc->shared_atlas_w,
		    .height = (uint32_t)msc->shared_atlas_h,
		    .layers = 1,
		};
		if (vk->vkCreateFramebuffer(vk->device, &fb_info, NULL, &msc->shared_atlas_fb) != VK_SUCCESS) {
			U_LOG_E("[#59] failed to create atlas framebuffer");
			return false;
		}
		msc->shared_atlas_fb_view = msc->shared_atlas_view;
	}
	return true;
}

/*!
 * Ensure the rounded-corner content-composite pipeline (Task 9) and the
 * atlas-wide framebuffer it renders into are ready. Reuses shared_ensure_chrome_blend
 * to (re)build msc->shared_atlas_fb — the content pipeline's render pass is
 * render-pass-compatible with the chrome blend's (same single color attachment,
 * format, samples), so the same framebuffer serves both.
 */
static bool
shared_ensure_content_blend(struct multi_system_compositor *msc, struct vk_bundle *vk)
{
	if (!shared_ensure_chrome_blend(msc, vk)) {
		return false;
	}
	if (!msc->shared_content_blend_initialized) {
		if (!comp_multi_content_blend_init(&msc->shared_content_blend, vk, msc->shared_atlas_format)) {
			U_LOG_E("[#59] failed to init shared content blend");
			return false;
		}
		msc->shared_content_blend_initialized = true;
	}
	return true;
}

/*!
 * Alpha-blend one decoration source image into the combined atlas (atlas must be
 * COLOR_ATTACHMENT_OPTIMAL on entry/exit). The dst rect is clamped to [clip_x0,
 * clip_x1) × [clip_y0, clip_y1) — the eye's tile — so a decoration can't bleed
 * into the other eye or off the atlas. The cross-process source rests in GENERAL.
 */
static void
shared_blend_into_atlas(struct multi_system_compositor *msc,
                        struct vk_bundle *vk,
                        VkCommandBuffer cmd,
                        VkImage src_image,
                        int dst_x,
                        int dst_y,
                        int dst_w,
                        int dst_h,
                        int clip_x0,
                        int clip_x1,
                        int clip_y0,
                        int clip_y1)
{
	if (src_image == VK_NULL_HANDLE || dst_w <= 0 || dst_h <= 0) {
		return;
	}
	// Clamp the dst rect to the clip (eye tile). vk_hud_blend maps the full source
	// across the dst rect, so clamping squishes a decoration that overhangs the
	// tile edge — acceptable; chrome/overlays normally sit well inside the tile.
	int x0 = dst_x < clip_x0 ? clip_x0 : dst_x;
	int y0 = dst_y < clip_y0 ? clip_y0 : dst_y;
	int x1 = dst_x + dst_w > clip_x1 ? clip_x1 : dst_x + dst_w;
	int y1 = dst_y + dst_h > clip_y1 ? clip_y1 : dst_y + dst_h;
	if (x1 - x0 < 1 || y1 - y0 < 1) {
		return;
	}

	VkImageMemoryBarrier to_src = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = 0,
	    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
	    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	    .image = src_image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
	                         NULL, 0, NULL, 1, &to_src);

	vk_hud_blend_draw_no_layout(&msc->shared_chrome_blend, vk, cmd, msc->shared_atlas_fb,
	                            (uint32_t)msc->shared_atlas_w, (uint32_t)msc->shared_atlas_h, src_image, x0, y0,
	                            (uint32_t)(x1 - x0), (uint32_t)(y1 - y0));

	VkImageMemoryBarrier to_general = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
	    .dstAccessMask = 0,
	    .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
	    .image = src_image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
	                         NULL, 0, NULL, 1, &to_general);
}

/*!
 * M3: composite the workspace decorations into the combined atlas, per eye,
 * BEFORE the weave so they carry 3D depth. Chrome floats above each window (its
 * window pose + a depth bias, projected per eye); session-global overlays
 * (taskbar/launcher) and the cursor are flat at the panel (z = 0, identical both
 * eyes). The atlas must be COLOR_ATTACHMENT_OPTIMAL on entry/exit.
 */
static void
shared_composite_decorations(struct multi_system_compositor *msc,
                             struct vk_bundle *vk,
                             VkCommandBuffer cmd,
                             const struct shared_client_render *order,
                             uint32_t order_count,
                             const struct xrt_eye_positions *eye_pos,
                             int eye_w,
                             int eye_h,
                             float px_per_m_x,
                             float px_per_m_y,
                             uint32_t tile_columns)
{
	if (!shared_ensure_chrome_blend(msc, vk)) {
		return;
	}

	// Per-client chrome (the title pill), floating above its window in 3D.
	for (uint32_t c = 0; c < order_count; c++) {
		const struct shared_client_render *e = &order[c];
		if (!e->placed) {
			continue;
		}
		struct xrt_swapchain *chrome_xsc = NULL;
		struct comp_multi_chrome_layout layout;
		if (!comp_multi_workspace_chrome_get(&e->mc->base.base, &chrome_xsc, &layout)) {
			continue;
		}
		struct comp_swapchain *sc = comp_swapchain(chrome_xsc);
		if (sc == NULL || sc->vkic.image_count == 0) {
			continue;
		}
		VkImage chrome_img = sc->vkic.images[0].handle;
		if (chrome_img == VK_NULL_HANDLE) {
			continue;
		}

		// Chrome extent + center in WORLD meters (window pose + window-local offset).
		float chrome_w_m = (layout.width_as_fraction_of_window > 0.0f)
		                       ? e->win_w_m * layout.width_as_fraction_of_window
		                       : layout.size_w_m;
		float chrome_h_m = layout.size_h_m;
		if (chrome_w_m <= 0.0f || chrome_h_m <= 0.0f) {
			continue;
		}
		// Chrome center in WINDOW-LOCAL meters (the pill floats above the window's
		// top edge, in the window's own frame). For a rotated window the chrome
		// follows the orientation: rotate this local offset by the window quat and
		// project the pill as a tilted quad (Task 10); for an axis-aligned window
		// the local offset is already world-relative (identity rotation).
		float clx = layout.pose_in_client.position.x;
		float cly = layout.anchor_to_window_top_edge
		                ? (e->win_h_m * 0.5f + layout.pose_in_client.position.y)
		                : layout.pose_in_client.position.y;
		float clz = layout.pose_in_client.position.z + SHARED_CHROME_DEPTH_BIAS_M;

		if (e->rotated) {
			// Rotate the window-local chrome offset into world, keep the window
			// orientation for the pill plane (coplanar with the window).
			struct xrt_vec3 loff = {clx, cly, clz};
			struct xrt_vec3 woff;
			math_quat_rotate_vec3(&e->pose_q, &loff, &woff);
			float ccx = e->pose_x + woff.x;
			float ccy = e->pose_y + woff.y;
			float ccz = e->pose_z + woff.z;

			// One render pass over both eyes (barriers must wrap, not nest, the pass).
			if (shared_ensure_content_blend(msc, vk)) {
				VkImageMemoryBarrier to_read = {
				    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				    .srcAccessMask = 0,
				    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
				    .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
				    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				    .image = chrome_img,
				    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
				};
				vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
				                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0,
				                         NULL, 1, &to_read);
				comp_multi_content_blend_begin(&msc->shared_content_blend, vk, cmd,
				                               msc->shared_atlas_fb, (uint32_t)msc->shared_atlas_w,
				                               (uint32_t)msc->shared_atlas_h);
				for (uint32_t eye = 0; eye < tile_columns; eye++) {
					uint32_t ei = (eye < eye_pos->count) ? eye : (eye_pos->count - 1);
					int tile_x0 = (int)eye * eye_w;
					float corners[4][4];
					shared_project_quad_for_eye(ccx, ccy, ccz, &e->pose_q, chrome_w_m, chrome_h_m,
					                            eye_pos->eyes[ei].x, eye_pos->eyes[ei].y,
					                            eye_pos->eyes[ei].z, tile_x0, eye_w, eye_h,
					                            (int)msc->shared_atlas_w, (int)msc->shared_atlas_h,
					                            px_per_m_x, px_per_m_y, corners);
					// Chrome pill: the shell texture carries the capsule shape in
					// alpha, so no runtime rounding/feather (use_src_alpha = 1).
					struct comp_multi_content_pc_quad pcq = {
					    .src_uv_off = {0.0f, 0.0f},
					    .src_uv_scale = {1.0f, 1.0f},
					    .corner_radius = 0.0f,
					    .corner_aspect = 0.0f,
					    .edge_feather = 0.0f,
					    .use_src_alpha = 1.0f,
					};
					memcpy(pcq.corners, corners, sizeof(corners));
					comp_multi_content_blend_draw_quad(&msc->shared_content_blend, vk, cmd,
					                                   chrome_img, 0, sc->vkic.info.format, &pcq,
					                                   tile_x0, 0, (uint32_t)eye_w, (uint32_t)eye_h,
					                                   (uint32_t)msc->shared_atlas_w,
					                                   (uint32_t)msc->shared_atlas_h);
				}
				comp_multi_content_blend_end(&msc->shared_content_blend, vk, cmd);
				VkImageMemoryBarrier to_general = {
				    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				    .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
				    .dstAccessMask = 0,
				    .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
				    .image = chrome_img,
				    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
				};
				vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
				                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL,
				                         1, &to_general);
			}
			continue;
		}

		float cx_world = e->pose_x + clx;
		float cy_world = e->pose_y + cly;
		float cz_world = e->pose_z + clz;

		for (uint32_t eye = 0; eye < tile_columns; eye++) {
			uint32_t ei = (eye < eye_pos->count) ? eye : (eye_pos->count - 1);
			int rx, ry, rw, rh;
			shared_project_rect_for_eye(cx_world, cy_world, cz_world, chrome_w_m, chrome_h_m,
			                            eye_pos->eyes[ei].x, eye_pos->eyes[ei].y, eye_pos->eyes[ei].z,
			                            eye_w, eye_h, px_per_m_x, px_per_m_y, &rx, &ry, &rw, &rh);
			int tile_x0 = (int)eye * eye_w;
			shared_blend_into_atlas(msc, vk, cmd, chrome_img, tile_x0 + rx, ry, rw, rh, tile_x0,
			                        tile_x0 + eye_w, 0, eye_h);
		}
	}

	// Session-global overlays (taskbar/launcher), flat at z = 0 in both eye tiles.
	{
		struct comp_multi_overlay_state states[COMP_MULTI_WORKSPACE_MAX_OVERLAYS];
		struct xrt_swapchain *xscs[COMP_MULTI_WORKSPACE_MAX_OVERLAYS];
		uint32_t n = comp_multi_workspace_copy_overlays(states, xscs, COMP_MULTI_WORKSPACE_MAX_OVERLAYS);
		for (uint32_t i = 0; i < n; i++) {
			struct comp_swapchain *sc = comp_swapchain(xscs[i]);
			if (sc == NULL || sc->vkic.image_count == 0) {
				continue;
			}
			VkImage img = sc->vkic.images[0].handle;
			if (img == VK_NULL_HANDLE) {
				continue;
			}
			int ow = (int)(states[i].size_w_m * px_per_m_x + 0.5f);
			int oh = (int)(states[i].size_h_m * px_per_m_y + 0.5f);
			if (ow < 1 || oh < 1) {
				continue;
			}
			int base_x = (int)(states[i].anchor_x * (float)eye_w - states[i].pivot_x * (float)ow);
			int base_y = (int)(states[i].anchor_y * (float)eye_h - states[i].pivot_y * (float)oh);
			for (uint32_t eye = 0; eye < tile_columns; eye++) {
				int tile_x0 = (int)eye * eye_w;
				shared_blend_into_atlas(msc, vk, cmd, img, tile_x0 + base_x, base_y, ow, oh,
				                        tile_x0, tile_x0 + eye_w, 0, eye_h);
			}
		}
	}

	// Session-global cursor, topmost, at the tracked pointer (flat, z = 0).
	{
		struct xrt_swapchain *cur_xsc = NULL;
		struct comp_multi_cursor_state cur;
		if (comp_multi_workspace_get_cursor(&cur_xsc, &cur)) {
			struct comp_swapchain *sc = comp_swapchain(cur_xsc);
			if (sc != NULL && sc->vkic.image_count > 0) {
				VkImage img = sc->vkic.images[0].handle;
				int size_px = (int)(cur.size_meters * px_per_m_x + 0.5f);
				int32_t pxg = 0, pyg = 0;
				comp_multi_workspace_get_pointer_px(&pxg, &pyg);
				if (img != VK_NULL_HANDLE && size_px >= 1 && pxg >= 0 && pxg < eye_w && pyg >= 0 &&
				    pyg < eye_h) {
					int dx = pxg - (int)(cur.hot_x * (float)size_px);
					int dy = pyg - (int)(cur.hot_y * (float)size_px);
					for (uint32_t eye = 0; eye < tile_columns; eye++) {
						int tile_x0 = (int)eye * eye_w;
						shared_blend_into_atlas(msc, vk, cmd, img, tile_x0 + dx, dy, size_px,
						                        size_px, tile_x0, tile_x0 + eye_w, 0, eye_h);
					}
				}
			}
		}
	}
}

/*!
 * Composite every active client into the one combined atlas, weave once, present
 * once. The macOS shared-surface analogue of render_per_session_clients_locked.
 */
static void
render_shared_surface_locked(struct multi_system_compositor *msc, int64_t display_time_ns)
{
	COMP_TRACE_MARKER();

	struct vk_bundle *vk = comp_target_service_get_vk(msc->target_service);
	if (vk == NULL) {
		return;
	}

	// Collect the clients to composite this frame (active, visible, with content).
	struct shared_client_render order[MULTI_MAX_CLIENTS];
	uint32_t order_count = 0;
	for (size_t k = 0; k < ARRAY_SIZE(msc->clients); k++) {
		struct multi_compositor *mc = msc->clients[k];
		if (mc == NULL) {
			continue;
		}
		// A minimized client (#61) contributes nothing to the surface; the
		// taskbar/overlays draw at the combined pass below regardless.
		if (comp_multi_workspace_is_window_hidden(&mc->base.base)) {
			continue;
		}
		if (!mc->delivered.active || mc->delivered.layer_count == 0) {
			continue;
		}
		struct multi_layer_entry *layer = NULL;
		for (uint32_t i = 0; i < mc->delivered.layer_count; i++) {
			enum xrt_layer_type t = mc->delivered.layers[i].data.type;
			if (t == XRT_LAYER_PROJECTION || t == XRT_LAYER_PROJECTION_DEPTH || t == XRT_LAYER_ZONE_3D) {
				layer = &mc->delivered.layers[i];
				break;
			}
		}
		if (layer == NULL) {
			continue;
		}

		struct shared_client_render *e = &order[order_count++];
		e->mc = mc;
		e->layer = layer;
		e->slot = (int)k;
		uint32_t vc = layer->data.view_count;
		if (vc < 1)
			vc = 1;
		if (vc > XRT_MAX_VIEWS)
			vc = XRT_MAX_VIEWS;
		e->view_count = vc;

		// Placement: the controller's window pose in meters (set_window_pose)
		// drives per-eye projection (M2). Fall back to the full display px-rect
		// for an unplaced lone app (M1 behavior, flat at the panel).
		struct xrt_pose pose = XRT_POSE_IDENTITY;
		float wm = 0.0f, hm = 0.0f;
		if (comp_multi_workspace_load_window_pose(&mc->base.base, &pose, &wm, &hm) && wm > 0.0f && hm > 0.0f) {
			e->placed = true;
			e->pose_x = pose.position.x;
			e->pose_y = pose.position.y;
			e->pose_z = pose.position.z;
			e->pose_q = pose.orientation;
			e->rotated = !shared_quat_is_identity(&pose.orientation);
			e->win_w_m = wm;
			e->win_h_m = hm;
		} else {
			e->placed = false;
			e->pose_x = e->pose_y = e->pose_z = 0.0f;
			e->pose_q = (struct xrt_quat){0.0f, 0.0f, 0.0f, 1.0f};
			e->rotated = false;
		}
		int wx = 0, wy = 0, ww = 0, wh = 0;
		if (!comp_multi_workspace_load_window_px_rect(&mc->base.base, &wx, &wy, &ww, &wh) || ww <= 0 ||
		    wh <= 0) {
			wx = 0;
			wy = 0;
			ww = msc->base.info.display_pixel_width;
			wh = msc->base.info.display_pixel_height;
		}
		e->win_x = wx;
		e->win_y = wy;
		e->win_w = ww;
		e->win_h = wh;
	}

	// Lazy-init the shared window/DP once a client is present. Avoids creating a
	// full-screen window with nothing to show. #61: a connected workspace
	// controller (the shell) submits no content yet still wants the surface up —
	// an empty spatial desktop must show its backdrop + splash + launcher band. So
	// also init when a workspace controller is active, not only when a content
	// client is rendering.
	if (order_count == 0 && !msc->shared_surface_initialized && !msc->workspace_active) {
		return;
	}
	if (!shared_surface_init(msc, vk)) {
		return;
	}

	// Painter's sort (far first).
	if (order_count > 1) {
		qsort(order, order_count, sizeof(order[0]), shared_client_sort);
	}

	struct comp_target *ct = msc->shared_target;

	// Per-eye tile = the display; combined atlas = 2 tiles (stereo) side by side.
	int eye_w = (int)msc->base.info.display_pixel_width;
	int eye_h = (int)msc->base.info.display_pixel_height;
	if (eye_w <= 0 || eye_h <= 0) {
		eye_w = (int)ct->width;
		eye_h = (int)ct->height;
	}
	const uint32_t tile_columns = 2; // M1: always stereo
	const uint32_t tile_rows = 1;

	// Atlas format: a SINGLE stable format (the target's) so the atlas is created
	// once, never per-frame. Clients can submit different swapchain formats (two
	// identical cubes can land on SRGB vs UNORM); vkCmdBlitImage converts each into
	// the atlas. Keying recreation on a per-client format made the atlas thrash
	// (rebuild every frame → full-screen blink) because the painter order — and
	// thus which client is order[0] — alternates frame to frame.
	VkFormat atlas_format = ct->format;
	if (!shared_ensure_atlas(msc, vk, eye_w, eye_h, tile_columns, atlas_format)) {
		return;
	}

	// Per-eye parallax inputs (M2): the predicted eye positions from the one
	// display processor, with a nominal 64 mm IPD @ display nominal-z fallback
	// when no eye tracker has lock (sim has none). px-per-meter converts the
	// projected window meters into display px.
	struct xrt_eye_positions eye_pos = {0};
	if (msc->shared_dp != NULL) {
		(void)xrt_display_processor_get_predicted_eye_positions(msc->shared_dp, &eye_pos);
	}
	if (!eye_pos.valid || eye_pos.count < 2) {
		float nom_y = (msc->base.info.nominal_viewer_y_m != 0.0f) ? msc->base.info.nominal_viewer_y_m : 0.0f;
		float nom_z = (msc->base.info.nominal_viewer_z_m > 0.0f) ? msc->base.info.nominal_viewer_z_m : 0.6f;
		eye_pos.eyes[0] = (struct xrt_eye_position){-0.032f, nom_y, nom_z};
		eye_pos.eyes[1] = (struct xrt_eye_position){0.032f, nom_y, nom_z};
		eye_pos.count = 2;
		eye_pos.valid = true;
	}
	float disp_w_m = msc->base.info.display_width_m;
	float disp_h_m = msc->base.info.display_height_m;
	if (disp_w_m <= 0.0f || disp_h_m <= 0.0f) {
		disp_w_m = 0.301f;
		disp_h_m = 0.196f;
	}
	float px_per_m_x = (float)eye_w / disp_w_m;
	float px_per_m_y = (float)eye_h / disp_h_m;

	// Wait for the previous frame's fence on whatever buffer we'll reuse.
	if (msc->shared_fenced_buffer >= 0) {
		vk->vkWaitForFences(vk->device, 1, &msc->shared_fences[msc->shared_fenced_buffer], VK_TRUE, UINT64_MAX);
		msc->shared_fenced_buffer = -1;
	}

	// Frame pacing (the shared target is the only consumer; it must drive pacing).
	{
		int64_t frame_id = -1, wake = 0, desired = 0, slop = 0, predicted = 0;
		comp_target_calc_frame_pacing(ct, &frame_id, &wake, &desired, &slop, &predicted);
		int64_t now_ns = os_monotonic_get_ns();
		comp_target_mark_wake_up(ct, frame_id, now_ns);
		comp_target_mark_begin(ct, frame_id, now_ns);
	}

	uint32_t buffer_index = 0;
	VkResult ret = comp_target_acquire(ct, &buffer_index);
	if (ret == VK_ERROR_OUT_OF_DATE_KHR || ret == VK_ERROR_SURFACE_LOST_KHR) {
		U_LOG_W("[#59] shared acquire %s, skipping frame", vk_result_string(ret));
		return;
	}
	if (ret != VK_SUCCESS && ret != VK_SUBOPTIMAL_KHR) {
		U_LOG_E("[#59] shared acquire failed: %s", vk_result_string(ret));
		return;
	}
	if (buffer_index >= msc->shared_buffer_count) {
		U_LOG_E("[#59] shared buffer_index %u out of range", buffer_index);
		return;
	}

	vk->vkResetFences(vk->device, 1, &msc->shared_fences[buffer_index]);

	uint32_t fb_w = ct->width, fb_h = ct->height;
	VkFormat fb_fmt = ct->format;
	VkFramebuffer framebuffer =
	    (msc->shared_framebuffers != NULL) ? msc->shared_framebuffers[buffer_index] : VK_NULL_HANDLE;

	VkCommandBuffer cmd = msc->shared_cmd_buffers[buffer_index];
	vk->vkResetCommandBuffer(cmd, 0);
	VkCommandBufferBeginInfo begin_info = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	if (vk->vkBeginCommandBuffer(cmd, &begin_info) != VK_SUCCESS) {
		return;
	}

	VkImageSubresourceRange atlas_range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

	// Atlas → TRANSFER_DST, then clear to the spatial-desktop backdrop (#1a1a1a).
	VkImageMemoryBarrier atlas_to_dst = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = 0,
	    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	    .image = msc->shared_atlas_image,
	    .subresourceRange = atlas_range,
	};
	vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
	                         NULL, 1, &atlas_to_dst);

	VkClearColorValue backdrop = {.float32 = {0.1f, 0.1f, 0.1f, 1.0f}};
	vk->vkCmdClearColorImage(cmd, msc->shared_atlas_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &backdrop, 1,
	                         &atlas_range);
	// Atlas → COLOR_ATTACHMENT for the content + decoration passes (the backdrop
	// clear above was the atlas's only TRANSFER write). Content composites with a
	// rounded-rect + edge-feather shader (Task 9) instead of a hard vkCmdBlitImage,
	// so each window's corners match the shell's rounded focus ring and its edges
	// feather into the backdrop. The decorations (chrome/overlays/cursor) follow in
	// the same COLOR_ATTACHMENT layout.
	VkImageMemoryBarrier atlas_to_color = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	    .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	    .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	    .image = msc->shared_atlas_image,
	    .subresourceRange = atlas_range,
	};
	vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
	                         0, NULL, 0, NULL, 1, &atlas_to_color);

	// Composite each client's content into its window rect, per eye, with rounded
	// corners + feathered edges. ONE render pass over the whole atlas so overlapping
	// windows blend in painter (far→near, already-sorted) submission order; every
	// content source image must therefore be SHADER_READ before the pass begins.
	if (shared_ensure_content_blend(msc, vk)) {
		// Corner radius matches the shell focus ring (fh*0.045) so the content
		// corners are concentric with it; feather is ~2 px of the window height,
		// capped at the radius (Windows caps so feather_band = feather/ry ≤ 1).
		const float CONTENT_CORNER_RADIUS_FRAC = 0.045f;
		const float CONTENT_FEATHER_PX = 2.0f;

		// Collect the unique (image, array-layer) content sources across all
		// clients and transition them GENERAL → SHADER_READ in one batch (barriers
		// can't sit inside the render pass).
		VkImage uniq[COMP_MULTI_CONTENT_BLEND_MAX_IMAGES];
		uint32_t uniq_arr[COMP_MULTI_CONTENT_BLEND_MAX_IMAGES];
		uint32_t uniq_n = 0;
		for (uint32_t c = 0; c < order_count; c++) {
			struct shared_client_render *e = &order[c];
			for (uint32_t v = 0; v < e->view_count; v++) {
				int tw = 0, th = 0;
				VkFormat fmt = VK_FORMAT_UNDEFINED;
				VkImageView iv = VK_NULL_HANDLE;
				VkImage im = VK_NULL_HANDLE;
				uint32_t ai = 0;
				if (!get_session_layer_view(e->layer, v, &tw, &th, &fmt, &iv, &im, &ai)) {
					continue;
				}
				bool found = false;
				for (uint32_t u = 0; u < uniq_n; u++) {
					if (uniq[u] == im && uniq_arr[u] == ai) {
						found = true;
						break;
					}
				}
				if (!found && uniq_n < COMP_MULTI_CONTENT_BLEND_MAX_IMAGES) {
					uniq[uniq_n] = im;
					uniq_arr[uniq_n] = ai;
					uniq_n++;
				}
			}
		}
		if (uniq_n > 0) {
			VkImageMemoryBarrier pre[COMP_MULTI_CONTENT_BLEND_MAX_IMAGES];
			for (uint32_t u = 0; u < uniq_n; u++) {
				pre[u] = (VkImageMemoryBarrier){
				    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				    .srcAccessMask = 0,
				    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
				    .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
				    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				    .image = uniq[u],
				    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, uniq_arr[u], 1},
				};
			}
			vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL,
			                         uniq_n, pre);
		}

		comp_multi_content_blend_begin(&msc->shared_content_blend, vk, cmd, msc->shared_atlas_fb,
		                               (uint32_t)msc->shared_atlas_w, (uint32_t)msc->shared_atlas_h);
		// Per client (far→near), per eye: project the window rect, clip it to the
		// eye tile, remap the source sub-rect, and draw with rounded-rect coverage.
		// M2: a placed window projects its center pose through each eye onto the
		// panel (Z=0), so z>0 windows float toward the viewer; an unplaced lone app
		// uses the flat full-display px-rect (M1). The client's own views supply
		// its internal stereo.
		for (uint32_t c = 0; c < order_count; c++) {
			struct shared_client_render *e = &order[c];
			bool flip_y = e->layer->data.flip_y;
			float corner_aspect = (e->win_h_m > 0.0f) ? (e->win_w_m / e->win_h_m) : 1.0f;
			float win_h_px = (float)(e->win_h > 0 ? e->win_h : eye_h);
			float edge_feather = CONTENT_FEATHER_PX / win_h_px;
			if (edge_feather > CONTENT_CORNER_RADIUS_FRAC) {
				edge_feather = CONTENT_CORNER_RADIUS_FRAC;
			}

			// Focus tint (#59 Task 10, Windows parity): when the controller's pushed
			// style enables a glow (it toggles intensity by focus), fade the content
			// toward the glow color across the feather band. Widen the feather to the
			// style's band so the highlight reads as a soft glow rather than a 2 px
			// line — capped at the corner radius (Windows caps feather/ry ≤ 1). The
			// tint follows the window's tilt for free because it rides the content
			// quad (axis-aligned or projected). Replaces the old shell overlay ring.
			float glow_color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
			float glow_intensity = 0.0f;
			// Tint only the FOCUSED client (the controller pushes a glow style for
			// every windowed client; the runtime gates on focus, like D3D11).
			if (comp_multi_workspace_is_focused(&e->mc->base.base)) {
				struct comp_multi_client_style st;
				if (comp_multi_workspace_get_client_style(&e->mc->base.base, &st) &&
				    st.focus_glow_intensity > 0.0f) {
					glow_intensity = st.focus_glow_intensity;
					glow_color[0] = st.focus_glow_color[0];
					glow_color[1] = st.focus_glow_color[1];
					glow_color[2] = st.focus_glow_color[2];
					glow_color[3] = st.focus_glow_color[3];
					if (st.edge_feather_meters > 0.0f && e->win_h_m > 0.0f) {
						float sf = st.edge_feather_meters / e->win_h_m;
						if (sf > edge_feather) {
							edge_feather = sf;
						}
						if (edge_feather > CONTENT_CORNER_RADIUS_FRAC) {
							edge_feather = CONTENT_CORNER_RADIUS_FRAC;
						}
					}
				}
			}

			for (uint32_t eye = 0; eye < tile_columns; eye++) {
				uint32_t v = (eye < e->view_count) ? eye : (e->view_count - 1);
				int tw = 0, th = 0;
				VkFormat fmt = VK_FORMAT_UNDEFINED;
				VkImageView iv = VK_NULL_HANDLE;
				VkImage im = VK_NULL_HANDLE;
				uint32_t ai = 0;
				if (!get_session_layer_view(e->layer, v, &tw, &th, &fmt, &iv, &im, &ai)) {
					continue;
				}
				int sx = e->layer->data.proj.v[v].sub.rect.offset.w;
				int sy = e->layer->data.proj.v[v].sub.rect.offset.h;
				// Full source image dims for UV normalization (the sub-rect is a
				// tile within the worst-case swapchain image).
				struct comp_swapchain *sc = comp_swapchain(e->layer->xscs[v]);
				float img_w = (float)sc->vkic.info.width;
				float img_h = (float)sc->vkic.info.height;
				if (img_w <= 0.0f || img_h <= 0.0f) {
					continue;
				}

				int tile_x0_e = (int)eye * eye_w;

				// Rotated window (Task 10): project the tilted window-local corners
				// per eye into a perspective quad (mirrors the D3D11 quad_mode path).
				// The whole window maps to win-local UV [0,1], so the rounded-corner
				// SDF stays at the window's real corners under tilt. The eye-tile
				// scissor confines the quad; no proportional source remap is needed
				// (the source sub-rect spans the full window). Only for placed
				// windows — an unplaced lone app is axis-aligned by definition.
				if (e->rotated && e->placed) {
					uint32_t ei = (eye < eye_pos.count) ? eye : (eye_pos.count - 1);
					float corners[4][4];
					shared_project_quad_for_eye(e->pose_x, e->pose_y, e->pose_z, &e->pose_q,
					                            e->win_w_m, e->win_h_m, eye_pos.eyes[ei].x,
					                            eye_pos.eyes[ei].y, eye_pos.eyes[ei].z, tile_x0_e,
					                            eye_w, eye_h, (int)msc->shared_atlas_w,
					                            (int)msc->shared_atlas_h, px_per_m_x, px_per_m_y,
					                            corners);
					// Source sub-rect → normalized UV with flip baked into scale.y.
					float so_y = (float)(flip_y ? sy + th : sy);
					float ss_y = (float)(flip_y ? -th : th);
					struct comp_multi_content_pc_quad pcq = {
					    .src_uv_off = {(float)sx / img_w, so_y / img_h},
					    .src_uv_scale = {(float)tw / img_w, ss_y / img_h},
					    .corner_radius = CONTENT_CORNER_RADIUS_FRAC,
					    .corner_aspect = corner_aspect,
					    .edge_feather = edge_feather,
					    .use_src_alpha = 0.0f, // content is opaque; coverage only
					    .glow_color = {glow_color[0], glow_color[1], glow_color[2], glow_color[3]},
					    .glow_intensity = glow_intensity,
					};
					memcpy(pcq.corners, corners, sizeof(corners));
					comp_multi_content_blend_draw_quad(
					    &msc->shared_content_blend, vk, cmd, im, ai, fmt, &pcq, tile_x0_e, 0,
					    (uint32_t)eye_w, (uint32_t)eye_h, (uint32_t)msc->shared_atlas_w,
					    (uint32_t)msc->shared_atlas_h);
					continue;
				}

				int rx = e->win_x, ry = e->win_y, rw = e->win_w, rh = e->win_h;
				if (e->placed) {
					uint32_t ei = (eye < eye_pos.count) ? eye : (eye_pos.count - 1);
					shared_project_rect_for_eye(e->pose_x, e->pose_y, e->pose_z, e->win_w_m,
					                            e->win_h_m, eye_pos.eyes[ei].x,
					                            eye_pos.eyes[ei].y, eye_pos.eyes[ei].z, eye_w,
					                            eye_h, px_per_m_x, px_per_m_y, &rx, &ry, &rw, &rh);
				}
				if (rw <= 0 || rh <= 0) {
					continue;
				}
				int tile_x0 = (int)eye * eye_w;
				float fdx0 = (float)(tile_x0 + rx);
				float fdy0 = (float)ry;
				float fdx1 = fdx0 + (float)rw;
				float fdy1 = fdy0 + (float)rh;
				// Source span (flip_y maps top↔bottom → negative src_uv_scale.y).
				float fsx0 = (float)sx;
				float fsx1 = (float)(sx + tw);
				float fsy0 = (float)(sy + (flip_y ? th : 0));
				float fsy1 = (float)(sy + (flip_y ? 0 : th));
				// Clip the destination to the tile bounds; a projected/edge window
				// can overflow its tile or the atlas. Map the clip back into the
				// source proportionally; the window-local UV domain (ux/uy) keeps the
				// rounded corners at the window's real corners even when clipped.
				float cdx0 = fmaxf(fdx0, (float)tile_x0);
				float cdx1 = fminf(fdx1, (float)(tile_x0 + eye_w));
				float cdy0 = fmaxf(fdy0, 0.0f);
				float cdy1 = fminf(fdy1, (float)eye_h);
				if (cdx1 - cdx0 < 1.0f || cdy1 - cdy0 < 1.0f) {
					continue;
				}
				float ux0 = (cdx0 - fdx0) / (fdx1 - fdx0), ux1 = (cdx1 - fdx0) / (fdx1 - fdx0);
				float uy0 = (cdy0 - fdy0) / (fdy1 - fdy0), uy1 = (cdy1 - fdy0) / (fdy1 - fdy0);
				float nsx0 = fsx0 + ux0 * (fsx1 - fsx0);
				float nsx1 = fsx0 + ux1 * (fsx1 - fsx0);
				float nsy0 = fsy0 + uy0 * (fsy1 - fsy0);
				float nsy1 = fsy0 + uy1 * (fsy1 - fsy0);

				struct comp_multi_content_pc pc = {
				    .src_uv_off = {nsx0 / img_w, nsy0 / img_h},
				    .src_uv_scale = {(nsx1 - nsx0) / img_w, (nsy1 - nsy0) / img_h},
				    .win_uv_off = {ux0, uy0},
				    .win_uv_scale = {ux1 - ux0, uy1 - uy0},
				    .corner_radius = CONTENT_CORNER_RADIUS_FRAC,
				    .corner_aspect = corner_aspect,
				    .edge_feather = edge_feather,
				    .glow_intensity = glow_intensity,
				    .glow_color = {glow_color[0], glow_color[1], glow_color[2], glow_color[3]},
				};
				comp_multi_content_blend_draw(&msc->shared_content_blend, vk, cmd, im, ai, fmt, &pc,
				                              (int32_t)cdx0, (int32_t)cdy0,
				                              (uint32_t)(cdx1 - cdx0), (uint32_t)(cdy1 - cdy0));
			}
		}
		comp_multi_content_blend_end(&msc->shared_content_blend, vk, cmd);

		// Restore the cross-process sources to GENERAL (their rest layout).
		if (uniq_n > 0) {
			VkImageMemoryBarrier post[COMP_MULTI_CONTENT_BLEND_MAX_IMAGES];
			for (uint32_t u = 0; u < uniq_n; u++) {
				post[u] = (VkImageMemoryBarrier){
				    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				    .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
				    .dstAccessMask = 0,
				    .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
				    .image = uniq[u],
				    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, uniq_arr[u], 1},
				};
			}
			vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL,
			                         uniq_n, post);
		}
	}

	// Make content's color-attachment writes visible to the decoration render pass
	// (both render into the atlas as a color attachment, in separate render passes).
	VkImageMemoryBarrier atlas_content_to_deco = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	    .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	    .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	    .image = msc->shared_atlas_image,
	    .subresourceRange = atlas_range,
	};
	vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1,
	                         &atlas_content_to_deco);

	shared_composite_decorations(msc, vk, cmd, order, order_count, &eye_pos, eye_w, eye_h, px_per_m_x, px_per_m_y,
	                             tile_columns);

	// Atlas → SHADER_READ for the weave.
	VkImageMemoryBarrier atlas_to_read = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	    .image = msc->shared_atlas_image,
	    .subresourceRange = atlas_range,
	};
	vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &atlas_to_read);

	// Target → COLOR_ATTACHMENT for the weave.
	VkImageMemoryBarrier pre_weave = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = 0,
	    .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	    .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	    .image = ct->images[buffer_index].handle,
	    .subresourceRange = atlas_range,
	};
	vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	                         0, 0, NULL, 0, NULL, 1, &pre_weave);

	if (msc->shared_dp != NULL) {
		xrt_display_processor_set_target_color_view(msc->shared_dp, ct->images[buffer_index].view);
		xrt_display_processor_process_atlas(msc->shared_dp, cmd,
		                                    (VkImage_XDP)msc->shared_atlas_image, msc->shared_atlas_view,
		                                    (uint32_t)eye_w, (uint32_t)eye_h, tile_columns, tile_rows,
		                                    (VkFormat_XDP)atlas_format, framebuffer,
		                                    (VkImage_XDP)ct->images[buffer_index].handle, fb_w, fb_h,
		                                    (VkFormat_XDP)fb_fmt, 0, 0, 0, 0);
	}

	// Target → PRESENT_SRC.
	VkImageMemoryBarrier post_weave = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	    .dstAccessMask = 0,
	    .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	    .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
	    .image = ct->images[buffer_index].handle,
	    .subresourceRange = atlas_range,
	};
	vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &post_weave);

	if (vk->vkEndCommandBuffer(cmd) != VK_SUCCESS) {
		return;
	}

	VkSemaphore wait_sem = ct->semaphores.present_complete;
	VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkSemaphore signal_sem = ct->semaphores.render_complete;
	VkSubmitInfo submit_info = {
	    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	    .waitSemaphoreCount = (wait_sem != VK_NULL_HANDLE) ? 1 : 0,
	    .pWaitSemaphores = (wait_sem != VK_NULL_HANDLE) ? &wait_sem : NULL,
	    .pWaitDstStageMask = (wait_sem != VK_NULL_HANDLE) ? &wait_stage : NULL,
	    .commandBufferCount = 1,
	    .pCommandBuffers = &cmd,
	    .signalSemaphoreCount = (signal_sem != VK_NULL_HANDLE) ? 1 : 0,
	    .pSignalSemaphores = (signal_sem != VK_NULL_HANDLE) ? &signal_sem : NULL,
	};
	ret = vk->vkQueueSubmit(vk->main_queue->queue, 1, &submit_info, msc->shared_fences[buffer_index]);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[#59] shared submit failed: %s", vk_result_string(ret));
		return;
	}

	// Cross-device shared memory has no GPU-level sync between app and compositor
	// devices (same rationale as the per-session path) — wait before present.
	vk->vkWaitForFences(vk->device, 1, &msc->shared_fences[buffer_index], VK_TRUE, UINT64_MAX);
	msc->shared_fenced_buffer = -1;

	ret = comp_target_present(ct, vk->main_queue->queue, buffer_index, 0, display_time_ns, 0);
	if (ret == VK_ERROR_OUT_OF_DATE_KHR || ret == VK_ERROR_SURFACE_LOST_KHR) {
		U_LOG_W("[#59] shared present %s", vk_result_string(ret));
	} else if (ret != VK_SUCCESS && ret != VK_SUBOPTIMAL_KHR) {
		U_LOG_E("[#59] shared present failed: %s", vk_result_string(ret));
	}

	// NOTE: do NOT retire the delivered frames here. Unlike the per-session path
	// (where each client owns a swapchain that persists its last present), the
	// shared surface rebuilds ONE atlas every compositor frame. Retiring would
	// clear `delivered`, so any client that did not submit a brand-new frame this
	// tick would fall back to backdrop → a per-window blink. Leaving `delivered`
	// sticky (the native compositor's model — it is overwritten on the next
	// delivery) means every placed client always has content to composite; a
	// frozen/slow client simply re-shows its last frame, exactly like a real
	// desktop window.
}
#endif // XRT_OS_MACOS

#ifndef XRT_OS_MACOS
/*!
 * Render all per-session clients to their own targets.
 * Called after xrt_comp_layer_commit() for sessions with external window handles.
 * macOS uses render_shared_surface_locked instead (one shared full-screen surface).
 *
 * @param msc The multi system compositor
 * @param display_time_ns The predicted display time
 */
static void
render_per_session_clients_locked(struct multi_system_compositor *msc, int64_t display_time_ns)
{
	COMP_TRACE_MARKER();

	struct vk_bundle *vk = comp_target_service_get_vk(msc->target_service);
	if (vk == NULL) {
		U_LOG_E("[per-session] No Vulkan bundle available from target service");
		return;
	}

	for (size_t k = 0; k < ARRAY_SIZE(msc->clients); k++) {
		struct multi_compositor *mc = msc->clients[k];

		if (mc == NULL || !mc->session_render.initialized) {
			continue;
		}

#ifdef XRT_OS_MACOS
		// Tier-2 deferred window placement (#59): apply a pending set_window_pose
		// once its size has settled (no newer pose for ~150 ms). Coalesces a layout
		// glide's per-frame poses into a single NSWindow resize + swapchain
		// recreate; recreating on every glide frame churns MoltenVK and tears down
		// every session a few seconds later. The reposition/resize + drawableSize
		// happen on the main thread inside comp_window_macos_set_window_rect; the
		// recreate flag drives the next acquire to rebuild at the new surface size.
		if (mc->session_render.resize_pending) {
			uint64_t now_ns = os_monotonic_get_ns();
			const uint64_t settle_ns = (uint64_t)150 * 1000 * 1000; // 150 ms
			if (now_ns - mc->session_render.resize_request_ns >= settle_ns) {
				comp_window_macos_set_window_rect(
				    mc->session_render.target, mc->session_render.resize_x,
				    mc->session_render.resize_y, mc->session_render.resize_w,
				    mc->session_render.resize_h);
				mc->session_render.swapchain_needs_recreate = true;
				mc->session_render.resize_pending = false;
				U_LOG_W("[#59] applied settled window rect (%d,%d %dx%d)",
				        mc->session_render.resize_x, mc->session_render.resize_y,
				        mc->session_render.resize_w, mc->session_render.resize_h);
			}
		}
#endif

		// A workspace-minimized client (#61) renders a black desktop canvas plus
		// the session-global overlays/cursor — NOT its content — so the taskbar
		// stays visible while minimized. The client app doesn't know it's
		// minimized, so it keeps submitting frames; force the black canvas
		// regardless of delivered state. A non-hidden client with no delivered
		// frame is skipped as before.
		bool hidden = comp_multi_workspace_is_window_hidden(&mc->base.base);
		if (!hidden && (!mc->delivered.active || mc->delivered.layer_count == 0)) {
			continue;
		}

#ifdef XRT_OS_WINDOWS
		// Skip rendering if self-owned window was closed
		if (mc->session_render.owns_window && mc->session_render.own_window != NULL &&
		    !comp_d3d11_window_is_valid(mc->session_render.own_window)) {
			U_LOG_W("[per-session] Client %zu: skipping (window closed)", k);
			int64_t now_ns = os_monotonic_get_ns();
			multi_compositor_retire_delivered_locked(mc, now_ns);
			continue;
		}
#endif
#ifdef XRT_OS_MACOS
		// Skip rendering if macOS window was closed
		{
			extern bool oxr_macos_window_closed(void);
			if (oxr_macos_window_closed()) {
				int64_t now_ns = os_monotonic_get_ns();
				multi_compositor_retire_delivered_locked(mc, now_ns);
				continue;
			}
		}
#endif

		// Render this session to its own target (black canvas + overlays if hidden)
		render_session_to_own_target(mc, vk, display_time_ns, hidden);

		// Retire the delivered frame for this session
		int64_t now_ns = os_monotonic_get_ns();
		multi_compositor_retire_delivered_locked(mc, now_ns);
	}
}
#endif // !XRT_OS_MACOS



static void
transfer_layers_locked(struct multi_system_compositor *msc, int64_t display_time_ns, int64_t system_frame_id)
{
	COMP_TRACE_MARKER();

	struct xrt_compositor *xc = &msc->xcn->base;

	struct multi_compositor *array[MULTI_MAX_CLIENTS] = {0};

	// To mark latching.
	int64_t now_ns = os_monotonic_get_ns();

	size_t count = 0;
	for (size_t k = 0; k < ARRAY_SIZE(array); k++) {
		struct multi_compositor *mc = msc->clients[k];

		// Array can be empty
		if (mc == NULL) {
			continue;
		}

		// Lazily initialize per-session render resources if session has external HWND or readback
		// This creates the per-session comp_target and display processor for multi-app support
		// Skip re-init if window close is in progress (avoids VK_ERROR_SURFACE_LOST_KHR on dead HWND).
		// Also require an ACTIVE session: end_session tears the target down on purpose
		// (xrEndSession when the app backgrounds behind a picker, #528) — re-initializing
		// here while it is still tearing down races vkCreateAndroidSurfaceKHR against the
		// old swapchain's BufferQueue connection (VK_ERROR_NATIVE_WINDOW_IN_USE_KHR storm)
		// and pointlessly rebuilds a target no layers will reach. begin_session flips
		// session_active back on and the next pass re-inits from the then-current surface.
		bool skip_session_render_init = false;
#ifdef XRT_OS_MACOS
		// Shared spatial surface (#59): every client composites into ONE
		// service-owned full-screen window, so the per-client NSWindow/target/DP
		// is never created. The shared surface reads each client's delivered
		// frames directly (deliver_any_frames below still runs). This is the only
		// macOS service render path now (the legacy per-NSWindow path is removed).
		skip_session_render_init = true;
#endif
		if (!skip_session_render_init && mc->state.session_active &&
		    multi_compositor_has_session_render(mc) && !mc->session_render.initialized &&
		    !mc->session_render.window_close_exit_sent) {
			multi_compositor_init_session_render(mc);
		}

		// Even if it's not shown, make sure that frames are delivered.
		multi_compositor_deliver_any_frames(mc, display_time_ns);

		// None of the data in this slot is valid, don't check access it.
		if (!mc->delivered.active) {
			continue;
		}

		// The client isn't visible, do not submit it's layers.
		if (!mc->state.visible) {
			// Need to drop delivered frame as it shouldn't be reused.
			multi_compositor_retire_delivered_locked(mc, now_ns);
			continue;
		}

		// Just in case.
		if (!mc->state.session_active) {
			U_LOG_W("Session is visible but not active.");

			// Need to drop delivered frame as it shouldn't be reused.
			multi_compositor_retire_delivered_locked(mc, now_ns);
			continue;
		}

		// The list_and_timing_lock is held when callign this function.
		multi_compositor_latch_frame_locked(mc, now_ns, system_frame_id);

		array[count++] = msc->clients[k];
	}

	// Sort the stack array
	qsort(array, count, sizeof(struct multi_compositor *), overlay_sort_func);

	// find first (ordered by bottom to top) active client to retrieve xrt_layer_frame_data
	const enum xrt_blend_mode blend_mode = find_active_blend_mode(array, count);

	const struct xrt_layer_frame_data data = {
	    .frame_id = system_frame_id,
	    .display_time_ns = display_time_ns,
	    .env_blend_mode = blend_mode,
	};
	xrt_comp_layer_begin(xc, &data);

	// Runtime-side 2D/3D toggle from qwerty V key for shared compositor path.
	// Per-session clients handle this in render_session_to_own_target().
#ifdef XRT_BUILD_DRIVER_QWERTY
	for (size_t k = 0; k < count; k++) {
		struct multi_compositor *mc = array[k];
		if (mc->session_render.initialized) {
			continue; // skip per-session clients
		}
		if (mc->xsysd != NULL) {
			bool force_2d = false;
			bool toggled =
			    qwerty_check_display_mode_toggle(mc->xsysd->xdevs, mc->xsysd->xdev_count, &force_2d);
			if (toggled) {
				struct xrt_device *head = mc->xsysd->static_roles.head;
				if (head != NULL && head->hmd != NULL) {
					if (force_2d) {
						mc->last_3d_mode_index = head->hmd->active_rendering_mode_index;
						for (uint32_t i = 0; i < head->rendering_mode_count; i++) {
							if (!head->rendering_modes[i].hardware_display_3d) {
								head->hmd->active_rendering_mode_index = i;
								break;
							}
						}
					} else {
						head->hmd->active_rendering_mode_index = mc->last_3d_mode_index;
					}
				}
				multi_compositor_request_display_mode(mc, !force_2d);
			}

			// Rendering mode change from qwerty 1/2/3 keys (disabled for legacy apps).
			if (!mc->msc->base.info.legacy_app_tile_scaling) {
				int render_mode = -1;
				if (qwerty_check_rendering_mode_change(mc->xsysd->xdevs, mc->xsysd->xdev_count,
				                                       &render_mode)) {
					struct xrt_device *head = mc->xsysd->static_roles.head;
					if (head != NULL) {
						xrt_device_set_property(head, XRT_DEVICE_PROPERTY_OUTPUT_MODE,
						                        render_mode);
					}
				}
			}

			break; // one check per frame is enough
		}
	}
#endif

	// Note: qwerty input forwarding to main compositor removed (comp_main no longer exists).
	// Per-session windows handle their own input forwarding.

	// Copy all active layers (skip sessions with per-session rendering - Phase 4).
	for (size_t k = 0; k < count; k++) {
		struct multi_compositor *mc = array[k];
		assert(mc != NULL);

		// Skip sessions with per-session rendering - they render separately to their own targets
		if (mc->session_render.initialized) {
			continue;
		}

		// Sync hardware_display_3d for shared clients
		struct xrt_device *shared_head = (mc->xsysd != NULL) ? mc->xsysd->static_roles.head : NULL;
		if (shared_head != NULL && shared_head->hmd != NULL) {
			uint32_t idx = shared_head->hmd->active_rendering_mode_index;
			if (idx < shared_head->rendering_mode_count) {
				mc->hardware_display_3d = shared_head->rendering_modes[idx].hardware_display_3d;
			}
		}

		for (uint32_t i = 0; i < mc->delivered.layer_count; i++) {
			struct multi_layer_entry *layer = &mc->delivered.layers[i];

			// When 2D mode is active, override projection layers to mono (view_count=1)
			// so the main compositor's dispatch_graphics() uses full-width viewport
			// and skips weaving.
#ifdef XRT_BUILD_DRIVER_QWERTY
			bool override_mono = !mc->hardware_display_3d &&
			                     (layer->data.type == XRT_LAYER_PROJECTION ||
			                      layer->data.type == XRT_LAYER_PROJECTION_DEPTH) &&
			                     layer->data.view_count > 1;
			if (override_mono) {
				struct multi_layer_entry mono_layer = *layer;
				mono_layer.data.view_count = 1;
				switch (layer->data.type) {
				case XRT_LAYER_PROJECTION:
					do_projection_layer(xc, mc, &mono_layer, i);
					break;
				case XRT_LAYER_PROJECTION_DEPTH:
					do_projection_layer_depth(xc, mc, &mono_layer, i);
					break;
				default: break;
				}
				continue;
			}
#endif

			switch (layer->data.type) {
			case XRT_LAYER_PROJECTION: do_projection_layer(xc, mc, layer, i); break;
			case XRT_LAYER_PROJECTION_DEPTH: do_projection_layer_depth(xc, mc, layer, i); break;
			case XRT_LAYER_QUAD: do_quad_layer(xc, mc, layer, i); break;
			case XRT_LAYER_CUBE: do_cube_layer(xc, mc, layer, i); break;
			case XRT_LAYER_CYLINDER: do_cylinder_layer(xc, mc, layer, i); break;
			case XRT_LAYER_EQUIRECT1: do_equirect1_layer(xc, mc, layer, i); break;
			case XRT_LAYER_EQUIRECT2: do_equirect2_layer(xc, mc, layer, i); break;
			case XRT_LAYER_ZONE_3D: do_zone_3d_layer(xc, mc, layer, i); break;
			case XRT_LAYER_LOCAL_2D:
				// 2D overlay consumed only by the per-session render path
				// (blit into the target's 2D region, #568); the shared/
				// downstream path has no consumer — drop quietly.
				break;
			default: U_LOG_E("Unhandled layer type '%i'!", layer->data.type); break;
			}
		}
	}
}

static void
broadcast_timings_to_clients(struct multi_system_compositor *msc, int64_t predicted_display_time_ns)
{
	COMP_TRACE_MARKER();

	os_mutex_lock(&msc->list_and_timing_lock);

	for (size_t i = 0; i < ARRAY_SIZE(msc->clients); i++) {
		struct multi_compositor *mc = msc->clients[i];
		if (mc == NULL) {
			continue;
		}

		os_mutex_lock(&mc->slot_lock);
		mc->slot_next_frame_display = predicted_display_time_ns;
		os_mutex_unlock(&mc->slot_lock);
	}

	os_mutex_unlock(&msc->list_and_timing_lock);
}

static void
broadcast_timings_to_pacers(struct multi_system_compositor *msc,
                            int64_t predicted_display_time_ns,
                            int64_t predicted_display_period_ns,
                            int64_t diff_ns)
{
	COMP_TRACE_MARKER();

	os_mutex_lock(&msc->list_and_timing_lock);

	for (size_t i = 0; i < ARRAY_SIZE(msc->clients); i++) {
		struct multi_compositor *mc = msc->clients[i];
		if (mc == NULL) {
			continue;
		}

		u_pa_info(                       //
		    mc->upa,                     //
		    predicted_display_time_ns,   //
		    predicted_display_period_ns, //
		    diff_ns);                    //

		os_mutex_lock(&mc->slot_lock);
		mc->slot_next_frame_display = predicted_display_time_ns;
		os_mutex_unlock(&mc->slot_lock);
	}

	msc->last_timings.predicted_display_time_ns = predicted_display_time_ns;
	msc->last_timings.predicted_display_period_ns = predicted_display_period_ns;
	msc->last_timings.diff_ns = diff_ns;

	os_mutex_unlock(&msc->list_and_timing_lock);
}

static void
wait_frame(struct os_precise_sleeper *sleeper, struct xrt_compositor *xc, int64_t frame_id, int64_t wake_up_time_ns)
{
	COMP_TRACE_MARKER();

	// Wait until the given wake up time.
	u_wait_until(sleeper, wake_up_time_ns);

	int64_t now_ns = os_monotonic_get_ns();

	// Signal that we woke up.
	xrt_comp_mark_frame(xc, frame_id, XRT_COMPOSITOR_FRAME_POINT_WOKE, now_ns);
}

static void
update_session_state_locked(struct multi_system_compositor *msc)
{
	struct xrt_compositor *xc = &msc->xcn->base;

	//! @todo Make this not be hardcoded.
	const struct xrt_begin_session_info begin_session_info = {
	    .view_type = XRT_VIEW_TYPE_STEREO,
	    .ext_hand_tracking_enabled = false,
	    .ext_hand_tracking_data_source_enabled = false,
	    .ext_eye_gaze_interaction_enabled = false,
	    .ext_hand_interaction_enabled = false,
	    .htc_facial_tracking_enabled = false,
	    .fb_body_tracking_enabled = false,
	    .fb_face_tracking2_enabled = false,
	    .meta_body_tracking_full_body_enabled = false,
	    .meta_body_tracking_calibration_enabled = false,
	};

	// #61: on macOS the shared spatial surface must keep rendering (empty backdrop
	// + DXR splash + launcher band) while a workspace CONTROLLER (the shell) is
	// connected, even with no active content app session — otherwise the render
	// thread sleeps and no window ever appears. A controller never xrBeginSession's
	// so it doesn't bump active_count; the activate handler sets workspace_active
	// (and wakes this thread). macOS-only: elsewhere effective_active ==
	// active_count, so the state machine is unchanged.
	uint64_t effective_active = msc->sessions.active_count;
#ifdef XRT_OS_MACOS
	if (msc->workspace_active) {
		effective_active = 1;
	}
#endif

	switch (msc->sessions.state) {
	case MULTI_SYSTEM_STATE_INIT_WARM_START:
		// Produce at least one frame on init.
		msc->sessions.state = MULTI_SYSTEM_STATE_STOPPING;
		xrt_comp_begin_session(xc, &begin_session_info);
		U_LOG_I("Doing warm start, %u active app session(s).", (uint32_t)msc->sessions.active_count);
		break;

	case MULTI_SYSTEM_STATE_STOPPED:
		if (effective_active == 0) {
			break;
		}

		msc->sessions.state = MULTI_SYSTEM_STATE_RUNNING;
		xrt_comp_begin_session(xc, &begin_session_info);
		U_LOG_I("Started native session, %u active app session(s).", (uint32_t)msc->sessions.active_count);
		break;

	case MULTI_SYSTEM_STATE_RUNNING:
		if (effective_active > 0) {
			break;
		}

		msc->sessions.state = MULTI_SYSTEM_STATE_STOPPING;
		U_LOG_D("Stopping native session, %u active app session(s).", (uint32_t)msc->sessions.active_count);
		break;

	case MULTI_SYSTEM_STATE_STOPPING:
		// Just in case
		if (effective_active > 0) {
			msc->sessions.state = MULTI_SYSTEM_STATE_RUNNING;
			U_LOG_D("Restarting native session, %u active app session(s).",
			        (uint32_t)msc->sessions.active_count);
			break;
		}

		msc->sessions.state = MULTI_SYSTEM_STATE_STOPPED;
		xrt_comp_end_session(xc);
		U_LOG_I("Stopped native session, %u active app session(s).", (uint32_t)msc->sessions.active_count);
		break;

	case MULTI_SYSTEM_STATE_INVALID:
	default:
		U_LOG_E("Got invalid state %u", msc->sessions.state);
		msc->sessions.state = MULTI_SYSTEM_STATE_STOPPING;
		assert(false);
	}
}

#ifdef XRT_OS_ANDROID
/*!
 * #563: pause/resume the clients' display processors when the published
 * output surface goes away / comes back (Client.java clearAppSurface /
 * passAppSurface, the #528 backgrounding case that does NOT end the
 * session). The panel's switchable 3D backlight is system-global — if the
 * weave just stops, the screen stays in physical 3D over the 2D home
 * screen. Session end/begin and client destroy have their own hooks; this
 * covers the surface-only transition.
 */
static void
android_window_transition_locked(struct multi_system_compositor *msc)
{
	struct _ANativeWindow *window = NULL;
	uint64_t generation = 0;
	bool valid = false;
	android_globals_get_window_state(&window, &generation, &valid);

	int state = (valid && window != NULL) ? 1 : 0;
	if (state == msc->android_window_valid_state) {
		return;
	}
	bool first = msc->android_window_valid_state < 0;
	msc->android_window_valid_state = state;
	if (first && state == 1) {
		return; // startup with a live surface — nothing to resume
	}

	for (size_t k = 0; k < ARRAY_SIZE(msc->clients); k++) {
		struct multi_compositor *mc = msc->clients[k];
		if (mc == NULL || !mc->session_render.initialized ||
		    mc->session_render.display_processor == NULL) {
			continue;
		}
		if (state == 0) {
			U_LOG_W("multi: output surface lost — pausing DP (backlight to 2D, #563)");
			xrt_display_processor_on_pause(mc->session_render.display_processor);
		} else {
			U_LOG_W("multi: output surface back — resuming DP (#563)");
			xrt_display_processor_on_resume(mc->session_render.display_processor);
		}
	}
}
#endif // XRT_OS_ANDROID

static int
multi_main_loop(struct multi_system_compositor *msc)
{
	U_TRACE_SET_THREAD_NAME("Multi Client Module");
	os_thread_helper_name(&msc->oth, "Multi Client Module");

#ifdef XRT_OS_LINUX
	// Try to raise priority of this thread.
	u_linux_try_to_set_realtime_priority_on_thread(U_LOGGING_INFO, "Multi Client Module");
#endif

	struct xrt_compositor *xc = &msc->xcn->base;

	// For wait frame.
	struct os_precise_sleeper sleeper = {0};
	os_precise_sleeper_init(&sleeper);

	// Protect the thread state and the sessions state.
	os_thread_helper_lock(&msc->oth);

	while (os_thread_helper_is_running_locked(&msc->oth)) {

		// Updates msc->sessions.active depending on active client sessions.
		update_session_state_locked(msc);

		if (msc->sessions.state == MULTI_SYSTEM_STATE_STOPPED) {
			// Sleep and wait to be signaled.
			os_thread_helper_wait_locked(&msc->oth);

			// Loop back to running and session check.
			continue;
		}

		// Unlock the thread after the checks has been done.
		os_thread_helper_unlock(&msc->oth);

		int64_t frame_id = -1;
		int64_t wake_up_time_ns = 0;
		int64_t predicted_gpu_time_ns = 0;
		int64_t predicted_display_time_ns = 0;
		int64_t predicted_display_period_ns = 0;

		// Get the information for the next frame.
		xrt_comp_predict_frame(            //
		    xc,                            //
		    &frame_id,                     //
		    &wake_up_time_ns,              //
		    &predicted_gpu_time_ns,        //
		    &predicted_display_time_ns,    //
		    &predicted_display_period_ns); //

		// Do this as soon as we have the new display time.
		broadcast_timings_to_clients(msc, predicted_display_time_ns);

		// Now we can wait.
		wait_frame(&sleeper, xc, frame_id, wake_up_time_ns);

		int64_t now_ns = os_monotonic_get_ns();
		int64_t diff_ns = predicted_display_time_ns - now_ns;

		// Now we know the diff, broadcast to pacers.
		broadcast_timings_to_pacers(msc, predicted_display_time_ns, predicted_display_period_ns, diff_ns);

		xrt_comp_begin_frame(xc, frame_id);

		// Make sure that the clients doesn't go away while we transfer layers.
		os_mutex_lock(&msc->list_and_timing_lock);
#ifdef XRT_OS_ANDROID
		android_window_transition_locked(msc);
#endif
		transfer_layers_locked(msc, predicted_display_time_ns, frame_id);
		os_mutex_unlock(&msc->list_and_timing_lock);

		xrt_comp_layer_commit(xc, XRT_GRAPHICS_SYNC_HANDLE_INVALID);

		// Render per-session clients to their own targets (Phase 4)
		// These sessions were skipped in transfer_layers_locked and render separately
		os_mutex_lock(&msc->list_and_timing_lock);
#ifdef XRT_OS_MACOS
		// Shared spatial surface (#59): composite every client into ONE
		// full-screen window + combined atlas → one weave → one present. This is
		// the only macOS service render path (the legacy per-NSWindow path is gone).
		render_shared_surface_locked(msc, predicted_display_time_ns);
#else
		render_per_session_clients_locked(msc, predicted_display_time_ns);
#endif
		os_mutex_unlock(&msc->list_and_timing_lock);

		// Re-lock the thread for check in while statement.
		os_thread_helper_lock(&msc->oth);
	}

	// Clean up the sessions state.
	switch (msc->sessions.state) {
	case MULTI_SYSTEM_STATE_RUNNING:
	case MULTI_SYSTEM_STATE_STOPPING:
		U_LOG_I("Stopped native session, shutting down.");
		xrt_comp_end_session(xc);
		break;
	case MULTI_SYSTEM_STATE_STOPPED: break;
	default: assert(false);
	}

	os_thread_helper_unlock(&msc->oth);

	os_precise_sleeper_deinit(&sleeper);

	return 0;
}

static void *
thread_func(void *ptr)
{
	return (void *)(intptr_t)multi_main_loop((struct multi_system_compositor *)ptr);
}


/*
 *
 * System multi compositor functions.
 *
 */

static xrt_result_t
system_compositor_set_state(struct xrt_system_compositor *xsc, struct xrt_compositor *xc, bool visible, bool focused)
{
	struct multi_system_compositor *msc = multi_system_compositor(xsc);
	struct multi_compositor *mc = multi_compositor(xc);
	(void)msc;

	//! @todo Locking?
	if (mc->state.visible != visible || mc->state.focused != focused) {
		mc->state.visible = visible;
		mc->state.focused = focused;

		union xrt_session_event xse = XRT_STRUCT_INIT;
		xse.type = XRT_SESSION_EVENT_STATE_CHANGE;
		xse.state.visible = visible;
		xse.state.focused = focused;

		return multi_compositor_push_event(mc, &xse);
	}

	return XRT_SUCCESS;
}

static xrt_result_t
system_compositor_set_z_order(struct xrt_system_compositor *xsc, struct xrt_compositor *xc, int64_t z_order)
{
	struct multi_system_compositor *msc = multi_system_compositor(xsc);
	struct multi_compositor *mc = multi_compositor(xc);
	(void)msc;

	//! @todo Locking?
	mc->state.z_order = z_order;

	return XRT_SUCCESS;
}

static xrt_result_t
system_compositor_set_main_app_visibility(struct xrt_system_compositor *xsc, struct xrt_compositor *xc, bool visible)
{
	struct multi_system_compositor *msc = multi_system_compositor(xsc);
	struct multi_compositor *mc = multi_compositor(xc);
	(void)msc;

	union xrt_session_event xse = XRT_STRUCT_INIT;
	xse.type = XRT_SESSION_EVENT_OVERLAY_CHANGE;
	xse.overlay.visible = visible;

	return multi_compositor_push_event(mc, &xse);
}

static xrt_result_t
system_compositor_notify_loss_pending(struct xrt_system_compositor *xsc,
                                      struct xrt_compositor *xc,
                                      int64_t loss_time_ns)
{
	struct multi_system_compositor *msc = multi_system_compositor(xsc);
	struct multi_compositor *mc = multi_compositor(xc);
	(void)msc;

	union xrt_session_event xse = XRT_STRUCT_INIT;
	xse.type = XRT_SESSION_EVENT_LOSS_PENDING;
	xse.loss_pending.loss_time_ns = loss_time_ns;

	return multi_compositor_push_event(mc, &xse);
}

static xrt_result_t
system_compositor_notify_lost(struct xrt_system_compositor *xsc, struct xrt_compositor *xc)
{
	struct multi_system_compositor *msc = multi_system_compositor(xsc);
	struct multi_compositor *mc = multi_compositor(xc);
	(void)msc;

	union xrt_session_event xse = XRT_STRUCT_INIT;
	xse.type = XRT_SESSION_EVENT_LOST;

	return multi_compositor_push_event(mc, &xse);
}

static xrt_result_t
system_compositor_notify_display_refresh_changed(struct xrt_system_compositor *xsc,
                                                 struct xrt_compositor *xc,
                                                 float from_display_refresh_rate_hz,
                                                 float to_display_refresh_rate_hz)
{
	struct multi_system_compositor *msc = multi_system_compositor(xsc);
	struct multi_compositor *mc = multi_compositor(xc);
	(void)msc;

	union xrt_session_event xse = XRT_STRUCT_INIT;
	xse.type = XRT_SESSION_EVENT_DISPLAY_REFRESH_RATE_CHANGE;
	xse.display.from_display_refresh_rate_hz = from_display_refresh_rate_hz;
	xse.display.to_display_refresh_rate_hz = to_display_refresh_rate_hz;

	return multi_compositor_push_event(mc, &xse);
}


/*
 *
 * System compositor functions.
 *
 */

static xrt_result_t
system_compositor_create_native_compositor(struct xrt_system_compositor *xsc,
                                           const struct xrt_session_info *xsi,
                                           struct xrt_session_event_sink *xses,
                                           struct xrt_compositor_native **out_xcn)
{
	struct multi_system_compositor *msc = multi_system_compositor(xsc);

	return multi_compositor_create(msc, xsi, xses, out_xcn);
}

static void
system_compositor_destroy(struct xrt_system_compositor *xsc)
{
	struct multi_system_compositor *msc = multi_system_compositor(xsc);

	// Destroy the render thread first, destroy also stops the thread.
	os_thread_helper_destroy(&msc->oth);

#ifdef XRT_OS_MACOS
	// Free the shared spatial surface resources (#59) now that the render thread
	// is stopped but BEFORE the native compositor (which owns the Vulkan device)
	// and the target service go away.
	shared_surface_fini(msc);
#endif

	u_paf_destroy(&msc->upaf);

	xrt_comp_native_destroy(&msc->xcn);

	os_mutex_destroy(&msc->list_and_timing_lock);

	free(msc);
}


/*
 *
 * 'Exported' functions.
 *
 */

void
multi_system_compositor_update_session_status(struct multi_system_compositor *msc, bool active)
{
	os_thread_helper_lock(&msc->oth);

	if (active) {
		assert(msc->sessions.active_count < UINT32_MAX);
		msc->sessions.active_count++;

		// If the thread is sleeping wake it up.
		os_thread_helper_signal_locked(&msc->oth);
	} else {
		assert(msc->sessions.active_count > 0);
		msc->sessions.active_count--;
	}

	os_thread_helper_unlock(&msc->oth);
}

#ifdef XRT_OS_MACOS
void
comp_multi_system_set_workspace_active(struct xrt_system_compositor *xsc, bool active)
{
	if (xsc == NULL) {
		return;
	}
	struct multi_system_compositor *msc = multi_system_compositor(xsc);
	os_thread_helper_lock(&msc->oth);
	msc->workspace_active = active;
	// Wake the render thread so it (re)starts the shared-surface session even with
	// no active content app (#61: empty spatial desktop + DXR splash + launcher).
	os_thread_helper_signal_locked(&msc->oth);
	os_thread_helper_unlock(&msc->oth);
}
#endif

xrt_result_t
comp_multi_create_system_compositor(struct xrt_compositor_native *xcn,
                                    struct u_pacing_app_factory *upaf,
                                    const struct xrt_system_compositor_info *xsci,
                                    bool do_warm_start,
                                    struct comp_target_service *target_service,
                                    struct xrt_system_compositor **out_xsysc)
{
	struct multi_system_compositor *msc = U_TYPED_CALLOC(struct multi_system_compositor);
#ifdef XRT_OS_ANDROID
	msc->android_window_valid_state = -1; /* #563: unknown until first loop tick */
#endif
	msc->base.create_native_compositor = system_compositor_create_native_compositor;
	msc->base.destroy = system_compositor_destroy;
	msc->xmcc.set_state = system_compositor_set_state;
	msc->xmcc.set_z_order = system_compositor_set_z_order;
	msc->xmcc.set_main_app_visibility = system_compositor_set_main_app_visibility;
	msc->xmcc.notify_loss_pending = system_compositor_notify_loss_pending;
	msc->xmcc.notify_lost = system_compositor_notify_lost;
	msc->xmcc.notify_display_refresh_changed = system_compositor_notify_display_refresh_changed;
	msc->base.xmcc = &msc->xmcc;
	msc->base.info = *xsci;
	msc->upaf = upaf;
	msc->xcn = xcn;

	// Store the target service for per-session rendering (Phase 3)
	msc->target_service = target_service;

#ifdef XRT_OS_MACOS
	// Shared spatial surface (#59) is the only macOS service render path: every
	// client app composites into ONE full-screen window as a 3D spatial window
	// (the macOS analogue of the Windows D3D11 monolith), instead of one NSWindow
	// per app. The legacy per-NSWindow path has been removed.
	msc->shared_fenced_buffer = -1;
	U_LOG_W("[#59] shared spatial surface (single full-screen window)");
#endif

	msc->sessions.active_count = 0;
	msc->sessions.state = do_warm_start ? MULTI_SYSTEM_STATE_INIT_WARM_START : MULTI_SYSTEM_STATE_STOPPED;

	os_mutex_init(&msc->list_and_timing_lock);

	//! @todo Make the clients not go from IDLE to READY before we have completed a first frame.
	// Make sure there is at least some sort of valid frame data here.
	msc->last_timings.predicted_display_time_ns = os_monotonic_get_ns();   // As good as any time.
	msc->last_timings.predicted_display_period_ns = U_TIME_1MS_IN_NS * 16; // Just a wild guess.
	msc->last_timings.diff_ns = U_TIME_1MS_IN_NS * 5;                      // Make sure it's not zero at least.

	int ret = os_thread_helper_init(&msc->oth);
	if (ret < 0) {
		return XRT_ERROR_THREADING_INIT_FAILURE;
	}

	os_thread_helper_start(&msc->oth, thread_func, msc);

	*out_xsysc = &msc->base;

	return XRT_SUCCESS;
}
