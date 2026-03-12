// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Vulkan renderer for layer compositing.
 * @author David Fattal
 * @ingroup comp_vk_native
 *
 * Creates a side-by-side stereo texture and copies/blits app swapchain
 * content into left/right eye regions. The stereo texture is then consumed
 * by the display processor (weaver) or blitted to the target for 2D fallback.
 *
 * Uses vkCmdBlitImage for simplicity — no render pass or pipeline needed.
 */

#include "comp_vk_native_renderer.h"
#include "comp_vk_native_compositor.h"
#include "comp_vk_native_swapchain.h"

#include "util/comp_layer_accum.h"

#include "xrt/xrt_vulkan_includes.h"
#include "vk/vk_helpers.h"

#include "util/u_logging.h"
#include "util/u_misc.h"

#include <string.h>
#include <math.h>

/*!
 * Vulkan renderer structure.
 */
struct comp_vk_native_renderer
{
	//! Vulkan bundle (borrowed).
	struct vk_bundle *vk;

	//! Command pool for recording blit commands.
	VkCommandPool cmd_pool;

	//! Side-by-side stereo texture (2*view_width x texture_height).
	VkImage stereo_image;

	//! Memory for stereo texture.
	VkDeviceMemory stereo_memory;

	//! Full image view for the stereo texture (used for SBS fallback blit).
	VkImageView stereo_view;

	//! Per-eye images for display processors that expect separate views.
	VkImage left_image;
	VkImage right_image;
	VkDeviceMemory left_memory;
	VkDeviceMemory right_memory;

	//! Per-eye image views (each is view_width x texture_height).
	VkImageView left_view;
	VkImageView right_view;

	//! Width per view.
	uint32_t view_width;

	//! Height per view.
	uint32_t view_height;

	//! Actual texture height (max of view_height and target_height).
	uint32_t texture_height;

	//! Format of the stereo texture.
	VkFormat format;
};

static void
destroy_stereo_resources(struct comp_vk_native_renderer *r)
{
	struct vk_bundle *vk = r->vk;

	if (r->left_view != VK_NULL_HANDLE) {
		vk->vkDestroyImageView(vk->device, r->left_view, NULL);
		r->left_view = VK_NULL_HANDLE;
	}
	if (r->right_view != VK_NULL_HANDLE) {
		vk->vkDestroyImageView(vk->device, r->right_view, NULL);
		r->right_view = VK_NULL_HANDLE;
	}
	if (r->stereo_view != VK_NULL_HANDLE) {
		vk->vkDestroyImageView(vk->device, r->stereo_view, NULL);
		r->stereo_view = VK_NULL_HANDLE;
	}
	if (r->stereo_image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, r->stereo_image, NULL);
		r->stereo_image = VK_NULL_HANDLE;
	}
	if (r->stereo_memory != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, r->stereo_memory, NULL);
		r->stereo_memory = VK_NULL_HANDLE;
	}
	if (r->left_image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, r->left_image, NULL);
		r->left_image = VK_NULL_HANDLE;
	}
	if (r->right_image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, r->right_image, NULL);
		r->right_image = VK_NULL_HANDLE;
	}
	if (r->left_memory != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, r->left_memory, NULL);
		r->left_memory = VK_NULL_HANDLE;
	}
	if (r->right_memory != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, r->right_memory, NULL);
		r->right_memory = VK_NULL_HANDLE;
	}
}

