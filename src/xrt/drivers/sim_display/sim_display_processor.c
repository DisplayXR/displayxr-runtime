// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Simulation display processor: SBS, anaglyph, alpha-blend output.
 *
 * Implements side-by-side, anaglyph, and alpha-blend stereo output
 * modes for development and testing on regular 2D displays.
 *
 * SBS mode is a no-op (compositor viewport config handles layout).
 * Anaglyph and blend modes use fullscreen-triangle fragment shaders.
 *
 * @author David Fattal
 * @ingroup drv_sim_display
 */

#include "sim_display_interface.h"

#include "xrt/xrt_display_processor.h"

#include "vk/vk_helpers.h"
#include "util/u_logging.h"

#include <stdlib.h>
#include <string.h>

// SPIR-V shader headers (generated at build time by spirv_shaders())
#include "sim_display/shaders/fullscreen.vert.h"
#include "sim_display/shaders/anaglyph.frag.h"
#include "sim_display/shaders/blend.frag.h"


/*!
 * Implementation struct for the simulation display processor.
 */
struct sim_display_processor
{
	struct xrt_display_processor base;
	enum sim_display_output_mode mode;
	struct vk_bundle *vk;           //!< NULL for SBS mode
	VkRenderPass render_pass;
	VkPipeline pipeline;
	VkPipelineLayout pipeline_layout;
	VkDescriptorSetLayout desc_layout;
	VkDescriptorPool desc_pool;
	VkSampler sampler;
};

static inline struct sim_display_processor *
sim_display_processor(struct xrt_display_processor *xdp)
{
	return (struct sim_display_processor *)xdp;
}


/*
 *
 * SBS output: no-op. The compositor's viewport config handles side-by-side layout.
 *
 */

static void
sim_dp_process_views_sbs(struct xrt_display_processor *xdp,
                         VkCommandBuffer cmd_buffer,
                         VkImageView left_view,
                         VkImageView right_view,
                         uint32_t view_width,
                         uint32_t view_height,
                         VkFormat_XDP view_format,
                         VkFramebuffer target_fb,
                         uint32_t target_width,
                         uint32_t target_height,
                         VkFormat_XDP target_format)
{
	(void)xdp;
	(void)cmd_buffer;
	(void)left_view;
	(void)right_view;
	(void)view_width;
	(void)view_height;
	(void)view_format;
	(void)target_fb;
	(void)target_width;
	(void)target_height;
	(void)target_format;
}


/*
 *
 * Anaglyph/blend output: fullscreen triangle with fragment shader.
 *
 */

