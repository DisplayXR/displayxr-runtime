// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The weave-rect cursor of the Vulkan XR_DXR_weave engines - the GPU
 *         pass (see comp_multi_weave_cursor_vk.h).
 * @author David Fattal
 * @ingroup comp_multi
 */

#include "comp_multi_weave_cursor_vk.h"

#include "util/u_logging.h"
#include "util/u_misc.h"

#include "vk/vk_helpers.h"

#include "shaders/weave_cursor_vert.h"
#include "shaders/weave_cursor_frag.h"

#include <math.h>
#include <string.h>

//! Must match weave_cursor.frag's push_constant block.
struct weave_cursor_push
{
	float uv_rect[4];
	float radius[2];
	float alpha;
	float pad;
};

#define WEAVE_CURSOR_SPRITE_FORMAT VK_FORMAT_R8G8B8A8_UNORM

static void
cursor_vk_destroy_sprite(struct comp_multi_weave_cursor_vk *cv, struct vk_bundle *vk)
{
	if (cv->sprite_view != VK_NULL_HANDLE) {
		vk->vkDestroyImageView(vk->device, cv->sprite_view, NULL);
		cv->sprite_view = VK_NULL_HANDLE;
	}
	if (cv->sprite != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, cv->sprite, NULL);
		cv->sprite = VK_NULL_HANDLE;
	}
	if (cv->sprite_memory != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, cv->sprite_memory, NULL);
		cv->sprite_memory = VK_NULL_HANDLE;
	}
	cv->sprite_w = 0;
	cv->sprite_h = 0;
	cv->sprite_ready = false;
	cv->sprite_serial = 0;
}

