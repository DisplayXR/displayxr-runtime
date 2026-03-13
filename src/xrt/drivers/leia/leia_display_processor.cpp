// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Leia display processor: wraps SR SDK Vulkan weaver
 *         as an @ref xrt_display_processor.
 *
 * The display processor owns the leiasr handle — it creates it
 * via the factory function and destroys it on cleanup.
 *
 * The SR SDK weaver expects side-by-side (SBS) stereo input. When the
 * compositor's atlas uses a different tiling layout (e.g. vertical stacking
 * with tile_columns=1, tile_rows=2), this DP rearranges the atlas into
 * SBS format via vkCmdBlitImage before passing to the weaver.
 *
 * @author David Fattal
 * @ingroup drv_leia
 */

#include "leia_display_processor.h"
#include "leia_sr.h"

#include "xrt/xrt_display_metrics.h"
#include "vk/vk_helpers.h"
#include "util/u_logging.h"
#include <cstdint>
#include <cstdlib>


/*!
 * Implementation struct wrapping leiasr as xrt_display_processor.
 */
struct leia_display_processor
{
	struct xrt_display_processor base;
	struct leiasr *leiasr; //!< Owned — destroyed in leia_dp_destroy.
	struct vk_bundle *vk;  //!< Cached vk_bundle (not owned).

	//! @name SBS staging resources for non-SBS atlas layouts
	//! @{
	VkImage sbs_image;          //!< Staging SBS image (lazy-created).
	VkDeviceMemory sbs_memory;  //!< Memory for staging image.
	VkImageView sbs_view;       //!< View for staging image.
	uint32_t sbs_width;         //!< Current staging image width.
	uint32_t sbs_height;        //!< Current staging image height.
	VkFormat sbs_format;        //!< Current staging image format.
	//! @}
};

static inline struct leia_display_processor *
leia_display_processor(struct xrt_display_processor *xdp)
{
	return (struct leia_display_processor *)xdp;
}


/*!
 * Ensure the SBS staging image exists with the right dimensions/format.
 */
static bool
ensure_sbs_staging_vk(struct leia_display_processor *ldp,
                      uint32_t view_width,
                      uint32_t view_height,
                      VkFormat format)
{
	uint32_t sbs_w = 2 * view_width;
	uint32_t sbs_h = view_height;
	struct vk_bundle *vk = ldp->vk;

	if (vk == NULL) {
		return false;
	}

	if (ldp->sbs_image != VK_NULL_HANDLE && ldp->sbs_width == sbs_w &&
	    ldp->sbs_height == sbs_h && ldp->sbs_format == format) {
		return true;
	}

	// Destroy old resources
	if (ldp->sbs_view != VK_NULL_HANDLE) {
		vk->vkDestroyImageView(vk->device, ldp->sbs_view, NULL);
		ldp->sbs_view = VK_NULL_HANDLE;
	}
	if (ldp->sbs_image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, ldp->sbs_image, NULL);
		ldp->sbs_image = VK_NULL_HANDLE;
	}
	if (ldp->sbs_memory != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, ldp->sbs_memory, NULL);
		ldp->sbs_memory = VK_NULL_HANDLE;
	}

	// Create SBS image
	VkExtent2D extent = {sbs_w, sbs_h};
	VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

	VkResult res = vk_create_image_simple(vk, extent, format, usage,
	                                      &ldp->sbs_memory, &ldp->sbs_image);
	if (res != VK_SUCCESS) {
		U_LOG_E("Leia VK DP: failed to create SBS staging image (%ux%u): %d",
		        sbs_w, sbs_h, res);
		return false;
	}

	// Create image view
	VkImageSubresourceRange range = {
	    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	    .baseMipLevel = 0,
	    .levelCount = 1,
	    .baseArrayLayer = 0,
	    .layerCount = 1,
	};
	res = vk_create_view(vk, ldp->sbs_image, VK_IMAGE_VIEW_TYPE_2D, format, range,
	                     &ldp->sbs_view);
	if (res != VK_SUCCESS) {
		U_LOG_E("Leia VK DP: failed to create SBS staging view: %d", res);
		vk->vkDestroyImage(vk->device, ldp->sbs_image, NULL);
		ldp->sbs_image = VK_NULL_HANDLE;
		vk->vkFreeMemory(vk->device, ldp->sbs_memory, NULL);
		ldp->sbs_memory = VK_NULL_HANDLE;
		return false;
	}

	ldp->sbs_width = sbs_w;
	ldp->sbs_height = sbs_h;
	ldp->sbs_format = format;

	U_LOG_I("Leia VK DP: created SBS staging image %ux%u", sbs_w, sbs_h);
	return true;
}


/*
 *
 * xrt_display_processor interface methods.
 *
 */

