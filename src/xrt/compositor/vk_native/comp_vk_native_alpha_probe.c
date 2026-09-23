// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Per-frame content-transparency probe (see the header).
 * @ingroup comp_vk_native
 */

#include "comp_vk_native_alpha_probe.h"

#include "util/u_logging.h"
#include "util/u_misc.h"

#include "shaders/alpha_probe.comp.h"

#include <stdlib.h>
#include <string.h>

//! Readback ring depth. The compositor fences every frame, so 2 would do; 3
//! leaves a slot of margin for the #1264 fence-park, where a frame can still be
//! in flight when the next one records.
#define PROBE_RING 3

//! Per-slot stride in the result buffer. 256 is the largest
//! minStorageBufferOffsetAlignment the spec allows, so it is always legal.
#define PROBE_SLOT_BYTES 256

//! Sample one texel per PROBE_STRIDE x PROBE_STRIDE block. A transparent
//! background is large by nature; this finds any transparent feature at least
//! PROBE_STRIDE pixels across.
#define PROBE_STRIDE 4u

//! Alpha strictly below this counts as transparent. Just under 1.0 so that
//! 8-bit alpha of 254 counts but a UNORM 255 (1.0) or a float 1.0 does not.
#define PROBE_THRESHOLD (254.5f / 255.0f)

struct comp_vk_alpha_probe
{
	struct vk_bundle *vk;

	VkShaderModule shader;
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout pipeline_layout;
	VkPipeline pipeline;
	VkSampler sampler;
	VkDescriptorPool pool;
	VkDescriptorSet sets[PROBE_RING];

	VkBuffer buffer;
	VkDeviceMemory memory;
	uint8_t *mapped;

	uint64_t recorded;  //!< probes recorded so far
	uint64_t read_upto; //!< probes whose result has been consumed
};

struct probe_push
{
	uint32_t extent[2];
	uint32_t stride;
	float threshold;
};

void
comp_vk_alpha_probe_destroy(struct comp_vk_alpha_probe **probe_ptr)
{
	struct comp_vk_alpha_probe *p = probe_ptr != NULL ? *probe_ptr : NULL;
	if (p == NULL) {
		return;
	}
	struct vk_bundle *vk = p->vk;
	if (p->mapped != NULL) {
		vk->vkUnmapMemory(vk->device, p->memory);
	}
	if (p->buffer != VK_NULL_HANDLE) {
		vk->vkDestroyBuffer(vk->device, p->buffer, NULL);
	}
	if (p->memory != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, p->memory, NULL);
	}
	if (p->pool != VK_NULL_HANDLE) {
		vk->vkDestroyDescriptorPool(vk->device, p->pool, NULL);
	}
	if (p->sampler != VK_NULL_HANDLE) {
		vk->vkDestroySampler(vk->device, p->sampler, NULL);
	}
	if (p->pipeline != VK_NULL_HANDLE) {
		vk->vkDestroyPipeline(vk->device, p->pipeline, NULL);
	}
	if (p->pipeline_layout != VK_NULL_HANDLE) {
		vk->vkDestroyPipelineLayout(vk->device, p->pipeline_layout, NULL);
	}
	if (p->set_layout != VK_NULL_HANDLE) {
		vk->vkDestroyDescriptorSetLayout(vk->device, p->set_layout, NULL);
	}
	if (p->shader != VK_NULL_HANDLE) {
		vk->vkDestroyShaderModule(vk->device, p->shader, NULL);
	}
	free(p);
	*probe_ptr = NULL;
}

