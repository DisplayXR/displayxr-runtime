// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Offscreen comp_target with GPU readback for composited pixels.
 *
 * Based on comp_window_debug_image.c. Instead of presenting to a window,
 * this target reads back the composited image from GPU to CPU via a staging
 * buffer and invokes a callback with the RGBA pixel data.
 *
 * Used by the WebXR bridge (Option A) to send Monado-composited frames
 * (including display processor output like lenticular interlacing) back
 * to the browser via WebSocket binary messages.
 *
 * @author David Fattal
 * @ingroup comp_main
 */

#include "util/u_misc.h"
#include "util/u_pacing.h"
#include "util/u_pretty_print.h"

#include "main/comp_window.h"


/*
 *
 * Structs and defines.
 *
 */

struct offscreen_target
{
	//! Base "class", so that we are a target the compositor can use.
	struct comp_target base;

	//! For error checking.
	int64_t index;

	//! Used to create the Vulkan resources, also manages index.
	struct comp_scratch_single_images target;

	/*!
	 * Storage for 'exported' images, these are pointed at by
	 * comp_target::images pointer in the @p base struct.
	 */
	struct comp_target_image images[COMP_SCRATCH_NUM_IMAGES];

	//! Compositor frame pacing helper.
	struct u_pacing_compositor *upc;

	// So we know we can free Vulkan resources safely.
	bool has_init_vulkan;

	//! @name Readback resources
	//! @{
	VkBuffer staging_buffer;
	VkDeviceMemory staging_memory;
	void *staging_mapped;
	VkDeviceSize staging_size;
	VkCommandPool cmd_pool;
	VkFence readback_fence;
	bool has_readback_resources;
	//! @}

	//! Readback callback — called with composited RGBA pixels
	void (*readback_callback)(const uint8_t *, uint32_t, uint32_t, void *);
	void *readback_userdata;
};


/*
 *
 * Helper: find host-visible memory type.
 *
 */

static uint32_t
find_memory_type(struct vk_bundle *vk, uint32_t type_bits, VkMemoryPropertyFlags props)
{
	VkPhysicalDeviceMemoryProperties mem_props;
	vk->vkGetPhysicalDeviceMemoryProperties(vk->physical_device, &mem_props);

	for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
		if ((type_bits & (1 << i)) && (mem_props.memoryTypes[i].propertyFlags & props) == props) {
			return i;
		}
	}
	return UINT32_MAX;
}


/*
 *
 * Target members.
 *
 */

static bool
target_init_pre_vulkan(struct comp_target *ct)
{
	return true; // No-op
}

static bool
target_init_post_vulkan(struct comp_target *ct, uint32_t preferred_width, uint32_t preferred_height)
{
	struct offscreen_target *ot = (struct offscreen_target *)ct;

	// We now know Vulkan is running and we can use it.
	ot->has_init_vulkan = true;

	return true;
}

static bool
target_check_ready(struct comp_target *ct)
{
	return true; // Always ready.
}

