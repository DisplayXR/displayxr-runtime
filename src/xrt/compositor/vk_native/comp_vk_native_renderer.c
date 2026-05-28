// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Vulkan renderer for layer compositing.
 * @author David Fattal
 * @ingroup comp_vk_native
 *
 * Creates a tiled atlas texture and copies/blits app swapchain
 * content into per-eye tile regions. The atlas texture is then consumed
 * by the display processor (weaver) or blitted to the target for 2D fallback.
 * Default layout is 2x1 (stereo); tile_columns and tile_rows
 * can be changed to support arbitrary atlas layouts (e.g. 2x2 for quad views).
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

	//! Number of tile columns in the atlas (default 2 for stereo).
	uint32_t tile_columns;

	//! Number of tile rows in the atlas (default 1 for stereo).
	uint32_t tile_rows;

	//! Atlas texture (tile_columns * view_width x tile_rows * texture_height).
	VkImage atlas_image;

	//! Memory for atlas texture.
	VkDeviceMemory atlas_memory;

	//! Full image view for the atlas texture.
	VkImageView atlas_view;

	//! Width per view.
	uint32_t view_width;

	//! Height per view.
	uint32_t view_height;

	//! Actual texture height (max of view_height and target_height).
	uint32_t texture_height;

	//! Atlas texture allocated dimensions (worst-case, may be > content dims).
	uint32_t atlas_alloc_width;
	uint32_t atlas_alloc_height;

	//! Format of the atlas texture.
	VkFormat format;

	//! When true, clear the atlas to alpha=0 (transparent) instead of
	//! opaque black, so app alpha<1 regions survive to the present (issue #392).
	bool transparent_background;

	//! Binary semaphore signaled at end of draw()'s queue submit so the
	//! compositor's downstream pre-DP submit can chain without a CPU
	//! waitIdle between them. Single-use per draw — take_frame_done_semaphore()
	//! returns this handle and clears @ref signal_pending so subsequent
	//! submits in the same frame don't double-wait.
	VkSemaphore frame_done_sem;

	//! Fence signaled alongside @ref frame_done_sem. Waited at the start of
	//! the next draw() to enforce CPU-side back-pressure and to know when
	//! @ref pending_cmd is safe to free.
	VkFence frame_done_fence;

	//! Cmd buffer in flight from the previous draw(). NULL on first call.
	//! Freed at the start of the next draw() once @ref frame_done_fence
	//! signals, or at destroy() time after vkDeviceWaitIdle.
	VkCommandBuffer pending_cmd;

	//! True after draw() signals @ref frame_done_sem and until a downstream
	//! submit takes it via take_frame_done_semaphore().
	bool signal_pending;
};

static void
destroy_atlas_resources(struct comp_vk_native_renderer *r)
{
	struct vk_bundle *vk = r->vk;

	if (r->atlas_view != VK_NULL_HANDLE) {
		vk->vkDestroyImageView(vk->device, r->atlas_view, NULL);
		r->atlas_view = VK_NULL_HANDLE;
	}
	if (r->atlas_image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, r->atlas_image, NULL);
		r->atlas_image = VK_NULL_HANDLE;
	}
	if (r->atlas_memory != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, r->atlas_memory, NULL);
		r->atlas_memory = VK_NULL_HANDLE;
	}
}