static void
leia_dp_process_atlas(struct xrt_display_processor *xdp,
                      VkCommandBuffer cmd_buffer,
                      VkImage_XDP atlas_image,
                      VkImageView atlas_view,
                      uint32_t view_width,
                      uint32_t view_height,
                      uint32_t tile_columns,
                      uint32_t tile_rows,
                      VkFormat_XDP view_format,
                      VkFramebuffer target_fb,
                      uint32_t target_width,
                      uint32_t target_height,
                      VkFormat_XDP target_format)
{
	struct leia_display_processor *ldp = leia_display_processor(xdp);
	struct vk_bundle *vk = ldp->vk;

	VkImageView weaver_view = atlas_view;

	// If atlas is already SBS (tile_columns=2, tile_rows=1), pass directly.
	// Otherwise, rearrange to SBS via vkCmdBlitImage.
	if (tile_columns != 2 || tile_rows != 1) {
		VkFormat vk_format = (VkFormat)view_format;
		if (!ensure_sbs_staging_vk(ldp, view_width, view_height, vk_format) ||
		    atlas_image == (VkImage_XDP)VK_NULL_HANDLE) {
			goto do_weave;
		}

		// Transition atlas from SHADER_READ to TRANSFER_SRC
		VkImageMemoryBarrier atlas_to_src = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
		    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		    .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		    .image = (VkImage)atlas_image,
		    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		};
		// Transition SBS staging to TRANSFER_DST
		VkImageMemoryBarrier sbs_to_dst = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = 0,
		    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		    .image = ldp->sbs_image,
		    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		};
		VkImageMemoryBarrier pre_barriers[2] = {atlas_to_src, sbs_to_dst};
		vk->vkCmdPipelineBarrier(cmd_buffer,
		    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		    VK_PIPELINE_STAGE_TRANSFER_BIT,
		    0, 0, NULL, 0, NULL, 2, pre_barriers);

		// Blit each view from tiled position to SBS position
		for (uint32_t i = 0; i < 2; i++) {
			int32_t src_x = (int32_t)((i % tile_columns) * view_width);
			int32_t src_y = (int32_t)((i / tile_columns) * view_height);
			int32_t dst_x = (int32_t)(i * view_width);

			VkImageBlit blit = {
			    .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
			    .srcOffsets = {{src_x, src_y, 0},
			                   {src_x + (int32_t)view_width, src_y + (int32_t)view_height, 1}},
			    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
			    .dstOffsets = {{dst_x, 0, 0},
			                   {dst_x + (int32_t)view_width, (int32_t)view_height, 1}},
			};
			vk->vkCmdBlitImage(cmd_buffer,
			    (VkImage)atlas_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			    ldp->sbs_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			    1, &blit, VK_FILTER_NEAREST);
		}

		// Transition atlas back to SHADER_READ
		VkImageMemoryBarrier atlas_back = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
		    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		    .image = (VkImage)atlas_image,
		    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		};
		// Transition SBS staging to SHADER_READ
		VkImageMemoryBarrier sbs_to_read = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
		    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		    .image = ldp->sbs_image,
		    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		};
		VkImageMemoryBarrier post_barriers[2] = {atlas_back, sbs_to_read};
		vk->vkCmdPipelineBarrier(cmd_buffer,
		    VK_PIPELINE_STAGE_TRANSFER_BIT,
		    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		    0, 0, NULL, 0, NULL, 2, post_barriers);

		weaver_view = ldp->sbs_view;
	}

do_weave:;
	// Build a fullscreen viewport from target dimensions.
	VkRect2D viewport = {};
	viewport.offset.x = 0;
	viewport.offset.y = 0;
	viewport.extent.width = target_width;
	viewport.extent.height = target_height;

	// SR weaver expects SBS atlas as left_view, VK_NULL_HANDLE as right
	leiasr_weave(ldp->leiasr,
	             cmd_buffer,
	             weaver_view,
	             (VkImageView)VK_NULL_HANDLE,
	             viewport,
	             (int)view_width,
	             (int)view_height,
	             (VkFormat)view_format,
	             target_fb,
	             (int)target_width,
	             (int)target_height,
	             (VkFormat)target_format);
}

static bool
leia_dp_get_predicted_eye_positions(struct xrt_display_processor *xdp, struct xrt_eye_pair *out_eye_pair)
{
	struct leia_display_processor *ldp = leia_display_processor(xdp);
	// leiasr_eye_pair is #defined to xrt_eye_pair in leia_types.h
	return leiasr_get_predicted_eye_positions(ldp->leiasr, (struct leiasr_eye_pair *)out_eye_pair);
}

static bool
leia_dp_get_window_metrics(struct xrt_display_processor *xdp, struct xrt_window_metrics *out_metrics)
{
	struct leia_display_processor *ldp = leia_display_processor(xdp);
	// leiasr_window_metrics is #defined to xrt_window_metrics in leia_types.h
	return leiasr_get_window_metrics(ldp->leiasr, (struct leiasr_window_metrics *)out_metrics);
}

static bool
leia_dp_request_display_mode(struct xrt_display_processor *xdp, bool enable_3d)
{
	struct leia_display_processor *ldp = leia_display_processor(xdp);
	return leiasr_request_display_mode(ldp->leiasr, enable_3d);
}

