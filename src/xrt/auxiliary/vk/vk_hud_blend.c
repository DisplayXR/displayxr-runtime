// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Alpha-blended HUD overlay blit implementation.
 * @author David Fattal
 * @ingroup aux_vk
 */

#include "vk_hud_blend.h"

#include "util/u_logging.h"

#include <string.h>

/*
 * Embedded SPIR-V for HUD overlay shaders.
 *
 * Vertex: fullscreen triangle (3 vertices, no vertex buffer).
 *   uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
 *   gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
 *
 * Fragment: sample hud_tex at uv, output with alpha.
 *   fragColor = texture(hud_tex, uv);
 */

// clang-format off
static const uint32_t hud_vert_spv[] = {
	0x07230203, 0x00010000, 0x000b0008, 0x0000002b, 0x00000000, 0x00020011,
	0x00000001, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
	0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0008000f, 0x00000000,
	0x00000004, 0x6e69616d, 0x00000000, 0x00000009, 0x0000000c, 0x0000001d,
	0x00030003, 0x00000002, 0x000001c2, 0x00040005, 0x00000004, 0x6e69616d,
	0x00000000, 0x00030005, 0x00000009, 0x00007675, 0x00060005, 0x0000000c,
	0x565f6c67, 0x65747265, 0x646e4978, 0x00007865, 0x00060005, 0x0000001b,
	0x505f6c67, 0x65567265, 0x78657472, 0x00000000, 0x00060006, 0x0000001b,
	0x00000000, 0x505f6c67, 0x7469736f, 0x006e6f69, 0x00070006, 0x0000001b,
	0x00000001, 0x505f6c67, 0x746e696f, 0x657a6953, 0x00000000, 0x00070006,
	0x0000001b, 0x00000002, 0x435f6c67, 0x4470696c, 0x61747369, 0x0065636e,
	0x00070006, 0x0000001b, 0x00000003, 0x435f6c67, 0x446c6c75, 0x61747369,
	0x0065636e, 0x00030005, 0x0000001d, 0x00000000, 0x00040047, 0x00000009,
	0x0000001e, 0x00000000, 0x00040047, 0x0000000c, 0x0000000b, 0x0000002a,
	0x00030047, 0x0000001b, 0x00000002, 0x00050048, 0x0000001b, 0x00000000,
	0x0000000b, 0x00000000, 0x00050048, 0x0000001b, 0x00000001, 0x0000000b,
	0x00000001, 0x00050048, 0x0000001b, 0x00000002, 0x0000000b, 0x00000003,
	0x00050048, 0x0000001b, 0x00000003, 0x0000000b, 0x00000004, 0x00020013,
	0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006,
	0x00000020, 0x00040017, 0x00000007, 0x00000006, 0x00000002, 0x00040020,
	0x00000008, 0x00000003, 0x00000007, 0x0004003b, 0x00000008, 0x00000009,
	0x00000003, 0x00040015, 0x0000000a, 0x00000020, 0x00000001, 0x00040020,
	0x0000000b, 0x00000001, 0x0000000a, 0x0004003b, 0x0000000b, 0x0000000c,
	0x00000001, 0x0004002b, 0x0000000a, 0x0000000e, 0x00000001, 0x0004002b,
	0x0000000a, 0x00000010, 0x00000002, 0x00040017, 0x00000017, 0x00000006,
	0x00000004, 0x00040015, 0x00000018, 0x00000020, 0x00000000, 0x0004002b,
	0x00000018, 0x00000019, 0x00000001, 0x0004001c, 0x0000001a, 0x00000006,
	0x00000019, 0x0006001e, 0x0000001b, 0x00000017, 0x00000006, 0x0000001a,
	0x0000001a, 0x00040020, 0x0000001c, 0x00000003, 0x0000001b, 0x0004003b,
	0x0000001c, 0x0000001d, 0x00000003, 0x0004002b, 0x0000000a, 0x0000001e,
	0x00000000, 0x0004002b, 0x00000006, 0x00000020, 0x40000000, 0x0004002b,
	0x00000006, 0x00000022, 0x3f800000, 0x0004002b, 0x00000006, 0x00000025,
	0x00000000, 0x00040020, 0x00000029, 0x00000003, 0x00000017, 0x00050036,
	0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005,
	0x0004003d, 0x0000000a, 0x0000000d, 0x0000000c, 0x000500c4, 0x0000000a,
	0x0000000f, 0x0000000d, 0x0000000e, 0x000500c7, 0x0000000a, 0x00000011,
	0x0000000f, 0x00000010, 0x0004006f, 0x00000006, 0x00000012, 0x00000011,
	0x0004003d, 0x0000000a, 0x00000013, 0x0000000c, 0x000500c7, 0x0000000a,
	0x00000014, 0x00000013, 0x00000010, 0x0004006f, 0x00000006, 0x00000015,
	0x00000014, 0x00050050, 0x00000007, 0x00000016, 0x00000012, 0x00000015,
	0x0003003e, 0x00000009, 0x00000016, 0x0004003d, 0x00000007, 0x0000001f,
	0x00000009, 0x0005008e, 0x00000007, 0x00000021, 0x0000001f, 0x00000020,
	0x00050050, 0x00000007, 0x00000023, 0x00000022, 0x00000022, 0x00050083,
	0x00000007, 0x00000024, 0x00000021, 0x00000023, 0x00050051, 0x00000006,
	0x00000026, 0x00000024, 0x00000000, 0x00050051, 0x00000006, 0x00000027,
	0x00000024, 0x00000001, 0x00070050, 0x00000017, 0x00000028, 0x00000026,
	0x00000027, 0x00000025, 0x00000022, 0x00050041, 0x00000029, 0x0000002a,
	0x0000001d, 0x0000001e, 0x0003003e, 0x0000002a, 0x00000028, 0x000100fd,
	0x00010038,
};

