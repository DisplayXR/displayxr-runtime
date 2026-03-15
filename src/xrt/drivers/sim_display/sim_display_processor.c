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
#include "xrt/xrt_display_metrics.h"

#include "vk/vk_helpers.h"
#include "util/u_debug.h"
#include "util/u_logging.h"
#include "os/os_time.h"

#include <stdlib.h>
#include <string.h>

DEBUG_GET_ONCE_FLOAT_OPTION(sim_display_nominal_z_m, "SIM_DISPLAY_NOMINAL_Z_M", 0.60f)

// SPIR-V shader headers (generated at build time by spirv_shaders())
#include "sim_display/shaders/fullscreen.vert.h"
#include "sim_display/shaders/anaglyph.frag.h"
#include "sim_display/shaders/blend.frag.h"
#include "sim_display/shaders/sbs.frag.h"
#include "sim_display/shaders/squeezed_sbs.frag.h"
#include "sim_display/shaders/quad.frag.h"
#include "sim_display/shaders/passthrough.frag.h"


/*!
 * Implementation struct for the simulation display processor.
 */
struct sim_display_processor
{
	struct xrt_display_processor base;
	struct vk_bundle *vk;
	VkRenderPass render_pass;
	VkPipeline pipelines[6];        //!< One per output mode (SBS, anaglyph, blend, squeezed SBS, quad, passthrough)
	VkPipelineLayout pipeline_layout;
	VkDescriptorSetLayout desc_layout;
	VkDescriptorPool desc_pool;
	VkDescriptorSet desc_set;       //!< Persistent descriptor set (allocated once)
	VkSampler sampler;

	//! Nominal viewer parameters for faked eye positions.
	float ipd_m;
	float nominal_x_m;
	float nominal_y_m;
	float nominal_z_m;
};

static inline struct sim_display_processor *
sim_display_processor(struct xrt_display_processor *xdp)
{
	return (struct sim_display_processor *)xdp;
}


/*
 *
 * Anaglyph/blend output: fullscreen triangle with fragment shader.
 *
 */

/*!
 * Push constant data for tile layout parameters.
 */
struct tile_push_constants
{
	float inv_tile_columns;
	float inv_tile_rows;
	float tile_columns;
	float tile_rows;
};