static xrt_result_t
create_stereo_resources(struct comp_vk_native_renderer *r,
                         uint32_t view_width,
                         uint32_t view_height,
                         uint32_t target_height)
{
	struct vk_bundle *vk = r->vk;

	r->view_width = view_width;
	r->view_height = view_height;
	r->texture_height = view_height > target_height ? view_height : target_height;

	uint32_t stereo_width = view_width * 2;

	VkImageCreateInfo image_ci = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .imageType = VK_IMAGE_TYPE_2D,
	    .format = r->format,
	    .extent = {stereo_width, r->texture_height, 1},
	    .mipLevels = 1,
	    .arrayLayers = 1,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .tiling = VK_IMAGE_TILING_OPTIMAL,
	    .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
	             VK_IMAGE_USAGE_SAMPLED_BIT |
	             VK_IMAGE_USAGE_TRANSFER_DST_BIT |
	             VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};

	VkResult res = vk->vkCreateImage(vk->device, &image_ci, NULL, &r->stereo_image);
	if (res != VK_SUCCESS) {
		U_LOG_E("Failed to create stereo image: %d", res);
		return XRT_ERROR_VULKAN;
	}

	VkMemoryRequirements mem_reqs;
	vk->vkGetImageMemoryRequirements(vk->device, r->stereo_image, &mem_reqs);

	// Find device-local memory type
	uint32_t mem_type_index = 0;
	VkPhysicalDeviceMemoryProperties mem_props;
	vk->vkGetPhysicalDeviceMemoryProperties(vk->physical_device, &mem_props);
	for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
		if ((mem_reqs.memoryTypeBits & (1 << i)) &&
		    (mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
			mem_type_index = i;
			break;
		}
	}

	VkMemoryAllocateInfo alloc_info = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .allocationSize = mem_reqs.size,
	    .memoryTypeIndex = mem_type_index,
	};

	res = vk->vkAllocateMemory(vk->device, &alloc_info, NULL, &r->stereo_memory);
	if (res != VK_SUCCESS) {
		U_LOG_E("Failed to allocate stereo memory: %d", res);
		return XRT_ERROR_VULKAN;
	}

	res = vk->vkBindImageMemory(vk->device, r->stereo_image, r->stereo_memory, 0);
	if (res != VK_SUCCESS) {
		U_LOG_E("Failed to bind stereo memory: %d", res);
		return XRT_ERROR_VULKAN;
	}

	VkImageViewCreateInfo view_ci = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
	    .image = r->stereo_image,
	    .viewType = VK_IMAGE_VIEW_TYPE_2D,
	    .format = r->format,
	    .subresourceRange = {
	        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	        .baseMipLevel = 0,
	        .levelCount = 1,
	        .baseArrayLayer = 0,
	        .layerCount = 1,
	    },
	};

	res = vk->vkCreateImageView(vk->device, &view_ci, NULL, &r->stereo_view);
	if (res != VK_SUCCESS) {
		U_LOG_E("Failed to create stereo view: %d", res);
		return XRT_ERROR_VULKAN;
	}

	// Create separate per-eye images for display processors that expect separate views.
	// VkImageView can't select a horizontal subregion of a 2D image, so we need
	// separate images that get blitted from the SBS stereo texture.
	VkImageCreateInfo eye_image_ci = image_ci;
	eye_image_ci.extent.width = view_width;
	eye_image_ci.extent.height = r->texture_height;

	VkImage eye_images[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
	VkDeviceMemory eye_memories[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
	VkImageView eye_views[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};

	for (int eye = 0; eye < 2; eye++) {
		res = vk->vkCreateImage(vk->device, &eye_image_ci, NULL, &eye_images[eye]);
		if (res != VK_SUCCESS) {
			U_LOG_E("Failed to create eye %d image: %d", eye, res);
			return XRT_ERROR_VULKAN;
		}

		VkMemoryRequirements eye_mem_reqs;
		vk->vkGetImageMemoryRequirements(vk->device, eye_images[eye], &eye_mem_reqs);

		VkMemoryAllocateInfo eye_alloc = {
		    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		    .allocationSize = eye_mem_reqs.size,
		    .memoryTypeIndex = mem_type_index,
		};

		res = vk->vkAllocateMemory(vk->device, &eye_alloc, NULL, &eye_memories[eye]);
		if (res != VK_SUCCESS) {
			U_LOG_E("Failed to allocate eye %d memory: %d", eye, res);
			return XRT_ERROR_VULKAN;
		}

		res = vk->vkBindImageMemory(vk->device, eye_images[eye], eye_memories[eye], 0);
		if (res != VK_SUCCESS) {
			U_LOG_E("Failed to bind eye %d memory: %d", eye, res);
			return XRT_ERROR_VULKAN;
		}

		VkImageViewCreateInfo eye_view_ci = view_ci;
		eye_view_ci.image = eye_images[eye];

		res = vk->vkCreateImageView(vk->device, &eye_view_ci, NULL, &eye_views[eye]);
		if (res != VK_SUCCESS) {
			U_LOG_E("Failed to create eye %d view: %d", eye, res);
			return XRT_ERROR_VULKAN;
		}
	}

	r->left_image = eye_images[0];
	r->right_image = eye_images[1];
	r->left_memory = eye_memories[0];
	r->right_memory = eye_memories[1];
	r->left_view = eye_views[0];
	r->right_view = eye_views[1];

	U_LOG_I("Created stereo texture: %ux%u (view %ux%u)", stereo_width, r->texture_height,
	        view_width, view_height);

	return XRT_SUCCESS;
}

xrt_result_t
comp_vk_native_renderer_create(struct comp_vk_native_compositor *c,
                                uint32_t view_width,
                                uint32_t view_height,
                                uint32_t target_height,
                                struct comp_vk_native_renderer **out_renderer)
{
	struct vk_bundle *vk = comp_vk_native_compositor_get_vk(c);
	uint32_t queue_family_index = comp_vk_native_compositor_get_queue_family(c);

	struct comp_vk_native_renderer *r = U_TYPED_CALLOC(struct comp_vk_native_renderer);
	if (r == NULL) {
		return XRT_ERROR_ALLOCATION;
	}

	r->vk = vk;
	r->format = VK_FORMAT_R8G8B8A8_UNORM;

	VkCommandPoolCreateInfo pool_ci = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
	    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
	    .queueFamilyIndex = queue_family_index,
	};

	VkResult res = vk->vkCreateCommandPool(vk->device, &pool_ci, NULL, &r->cmd_pool);
	if (res != VK_SUCCESS) {
		U_LOG_E("Failed to create command pool: %d", res);
		free(r);
		return XRT_ERROR_VULKAN;
	}

	xrt_result_t xret = create_stereo_resources(r, view_width, view_height, target_height);
	if (xret != XRT_SUCCESS) {
		vk->vkDestroyCommandPool(vk->device, r->cmd_pool, NULL);
		free(r);
		return xret;
	}

	*out_renderer = r;
	return XRT_SUCCESS;
}