static bool
cursor_vk_init(struct comp_multi_weave_cursor_vk *cv, struct vk_bundle *vk, VkFormat target_format)
{
	VkResult ret;

	VkAttachmentDescription att = {
	    .format = target_format,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
	    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
	    .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
	    .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
	    .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	    .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	};
	VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
	VkSubpassDescription sub = {
	    .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
	    .colorAttachmentCount = 1,
	    .pColorAttachments = &ref,
	};
	VkRenderPassCreateInfo rp_ci = {
	    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
	    .attachmentCount = 1,
	    .pAttachments = &att,
	    .subpassCount = 1,
	    .pSubpasses = &sub,
	};
	ret = vk->vkCreateRenderPass(vk->device, &rp_ci, NULL, &cv->render_pass);
	if (ret != VK_SUCCESS) {
		return false;
	}

	VkShaderModuleCreateInfo vs_ci = {
	    .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
	    .codeSize = sizeof(weave_cursor_vert_spv),
	    .pCode = weave_cursor_vert_spv,
	};
	VkShaderModuleCreateInfo fs_ci = {
	    .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
	    .codeSize = sizeof(weave_cursor_frag_spv),
	    .pCode = weave_cursor_frag_spv,
	};
	if (vk->vkCreateShaderModule(vk->device, &vs_ci, NULL, &cv->vert) != VK_SUCCESS ||
	    vk->vkCreateShaderModule(vk->device, &fs_ci, NULL, &cv->frag) != VK_SUCCESS) {
		return false;
	}

	VkSamplerCreateInfo s_ci = {
	    .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
	    .magFilter = VK_FILTER_LINEAR,
	    .minFilter = VK_FILTER_LINEAR,
	    .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	    .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	    .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	};
	if (vk->vkCreateSampler(vk->device, &s_ci, NULL, &cv->sampler) != VK_SUCCESS) {
		return false;
	}

	VkDescriptorSetLayoutBinding binding = {
	    .binding = 0,
	    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	    .descriptorCount = 1,
	    .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
	};
	VkDescriptorSetLayoutCreateInfo dsl_ci = {
	    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
	    .bindingCount = 1,
	    .pBindings = &binding,
	};
	if (vk->vkCreateDescriptorSetLayout(vk->device, &dsl_ci, NULL, &cv->dsl) != VK_SUCCESS) {
		return false;
	}
	VkDescriptorPoolSize pool_size = {
	    .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	    .descriptorCount = 1,
	};
	VkDescriptorPoolCreateInfo pool_ci = {
	    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
	    .maxSets = 1,
	    .poolSizeCount = 1,
	    .pPoolSizes = &pool_size,
	};
	if (vk->vkCreateDescriptorPool(vk->device, &pool_ci, NULL, &cv->pool) != VK_SUCCESS) {
		return false;
	}
	VkDescriptorSetAllocateInfo set_ai = {
	    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
	    .descriptorPool = cv->pool,
	    .descriptorSetCount = 1,
	    .pSetLayouts = &cv->dsl,
	};
	if (vk->vkAllocateDescriptorSets(vk->device, &set_ai, &cv->set) != VK_SUCCESS) {
		return false;
	}

	VkPushConstantRange pcr = {
	    .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
	    .offset = 0,
	    .size = sizeof(struct weave_cursor_push),
	};
	VkPipelineLayoutCreateInfo pl_ci = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
	    .setLayoutCount = 1,
	    .pSetLayouts = &cv->dsl,
	    .pushConstantRangeCount = 1,
	    .pPushConstantRanges = &pcr,
	};
	if (vk->vkCreatePipelineLayout(vk->device, &pl_ci, NULL, &cv->layout) != VK_SUCCESS) {
		return false;
	}

	VkPipelineShaderStageCreateInfo stages[2] = {
	    {
	        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	        .stage = VK_SHADER_STAGE_VERTEX_BIT,
	        .module = cv->vert,
	        .pName = "main",
	    },
	    {
	        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	        .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
	        .module = cv->frag,
	        .pName = "main",
	    },
	};
	VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	VkPipelineInputAssemblyStateCreateInfo ia = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
	    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	};
	VkPipelineViewportStateCreateInfo vps = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
	    .viewportCount = 1,
	    .scissorCount = 1,
	};
	VkPipelineRasterizationStateCreateInfo rs = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
	    .polygonMode = VK_POLYGON_MODE_FILL,
	    .cullMode = VK_CULL_MODE_NONE,
	    .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
	    .lineWidth = 1.0f,
	};
	VkPipelineMultisampleStateCreateInfo ms = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
	    .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
	};
	// Premultiplied "over": every carrier is premultiplied.
	VkPipelineColorBlendAttachmentState blend = {
	    .blendEnable = VK_TRUE,
	    .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
	    .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
	    .colorBlendOp = VK_BLEND_OP_ADD,
	    .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
	    .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
	    .alphaBlendOp = VK_BLEND_OP_ADD,
	    .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
	                      VK_COLOR_COMPONENT_A_BIT,
	};
	VkPipelineColorBlendStateCreateInfo cb = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
	    .attachmentCount = 1,
	    .pAttachments = &blend,
	};
	VkPipelineDepthStencilStateCreateInfo ds = {.sType =
	                                                VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
	VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dyn = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
	    .dynamicStateCount = 2,
	    .pDynamicStates = dyn_states,
	};
	VkGraphicsPipelineCreateInfo p_ci = {
	    .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
	    .stageCount = 2,
	    .pStages = stages,
	    .pVertexInputState = &vi,
	    .pInputAssemblyState = &ia,
	    .pViewportState = &vps,
	    .pRasterizationState = &rs,
	    .pMultisampleState = &ms,
	    .pDepthStencilState = &ds,
	    .pColorBlendState = &cb,
	    .pDynamicState = &dyn,
	    .layout = cv->layout,
	    .renderPass = cv->render_pass,
	    .subpass = 0,
	};
	if (vk->vkCreateGraphicsPipelines(vk->device, VK_NULL_HANDLE, 1, &p_ci, NULL, &cv->pipeline) != VK_SUCCESS) {
		return false;
	}
	cv->target_format = target_format;
	return true;
}

static void
cursor_vk_destroy_objects(struct comp_multi_weave_cursor_vk *cv, struct vk_bundle *vk)
{
	if (cv->pipeline != VK_NULL_HANDLE) {
		vk->vkDestroyPipeline(vk->device, cv->pipeline, NULL);
	}
	if (cv->layout != VK_NULL_HANDLE) {
		vk->vkDestroyPipelineLayout(vk->device, cv->layout, NULL);
	}
	if (cv->pool != VK_NULL_HANDLE) {
		vk->vkDestroyDescriptorPool(vk->device, cv->pool, NULL); // frees the set
	}
	if (cv->dsl != VK_NULL_HANDLE) {
		vk->vkDestroyDescriptorSetLayout(vk->device, cv->dsl, NULL);
	}
	if (cv->sampler != VK_NULL_HANDLE) {
		vk->vkDestroySampler(vk->device, cv->sampler, NULL);
	}
	if (cv->vert != VK_NULL_HANDLE) {
		vk->vkDestroyShaderModule(vk->device, cv->vert, NULL);
	}
	if (cv->frag != VK_NULL_HANDLE) {
		vk->vkDestroyShaderModule(vk->device, cv->frag, NULL);
	}
	if (cv->render_pass != VK_NULL_HANDLE) {
		vk->vkDestroyRenderPass(vk->device, cv->render_pass, NULL);
	}
	cv->pipeline = VK_NULL_HANDLE;
	cv->layout = VK_NULL_HANDLE;
	cv->pool = VK_NULL_HANDLE;
	cv->set = VK_NULL_HANDLE;
	cv->dsl = VK_NULL_HANDLE;
	cv->sampler = VK_NULL_HANDLE;
	cv->vert = VK_NULL_HANDLE;
	cv->frag = VK_NULL_HANDLE;
	cv->render_pass = VK_NULL_HANDLE;
	cv->initialized = false;
}