static void
sim_dp_process_views_shader(struct xrt_display_processor *xdp,
                            VkCommandBuffer cmd_buffer,
                            VkImageView left_view,
                            VkImageView right_view,
                            uint32_t view_width,
                            uint32_t view_height,
                            VkFormat_XDP view_format,
                            VkFramebuffer target_fb,
                            uint32_t target_width,
                            uint32_t target_height,
                            VkFormat_XDP target_format)
{
	struct sim_display_processor *sdp = sim_display_processor(xdp);
	struct vk_bundle *vk = sdp->vk;

	if (vk == NULL || sdp->pipeline == VK_NULL_HANDLE) {
		return;
	}

	// Allocate a descriptor set from the pool
	VkDescriptorSetAllocateInfo alloc_info = {
	    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
	    .descriptorPool = sdp->desc_pool,
	    .descriptorSetCount = 1,
	    .pSetLayouts = &sdp->desc_layout,
	};

	VkDescriptorSet desc_set;
	VkResult ret = vk->vkAllocateDescriptorSets(vk->device, &alloc_info, &desc_set);
	if (ret != VK_SUCCESS) {
		U_LOG_E("sim_display: Failed to allocate descriptor set: %d", ret);
		return;
	}

	// Update descriptor set with left and right image views
	VkDescriptorImageInfo image_infos[2] = {
	    {
	        .sampler = sdp->sampler,
	        .imageView = left_view,
	        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	    },
	    {
	        .sampler = sdp->sampler,
	        .imageView = right_view,
	        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	    },
	};

	VkWriteDescriptorSet writes[2] = {
	    {
	        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	        .dstSet = desc_set,
	        .dstBinding = 0,
	        .descriptorCount = 1,
	        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	        .pImageInfo = &image_infos[0],
	    },
	    {
	        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	        .dstSet = desc_set,
	        .dstBinding = 1,
	        .descriptorCount = 1,
	        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	        .pImageInfo = &image_infos[1],
	    },
	};

	vk->vkUpdateDescriptorSets(vk->device, 2, writes, 0, NULL);

	// Begin render pass
	VkClearValue clear_value = {.color = {{0.0f, 0.0f, 0.0f, 1.0f}}};
	VkRenderPassBeginInfo rp_begin = {
	    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
	    .renderPass = sdp->render_pass,
	    .framebuffer = target_fb,
	    .renderArea =
	        {
	            .offset = {0, 0},
	            .extent = {target_width, target_height},
	        },
	    .clearValueCount = 1,
	    .pClearValues = &clear_value,
	};

	vk->vkCmdBeginRenderPass(cmd_buffer, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

	// Bind pipeline and descriptor set
	vk->vkCmdBindPipeline(cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, sdp->pipeline);
	vk->vkCmdBindDescriptorSets(cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, sdp->pipeline_layout, 0, 1,
	                             &desc_set, 0, NULL);

	// Set dynamic viewport and scissor
	VkViewport viewport = {
	    .x = 0.0f,
	    .y = 0.0f,
	    .width = (float)target_width,
	    .height = (float)target_height,
	    .minDepth = 0.0f,
	    .maxDepth = 1.0f,
	};
	vk->vkCmdSetViewport(cmd_buffer, 0, 1, &viewport);

	VkRect2D scissor = {
	    .offset = {0, 0},
	    .extent = {target_width, target_height},
	};
	vk->vkCmdSetScissor(cmd_buffer, 0, 1, &scissor);

	// Draw fullscreen triangle (3 vertices, no VBO)
	vk->vkCmdDraw(cmd_buffer, 3, 1, 0, 0);

	vk->vkCmdEndRenderPass(cmd_buffer);

	// Free descriptor set back to pool
	vk->vkFreeDescriptorSets(vk->device, sdp->desc_pool, 1, &desc_set);
}


/*
 *
 * Create Vulkan pipeline resources for anaglyph/blend modes.
 *
 */

static VkResult
create_shader_module(struct vk_bundle *vk, const uint32_t *code, size_t code_size, VkShaderModule *out_module)
{
	VkShaderModuleCreateInfo create_info = {
	    .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
	    .codeSize = code_size,
	    .pCode = code,
	};

	return vk->vkCreateShaderModule(vk->device, &create_info, NULL, out_module);
}

static bool
create_pipeline_resources(struct sim_display_processor *sdp, int32_t target_format)
{
	struct vk_bundle *vk = sdp->vk;
	VkResult ret;

	// 1. Create render pass (single color attachment, LOAD_OP_DONT_CARE since we write every pixel)
	VkAttachmentDescription color_attachment = {
	    .format = (VkFormat)target_format,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
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
	    .pAttachments = &color_attachment,
	    .subpassCount = 1,
	    .pSubpasses = &subpass,
	};

	ret = vk->vkCreateRenderPass(vk->device, &rp_info, NULL, &sdp->render_pass);
	if (ret != VK_SUCCESS) {
		U_LOG_E("sim_display: Failed to create render pass: %d", ret);
		return false;
	}

	// 2. Create descriptor set layout (2 combined image samplers)
	VkDescriptorSetLayoutBinding bindings[2] = {
	    {
	        .binding = 0,
	        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	        .descriptorCount = 1,
	        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
	    },
	    {
	        .binding = 1,
	        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	        .descriptorCount = 1,
	        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
	    },
	};

	VkDescriptorSetLayoutCreateInfo desc_layout_info = {
	    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
	    .bindingCount = 2,
	    .pBindings = bindings,
	};

	ret = vk->vkCreateDescriptorSetLayout(vk->device, &desc_layout_info, NULL, &sdp->desc_layout);
	if (ret != VK_SUCCESS) {
		U_LOG_E("sim_display: Failed to create descriptor set layout: %d", ret);
		return false;
	}

	// 3. Create pipeline layout
	VkPipelineLayoutCreateInfo pipe_layout_info = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
	    .setLayoutCount = 1,
	    .pSetLayouts = &sdp->desc_layout,
	};

	ret = vk->vkCreatePipelineLayout(vk->device, &pipe_layout_info, NULL, &sdp->pipeline_layout);
	if (ret != VK_SUCCESS) {
		U_LOG_E("sim_display: Failed to create pipeline layout: %d", ret);
		return false;
	}

	// 4. Create shader modules
	VkShaderModule vert_module = VK_NULL_HANDLE;
	VkShaderModule frag_module = VK_NULL_HANDLE;

	ret = create_shader_module(vk, sim_display_shaders_fullscreen_vert, sizeof(sim_display_shaders_fullscreen_vert),
	                           &vert_module);
	if (ret != VK_SUCCESS) {
		U_LOG_E("sim_display: Failed to create vertex shader module: %d", ret);
		return false;
	}

	const uint32_t *frag_code;
	size_t frag_size;
	if (sdp->mode == SIM_DISPLAY_OUTPUT_ANAGLYPH) {
		frag_code = sim_display_shaders_anaglyph_frag;
		frag_size = sizeof(sim_display_shaders_anaglyph_frag);
	} else {
		frag_code = sim_display_shaders_blend_frag;
		frag_size = sizeof(sim_display_shaders_blend_frag);
	}

	ret = create_shader_module(vk, frag_code, frag_size, &frag_module);
	if (ret != VK_SUCCESS) {
		U_LOG_E("sim_display: Failed to create fragment shader module: %d", ret);
		vk->vkDestroyShaderModule(vk->device, vert_module, NULL);
		return false;
	}

	// 5. Create graphics pipeline
	VkPipelineShaderStageCreateInfo stages[2] = {
	    {
	        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	        .stage = VK_SHADER_STAGE_VERTEX_BIT,
	        .module = vert_module,
	        .pName = "main",
	    },
	    {
	        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	        .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
	        .module = frag_module,
	        .pName = "main",
	    },
	};

	VkPipelineVertexInputStateCreateInfo vertex_input = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
	};

	VkPipelineInputAssemblyStateCreateInfo input_assembly = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
	    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	};

	VkPipelineViewportStateCreateInfo viewport_state = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
	    .viewportCount = 1,
	    .scissorCount = 1,
	};

	VkPipelineRasterizationStateCreateInfo rasterization = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
	    .polygonMode = VK_POLYGON_MODE_FILL,
	    .cullMode = VK_CULL_MODE_NONE,
	    .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
	    .lineWidth = 1.0f,
	};

	VkPipelineMultisampleStateCreateInfo multisample = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
	    .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
	};

	VkPipelineColorBlendAttachmentState blend_attachment = {
	    .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
	                      VK_COLOR_COMPONENT_A_BIT,
	};

	VkPipelineColorBlendStateCreateInfo color_blend = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
	    .attachmentCount = 1,
	    .pAttachments = &blend_attachment,
	};

	VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dynamic_state = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
	    .dynamicStateCount = 2,
	    .pDynamicStates = dynamic_states,
	};

	VkGraphicsPipelineCreateInfo pipeline_info = {
	    .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
	    .stageCount = 2,
	    .pStages = stages,
	    .pVertexInputState = &vertex_input,
	    .pInputAssemblyState = &input_assembly,
	    .pViewportState = &viewport_state,
	    .pRasterizationState = &rasterization,
	    .pMultisampleState = &multisample,
	    .pColorBlendState = &color_blend,
	    .pDynamicState = &dynamic_state,
	    .layout = sdp->pipeline_layout,
	    .renderPass = sdp->render_pass,
	    .subpass = 0,
	};

	ret = vk->vkCreateGraphicsPipelines(vk->device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &sdp->pipeline);

	// Shader modules no longer needed after pipeline creation
	vk->vkDestroyShaderModule(vk->device, vert_module, NULL);
	vk->vkDestroyShaderModule(vk->device, frag_module, NULL);

	if (ret != VK_SUCCESS) {
		U_LOG_E("sim_display: Failed to create graphics pipeline: %d", ret);
		return false;
	}

	// 6. Create sampler (linear filtering, clamp to edge)
	VkSamplerCreateInfo sampler_info = {
	    .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
	    .magFilter = VK_FILTER_LINEAR,
	    .minFilter = VK_FILTER_LINEAR,
	    .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
	    .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	    .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	    .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	    .maxLod = 1.0f,
	};

	ret = vk->vkCreateSampler(vk->device, &sampler_info, NULL, &sdp->sampler);
	if (ret != VK_SUCCESS) {
		U_LOG_E("sim_display: Failed to create sampler: %d", ret);
		return false;
	}

	// 7. Create descriptor pool (allow per-frame alloc/free)
	VkDescriptorPoolSize pool_size = {
	    .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	    .descriptorCount = 2, // One set with 2 samplers
	};

	VkDescriptorPoolCreateInfo pool_info = {
	    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
	    .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
	    .maxSets = 1,
	    .poolSizeCount = 1,
	    .pPoolSizes = &pool_size,
	};

	ret = vk->vkCreateDescriptorPool(vk->device, &pool_info, NULL, &sdp->desc_pool);
	if (ret != VK_SUCCESS) {
		U_LOG_E("sim_display: Failed to create descriptor pool: %d", ret);
		return false;
	}

	return true;
}