static void
sim_dp_process_atlas(struct xrt_display_processor *xdp,
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
	(void)atlas_image; // sim_display uses atlas_view via shader sampling
	struct sim_display_processor *sdp = sim_display_processor(xdp);
	struct vk_bundle *vk = sdp->vk;

	// Read the current mode (may change at runtime via 1/2/3 keys)
	enum sim_display_output_mode mode = sim_display_get_output_mode();
	VkPipeline active_pipeline = sdp->pipelines[mode];

	if (vk == NULL || active_pipeline == VK_NULL_HANDLE) {
		return;
	}

	// Update persistent descriptor set with atlas image view
	VkDescriptorImageInfo image_info = {
	    .sampler = sdp->sampler,
	    .imageView = atlas_view,
	    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};

	VkWriteDescriptorSet write = {
	    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	    .dstSet = sdp->desc_set,
	    .dstBinding = 0,
	    .descriptorCount = 1,
	    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	    .pImageInfo = &image_info,
	};

	vk->vkUpdateDescriptorSets(vk->device, 1, &write, 0, NULL);

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
	vk->vkCmdBindPipeline(cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, active_pipeline);
	vk->vkCmdBindDescriptorSets(cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, sdp->pipeline_layout, 0, 1,
	                             &sdp->desc_set, 0, NULL);

	// Push tile layout constants to fragment shader
	struct tile_push_constants pc = {
	    .inv_tile_columns = 1.0f / (float)tile_columns,
	    .inv_tile_rows = 1.0f / (float)tile_rows,
	    .tile_columns = (float)tile_columns,
	    .tile_rows = (float)tile_rows,
	};
	vk->vkCmdPushConstants(cmd_buffer, sdp->pipeline_layout,
	                        VK_SHADER_STAGE_FRAGMENT_BIT, 0,
	                        sizeof(pc), &pc);

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

	// 1. Create render pass (single color attachment)
	//    initialLayout = UNDEFINED: image may be in any layout (PRESENT_SRC after present, etc.)
	//    finalLayout = PRESENT_SRC_KHR: image goes directly to presentation after display processing
	//    Using LOAD_OP_CLEAR to ensure entire framebuffer is initialized (diagnostic: magenta background)
	VkAttachmentDescription color_attachment = {
	    .format = (VkFormat)target_format,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
	    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
	    .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
	    .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	    .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
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

	// 2. Create descriptor set layout (1 combined image sampler for atlas)
	VkDescriptorSetLayoutBinding binding = {
	    .binding = 0,
	    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	    .descriptorCount = 1,
	    .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
	};

	VkDescriptorSetLayoutCreateInfo desc_layout_info = {
	    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
	    .bindingCount = 1,
	    .pBindings = &binding,
	};

	ret = vk->vkCreateDescriptorSetLayout(vk->device, &desc_layout_info, NULL, &sdp->desc_layout);
	if (ret != VK_SUCCESS) {
		U_LOG_E("sim_display: Failed to create descriptor set layout: %d", ret);
		return false;
	}

	// 3. Create pipeline layout (with push constants for tile params)
	VkPushConstantRange push_range = {
	    .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
	    .offset = 0,
	    .size = sizeof(struct tile_push_constants),
	};

	VkPipelineLayoutCreateInfo pipe_layout_info = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
	    .setLayoutCount = 1,
	    .pSetLayouts = &sdp->desc_layout,
	    .pushConstantRangeCount = 1,
	    .pPushConstantRanges = &push_range,
	};

	ret = vk->vkCreatePipelineLayout(vk->device, &pipe_layout_info, NULL, &sdp->pipeline_layout);
	if (ret != VK_SUCCESS) {
		U_LOG_E("sim_display: Failed to create pipeline layout: %d", ret);
		return false;
	}

	// 4. Create vertex shader module (shared by all modes)
	VkShaderModule vert_module = VK_NULL_HANDLE;
	ret = create_shader_module(vk, sim_display_shaders_fullscreen_vert, sizeof(sim_display_shaders_fullscreen_vert),
	                           &vert_module);
	if (ret != VK_SUCCESS) {
		U_LOG_E("sim_display: Failed to create vertex shader module: %d", ret);
		return false;
	}

	// 5. Create one graphics pipeline per output mode (SBS, anaglyph, blend)
	//    so runtime switching is instant (no pipeline recreation).
	//    All shader modules are created upfront and all pipelines are created
	//    in a single batch call to avoid potential driver issues with module
	//    handle reuse between iterations (observed on MoltenVK/macOS).
	struct {
		const uint32_t *code;
		size_t size;
		const char *name;
	} frag_shaders[6] = {
	    {sim_display_shaders_sbs_frag, sizeof(sim_display_shaders_sbs_frag), "SBS"},
	    {sim_display_shaders_anaglyph_frag, sizeof(sim_display_shaders_anaglyph_frag), "Anaglyph"},
	    {sim_display_shaders_blend_frag, sizeof(sim_display_shaders_blend_frag), "Blend"},
	    {sim_display_shaders_squeezed_sbs_frag, sizeof(sim_display_shaders_squeezed_sbs_frag), "Squeezed SBS"},
	    {sim_display_shaders_quad_frag, sizeof(sim_display_shaders_quad_frag), "Quad"},
	    {sim_display_shaders_passthrough_frag, sizeof(sim_display_shaders_passthrough_frag), "Passthrough"},
	};

	// Create all fragment shader modules upfront (keep alive until all pipelines are created)
	VkShaderModule frag_modules[6] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
	for (int i = 0; i < 6; i++) {
		ret = create_shader_module(vk, frag_shaders[i].code, frag_shaders[i].size, &frag_modules[i]);
		if (ret != VK_SUCCESS) {
			U_LOG_E("sim_display: Failed to create %s fragment shader: %d", frag_shaders[i].name, ret);
			for (int j = 0; j < i; j++)
				vk->vkDestroyShaderModule(vk->device, frag_modules[j], NULL);
			vk->vkDestroyShaderModule(vk->device, vert_module, NULL);
			return false;
		}
	}

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

	// Build all pipeline create infos with their own stage arrays
	VkPipelineShaderStageCreateInfo all_stages[6][2];
	VkGraphicsPipelineCreateInfo pipeline_infos[6];
	for (int i = 0; i < 6; i++) {
		all_stages[i][0] = (VkPipelineShaderStageCreateInfo){
		    .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		    .stage = VK_SHADER_STAGE_VERTEX_BIT,
		    .module = vert_module,
		    .pName = "main",
		};
		all_stages[i][1] = (VkPipelineShaderStageCreateInfo){
		    .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		    .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
		    .module = frag_modules[i],
		    .pName = "main",
		};
		pipeline_infos[i] = (VkGraphicsPipelineCreateInfo){
		    .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		    .stageCount = 2,
		    .pStages = all_stages[i],
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
	}

	// Create all pipelines in a single batch call
	ret = vk->vkCreateGraphicsPipelines(vk->device, VK_NULL_HANDLE, 6, pipeline_infos, NULL, sdp->pipelines);

	// Destroy all shader modules now that pipelines are created
	for (int i = 0; i < 6; i++)
		vk->vkDestroyShaderModule(vk->device, frag_modules[i], NULL);
	vk->vkDestroyShaderModule(vk->device, vert_module, NULL);

	if (ret != VK_SUCCESS) {
		U_LOG_E("sim_display: Failed to create pipelines: %d", ret);
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

	// 7. Create descriptor pool (persistent set, never freed individually)
	VkDescriptorPoolSize pool_size = {
	    .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	    .descriptorCount = 1, // One set with 1 atlas sampler
	};

	VkDescriptorPoolCreateInfo pool_info = {
	    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
	    .maxSets = 1,
	    .poolSizeCount = 1,
	    .pPoolSizes = &pool_size,
	};

	ret = vk->vkCreateDescriptorPool(vk->device, &pool_info, NULL, &sdp->desc_pool);
	if (ret != VK_SUCCESS) {
		U_LOG_E("sim_display: Failed to create descriptor pool: %d", ret);
		return false;
	}

	// 8. Allocate persistent descriptor set (updated each frame, never freed)
	VkDescriptorSetAllocateInfo alloc_info = {
	    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
	    .descriptorPool = sdp->desc_pool,
	    .descriptorSetCount = 1,
	    .pSetLayouts = &sdp->desc_layout,
	};

	ret = vk->vkAllocateDescriptorSets(vk->device, &alloc_info, &sdp->desc_set);
	if (ret != VK_SUCCESS) {
		U_LOG_E("sim_display: Failed to allocate descriptor set: %d", ret);
		return false;
	}

	return true;
}


static bool
sim_dp_get_predicted_eye_positions(struct xrt_display_processor *xdp, struct xrt_eye_positions *out)
{
	struct sim_display_processor *sdp = sim_display_processor(xdp);
	float half_ipd = sdp->ipd_m / 2.0f;
	uint32_t vc = sim_display_get_view_count();

	if (vc == 1) {
		out->eyes[0] = (struct xrt_eye_position){sdp->nominal_x_m, sdp->nominal_y_m, sdp->nominal_z_m};
		out->count = 1;
	} else if (vc >= 4) {
		out->eyes[0] = (struct xrt_eye_position){sdp->nominal_x_m - half_ipd, sdp->nominal_y_m - 0.032f, sdp->nominal_z_m};
		out->eyes[1] = (struct xrt_eye_position){sdp->nominal_x_m + half_ipd, sdp->nominal_y_m - 0.032f, sdp->nominal_z_m};
		out->eyes[2] = (struct xrt_eye_position){sdp->nominal_x_m - half_ipd, sdp->nominal_y_m + 0.032f, sdp->nominal_z_m};
		out->eyes[3] = (struct xrt_eye_position){sdp->nominal_x_m + half_ipd, sdp->nominal_y_m + 0.032f, sdp->nominal_z_m};
		out->count = 4;
	} else {
		out->eyes[0] = (struct xrt_eye_position){sdp->nominal_x_m - half_ipd, sdp->nominal_y_m, sdp->nominal_z_m};
		out->eyes[1] = (struct xrt_eye_position){sdp->nominal_x_m + half_ipd, sdp->nominal_y_m, sdp->nominal_z_m};
		out->count = 2;
	}
	out->timestamp_ns = os_monotonic_get_ns();
	out->valid = true;
	out->is_tracking = false; // Nominal, not real tracking
	return true;
}

static VkRenderPass
sim_dp_get_render_pass(struct xrt_display_processor *xdp)
{
	struct sim_display_processor *sdp = sim_display_processor(xdp);
	return sdp->render_pass;
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
		for (int i = 0; i < 6; i++) {
			if (sdp->pipelines[i] != VK_NULL_HANDLE) {
				vk->vkDestroyPipeline(vk->device, sdp->pipelines[i], NULL);
			}
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

	sdp->base.destroy = sim_dp_destroy;
	sdp->base.get_render_pass = sim_dp_get_render_pass;
	sdp->base.get_predicted_eye_positions = sim_dp_get_predicted_eye_positions;

	// Nominal viewer parameters (same defaults as sim_display_hmd_create)
	sdp->ipd_m = 0.06f;
	sdp->nominal_x_m = 0.0f;
	sdp->nominal_y_m = 0.1f;
	sdp->nominal_z_m = debug_get_float_option_sim_display_nominal_z_m();

	if (vk == NULL) {
		U_LOG_E("sim_display: Vulkan bundle required for display processor");
		free(sdp);
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	sdp->vk = vk;
	sdp->base.process_atlas = sim_dp_process_atlas;

	if (!create_pipeline_resources(sdp, target_format)) {
		U_LOG_E("sim_display: Failed to create pipeline resources");
		sim_dp_destroy(&sdp->base);
		return XRT_ERROR_VULKAN;
	}

	// Set the initial output mode (atomic global read by process_atlas each frame)
	sim_display_set_output_mode(mode);

	U_LOG_W("Created sim display processor (all 6 pipelines), initial mode: %s",
	        mode == SIM_DISPLAY_OUTPUT_SBS           ? "SBS" :
	        mode == SIM_DISPLAY_OUTPUT_ANAGLYPH       ? "Anaglyph" :
	        mode == SIM_DISPLAY_OUTPUT_SQUEEZED_SBS   ? "Squeezed SBS" :
	        mode == SIM_DISPLAY_OUTPUT_QUAD            ? "Quad" :
	        mode == SIM_DISPLAY_OUTPUT_PASSTHROUGH     ? "Passthrough" : "Blend");

	*out_xdp = &sdp->base;
	return XRT_SUCCESS;
}


/*
 *
 * Factory function — matches xrt_dp_factory_vk_fn_t signature.
 *
 */

xrt_result_t
sim_display_dp_factory_vk(void *vk_bundle_ptr,
                          void *vk_cmd_pool,
                          void *window_handle,
                          int32_t target_format,
                          struct xrt_display_processor **out_xdp)
{
	(void)vk_cmd_pool;
	(void)window_handle;

	struct vk_bundle *vk = (struct vk_bundle *)vk_bundle_ptr;
	enum sim_display_output_mode mode = sim_display_get_output_mode();

	return sim_display_processor_create(mode, vk, target_format, out_xdp);
}