static void
create_readback_resources(struct offscreen_target *ot, struct vk_bundle *vk, VkExtent2D extent)
{
	VkDeviceSize size = (VkDeviceSize)extent.width * extent.height * 4;

	// Create staging buffer
	VkBufferCreateInfo buf_info = {
	    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
	    .size = size,
	    .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};

	VkResult ret = vk->vkCreateBuffer(vk->device, &buf_info, NULL, &ot->staging_buffer);
	if (ret != VK_SUCCESS) {
		COMP_ERROR(ot->base.c, "Failed to create readback staging buffer: %d", ret);
		return;
	}

	// Get memory requirements and allocate
	VkMemoryRequirements mem_reqs;
	vk->vkGetBufferMemoryRequirements(vk->device, ot->staging_buffer, &mem_reqs);

	uint32_t mem_type = find_memory_type(
	    vk, mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (mem_type == UINT32_MAX) {
		COMP_ERROR(ot->base.c, "No host-visible coherent memory type for readback");
		vk->vkDestroyBuffer(vk->device, ot->staging_buffer, NULL);
		ot->staging_buffer = VK_NULL_HANDLE;
		return;
	}

	VkMemoryAllocateInfo alloc_info = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .allocationSize = mem_reqs.size,
	    .memoryTypeIndex = mem_type,
	};

	ret = vk->vkAllocateMemory(vk->device, &alloc_info, NULL, &ot->staging_memory);
	if (ret != VK_SUCCESS) {
		COMP_ERROR(ot->base.c, "Failed to allocate readback staging memory: %d", ret);
		vk->vkDestroyBuffer(vk->device, ot->staging_buffer, NULL);
		ot->staging_buffer = VK_NULL_HANDLE;
		return;
	}

	vk->vkBindBufferMemory(vk->device, ot->staging_buffer, ot->staging_memory, 0);

	// Persistently map
	vk->vkMapMemory(vk->device, ot->staging_memory, 0, size, 0, &ot->staging_mapped);
	ot->staging_size = size;

	// Create command pool
	VkCommandPoolCreateInfo pool_info = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
	    .queueFamilyIndex = vk->main_queue->family_index,
	    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
	};

	ret = vk->vkCreateCommandPool(vk->device, &pool_info, NULL, &ot->cmd_pool);
	if (ret != VK_SUCCESS) {
		COMP_ERROR(ot->base.c, "Failed to create readback command pool: %d", ret);
		return;
	}

	// Create fence (unsignaled)
	VkFenceCreateInfo fence_info = {
	    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
	    .flags = 0,
	};

	ret = vk->vkCreateFence(vk->device, &fence_info, NULL, &ot->readback_fence);
	if (ret != VK_SUCCESS) {
		COMP_ERROR(ot->base.c, "Failed to create readback fence: %d", ret);
		return;
	}

	ot->has_readback_resources = true;
}

static void
target_create_images(struct comp_target *ct, const struct comp_target_create_images_info *create_info)
{
	struct offscreen_target *ot = (struct offscreen_target *)ct;
	struct vk_bundle *vk = &ot->base.c->base.vk;
	bool use_unorm = false, use_srgb = false, maybe_convert = false;

	// Paranoia.
	assert(ot->has_init_vulkan);

	// Find format — same logic as debug_image
	for (uint32_t i = 0; i < create_info->format_count; i++) {
		VkFormat format = create_info->formats[i];

		if (!maybe_convert && format == VK_FORMAT_B8G8R8A8_UNORM) {
			use_unorm = true;
			maybe_convert = true;
			continue;
		}
		if (!maybe_convert && format == VK_FORMAT_B8G8R8A8_SRGB) {
			use_srgb = true;
			maybe_convert = true;
			continue;
		}

		if (format == VK_FORMAT_R8G8B8A8_UNORM) {
			use_unorm = true;
			maybe_convert = false;
			break;
		}
		if (format == VK_FORMAT_R8G8B8A8_SRGB) {
			use_srgb = true;
			maybe_convert = false;
			break;
		}
	}

	assert(use_unorm || use_srgb);
	if (maybe_convert) {
		COMP_WARN(ct->c, "Ignoring the format and picking something we use.");
	}

	// Allocate scratch images.
	comp_scratch_single_images_ensure_mutable(&ot->target, vk, create_info->extent);

	// Share Vulkan handles.
	for (uint32_t i = 0; i < COMP_SCRATCH_NUM_IMAGES; i++) {
		ot->images[i].handle = ot->target.images[i].image;
		if (use_unorm) {
			ot->images[i].view = ot->target.images[i].unorm_view;
		}
		if (use_srgb) {
			ot->images[i].view = ot->target.images[i].srgb_view;
		}
	}

	// Fill in exported data.
	ot->base.image_count = COMP_SCRATCH_NUM_IMAGES;
	ot->base.images = &ot->images[0];
	ot->base.width = create_info->extent.width;
	ot->base.height = create_info->extent.height;
	if (use_unorm) {
		ot->base.format = VK_FORMAT_R8G8B8A8_UNORM;
	}
	if (use_srgb) {
		ot->base.format = VK_FORMAT_R8G8B8A8_SRGB;
	}
	ot->base.final_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

	// Allocate readback resources (staging buffer, cmd pool, fence).
	create_readback_resources(ot, vk, create_info->extent);
}