void
comp_vk_native_renderer_destroy(struct comp_vk_native_renderer **renderer_ptr)
{
	if (renderer_ptr == NULL || *renderer_ptr == NULL) {
		return;
	}

	struct comp_vk_native_renderer *r = *renderer_ptr;
	struct vk_bundle *vk = r->vk;

	vk->vkDeviceWaitIdle(vk->device);

	destroy_stereo_resources(r);

	if (r->cmd_pool != VK_NULL_HANDLE) {
		vk->vkDestroyCommandPool(vk->device, r->cmd_pool, NULL);
	}

	free(r);
	*renderer_ptr = NULL;
}

static void
cmd_image_barrier(struct vk_bundle *vk,
                   VkCommandBuffer cmd,
                   VkImage image,
                   VkImageLayout old_layout,
                   VkImageLayout new_layout,
                   VkAccessFlags src_access,
                   VkAccessFlags dst_access,
                   VkPipelineStageFlags src_stage,
                   VkPipelineStageFlags dst_stage)
{
	VkImageMemoryBarrier barrier = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = src_access,
	    .dstAccessMask = dst_access,
	    .oldLayout = old_layout,
	    .newLayout = new_layout,
	    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .image = image,
	    .subresourceRange = {
	        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	        .baseMipLevel = 0,
	        .levelCount = 1,
	        .baseArrayLayer = 0,
	        .layerCount = 1,
	    },
	};

	vk->vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0,
	                          0, NULL, 0, NULL, 1, &barrier);
}