struct comp_vk_alpha_probe *
comp_vk_alpha_probe_create(struct vk_bundle *vk)
{
	struct comp_vk_alpha_probe *p = U_TYPED_CALLOC(struct comp_vk_alpha_probe);
	if (p == NULL) {
		return NULL;
	}
	p->vk = vk;
	VkResult ret = VK_ERROR_FEATURE_NOT_PRESENT;

	// The probe dispatches on the compositor's own queue. Every graphics
	// family on shipping hardware also does compute, but it is not a spec
	// guarantee, so check rather than assume.
	{
		uint32_t n = 0;
		vk->vkGetPhysicalDeviceQueueFamilyProperties(vk->physical_device, &n, NULL);
		VkQueueFamilyProperties props[16];
		n = n > 16 ? 16 : n;
		vk->vkGetPhysicalDeviceQueueFamilyProperties(vk->physical_device, &n, props);
		const uint32_t fam = vk->main_queue != NULL ? vk->main_queue->family_index : UINT32_MAX;
		if (fam >= n || (props[fam].queueFlags & VK_QUEUE_COMPUTE_BIT) == 0) {
			goto fail;
		}
	}

	VkShaderModuleCreateInfo sm_ci = {
	    .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
	    .codeSize = sizeof(shaders_alpha_probe_comp),
	    .pCode = shaders_alpha_probe_comp,
	};
	ret = vk->vkCreateShaderModule(vk->device, &sm_ci, NULL, &p->shader);
	if (ret != VK_SUCCESS) {
		goto fail;
	}

	VkDescriptorSetLayoutBinding bindings[2] = {
	    {.binding = 0,
	     .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	     .descriptorCount = 1,
	     .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
	    {.binding = 1,
	     .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
	     .descriptorCount = 1,
	     .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
	};
	VkDescriptorSetLayoutCreateInfo dsl_ci = {
	    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
	    .bindingCount = 2,
	    .pBindings = bindings,
	};
	ret = vk->vkCreateDescriptorSetLayout(vk->device, &dsl_ci, NULL, &p->set_layout);
	if (ret != VK_SUCCESS) {
		goto fail;
	}

	VkPushConstantRange pcr = {
	    .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
	    .offset = 0,
	    .size = sizeof(struct probe_push),
	};
	VkPipelineLayoutCreateInfo pl_ci = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
	    .setLayoutCount = 1,
	    .pSetLayouts = &p->set_layout,
	    .pushConstantRangeCount = 1,
	    .pPushConstantRanges = &pcr,
	};
	ret = vk->vkCreatePipelineLayout(vk->device, &pl_ci, NULL, &p->pipeline_layout);
	if (ret != VK_SUCCESS) {
		goto fail;
	}

	VkComputePipelineCreateInfo cp_ci = {
	    .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
	    .stage =
	        {
	            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
	            .module = p->shader,
	            .pName = "main",
	        },
	    .layout = p->pipeline_layout,
	};
	ret = vk->vkCreateComputePipelines(vk->device, VK_NULL_HANDLE, 1, &cp_ci, NULL, &p->pipeline);
	if (ret != VK_SUCCESS) {
		goto fail;
	}

	VkSamplerCreateInfo s_ci = {
	    .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
	    .magFilter = VK_FILTER_NEAREST,
	    .minFilter = VK_FILTER_NEAREST,
	    .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
	    .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	    .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	    .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	    .maxLod = 0.0f,
	};
	ret = vk->vkCreateSampler(vk->device, &s_ci, NULL, &p->sampler);
	if (ret != VK_SUCCESS) {
		goto fail;
	}

	VkDescriptorPoolSize sizes[2] = {
	    {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = PROBE_RING},
	    {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = PROBE_RING},
	};
	VkDescriptorPoolCreateInfo dp_ci = {
	    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
	    .maxSets = PROBE_RING,
	    .poolSizeCount = 2,
	    .pPoolSizes = sizes,
	};
	ret = vk->vkCreateDescriptorPool(vk->device, &dp_ci, NULL, &p->pool);
	if (ret != VK_SUCCESS) {
		goto fail;
	}
	VkDescriptorSetLayout layouts[PROBE_RING];
	for (uint32_t i = 0; i < PROBE_RING; i++) {
		layouts[i] = p->set_layout;
	}
	VkDescriptorSetAllocateInfo ds_ai = {
	    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
	    .descriptorPool = p->pool,
	    .descriptorSetCount = PROBE_RING,
	    .pSetLayouts = layouts,
	};
	ret = vk->vkAllocateDescriptorSets(vk->device, &ds_ai, p->sets);
	if (ret != VK_SUCCESS) {
		goto fail;
	}

	if (!vk_buffer_init(vk, (VkDeviceSize)PROBE_RING * PROBE_SLOT_BYTES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
	                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &p->buffer,
	                    &p->memory)) {
		goto fail;
	}
	void *mapped = NULL;
	ret = vk->vkMapMemory(vk->device, p->memory, 0, VK_WHOLE_SIZE, 0, &mapped);
	if (ret != VK_SUCCESS || mapped == NULL) {
		goto fail;
	}
	p->mapped = (uint8_t *)mapped;
	memset(p->mapped, 0, (size_t)PROBE_RING * PROBE_SLOT_BYTES);

	// The storage-buffer half of each set never changes: bind it once.
	for (uint32_t i = 0; i < PROBE_RING; i++) {
		VkDescriptorBufferInfo bi = {
		    .buffer = p->buffer,
		    .offset = (VkDeviceSize)i * PROBE_SLOT_BYTES,
		    .range = 2 * sizeof(uint32_t),
		};
		VkWriteDescriptorSet w = {
		    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		    .dstSet = p->sets[i],
		    .dstBinding = 1,
		    .descriptorCount = 1,
		    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		    .pBufferInfo = &bi,
		};
		vk->vkUpdateDescriptorSets(vk->device, 1, &w, 0, NULL);
	}

	return p;

fail:
	U_LOG_W(
	    "lazy transparency: alpha probe setup failed (VkResult %d) — transparency stays active for the "
	    "whole session, as before",
	    (int)ret);
	comp_vk_alpha_probe_destroy(&p);
	return NULL;
}

bool
comp_vk_alpha_probe_read(struct comp_vk_alpha_probe *p, struct comp_vk_alpha_probe_result *out)
{
	if (p == NULL || p->read_upto >= p->recorded) {
		return false;
	}
	// Only the newest recorded probe matters; anything older is superseded.
	const uint64_t idx = p->recorded - 1;
	const uint32_t *slot = (const uint32_t *)(p->mapped + (idx % PROBE_RING) * PROBE_SLOT_BYTES);
	out->transparent_blocks = slot[0];
	out->total_blocks = slot[1];
	p->read_upto = p->recorded;
	return true;
}

void
comp_vk_alpha_probe_record(
    struct comp_vk_alpha_probe *p, VkCommandBuffer cmd, VkImageView view, uint32_t width, uint32_t height)
{
	if (p == NULL || cmd == VK_NULL_HANDLE || view == VK_NULL_HANDLE || width == 0 || height == 0) {
		return;
	}
	struct vk_bundle *vk = p->vk;
	const uint32_t slot = (uint32_t)(p->recorded % PROBE_RING);

	// Zero the slot from the host. Its previous user is PROBE_RING frames old
	// and fenced; vkQueueSubmit makes host-coherent writes visible to the
	// device, so no GPU-side clear is needed.
	memset(p->mapped + (size_t)slot * PROBE_SLOT_BYTES, 0, 2 * sizeof(uint32_t));

	VkDescriptorImageInfo ii = {
	    .sampler = p->sampler,
	    .imageView = view,
	    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};
	VkWriteDescriptorSet w = {
	    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	    .dstSet = p->sets[slot],
	    .dstBinding = 0,
	    .descriptorCount = 1,
	    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	    .pImageInfo = &ii,
	};
	vk->vkUpdateDescriptorSets(vk->device, 1, &w, 0, NULL);

	// Whatever produced the atlas this frame (the crop blit, the layer
	// renderer, or the app for a zero-copy swapchain image) → compute read.
	VkMemoryBarrier pre = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
	    .srcAccessMask =
	        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
	    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
	};
	vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
	                         &pre, 0, NULL, 0, NULL);

	vk->vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->pipeline);
	vk->vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->pipeline_layout, 0, 1, &p->sets[slot], 0,
	                            NULL);
	struct probe_push push = {
	    .extent = {width, height},
	    .stride = PROBE_STRIDE,
	    .threshold = PROBE_THRESHOLD,
	};
	vk->vkCmdPushConstants(cmd, p->pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);

	const uint32_t samples_x = (width + PROBE_STRIDE - 1) / PROBE_STRIDE;
	const uint32_t samples_y = (height + PROBE_STRIDE - 1) / PROBE_STRIDE;
	vk->vkCmdDispatch(cmd, (samples_x + 7) / 8, (samples_y + 7) / 8, 1);

	// Result → host, read after the compositor's per-frame fence. Nothing
	// later in the frame writes the atlas (the DP only samples it), so the
	// probe's read needs no ordering against later commands.
	VkMemoryBarrier post = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
	    .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
	};
	vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &post, 0,
	                         NULL, 0, NULL);

	p->recorded++;
}