static xrt_result_t
create_atlas_resources(struct comp_vk_native_renderer *r,
                       uint32_t view_width,
                       uint32_t view_height,
                       uint32_t atlas_width,
                       uint32_t atlas_height)
{
	struct vk_bundle *vk = r->vk;

	r->view_width = view_width;
	r->view_height = view_height;
	r->texture_height = view_height;
	r->atlas_alloc_width = atlas_width;
	r->atlas_alloc_height = atlas_height;

	VkImageCreateInfo image_ci = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .imageType = VK_IMAGE_TYPE_2D,
	    .format = r->format,
	    .extent = {atlas_width, atlas_height, 1},
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

	VkResult res = vk->vkCreateImage(vk->device, &image_ci, NULL, &r->atlas_image);
	if (res != VK_SUCCESS) {
		U_LOG_E("Failed to create atlas image: %d", res);
		return XRT_ERROR_VULKAN;
	}

	VkMemoryRequirements mem_reqs;
	vk->vkGetImageMemoryRequirements(vk->device, r->atlas_image, &mem_reqs);

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

	res = vk->vkAllocateMemory(vk->device, &alloc_info, NULL, &r->atlas_memory);
	if (res != VK_SUCCESS) {
		U_LOG_E("Failed to allocate atlas memory: %d", res);
		return XRT_ERROR_VULKAN;
	}

	res = vk->vkBindImageMemory(vk->device, r->atlas_image, r->atlas_memory, 0);
	if (res != VK_SUCCESS) {
		U_LOG_E("Failed to bind atlas memory: %d", res);
		return XRT_ERROR_VULKAN;
	}

	VkImageViewCreateInfo view_ci = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
	    .image = r->atlas_image,
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

	res = vk->vkCreateImageView(vk->device, &view_ci, NULL, &r->atlas_view);
	if (res != VK_SUCCESS) {
		U_LOG_E("Failed to create atlas view: %d", res);
		return XRT_ERROR_VULKAN;
	}

	U_LOG_I("Created atlas texture: %ux%u (view %ux%u, tiles %ux%u)", atlas_width, atlas_height,
	        view_width, view_height, r->tile_columns, r->tile_rows);

	return XRT_SUCCESS;
}

xrt_result_t
comp_vk_native_renderer_create(struct comp_vk_native_compositor *c,
                                uint32_t view_width,
                                uint32_t view_height,
                                uint32_t atlas_width,
                                uint32_t atlas_height,
                                struct comp_vk_native_renderer **out_renderer)
{
	struct vk_bundle *vk = comp_vk_native_compositor_get_vk(c);
	uint32_t queue_family_index = comp_vk_native_compositor_get_queue_family(c);

	struct comp_vk_native_renderer *r = U_TYPED_CALLOC(struct comp_vk_native_renderer);
	if (r == NULL) {
		return XRT_ERROR_ALLOCATION;
	}

	r->vk = vk;
	r->format = VK_FORMAT_B8G8R8A8_UNORM;
	r->tile_columns = 2;
	r->tile_rows = 1;

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

	xrt_result_t xret = create_atlas_resources(r, view_width, view_height, atlas_width, atlas_height);
	if (xret != XRT_SUCCESS) {
		vk->vkDestroyCommandPool(vk->device, r->cmd_pool, NULL);
		free(r);
		return xret;
	}

	// Per-frame sync primitives for the combined-submit chain
	// (renderer.draw() signals → compositor's pre-DP submit waits, no
	// vkQueueWaitIdle between them). The fence starts signaled so the
	// first draw() can free a (non-existent) previous cmd buffer and
	// vkResetFences without an initial wait stall.
	VkSemaphoreCreateInfo sem_ci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
	res = vk->vkCreateSemaphore(vk->device, &sem_ci, NULL, &r->frame_done_sem);
	if (res != VK_SUCCESS) {
		U_LOG_E("Failed to create renderer frame-done semaphore: %d", res);
		destroy_atlas_resources(r);
		vk->vkDestroyCommandPool(vk->device, r->cmd_pool, NULL);
		free(r);
		return XRT_ERROR_VULKAN;
	}