static bool
target_has_images(struct comp_target *ct)
{
	struct offscreen_target *ot = (struct offscreen_target *)ct;
	return ot->base.images != NULL;
}

static VkResult
target_acquire(struct comp_target *ct, uint32_t *out_index)
{
	struct offscreen_target *ot = (struct offscreen_target *)ct;

	// Error checking.
	assert(ot->index == -1);

	uint32_t index = 0;
	comp_scratch_single_images_get(&ot->target, &index);

	ot->index = index;
	*out_index = index;

	return VK_SUCCESS;
}

static VkResult
target_present(struct comp_target *ct,
               VkQueue queue,
               uint32_t index,
               uint64_t timeline_semaphore_value,
               int64_t desired_present_time_ns,
               int64_t present_slop_ns)
{
	struct offscreen_target *ot = (struct offscreen_target *)ct;
	struct vk_bundle *vk = &ot->base.c->base.vk;

	assert(index == ot->index);

	if (!ot->has_readback_resources || ot->readback_callback == NULL) {
		// No readback — behave like debug_image (discard frame).
		COMP_WARN(ct->c, "Offscreen present: no readback resources (%d) or callback (%p), discarding",
		          ot->has_readback_resources, (void *)(uintptr_t)ot->readback_callback);
		comp_scratch_single_images_done(&ot->target);
		ot->index = -1;
		return VK_SUCCESS;
	}

	VkResult ret;

	// Allocate a one-time command buffer.
	VkCommandBufferAllocateInfo cb_alloc = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	    .commandPool = ot->cmd_pool,
	    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
	    .commandBufferCount = 1,
	};

	VkCommandBuffer cmd;
	ret = vk->vkAllocateCommandBuffers(vk->device, &cb_alloc, &cmd);
	if (ret != VK_SUCCESS) {
		COMP_ERROR(ct->c, "Failed to allocate readback command buffer: %d", ret);
		comp_scratch_single_images_done(&ot->target);
		ot->index = -1;
		return VK_SUCCESS; // Don't fail the present
	}

	VkCommandBufferBeginInfo begin_info = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	vk->vkBeginCommandBuffer(cmd, &begin_info);

	// Barrier: PRESENT_SRC_KHR → TRANSFER_SRC_OPTIMAL
	VkImageMemoryBarrier barrier = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
	    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .image = ot->images[index].handle,
	    .subresourceRange =
	        {
	            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	            .baseMipLevel = 0,
	            .levelCount = 1,
	            .baseArrayLayer = 0,
	            .layerCount = 1,
	        },
	};

	vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);

	// Copy image → staging buffer
	VkBufferImageCopy region = {
	    .bufferOffset = 0,
	    .bufferRowLength = 0,
	    .bufferImageHeight = 0,
	    .imageSubresource =
	        {
	            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	            .mipLevel = 0,
	            .baseArrayLayer = 0,
	            .layerCount = 1,
	        },
	    .imageOffset = {0, 0, 0},
	    .imageExtent = {ct->width, ct->height, 1},
	};

	vk->vkCmdCopyImageToBuffer(cmd, ot->images[index].handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                           ot->staging_buffer, 1, &region);

	vk->vkEndCommandBuffer(cmd);

	// Build wait semaphore info — wait on render_complete from the compositor
	VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	VkSemaphore wait_sem = ct->semaphores.render_complete;

	VkSubmitInfo submit_info = {
	    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	    .commandBufferCount = 1,
	    .pCommandBuffers = &cmd,
	};

	// Only wait on render_complete semaphore if it exists
	if (wait_sem != VK_NULL_HANDLE) {
		submit_info.waitSemaphoreCount = 1;
		submit_info.pWaitSemaphores = &wait_sem;
		submit_info.pWaitDstStageMask = &wait_stage;
	}

	vk->vkResetFences(vk->device, 1, &ot->readback_fence);
	ret = vk->vkQueueSubmit(queue, 1, &submit_info, ot->readback_fence);
	if (ret != VK_SUCCESS) {
		COMP_ERROR(ct->c, "Failed to submit readback commands: %d", ret);
		vk->vkFreeCommandBuffers(vk->device, ot->cmd_pool, 1, &cmd);
		comp_scratch_single_images_done(&ot->target);
		ot->index = -1;
		return VK_SUCCESS;
	}

	// Wait for GPU copy to complete
	vk->vkWaitForFences(vk->device, 1, &ot->readback_fence, VK_TRUE, UINT64_MAX);

	// Invoke callback with the composited pixels
	ot->readback_callback((const uint8_t *)ot->staging_mapped, ct->width, ct->height, ot->readback_userdata);
	COMP_INFO(ct->c, "Offscreen readback: delivered %ux%u frame to callback", ct->width, ct->height);

	// Cleanup
	vk->vkFreeCommandBuffers(vk->device, ot->cmd_pool, 1, &cmd);
	comp_scratch_single_images_done(&ot->target);
	ot->index = -1;

	return VK_SUCCESS;
}