static const uint32_t hud_frag_spv[] = {
	0x07230203, 0x00010000, 0x000b0008, 0x00000014, 0x00000000, 0x00020011,
	0x00000001, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
	0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0007000f, 0x00000004,
	0x00000004, 0x6e69616d, 0x00000000, 0x00000009, 0x00000011, 0x00030010,
	0x00000004, 0x00000007, 0x00030003, 0x00000002, 0x000001c2, 0x00040005,
	0x00000004, 0x6e69616d, 0x00000000, 0x00050005, 0x00000009, 0x67617266,
	0x6f6c6f43, 0x00000072, 0x00040005, 0x0000000d, 0x5f647568, 0x00786574,
	0x00030005, 0x00000011, 0x00007675, 0x00040047, 0x00000009, 0x0000001e,
	0x00000000, 0x00040047, 0x0000000d, 0x00000021, 0x00000000, 0x00040047,
	0x0000000d, 0x00000022, 0x00000000, 0x00040047, 0x00000011, 0x0000001e,
	0x00000000, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002,
	0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006,
	0x00000004, 0x00040020, 0x00000008, 0x00000003, 0x00000007, 0x0004003b,
	0x00000008, 0x00000009, 0x00000003, 0x00090019, 0x0000000a, 0x00000006,
	0x00000001, 0x00000000, 0x00000000, 0x00000000, 0x00000001, 0x00000000,
	0x0003001b, 0x0000000b, 0x0000000a, 0x00040020, 0x0000000c, 0x00000000,
	0x0000000b, 0x0004003b, 0x0000000c, 0x0000000d, 0x00000000, 0x00040017,
	0x0000000f, 0x00000006, 0x00000002, 0x00040020, 0x00000010, 0x00000001,
	0x0000000f, 0x0004003b, 0x00000010, 0x00000011, 0x00000001, 0x00050036,
	0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005,
	0x0004003d, 0x0000000b, 0x0000000e, 0x0000000d, 0x0004003d, 0x0000000f,
	0x00000012, 0x00000011, 0x00050057, 0x00000007, 0x00000013, 0x0000000e,
	0x00000012, 0x0003003e, 0x00000009, 0x00000013, 0x000100fd, 0x00010038,
};
// clang-format on