	VkFenceCreateInfo fence_ci = {
	    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
	    .flags = VK_FENCE_CREATE_SIGNALED_BIT,
	};
	res = vk->vkCreateFence(vk->device, &fence_ci, NULL, &r->frame_done_fence);
	if (res != VK_SUCCESS) {
		U_LOG_E("Failed to create renderer frame-done fence: %d", res);
		vk->vkDestroySemaphore(vk->device, r->frame_done_sem, NULL);
		destroy_atlas_resources(r);
		vk->vkDestroyCommandPool(vk->device, r->cmd_pool, NULL);
		free(r);
		return XRT_ERROR_VULKAN;
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

	if (r->pending_cmd != VK_NULL_HANDLE) {
		vk->vkFreeCommandBuffers(vk->device, r->cmd_pool, 1, &r->pending_cmd);
		r->pending_cmd = VK_NULL_HANDLE;
	}

	if (r->frame_done_fence != VK_NULL_HANDLE) {
		vk->vkDestroyFence(vk->device, r->frame_done_fence, NULL);
	}
	if (r->frame_done_sem != VK_NULL_HANDLE) {
		vk->vkDestroySemaphore(vk->device, r->frame_done_sem, NULL);
	}

	destroy_atlas_resources(r);

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
                              bool hardware_display_3d)
{
	struct vk_bundle *vk = r->vk;
	(void)left_eye;
	(void)right_eye;

	// Wait for the previous frame's submit to finish so we can safely
	// free its cmd buffer and (later) reuse the per-frame fence. Fence
	// starts signaled at create time so the first call returns
	// immediately. We DON'T reset the fence here — that's deferred
	// until just before the main vkQueueSubmit so any early-return
	// failure (cmd-buffer alloc fails, drain fails, etc.) leaves the
	// fence in its signaled state. Otherwise the next draw() would
	// vkWaitForFences forever on a fence nothing ever signals.
	vk->vkWaitForFences(vk->device, 1, &r->frame_done_fence, VK_TRUE, UINT64_MAX);

	if (r->pending_cmd != VK_NULL_HANDLE) {
		vk->vkFreeCommandBuffers(vk->device, r->cmd_pool, 1, &r->pending_cmd);
		r->pending_cmd = VK_NULL_HANDLE;
	}

	// Defensive: if the previous frame's frame-done semaphore was
	// signaled but never consumed by a downstream submit (e.g. that
	// submit failed, or resize() drained the GPU without going through
	// the compositor's frame loop), drain it now via a no-op
	// wait-submit so we don't double-signal a binary semaphore on the
	// vkQueueSubmit below — that's undefined behavior per Vulkan spec.
	// Only clear signal_pending if the drain actually went through;
	// otherwise the next signaling submit would risk the double-signal UB.
	if (r->signal_pending) {
		VkPipelineStageFlags drain_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		VkSubmitInfo drain = {
		    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		    .waitSemaphoreCount = 1,
		    .pWaitSemaphores = &r->frame_done_sem,
		    .pWaitDstStageMask = &drain_stage,
		};
		VkResult drain_res = vk->vkQueueSubmit(vk->main_queue->queue, 1, &drain, VK_NULL_HANDLE);
		if (drain_res == VK_SUCCESS) {
			r->signal_pending = false;
		} else {
			U_LOG_E("Failed to drain stuck frame-done semaphore: %d", drain_res);
			return XRT_ERROR_VULKAN;
		}
	}

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

	// Transition atlas image to transfer dst
	cmd_image_barrier(vk, cmd, r->atlas_image,
	                   VK_IMAGE_LAYOUT_UNDEFINED,
	                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                   0, VK_ACCESS_TRANSFER_WRITE_BIT,
	                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	                   VK_PIPELINE_STAGE_TRANSFER_BIT);

	// Clear atlas texture to black. Use alpha=0 in transparent-background mode
	// so atlas regions not overwritten by a tile blit stay see-through (issue #392).
	VkClearColorValue clear_color = {{0.0f, 0.0f, 0.0f, r->transparent_background ? 0.0f : 1.0f}};
	VkImageSubresourceRange range = {
	    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	    .baseMipLevel = 0,
	    .levelCount = 1,
	    .baseArrayLayer = 0,
	    .layerCount = 1,
	};
	vk->vkCmdClearColorImage(cmd, r->atlas_image,
	                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                          &clear_color, 1, &range);

	// Blit each projection layer into the atlas texture
	for (uint32_t i = 0; i < layers->layer_count; i++) {
		struct comp_layer *layer = &layers->layers[i];

		if (layer->data.type != XRT_LAYER_PROJECTION &&
		    layer->data.type != XRT_LAYER_PROJECTION_DEPTH) {
			continue;
		}

		uint32_t view_count = hardware_display_3d ? layer->data.view_count : 1;
		if (view_count == 0) view_count = 1;

		static bool blit_logged = false;
		if (!blit_logged) {
			U_LOG_W("Atlas blit: view_count=%u, hardware_3d=%d, tiles=%ux%u, "
			        "view=%ux%u, layer_view_count=%u",
			        view_count, (int)hardware_display_3d,
			        r->tile_columns, r->tile_rows,
			        r->view_width, r->view_height,
			        layer->data.view_count);
		}

		for (uint32_t eye = 0; eye < view_count; eye++) {
			struct xrt_swapchain *xsc = layer->sc_array[eye];
			if (xsc == NULL) {
				if (!blit_logged) U_LOG_W("Atlas blit: eye %u swapchain NULL", eye);
				continue;
			}

			uint32_t sc_index = layer->data.proj.v[eye].sub.image_index;
			VkImage src_image = (VkImage)(uintptr_t)comp_vk_native_swapchain_get_image(xsc, sc_index);
			if (src_image == VK_NULL_HANDLE) {
				if (!blit_logged) U_LOG_W("Atlas blit: eye %u image NULL", eye);
				continue;
			}

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
			if (!hardware_display_3d || view_count == 1) {
				// 2D mode: stretch across entire atlas
				dx0 = 0;
				dy0 = 0;
				dx1 = (int32_t)(r->tile_columns * r->view_width);
				dy1 = (int32_t)(r->tile_rows * r->view_height);
			} else {
				// Tiled layout: place each eye in its tile
				uint32_t tile_x = eye % r->tile_columns;
				uint32_t tile_y = eye / r->tile_columns;
				dx0 = (int32_t)(tile_x * r->view_width);
				dy0 = (int32_t)(tile_y * r->view_height);
				dx1 = dx0 + (int32_t)r->view_width;
				dy1 = dy0 + (int32_t)r->view_height;
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
			                    r->atlas_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			                    1, &blit, VK_FILTER_LINEAR);

			cmd_image_barrier(vk, cmd, src_image,
			                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			                   VK_ACCESS_TRANSFER_READ_BIT,
			                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			                   VK_PIPELINE_STAGE_TRANSFER_BIT,
			                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
		}

		blit_logged = true;
	}

	// Transition atlas image to shader read for display processor
	cmd_image_barrier(vk, cmd, r->atlas_image,
	                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	                   VK_ACCESS_TRANSFER_WRITE_BIT,
	                   VK_ACCESS_SHADER_READ_BIT,
	                   VK_PIPELINE_STAGE_TRANSFER_BIT,
	                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

	vk->vkEndCommandBuffer(cmd);

	// Signal the frame-done semaphore on submit so the compositor's
	// pre-DP submit (which reads from atlas_image) can chain after us
	// without a CPU vkQueueWaitIdle. The fence catches the same event
	// CPU-side so the next draw() can safely free this cmd buffer.
	//
	// Reset the fence here (not at the top of draw) so any early-return
	// failure above leaves the fence in its previous signaled state
	// rather than deadlocking the next draw() on a fence nothing signals.
	vk->vkResetFences(vk->device, 1, &r->frame_done_fence);

	VkSubmitInfo submit_info = {
	    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	    .commandBufferCount = 1,
	    .pCommandBuffers = &cmd,
	    .signalSemaphoreCount = 1,
	    .pSignalSemaphores = &r->frame_done_sem,
	};

	res = vk->vkQueueSubmit(vk->main_queue->queue, 1, &submit_info, r->frame_done_fence);
	if (res != VK_SUCCESS) {
		U_LOG_E("Failed to submit renderer commands: %d", res);
		vk->vkFreeCommandBuffers(vk->device, r->cmd_pool, 1, &cmd);
		// Re-signal the fence with a no-op submit so the next draw()
		// doesn't deadlock waiting on a fence that vkQueueSubmit
		// refused to associate with a batch.
		VkSubmitInfo signal_only = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
		vk->vkQueueSubmit(vk->main_queue->queue, 1, &signal_only, r->frame_done_fence);
		return XRT_ERROR_VULKAN;
	}

	r->pending_cmd = cmd;
	r->signal_pending = true;

	return XRT_SUCCESS;
}

uint64_t
comp_vk_native_renderer_get_atlas_view(struct comp_vk_native_renderer *r)
{
	return (uint64_t)(uintptr_t)r->atlas_view;
}

uint64_t
comp_vk_native_renderer_get_atlas_image(struct comp_vk_native_renderer *r)
{
	return (uint64_t)(uintptr_t)r->atlas_image;
}

void
comp_vk_native_renderer_get_view_dimensions(struct comp_vk_native_renderer *r,
                                             uint32_t *out_view_width,
                                             uint32_t *out_view_height)
{
	*out_view_width = r->view_width;
	*out_view_height = r->view_height;
}

void
comp_vk_native_renderer_get_atlas_dimensions(struct comp_vk_native_renderer *r,
                                              uint32_t *out_width,
                                              uint32_t *out_height)
{
	*out_width = r->atlas_alloc_width;
	*out_height = r->atlas_alloc_height;
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
                                uint32_t new_atlas_width,
                                uint32_t new_atlas_height)
{
	struct vk_bundle *vk = r->vk;

	if (new_view_width < 64) new_view_width = 64;
	if (new_view_height < 64) new_view_height = 64;

	uint32_t cur_atlas_w = r->tile_columns * r->view_width;
	uint32_t cur_atlas_h = r->tile_rows * r->view_height;
	if (new_view_width == r->view_width && new_view_height == r->view_height &&
	    new_atlas_width == cur_atlas_w && new_atlas_height == cur_atlas_h) {
		return XRT_SUCCESS;
	}

	vk->vkDeviceWaitIdle(vk->device);
	destroy_atlas_resources(r);

	return create_atlas_resources(r, new_view_width, new_view_height, new_atlas_width, new_atlas_height);
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

	cmd_image_barrier(vk, cmd, r->atlas_image,
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
	    .srcOffsets = {{0, 0, 0}, {(int32_t)(r->tile_columns * r->view_width), (int32_t)(r->tile_rows * r->view_height), 1}},
	    .dstSubresource = {
	        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	        .mipLevel = 0,
	        .baseArrayLayer = 0,
	        .layerCount = 1,
	    },
	    .dstOffsets = {{0, 0, 0}, {(int32_t)dst_width, (int32_t)dst_height, 1}},
	};

	vk->vkCmdBlitImage(cmd,
	                    r->atlas_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                    dst_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                    1, &blit, VK_FILTER_LINEAR);

	cmd_image_barrier(vk, cmd, r->atlas_image,
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

	cmd_image_barrier(vk, cmd, r->atlas_image,
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
	    .srcOffsets = {{0, 0, 0}, {(int32_t)(r->tile_columns * r->view_width), (int32_t)(r->tile_rows * r->view_height), 1}},
	    .dstSubresource = {
	        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	        .mipLevel = 0,
	        .baseArrayLayer = 0,
	        .layerCount = 1,
	    },
	    .dstOffsets = {{0, 0, 0}, {(int32_t)dst_width, (int32_t)dst_height, 1}},
	};

	vk->vkCmdBlitImage(cmd,
	                    r->atlas_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                    dst_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                    1, &blit, VK_FILTER_LINEAR);

	cmd_image_barrier(vk, cmd, r->atlas_image,
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

void
comp_vk_native_renderer_get_tile_layout(struct comp_vk_native_renderer *r,
                                         uint32_t *out_tile_columns,
                                         uint32_t *out_tile_rows)
{
	*out_tile_columns = r->tile_columns;
	*out_tile_rows = r->tile_rows;
}

void
comp_vk_native_renderer_set_tile_layout(struct comp_vk_native_renderer *r,
                                         uint32_t tile_columns,
                                         uint32_t tile_rows)
{
	r->tile_columns = tile_columns;
	r->tile_rows = tile_rows;
}

uint64_t
comp_vk_native_renderer_get_cmd_pool(struct comp_vk_native_renderer *r)
{
	return (uint64_t)(uintptr_t)r->cmd_pool;
}

void
comp_vk_native_renderer_set_transparent(struct comp_vk_native_renderer *r, bool transparent_background)
{
	r->transparent_background = transparent_background;
}

uint64_t
comp_vk_native_renderer_take_frame_done_semaphore(struct comp_vk_native_renderer *r)
{
	if (!r->signal_pending) {
		return 0;
	}
	r->signal_pending = false;
	return (uint64_t)(uintptr_t)r->frame_done_sem;
}