static void
cursor_vk_barrier(struct vk_bundle *vk,
                  VkCommandBuffer cmd,
                  VkImage image,
                  VkImageLayout old_layout,
                  VkImageLayout new_layout,
                  VkAccessFlags src_access,
                  VkAccessFlags dst_access,
                  VkPipelineStageFlags src_stage,
                  VkPipelineStageFlags dst_stage)
{
	VkImageMemoryBarrier b = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = src_access,
	    .dstAccessMask = dst_access,
	    .oldLayout = old_layout,
	    .newLayout = new_layout,
	    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .image = image,
	    .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1},
	};
	vk->vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

//! (Re)create the sprite image at @p w x @p h and point the descriptor set at it.
static bool
cursor_vk_ensure_sprite(struct comp_multi_weave_cursor_vk *cv, struct vk_bundle *vk, uint32_t w, uint32_t h)
{
	if (cv->sprite != VK_NULL_HANDLE && cv->sprite_w == w && cv->sprite_h == h) {
		return true;
	}
	cursor_vk_destroy_sprite(cv, vk);
	const VkExtent2D extent = {.width = w, .height = h};
	VkResult ret = vk_create_image_simple(vk, extent, WEAVE_CURSOR_SPRITE_FORMAT,
	                                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
	                                      &cv->sprite_memory, &cv->sprite);
	if (ret != VK_SUCCESS) {
		cv->sprite = VK_NULL_HANDLE;
		cv->sprite_memory = VK_NULL_HANDLE;
		return false;
	}
	const VkImageSubresourceRange range = {
	    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1};
	ret =
	    vk_create_view(vk, cv->sprite, VK_IMAGE_VIEW_TYPE_2D, WEAVE_CURSOR_SPRITE_FORMAT, range, &cv->sprite_view);
	if (ret != VK_SUCCESS) {
		cursor_vk_destroy_sprite(cv, vk);
		return false;
	}
	cv->sprite_w = w;
	cv->sprite_h = h;
	const VkDescriptorImageInfo ii = {
	    .sampler = cv->sampler,
	    .imageView = cv->sprite_view,
	    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};
	const VkWriteDescriptorSet wr = {
	    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	    .dstSet = cv->set,
	    .dstBinding = 0,
	    .descriptorCount = 1,
	    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	    .pImageInfo = &ii,
	};
	vk->vkUpdateDescriptorSets(vk->device, 1, &wr, 0, NULL);
	return true;
}