bool
vk_hud_blend_init(struct vk_hud_blend *blend,
                   struct vk_bundle *vk,
                   VkFormat target_fmt,
                   VkImage hud_image,
                   uint32_t hud_w,
                   uint32_t hud_h)
{
	VkResult ret;

	// Shader modules from embedded SPIR-V
	VkShaderModuleCreateInfo vert_ci = {
	    .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
	    .codeSize = sizeof(hud_vert_spv),
	    .pCode = hud_vert_spv,
	};
	ret = vk->vkCreateShaderModule(vk->device, &vert_ci, NULL, &blend->vert_mod);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[HUD blend] Failed to create vert shader: %d", ret);
		return false;
	}

	VkShaderModuleCreateInfo frag_ci = {
	    .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
	    .codeSize = sizeof(hud_frag_spv),
	    .pCode = hud_frag_spv,
	};
	ret = vk->vkCreateShaderModule(vk->device, &frag_ci, NULL, &blend->frag_mod);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[HUD blend] Failed to create frag shader: %d", ret);
		return false;
	}

	// Sampler (nearest for crisp pixel text)
	VkSamplerCreateInfo samp_ci = {
	    .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
	    .magFilter = VK_FILTER_NEAREST,
	    .minFilter = VK_FILTER_NEAREST,
	    .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	    .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	};
	ret = vk->vkCreateSampler(vk->device, &samp_ci, NULL, &blend->sampler);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[HUD blend] Failed to create sampler: %d", ret);
		return false;
	}

	// HUD image view
	VkImageViewCreateInfo view_ci = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
	    .image = hud_image,
	    .viewType = VK_IMAGE_VIEW_TYPE_2D,
	    .format = VK_FORMAT_R8G8B8A8_UNORM,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	ret = vk->vkCreateImageView(vk->device, &view_ci, NULL, &blend->hud_view);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[HUD blend] Failed to create HUD image view: %d", ret);
		return false;
	}

	// Descriptor set layout (one combined image sampler)
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
	ret = vk->vkCreateDescriptorSetLayout(vk->device, &dsl_ci, NULL, &blend->desc_layout);
	if (ret != VK_SUCCESS) {
		return false;
	}

	// Pipeline layout
	VkPipelineLayoutCreateInfo pl_ci = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
	    .setLayoutCount = 1,
	    .pSetLayouts = &blend->desc_layout,
	};
	ret = vk->vkCreatePipelineLayout(vk->device, &pl_ci, NULL, &blend->pipe_layout);
	if (ret != VK_SUCCESS) {
		return false;
	}

	// Descriptor pool + set
	VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
	VkDescriptorPoolCreateInfo dp_ci = {
	    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
	    .maxSets = 1,
	    .poolSizeCount = 1,
	    .pPoolSizes = &pool_size,
	};
	ret = vk->vkCreateDescriptorPool(vk->device, &dp_ci, NULL, &blend->desc_pool);
	if (ret != VK_SUCCESS) {
		return false;
	}

	VkDescriptorSetAllocateInfo ds_ai = {
	    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
	    .descriptorPool = blend->desc_pool,
	    .descriptorSetCount = 1,
	    .pSetLayouts = &blend->desc_layout,
	};
	ret = vk->vkAllocateDescriptorSets(vk->device, &ds_ai, &blend->desc_set);
	if (ret != VK_SUCCESS) {
		return false;
	}

	// Write descriptor set: HUD texture
	VkDescriptorImageInfo img_info = {
	    .sampler = blend->sampler,
	    .imageView = blend->hud_view,
	    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};
	VkWriteDescriptorSet write = {
	    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	    .dstSet = blend->desc_set,
	    .dstBinding = 0,
	    .descriptorCount = 1,
	    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	    .pImageInfo = &img_info,
	};
	vk->vkUpdateDescriptorSets(vk->device, 1, &write, 0, NULL);

	// Render pass: single color attachment, LOAD_OP_LOAD, STORE_OP_STORE
	VkAttachmentDescription att = {
	    .format = target_fmt,
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
	ret = vk->vkCreateRenderPass(vk->device, &rp_ci, NULL, &blend->render_pass);
	if (ret != VK_SUCCESS) {
		return false;
	}

	// Graphics pipeline: fullscreen triangle, alpha blending, dynamic viewport/scissor
	VkPipelineShaderStageCreateInfo stages[2] = {
	    {
	        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	        .stage = VK_SHADER_STAGE_VERTEX_BIT,
	        .module = blend->vert_mod,
	        .pName = "main",
	    },
	    {
	        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	        .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
	        .module = blend->frag_mod,
	        .pName = "main",
	    },
	};

	VkPipelineVertexInputStateCreateInfo vi = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	VkPipelineInputAssemblyStateCreateInfo ia = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
	    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
	VkPipelineViewportStateCreateInfo vps = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
	    .viewportCount = 1,
	    .scissorCount = 1};
	VkPipelineRasterizationStateCreateInfo rs = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
	    .polygonMode = VK_POLYGON_MODE_FILL,
	    .cullMode = VK_CULL_MODE_NONE,
	    .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
	    .lineWidth = 1.0f};
	VkPipelineMultisampleStateCreateInfo ms = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
	    .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
	VkPipelineColorBlendAttachmentState blend_att = {
	    .blendEnable = VK_TRUE,
	    .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
	                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
	    .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
	    .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
	    .colorBlendOp = VK_BLEND_OP_ADD,
	    .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
	    .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
	    .alphaBlendOp = VK_BLEND_OP_ADD};
	VkPipelineColorBlendStateCreateInfo cb = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
	    .attachmentCount = 1,
	    .pAttachments = &blend_att};
	VkPipelineDepthStencilStateCreateInfo ds = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
	VkDynamicState dyn_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dyn = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
	    .dynamicStateCount = 2,
	    .pDynamicStates = dyn_states};

	VkGraphicsPipelineCreateInfo pipe_ci = {
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
	    .layout = blend->pipe_layout,
	    .renderPass = blend->render_pass,
	    .subpass = 0,
	};
	ret = vk->vkCreateGraphicsPipelines(vk->device, VK_NULL_HANDLE, 1, &pipe_ci, NULL, &blend->pipeline);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[HUD blend] Failed to create pipeline: %d", ret);
		return false;
	}

	blend->initialized = true;
	return true;
}