xrt_result_t
comp_vk_native_renderer_draw(struct comp_vk_native_renderer *r,
                              struct comp_layer_accum *layers,
                              struct xrt_vec3 *left_eye,
                              struct xrt_vec3 *right_eye,
                              uint32_t target_width,
                              uint32_t target_height,
                              bool force_mono)
{
	struct vk_bundle *vk = r->vk;
	(void)left_eye;
	(void)right_eye;

	VkCommandBufferAllocateInfo alloc_info = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	    .commandPool = r->cmd_pool,
	    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
	    .commandBufferCount = 1,
	};

	VkCommandBuffer cmd;
	VkResult res = vk->vkAllocateCommandBuffers(vk->device, &alloc_info, &cmd);
	if (res != VK_SUCCESS) {
		U_LOG_E("Failed to allocate command buffer: %d", res);
		return XRT_ERROR_VULKAN;
	}

	VkCommandBufferBeginInfo begin_info = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	vk->vkBeginCommandBuffer(cmd, &begin_info);

	// Transition stereo image to transfer dst
	cmd_image_barrier(vk, cmd, r->stereo_image,
	                   VK_IMAGE_LAYOUT_UNDEFINED,
	                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                   0, VK_ACCESS_TRANSFER_WRITE_BIT,
	                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	                   VK_PIPELINE_STAGE_TRANSFER_BIT);

	// Clear stereo texture to black
	VkClearColorValue clear_color = {{0.0f, 0.0f, 0.0f, 1.0f}};
	VkImageSubresourceRange range = {
	    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	    .baseMipLevel = 0,
	    .levelCount = 1,
	    .baseArrayLayer = 0,
	    .layerCount = 1,
	};
	vk->vkCmdClearColorImage(cmd, r->stereo_image,
	                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                          &clear_color, 1, &range);

	// Blit each projection layer into the stereo texture
	for (uint32_t i = 0; i < layers->layer_count; i++) {
		struct comp_layer *layer = &layers->layers[i];

		if (layer->data.type != XRT_LAYER_PROJECTION &&
		    layer->data.type != XRT_LAYER_PROJECTION_DEPTH) {
			continue;
		}

		uint32_t view_count = force_mono ? 1 : layer->data.view_count;
		if (view_count == 0) view_count = 1;

		for (uint32_t eye = 0; eye < view_count; eye++) {
			struct xrt_swapchain *xsc = layer->sc_array[eye];
			if (xsc == NULL) continue;

			uint32_t sc_index = layer->data.proj.v[eye].sub.image_index;
			VkImage src_image = (VkImage)(uintptr_t)comp_vk_native_swapchain_get_image(xsc, sc_index);
			if (src_image == VK_NULL_HANDLE) continue;

			cmd_image_barrier(vk, cmd, src_image,
			                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			                   VK_ACCESS_TRANSFER_READ_BIT,
			                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			                   VK_PIPELINE_STAGE_TRANSFER_BIT);

			struct xrt_rect *src_rect = &layer->data.proj.v[eye].sub.rect;
			int32_t sx0 = src_rect->offset.w;
			int32_t sy0 = src_rect->offset.h;
			int32_t sx1 = sx0 + (int32_t)src_rect->extent.w;
			int32_t sy1 = sy0 + (int32_t)src_rect->extent.h;

			int32_t dx0, dy0, dx1, dy1;
			if (force_mono || view_count == 1) {
				dx0 = 0;
				dy0 = 0;
				dx1 = (int32_t)(r->view_width * 2);
				dy1 = (int32_t)r->view_height;
			} else {
				dx0 = (int32_t)(eye * r->view_width);
				dy0 = 0;
				dx1 = dx0 + (int32_t)r->view_width;
				dy1 = (int32_t)r->view_height;
			}

			VkImageBlit blit = {
			    .srcSubresource = {
			        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			        .mipLevel = 0,
			        .baseArrayLayer = layer->data.proj.v[eye].sub.array_index,
			        .layerCount = 1,
			    },
			    .srcOffsets = {{sx0, sy0, 0}, {sx1, sy1, 1}},
			    .dstSubresource = {
			        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			        .mipLevel = 0,
			        .baseArrayLayer = 0,
			        .layerCount = 1,
			    },
			    .dstOffsets = {{dx0, dy0, 0}, {dx1, dy1, 1}},
			};

			vk->vkCmdBlitImage(cmd,
			                    src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			                    r->stereo_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			                    1, &blit, VK_FILTER_LINEAR);

			cmd_image_barrier(vk, cmd, src_image,
			                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			                   VK_ACCESS_TRANSFER_READ_BIT,
			                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			                   VK_PIPELINE_STAGE_TRANSFER_BIT,
			                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
		}
	}

	// Transition stereo image to transfer src, then blit each half to per-eye images
	cmd_image_barrier(vk, cmd, r->stereo_image,
	                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                   VK_ACCESS_TRANSFER_WRITE_BIT,
	                   VK_ACCESS_TRANSFER_READ_BIT,
	                   VK_PIPELINE_STAGE_TRANSFER_BIT,
	                   VK_PIPELINE_STAGE_TRANSFER_BIT);

	// Transition per-eye images to transfer dst
	cmd_image_barrier(vk, cmd, r->left_image,
	                   VK_IMAGE_LAYOUT_UNDEFINED,
	                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                   0,
	                   VK_ACCESS_TRANSFER_WRITE_BIT,
	                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	                   VK_PIPELINE_STAGE_TRANSFER_BIT);
	cmd_image_barrier(vk, cmd, r->right_image,
	                   VK_IMAGE_LAYOUT_UNDEFINED,
	                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                   0,
	                   VK_ACCESS_TRANSFER_WRITE_BIT,
	                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	                   VK_PIPELINE_STAGE_TRANSFER_BIT);

	// Blit left half of stereo texture to left eye image
	VkImageBlit left_blit = {
	    .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	    .srcOffsets = {{0, 0, 0}, {(int32_t)r->view_width, (int32_t)r->texture_height, 1}},
	    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	    .dstOffsets = {{0, 0, 0}, {(int32_t)r->view_width, (int32_t)r->texture_height, 1}},
	};
	vk->vkCmdBlitImage(cmd,
	                    r->stereo_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                    r->left_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                    1, &left_blit, VK_FILTER_NEAREST);

	// Blit right half of stereo texture to right eye image
	VkImageBlit right_blit = {
	    .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	    .srcOffsets = {{(int32_t)r->view_width, 0, 0}, {(int32_t)(r->view_width * 2), (int32_t)r->texture_height, 1}},
	    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	    .dstOffsets = {{0, 0, 0}, {(int32_t)r->view_width, (int32_t)r->texture_height, 1}},
	};
	vk->vkCmdBlitImage(cmd,
	                    r->stereo_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                    r->right_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                    1, &right_blit, VK_FILTER_NEAREST);

	// Transition all images to shader read for display processor
	cmd_image_barrier(vk, cmd, r->stereo_image,
	                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	                   VK_ACCESS_TRANSFER_READ_BIT,
	                   VK_ACCESS_SHADER_READ_BIT,
	                   VK_PIPELINE_STAGE_TRANSFER_BIT,
	                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
	cmd_image_barrier(vk, cmd, r->left_image,
	                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	                   VK_ACCESS_TRANSFER_WRITE_BIT,
	                   VK_ACCESS_SHADER_READ_BIT,
	                   VK_PIPELINE_STAGE_TRANSFER_BIT,
	                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
	cmd_image_barrier(vk, cmd, r->right_image,
	                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	                   VK_ACCESS_TRANSFER_WRITE_BIT,
	                   VK_ACCESS_SHADER_READ_BIT,
	                   VK_PIPELINE_STAGE_TRANSFER_BIT,
	                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

	vk->vkEndCommandBuffer(cmd);

	VkSubmitInfo submit_info = {
	    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	    .commandBufferCount = 1,
	    .pCommandBuffers = &cmd,
	};

	res = vk->vkQueueSubmit(vk->main_queue->queue, 1, &submit_info, VK_NULL_HANDLE);
	if (res != VK_SUCCESS) {
		U_LOG_E("Failed to submit renderer commands: %d", res);
		vk->vkFreeCommandBuffers(vk->device, r->cmd_pool, 1, &cmd);
		return XRT_ERROR_VULKAN;
	}

	vk->vkQueueWaitIdle(vk->main_queue->queue);
	vk->vkFreeCommandBuffers(vk->device, r->cmd_pool, 1, &cmd);

	return XRT_SUCCESS;
}

void
comp_vk_native_renderer_get_stereo_views(struct comp_vk_native_renderer *r,
                                          uint64_t *out_left_view,
                                          uint64_t *out_right_view)
{
	*out_left_view = (uint64_t)(uintptr_t)r->left_view;
	*out_right_view = (uint64_t)(uintptr_t)r->right_view;
}

uint64_t
comp_vk_native_renderer_get_stereo_image(struct comp_vk_native_renderer *r)
{
	return (uint64_t)(uintptr_t)r->stereo_image;
}

void
comp_vk_native_renderer_get_eye_images(struct comp_vk_native_renderer *r,
                                        uint64_t *out_left_image,
                                        uint64_t *out_right_image)
{
	*out_left_image = (uint64_t)(uintptr_t)r->left_image;
	*out_right_image = (uint64_t)(uintptr_t)r->right_image;
}

void
comp_vk_native_renderer_get_view_dimensions(struct comp_vk_native_renderer *r,
                                             uint32_t *out_view_width,
                                             uint32_t *out_view_height)
{
	*out_view_width = r->view_width;
	*out_view_height = r->view_height;
}

int32_t
comp_vk_native_renderer_get_format(struct comp_vk_native_renderer *r)
{
	return (int32_t)r->format;
}

xrt_result_t
comp_vk_native_renderer_resize(struct comp_vk_native_renderer *r,
                                uint32_t new_view_width,
                                uint32_t new_view_height,
                                uint32_t new_target_height)
{
	struct vk_bundle *vk = r->vk;

	if (new_view_width < 64) new_view_width = 64;
	if (new_view_height < 64) new_view_height = 64;

	if (new_view_width == r->view_width && new_view_height == r->view_height) {
		return XRT_SUCCESS;
	}

	vk->vkDeviceWaitIdle(vk->device);
	destroy_stereo_resources(r);

	return create_stereo_resources(r, new_view_width, new_view_height, new_target_height);
}

void
comp_vk_native_renderer_blit_to_target(struct comp_vk_native_renderer *r,
                                        void *cmd_ptr,
                                        uint64_t dst_image_u64,
                                        uint32_t dst_width,
                                        uint32_t dst_height)
{
	struct vk_bundle *vk = r->vk;
	VkCommandBuffer cmd = (VkCommandBuffer)cmd_ptr;
	VkImage dst_image = (VkImage)(uintptr_t)dst_image_u64;

	cmd_image_barrier(vk, cmd, r->stereo_image,
	                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                   VK_ACCESS_SHADER_READ_BIT,
	                   VK_ACCESS_TRANSFER_READ_BIT,
	                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	                   VK_PIPELINE_STAGE_TRANSFER_BIT);

	cmd_image_barrier(vk, cmd, dst_image,
	                   VK_IMAGE_LAYOUT_UNDEFINED,
	                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                   0, VK_ACCESS_TRANSFER_WRITE_BIT,
	                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	                   VK_PIPELINE_STAGE_TRANSFER_BIT);

	VkImageBlit blit = {
	    .srcSubresource = {
	        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	        .mipLevel = 0,
	        .baseArrayLayer = 0,
	        .layerCount = 1,
	    },
	    .srcOffsets = {{0, 0, 0}, {(int32_t)(r->view_width * 2), (int32_t)r->view_height, 1}},
	    .dstSubresource = {
	        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	        .mipLevel = 0,
	        .baseArrayLayer = 0,
	        .layerCount = 1,
	    },
	    .dstOffsets = {{0, 0, 0}, {(int32_t)dst_width, (int32_t)dst_height, 1}},
	};

	vk->vkCmdBlitImage(cmd,
	                    r->stereo_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                    dst_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                    1, &blit, VK_FILTER_LINEAR);

	cmd_image_barrier(vk, cmd, r->stereo_image,
	                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	                   VK_ACCESS_TRANSFER_READ_BIT,
	                   VK_ACCESS_SHADER_READ_BIT,
	                   VK_PIPELINE_STAGE_TRANSFER_BIT,
	                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

	cmd_image_barrier(vk, cmd, dst_image,
	                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                   VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
	                   VK_ACCESS_TRANSFER_WRITE_BIT,
	                   0,
	                   VK_PIPELINE_STAGE_TRANSFER_BIT,
	                   VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
}

void
comp_vk_native_renderer_blit_to_shared(struct comp_vk_native_renderer *r,
                                        void *cmd_ptr,
                                        uint64_t dst_image_u64,
                                        uint32_t dst_width,
                                        uint32_t dst_height)
{
	struct vk_bundle *vk = r->vk;
	VkCommandBuffer cmd = (VkCommandBuffer)cmd_ptr;
	VkImage dst_image = (VkImage)(uintptr_t)dst_image_u64;

	cmd_image_barrier(vk, cmd, r->stereo_image,
	                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                   VK_ACCESS_SHADER_READ_BIT,
	                   VK_ACCESS_TRANSFER_READ_BIT,
	                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	                   VK_PIPELINE_STAGE_TRANSFER_BIT);

	cmd_image_barrier(vk, cmd, dst_image,
	                   VK_IMAGE_LAYOUT_UNDEFINED,
	                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                   0, VK_ACCESS_TRANSFER_WRITE_BIT,
	                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	                   VK_PIPELINE_STAGE_TRANSFER_BIT);

	VkImageBlit blit = {
	    .srcSubresource = {
	        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	        .mipLevel = 0,
	        .baseArrayLayer = 0,
	        .layerCount = 1,
	    },
	    .srcOffsets = {{0, 0, 0}, {(int32_t)(r->view_width * 2), (int32_t)r->view_height, 1}},
	    .dstSubresource = {
	        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	        .mipLevel = 0,
	        .baseArrayLayer = 0,
	        .layerCount = 1,
	    },
	    .dstOffsets = {{0, 0, 0}, {(int32_t)dst_width, (int32_t)dst_height, 1}},
	};

	vk->vkCmdBlitImage(cmd,
	                    r->stereo_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                    dst_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                    1, &blit, VK_FILTER_LINEAR);

	cmd_image_barrier(vk, cmd, r->stereo_image,
	                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	                   VK_ACCESS_TRANSFER_READ_BIT,
	                   VK_ACCESS_SHADER_READ_BIT,
	                   VK_PIPELINE_STAGE_TRANSFER_BIT,
	                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

	// Transition to GENERAL (not PRESENT_SRC — shared texture, not a swapchain image)
	cmd_image_barrier(vk, cmd, dst_image,
	                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                   VK_IMAGE_LAYOUT_GENERAL,
	                   VK_ACCESS_TRANSFER_WRITE_BIT,
	                   0,
	                   VK_PIPELINE_STAGE_TRANSFER_BIT,
	                   VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
}

uint64_t
comp_vk_native_renderer_get_cmd_pool(struct comp_vk_native_renderer *r)
{
	return (uint64_t)(uintptr_t)r->cmd_pool;
}