static void
sim_dp_destroy(struct xrt_display_processor *xdp)
{
	struct sim_display_processor *sdp = sim_display_processor(xdp);

	if (sdp->vk != NULL) {
		struct vk_bundle *vk = sdp->vk;

		if (sdp->desc_pool != VK_NULL_HANDLE) {
			vk->vkDestroyDescriptorPool(vk->device, sdp->desc_pool, NULL);
		}
		if (sdp->sampler != VK_NULL_HANDLE) {
			vk->vkDestroySampler(vk->device, sdp->sampler, NULL);
		}
		if (sdp->pipeline != VK_NULL_HANDLE) {
			vk->vkDestroyPipeline(vk->device, sdp->pipeline, NULL);
		}
		if (sdp->pipeline_layout != VK_NULL_HANDLE) {
			vk->vkDestroyPipelineLayout(vk->device, sdp->pipeline_layout, NULL);
		}
		if (sdp->desc_layout != VK_NULL_HANDLE) {
			vk->vkDestroyDescriptorSetLayout(vk->device, sdp->desc_layout, NULL);
		}
		if (sdp->render_pass != VK_NULL_HANDLE) {
			vk->vkDestroyRenderPass(vk->device, sdp->render_pass, NULL);
		}
	}

	free(sdp);
}