static VkResult
target_wait_for_present(struct comp_target *ct, time_duration_ns timeout_ns)
{
	return VK_ERROR_EXTENSION_NOT_PRESENT;
}

static void
target_flush(struct comp_target *ct)
{
	// No-op
}

static void
target_calc_frame_pacing(struct comp_target *ct,
                         int64_t *out_frame_id,
                         int64_t *out_wake_up_time_ns,
                         int64_t *out_desired_present_time_ns,
                         int64_t *out_present_slop_ns,
                         int64_t *out_predicted_display_time_ns)
{
	struct offscreen_target *ot = (struct offscreen_target *)ct;

	int64_t frame_id = -1;
	int64_t wake_up_time_ns = 0;
	int64_t desired_present_time_ns = 0;
	int64_t present_slop_ns = 0;
	int64_t predicted_display_time_ns = 0;
	int64_t predicted_display_period_ns = 0;
	int64_t min_display_period_ns = 0;
	int64_t now_ns = os_monotonic_get_ns();

	u_pc_predict(ot->upc,                     //
	             now_ns,                       //
	             &frame_id,                    //
	             &wake_up_time_ns,             //
	             &desired_present_time_ns,     //
	             &present_slop_ns,             //
	             &predicted_display_time_ns,   //
	             &predicted_display_period_ns, //
	             &min_display_period_ns);      //

	*out_frame_id = frame_id;
	*out_wake_up_time_ns = wake_up_time_ns;
	*out_desired_present_time_ns = desired_present_time_ns;
	*out_predicted_display_time_ns = predicted_display_time_ns;
	*out_present_slop_ns = present_slop_ns;
}

static void
target_mark_timing_point(struct comp_target *ct, enum comp_target_timing_point point, int64_t frame_id, int64_t when_ns)
{
	struct offscreen_target *ot = (struct offscreen_target *)ct;

	switch (point) {
	case COMP_TARGET_TIMING_POINT_WAKE_UP:
		u_pc_mark_point(ot->upc, U_TIMING_POINT_WAKE_UP, frame_id, when_ns);
		break;
	case COMP_TARGET_TIMING_POINT_BEGIN: //
		u_pc_mark_point(ot->upc, U_TIMING_POINT_BEGIN, frame_id, when_ns);
		break;
	case COMP_TARGET_TIMING_POINT_SUBMIT_BEGIN:
		u_pc_mark_point(ot->upc, U_TIMING_POINT_SUBMIT_BEGIN, frame_id, when_ns);
		break;
	case COMP_TARGET_TIMING_POINT_SUBMIT_END:
		u_pc_mark_point(ot->upc, U_TIMING_POINT_SUBMIT_END, frame_id, when_ns);
		break;
	default: assert(false);
	}
}

static VkResult
target_update_timings(struct comp_target *ct)
{
	return VK_SUCCESS; // No-op
}

static void
target_info_gpu(struct comp_target *ct, int64_t frame_id, int64_t gpu_start_ns, int64_t gpu_end_ns, int64_t when_ns)
{
	struct offscreen_target *ot = (struct offscreen_target *)ct;

	u_pc_info_gpu(ot->upc, frame_id, gpu_start_ns, gpu_end_ns, when_ns);
}