void
vk_hud_blend_draw(struct vk_hud_blend *blend,
                   struct vk_bundle *vk,
                   VkCommandBuffer cmd,
                   VkImageView target_view,
                   VkImage target_image,
                   uint32_t fb_w,
                   uint32_t fb_h,
                   uint32_t hud_w,
                   uint32_t hud_h)
{
	if (!blend->initialized) {
		return;
	}

	// HUD position: bottom-left with 10px margin
	uint32_t dst_x = 10;
	uint32_t dst_y = (fb_h > hud_h + 10) ? (fb_h - hud_h - 10) : 0;

	// Invalidate cache if dimensions changed (e.g. swapchain resize)
	if (blend->fb_w != fb_w || blend->fb_h != fb_h) {
		for (uint32_t i = 0; i < blend->fb_count; i++) {
			vk->vkDestroyFramebuffer(vk->device, blend->cached_fbs[i], NULL);
		}
		blend->fb_count = 0;
		blend->fb_w = fb_w;
		blend->fb_h = fb_h;
	}

	// Find or create cached framebuffer for this swapchain image view
	VkFramebuffer fb = VK_NULL_HANDLE;
	for (uint32_t i = 0; i < blend->fb_count; i++) {
		if (blend->cached_views[i] == target_view) {
			fb = blend->cached_fbs[i];
			break;
		}
	}
	if (fb == VK_NULL_HANDLE && blend->fb_count < VK_HUD_BLEND_MAX_FBS) {
		VkFramebufferCreateInfo fb_ci = {
		    .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		    .renderPass = blend->render_pass,
		    .attachmentCount = 1,
		    .pAttachments = &target_view,
		    .width = fb_w,
		    .height = fb_h,
		    .layers = 1,
		};
		VkResult ret = vk->vkCreateFramebuffer(vk->device, &fb_ci, NULL, &fb);
		if (ret != VK_SUCCESS) {
			return;
		}
		blend->cached_views[blend->fb_count] = target_view;
		blend->cached_fbs[blend->fb_count] = fb;
		blend->fb_count++;
	}
	if (fb == VK_NULL_HANDLE) {
		return;
	}

	// Transition swapchain: PRESENT_SRC -> COLOR_ATTACHMENT_OPTIMAL
	// srcAccessMask must include all write types that may have touched the
	// target before this barrier: TRANSFER_WRITE (mono blit path) and
	// COLOR_ATTACHMENT_WRITE (stereo weaver/render pass path).  Without
	// TRANSFER_WRITE, the mono blit data is not made available for the
	// layout transition, causing LOAD_OP_LOAD to read stale content.
	VkImageMemoryBarrier to_color = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	    .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
	    .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	    .image = target_image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
	                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &to_color);

	// Begin render pass (LOAD_OP_LOAD preserves 3D content)
	VkRenderPassBeginInfo rp_bi = {
	    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
	    .renderPass = blend->render_pass,
	    .framebuffer = fb,
	    .renderArea = {{0, 0}, {fb_w, fb_h}},
	};
	vk->vkCmdBeginRenderPass(cmd, &rp_bi, VK_SUBPASS_CONTENTS_INLINE);

	// Set viewport and scissor to HUD region only
	VkViewport vp = {(float)dst_x, (float)dst_y, (float)hud_w, (float)hud_h, 0.0f, 1.0f};
	vk->vkCmdSetViewport(cmd, 0, 1, &vp);
	VkRect2D scissor = {{(int32_t)dst_x, (int32_t)dst_y}, {hud_w, hud_h}};
	vk->vkCmdSetScissor(cmd, 0, 1, &scissor);

	// Draw fullscreen triangle (clipped to HUD region by viewport)
	vk->vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, blend->pipeline);
	vk->vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, blend->pipe_layout, 0, 1,
	                             &blend->desc_set, 0, NULL);
	vk->vkCmdDraw(cmd, 3, 1, 0, 0);

	vk->vkCmdEndRenderPass(cmd);

	// Transition swapchain: COLOR_ATTACHMENT_OPTIMAL -> PRESENT_SRC
	VkImageMemoryBarrier to_present = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	    .dstAccessMask = 0,
	    .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	    .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
	    .image = target_image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &to_present);
}