/*
 *
 * Exported creation function.
 *
 */

xrt_result_t
sim_display_processor_create(enum sim_display_output_mode mode,
                             struct vk_bundle *vk,
                             int32_t target_format,
                             struct xrt_display_processor **out_xdp)
{
	if (out_xdp == NULL) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	struct sim_display_processor *sdp = calloc(1, sizeof(*sdp));
	if (sdp == NULL) {
		return XRT_ERROR_ALLOCATION;
	}

	sdp->mode = mode;
	sdp->base.destroy = sim_dp_destroy;

	switch (mode) {
	case SIM_DISPLAY_OUTPUT_SBS:
		sdp->base.process_views = sim_dp_process_views_sbs;
		U_LOG_W("Created sim display processor: SBS mode");
		break;

	case SIM_DISPLAY_OUTPUT_ANAGLYPH:
	case SIM_DISPLAY_OUTPUT_BLEND:
		if (vk == NULL) {
			U_LOG_E("sim_display: Vulkan bundle required for anaglyph/blend modes");
			free(sdp);
			return XRT_ERROR_DEVICE_CREATION_FAILED;
		}
		sdp->vk = vk;
		sdp->base.process_views = sim_dp_process_views_shader;

		if (!create_pipeline_resources(sdp, target_format)) {
			U_LOG_E("sim_display: Failed to create pipeline resources");
			sim_dp_destroy(&sdp->base);
			return XRT_ERROR_VULKAN;
		}

		U_LOG_W("Created sim display processor: %s mode",
		        mode == SIM_DISPLAY_OUTPUT_ANAGLYPH ? "Anaglyph" : "Blend");
		break;

	default:
		U_LOG_E("Unknown sim display output mode: %d", (int)mode);
		free(sdp);
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	*out_xdp = &sdp->base;
	return XRT_SUCCESS;
}