//! Bring this draw's pixels into the sprite image (records the copy into @p cmd).
static bool
cursor_vk_load_pixels(struct comp_multi_weave_cursor_vk *cv,
                      struct vk_bundle *vk,
                      VkCommandBuffer cmd,
                      const struct comp_multi_weave_cursor_draw *d,
                      const struct comp_multi_weave_cursor_vk_source *source)
{
	if (d->rgba == NULL && (source == NULL || source->image == VK_NULL_HANDLE)) {
		return false; // a GPU-held carrier the engine could not supply
	}
	uint32_t w = d->image_w, h = d->image_h;
	if (d->rgba == NULL) {
		w = w < source->w ? w : source->w;
		h = h < source->h ? h : source->h;
	}
	if (w == 0 || h == 0 || !cursor_vk_ensure_sprite(cv, vk, w, h)) {
		return false;
	}
	if (d->rgba != NULL) {
		const VkDeviceSize size = (VkDeviceSize)w * h * 4;
		if (cv->staging_size < size) {
			if (cv->staging != VK_NULL_HANDLE) {
				vk->vkDestroyBuffer(vk->device, cv->staging, NULL);
				vk->vkFreeMemory(vk->device, cv->staging_memory, NULL);
				cv->staging = VK_NULL_HANDLE;
				cv->staging_memory = VK_NULL_HANDLE;
				cv->staging_size = 0;
			}
			if (!vk_buffer_init(vk, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			                    &cv->staging, &cv->staging_memory)) {
				cv->staging = VK_NULL_HANDLE;
				cv->staging_memory = VK_NULL_HANDLE;
				return false;
			}
			cv->staging_size = size;
		}
		void *mapped = NULL;
		if (vk->vkMapMemory(vk->device, cv->staging_memory, 0, size, 0, &mapped) != VK_SUCCESS ||
		    mapped == NULL) {
			return false;
		}
		memcpy(mapped, d->rgba, (size_t)size);
		vk->vkUnmapMemory(vk->device, cv->staging_memory);
	}

	cursor_vk_barrier(vk, cmd, cv->sprite,
	                  cv->sprite_ready ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
	                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, cv->sprite_ready ? VK_ACCESS_SHADER_READ_BIT : 0,
	                  VK_ACCESS_TRANSFER_WRITE_BIT,
	                  cv->sprite_ready ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	                  VK_PIPELINE_STAGE_TRANSFER_BIT);
	const VkImageSubresourceLayers layers = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1};
	if (d->rgba != NULL) {
		const VkBufferImageCopy region = {
		    .imageSubresource = layers,
		    .imageExtent = {w, h, 1},
		};
		vk->vkCmdCopyBufferToImage(cmd, cv->staging, cv->sprite, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
		                           &region);
	} else {
		// A 1:1 blit, not a copy: it converts the channel order of a BGRA
		// carrier into the RGBA sprite.
		const VkImageBlit blit = {
		    .srcSubresource = layers,
		    .srcOffsets = {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}},
		    .dstSubresource = layers,
		    .dstOffsets = {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}},
		};
		vk->vkCmdBlitImage(cmd, source->image, VK_IMAGE_LAYOUT_GENERAL, cv->sprite,
		                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
	}
	cursor_vk_barrier(vk, cmd, cv->sprite, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
	                  VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
	cv->sprite_ready = true;
	cv->sprite_serial = d->image_serial;
	return true;
}

bool
comp_multi_weave_cursor_vk_needs_pixels(const struct comp_multi_weave_cursor_vk *cv,
                                        const struct comp_multi_weave_cursor_draw *d)
{
	return d != NULL && d->quad_count > 0 && (!cv->sprite_ready || cv->sprite_serial != d->image_serial);
}