void
vk_hud_blend_fini(struct vk_hud_blend *blend, struct vk_bundle *vk)
{
	if (!blend->initialized) {
		return;
	}

	for (uint32_t i = 0; i < blend->fb_count; i++) {
		vk->vkDestroyFramebuffer(vk->device, blend->cached_fbs[i], NULL);
	}

	if (blend->pipeline != VK_NULL_HANDLE) {
		vk->vkDestroyPipeline(vk->device, blend->pipeline, NULL);
	}
	if (blend->render_pass != VK_NULL_HANDLE) {
		vk->vkDestroyRenderPass(vk->device, blend->render_pass, NULL);
	}
	if (blend->pipe_layout != VK_NULL_HANDLE) {
		vk->vkDestroyPipelineLayout(vk->device, blend->pipe_layout, NULL);
	}
	if (blend->desc_pool != VK_NULL_HANDLE) {
		vk->vkDestroyDescriptorPool(vk->device, blend->desc_pool, NULL);
	}
	if (blend->desc_layout != VK_NULL_HANDLE) {
		vk->vkDestroyDescriptorSetLayout(vk->device, blend->desc_layout, NULL);
	}
	if (blend->sampler != VK_NULL_HANDLE) {
		vk->vkDestroySampler(vk->device, blend->sampler, NULL);
	}
	if (blend->hud_view != VK_NULL_HANDLE) {
		vk->vkDestroyImageView(vk->device, blend->hud_view, NULL);
	}
	if (blend->vert_mod != VK_NULL_HANDLE) {
		vk->vkDestroyShaderModule(vk->device, blend->vert_mod, NULL);
	}
	if (blend->frag_mod != VK_NULL_HANDLE) {
		vk->vkDestroyShaderModule(vk->device, blend->frag_mod, NULL);
	}

	memset(blend, 0, sizeof(*blend));
}