static bool
leia_dp_get_display_dimensions(struct xrt_display_processor *xdp, float *out_width_m, float *out_height_m)
{
	struct leia_display_processor *ldp = leia_display_processor(xdp);
	struct leiasr_display_dimensions dims = {};
	if (!leiasr_get_display_dimensions(ldp->leiasr, &dims) || !dims.valid) {
		return false;
	}
	*out_width_m = dims.width_m;
	*out_height_m = dims.height_m;
	return true;
}

static bool
leia_dp_get_display_pixel_info(struct xrt_display_processor *xdp,
                               uint32_t *out_pixel_width,
                               uint32_t *out_pixel_height,
                               int32_t *out_screen_left,
                               int32_t *out_screen_top)
{
	struct leia_display_processor *ldp = leia_display_processor(xdp);
	float w_m, h_m; // unused but required by API
	return leiasr_get_display_pixel_info(ldp->leiasr, out_pixel_width, out_pixel_height, out_screen_left,
	                                     out_screen_top, &w_m, &h_m);
}

static void
leia_dp_destroy(struct xrt_display_processor *xdp)
{
	struct leia_display_processor *ldp = leia_display_processor(xdp);
	struct vk_bundle *vk = ldp->vk;

	if (vk != NULL) {
		if (ldp->sbs_view != VK_NULL_HANDLE) {
			vk->vkDestroyImageView(vk->device, ldp->sbs_view, NULL);
		}
		if (ldp->sbs_image != VK_NULL_HANDLE) {
			vk->vkDestroyImage(vk->device, ldp->sbs_image, NULL);
		}
		if (ldp->sbs_memory != VK_NULL_HANDLE) {
			vk->vkFreeMemory(vk->device, ldp->sbs_memory, NULL);
		}
	}

	leiasr_destroy(ldp->leiasr);
	free(ldp);
}


/*
 *
 * Factory function — matches xrt_dp_factory_vk_fn_t signature.
 *
 */

extern "C" xrt_result_t
leia_dp_factory_vk(void *vk_bundle_ptr,
                   void *vk_cmd_pool,
                   void *window_handle,
                   int32_t target_format,
                   struct xrt_display_processor **out_xdp)
{
	(void)target_format; // unused by SR weaver

	// Extract Vulkan handles from vk_bundle.
	struct vk_bundle *vk = (struct vk_bundle *)vk_bundle_ptr;

	struct leiasr *leiasr = NULL;
	xrt_result_t ret = leiasr_create(5.0, vk->device, vk->physical_device, vk->main_queue->queue,
	                                 (VkCommandPool)(uintptr_t)vk_cmd_pool, window_handle, &leiasr);
	if (ret != XRT_SUCCESS || leiasr == NULL) {
		U_LOG_W("Failed to create SR Vulkan weaver, continuing without interlacing");
		return ret != XRT_SUCCESS ? ret : XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	struct leia_display_processor *ldp = (struct leia_display_processor *)calloc(1, sizeof(*ldp));
	if (ldp == NULL) {
		leiasr_destroy(leiasr);
		return XRT_ERROR_ALLOCATION;
	}

	ldp->base.process_atlas = leia_dp_process_atlas;
	ldp->base.get_predicted_eye_positions = leia_dp_get_predicted_eye_positions;
	ldp->base.get_window_metrics = leia_dp_get_window_metrics;
	ldp->base.request_display_mode = leia_dp_request_display_mode;
	ldp->base.get_display_dimensions = leia_dp_get_display_dimensions;
	ldp->base.get_display_pixel_info = leia_dp_get_display_pixel_info;
	ldp->base.destroy = leia_dp_destroy;

	ldp->leiasr = leiasr;
	ldp->vk = vk;

	*out_xdp = &ldp->base;

	U_LOG_W("Created Leia SR display processor (factory, owns weaver)");

	return XRT_SUCCESS;
}


/*
 *
 * Legacy creation function — wraps an existing leiasr handle.
 * Kept for backward compatibility during the refactoring transition.
 *
 */

extern "C" xrt_result_t
leia_display_processor_create(struct leiasr *leiasr, struct xrt_display_processor **out_xdp)
{
	if (leiasr == NULL || out_xdp == NULL) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	struct leia_display_processor *ldp = (struct leia_display_processor *)calloc(1, sizeof(*ldp));
	if (ldp == NULL) {
		return XRT_ERROR_ALLOCATION;
	}

	ldp->base.process_atlas = leia_dp_process_atlas;
	ldp->base.get_predicted_eye_positions = leia_dp_get_predicted_eye_positions;
	ldp->base.get_window_metrics = leia_dp_get_window_metrics;
	ldp->base.request_display_mode = leia_dp_request_display_mode;
	ldp->base.get_display_dimensions = leia_dp_get_display_dimensions;
	ldp->base.get_display_pixel_info = leia_dp_get_display_pixel_info;
	// Legacy: does NOT own leiasr — use a destroy that skips leiasr_destroy.
	// For now just assign the full destroy; callers will be migrated to factory.
	ldp->base.destroy = leia_dp_destroy;

	ldp->leiasr = leiasr;

	*out_xdp = &ldp->base;

	U_LOG_W("Created Leia SR display processor (legacy, owns weaver)");

	return XRT_SUCCESS;
}