bool
comp_multi_weave_cursor_vk_draw(struct comp_multi_weave_cursor_vk *cv,
                                struct vk_bundle *vk,
                                VkCommandBuffer cmd,
                                const struct comp_multi_weave_cursor_draw *d,
                                const struct comp_multi_weave_cursor_vk_source *source,
                                VkImage target,
                                VkImageView target_view,
                                VkFormat target_format,
                                uint32_t target_w,
                                uint32_t target_h,
                                VkImageLayout layout,
                                VkAccessFlags access,
                                VkPipelineStageFlags stage)
{
	if (cv == NULL || d == NULL || d->quad_count == 0 || target == VK_NULL_HANDLE ||
	    target_view == VK_NULL_HANDLE || target_w == 0 || target_h == 0 || cv->failed) {
		return false;
	}
	if (cv->initialized && cv->target_format != target_format) {
		comp_multi_weave_cursor_vk_invalidate_target(cv, vk);
		cursor_vk_destroy_objects(cv, vk);
	}
	if (!cv->initialized) {
		if (!cursor_vk_init(cv, vk, target_format)) {
			cursor_vk_destroy_objects(cv, vk);
			cv->failed = true;
			U_LOG_W(
			    "[weave cursor] the Vulkan cursor pass could not be created - the OS cursor stays "
			    "(ADR-046 Amendment 6)");
			return false;
		}
		cv->initialized = true;
		// The set was just (re)allocated: it names no image yet.
		cursor_vk_destroy_sprite(cv, vk);
	}

	if (comp_multi_weave_cursor_vk_needs_pixels(cv, d) && !cursor_vk_load_pixels(cv, vk, cmd, d, source)) {
		return false;
	}

	if (cv->fb == VK_NULL_HANDLE || cv->fb_view != target_view || cv->fb_w != target_w || cv->fb_h != target_h) {
		comp_multi_weave_cursor_vk_invalidate_target(cv, vk);
		VkFramebufferCreateInfo fb_ci = {
		    .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		    .renderPass = cv->render_pass,
		    .attachmentCount = 1,
		    .pAttachments = &target_view,
		    .width = target_w,
		    .height = target_h,
		    .layers = 1,
		};
		if (vk->vkCreateFramebuffer(vk->device, &fb_ci, NULL, &cv->fb) != VK_SUCCESS) {
			cv->fb = VK_NULL_HANDLE;
			return false;
		}
		cv->fb_view = target_view;
		cv->fb_w = target_w;
		cv->fb_h = target_h;
	}

	cursor_vk_barrier(vk, cmd, target, layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, access,
	                  VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, stage,
	                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

	const VkRenderPassBeginInfo rp_bi = {
	    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
	    .renderPass = cv->render_pass,
	    .framebuffer = cv->fb,
	    .renderArea = {{0, 0}, {target_w, target_h}},
	};
	vk->vkCmdBeginRenderPass(cmd, &rp_bi, VK_SUBPASS_CONTENTS_INLINE);
	vk->vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, cv->pipeline);
	vk->vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, cv->layout, 0, 1, &cv->set, 0, NULL);
	uint32_t drawn = 0;
	for (uint32_t i = 0; i < d->quad_count; i++) {
		const struct comp_multi_weave_cursor_quad *q = &d->quads[i];
		// The scissor: the rect's part of this view's tile, inside the target.
		int32_t sx0 = q->clip[0] < 0 ? 0 : q->clip[0];
		int32_t sy0 = q->clip[1] < 0 ? 0 : q->clip[1];
		int32_t sx1 = q->clip[0] + q->clip[2];
		int32_t sy1 = q->clip[1] + q->clip[3];
		sx1 = sx1 > (int32_t)target_w ? (int32_t)target_w : sx1;
		sy1 = sy1 > (int32_t)target_h ? (int32_t)target_h : sy1;
		if (sx1 <= sx0 || sy1 <= sy0 || !(q->rect[2] > 0.0f) || !(q->rect[3] > 0.0f)) {
			continue;
		}
		const VkViewport vp = {
		    .x = q->rect[0],
		    .y = q->rect[1],
		    .width = q->rect[2],
		    .height = q->rect[3],
		    .minDepth = 0.0f,
		    .maxDepth = 1.0f,
		};
		const VkRect2D sc = {{sx0, sy0}, {(uint32_t)(sx1 - sx0), (uint32_t)(sy1 - sy0)}};
		const struct weave_cursor_push pc = {
		    .uv_rect = {q->uv[0], q->uv[1], q->uv[2], q->uv[3]},
		    .radius = {d->radius_uv[0], d->radius_uv[1]},
		    .alpha = d->alpha,
		};
		vk->vkCmdSetViewport(cmd, 0, 1, &vp);
		vk->vkCmdSetScissor(cmd, 0, 1, &sc);
		vk->vkCmdPushConstants(cmd, cv->layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
		vk->vkCmdDraw(cmd, 3, 1, 0, 0);
		drawn++;
	}
	vk->vkCmdEndRenderPass(cmd);

	cursor_vk_barrier(vk, cmd, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, layout,
	                  VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, access,
	                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, stage);
	return drawn > 0;
}

void
comp_multi_weave_cursor_vk_invalidate_target(struct comp_multi_weave_cursor_vk *cv, struct vk_bundle *vk)
{
	if (cv == NULL || vk == NULL) {
		return;
	}
	if (cv->fb != VK_NULL_HANDLE) {
		vk->vkDestroyFramebuffer(vk->device, cv->fb, NULL);
		cv->fb = VK_NULL_HANDLE;
	}
	cv->fb_view = VK_NULL_HANDLE;
	cv->fb_w = 0;
	cv->fb_h = 0;
}

void
comp_multi_weave_cursor_vk_release_sprite(struct comp_multi_weave_cursor_vk *cv, struct vk_bundle *vk)
{
	if (cv == NULL || vk == NULL) {
		return;
	}
	cursor_vk_destroy_sprite(cv, vk);
	if (cv->staging != VK_NULL_HANDLE) {
		vk->vkDestroyBuffer(vk->device, cv->staging, NULL);
		vk->vkFreeMemory(vk->device, cv->staging_memory, NULL);
		cv->staging = VK_NULL_HANDLE;
		cv->staging_memory = VK_NULL_HANDLE;
		cv->staging_size = 0;
	}
}

void
comp_multi_weave_cursor_vk_fini(struct comp_multi_weave_cursor_vk *cv, struct vk_bundle *vk)
{
	if (cv == NULL || vk == NULL) {
		return;
	}
	comp_multi_weave_cursor_vk_invalidate_target(cv, vk);
	comp_multi_weave_cursor_vk_release_sprite(cv, vk);
	cursor_vk_destroy_objects(cv, vk);
	U_ZERO(cv);
}