static void
target_set_title(struct comp_target *ct, const char *title)
{
	// No-op
}

static void
target_destroy(struct comp_target *ct)
{
	struct offscreen_target *ot = (struct offscreen_target *)ct;
	struct vk_bundle *vk = &ot->base.c->base.vk;

	// Do this first.
	u_var_remove_root(ot);

	// Can only free if we have Vulkan.
	if (ot->has_init_vulkan) {
		// Free readback resources first.
		if (ot->has_readback_resources) {
			if (ot->readback_fence != VK_NULL_HANDLE) {
				vk->vkDestroyFence(vk->device, ot->readback_fence, NULL);
			}
			if (ot->cmd_pool != VK_NULL_HANDLE) {
				vk->vkDestroyCommandPool(vk->device, ot->cmd_pool, NULL);
			}
			if (ot->staging_mapped != NULL) {
				vk->vkUnmapMemory(vk->device, ot->staging_memory);
			}
			if (ot->staging_memory != VK_NULL_HANDLE) {
				vk->vkFreeMemory(vk->device, ot->staging_memory, NULL);
			}
			if (ot->staging_buffer != VK_NULL_HANDLE) {
				vk->vkDestroyBuffer(vk->device, ot->staging_buffer, NULL);
			}
			ot->has_readback_resources = false;
		}

		comp_scratch_single_images_free(&ot->target, vk);
		ot->has_init_vulkan = false;
		ot->base.image_count = 0;
		ot->base.images = NULL;
		ot->base.width = 0;
		ot->base.height = 0;
		ot->base.format = VK_FORMAT_UNDEFINED;
		ot->base.final_layout = VK_IMAGE_LAYOUT_UNDEFINED;
	}

	// Always free non-Vulkan resources.
	comp_scratch_single_images_destroy(&ot->target);

	// Pacing is always created.
	u_pc_destroy(&ot->upc);

	// Free memory.
	free(ot);
}


/*
 *
 * Public create function.
 *
 */

struct comp_target *
comp_window_offscreen_create(struct comp_compositor *c,
                             void (*callback)(const uint8_t *, uint32_t, uint32_t, void *),
                             void *userdata)
{
	struct offscreen_target *ot = U_TYPED_CALLOC(struct offscreen_target);

	ot->base.name = "offscreen_readback";
	ot->base.init_pre_vulkan = target_init_pre_vulkan;
	ot->base.init_post_vulkan = target_init_post_vulkan;
	ot->base.check_ready = target_check_ready;
	ot->base.create_images = target_create_images;
	ot->base.has_images = target_has_images;
	ot->base.acquire = target_acquire;
	ot->base.present = target_present;
	ot->base.wait_for_present = target_wait_for_present;
	ot->base.flush = target_flush;
	ot->base.calc_frame_pacing = target_calc_frame_pacing;
	ot->base.mark_timing_point = target_mark_timing_point;
	ot->base.update_timings = target_update_timings;
	ot->base.info_gpu = target_info_gpu;
	ot->base.set_title = target_set_title;
	ot->base.destroy = target_destroy;
	ot->base.c = c;

	ot->base.wait_for_present_supported = false;

	// Store callback.
	ot->readback_callback = callback;
	ot->readback_userdata = userdata;

	// Create the pacer.
	uint64_t now_ns = os_monotonic_get_ns();
	u_pc_fake_create(c->settings.nominal_frame_interval_ns, now_ns, &ot->upc);

	// Only inits locking, Vulkan resources inited later.
	comp_scratch_single_images_init(&ot->target);

	// For error checking.
	ot->index = -1;

	// Variable tracking.
	u_var_add_root(ot, "Offscreen readback output", true);
	u_var_add_native_images_debug(ot, &ot->target.unid, "Image");

	COMP_INFO(c, "Created offscreen readback target (callback=%p, userdata=%p)", (void *)(uintptr_t)callback,
	          userdata);

	return &ot->base;
}
