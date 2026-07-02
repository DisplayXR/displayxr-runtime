// Copyright 2025, The DisplayXR Project
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
 *
 * Exception: XR_EXT_display_zones frames (ADR-027) draw zone layers through
 * a small blend pipeline (zone_blit.vert/.frag) so overlapping zones
 * composite alpha-over in layer-list order — blits cannot blend. Zones
 * frames are all-or-none at the oxr layer, so normal frames never touch the
 * draw path.
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

// SPIR-V shader headers (generated at build time by spirv_shaders())
#include "shaders/zone_blit.vert.h"
#include "shaders/zone_blit.frag.h"

//! Upper bound on zone draws (and so descriptor sets) per frame:
//! layers (16) × views (8).
#define VK_ZONE_MAX_DRAWS 128

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

	//! XR_EXT_display_zones alpha-over draw path (ADR-027). Lazily created
	//! on the first zones frame; the framebuffer alone is dropped on atlas
	//! resize (it wraps atlas_view) and re-created on demand.
	struct
	{
		VkRenderPass render_pass;
		VkFramebuffer framebuffer;
		VkDescriptorSetLayout set_layout;
		VkPipelineLayout pipeline_layout;
		VkSampler sampler;
		VkPipeline pipeline_premult;
		VkPipeline pipeline_unpremult;
		VkDescriptorPool descriptor_pool;
		bool ready;
		bool failed; //!< init failed once — stay on the blit fallback
	} zone;
};

static void
zone_draw_destroy_framebuffer(struct comp_vk_native_renderer *r)
{
	struct vk_bundle *vk = r->vk;

	if (r->zone.framebuffer != VK_NULL_HANDLE) {
		vk->vkDestroyFramebuffer(vk->device, r->zone.framebuffer, NULL);
		r->zone.framebuffer = VK_NULL_HANDLE;
	}
}

static void
zone_draw_destroy(struct comp_vk_native_renderer *r)
{
	struct vk_bundle *vk = r->vk;

	zone_draw_destroy_framebuffer(r);
	if (r->zone.pipeline_premult != VK_NULL_HANDLE) {
		vk->vkDestroyPipeline(vk->device, r->zone.pipeline_premult, NULL);
		r->zone.pipeline_premult = VK_NULL_HANDLE;
	}
	if (r->zone.pipeline_unpremult != VK_NULL_HANDLE) {
		vk->vkDestroyPipeline(vk->device, r->zone.pipeline_unpremult, NULL);
		r->zone.pipeline_unpremult = VK_NULL_HANDLE;
	}
	if (r->zone.descriptor_pool != VK_NULL_HANDLE) {
		vk->vkDestroyDescriptorPool(vk->device, r->zone.descriptor_pool, NULL);
		r->zone.descriptor_pool = VK_NULL_HANDLE;
	}
	if (r->zone.sampler != VK_NULL_HANDLE) {
		vk->vkDestroySampler(vk->device, r->zone.sampler, NULL);
		r->zone.sampler = VK_NULL_HANDLE;
	}
	if (r->zone.pipeline_layout != VK_NULL_HANDLE) {
		vk->vkDestroyPipelineLayout(vk->device, r->zone.pipeline_layout, NULL);
		r->zone.pipeline_layout = VK_NULL_HANDLE;
	}
	if (r->zone.set_layout != VK_NULL_HANDLE) {
		vk->vkDestroyDescriptorSetLayout(vk->device, r->zone.set_layout, NULL);
		r->zone.set_layout = VK_NULL_HANDLE;
	}
	if (r->zone.render_pass != VK_NULL_HANDLE) {
		vk->vkDestroyRenderPass(vk->device, r->zone.render_pass, NULL);
		r->zone.render_pass = VK_NULL_HANDLE;
	}
	r->zone.ready = false;
}

static void
destroy_atlas_resources(struct comp_vk_native_renderer *r)
{
	struct vk_bundle *vk = r->vk;

	// The zone framebuffer wraps atlas_view — drop it with the atlas; the
	// rest of the zone bundle is atlas-independent and survives resizes.
	zone_draw_destroy_framebuffer(r);

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

	destroy_atlas_resources(r);
	zone_draw_destroy(r);

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


/*
 *
 * XR_EXT_display_zones alpha-over draw path (ADR-027).
 *
 */

static bool
zone_create_pipeline(struct comp_vk_native_renderer *r,
                     VkShaderModule vert,
                     VkShaderModule frag,
                     bool unpremultiplied,
                     VkPipeline *out_pipeline)
{
	struct vk_bundle *vk = r->vk;

	VkPipelineShaderStageCreateInfo stages[2] = {
	    {
	        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	        .stage = VK_SHADER_STAGE_VERTEX_BIT,
	        .module = vert,
	        .pName = "main",
	    },
	    {
	        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	        .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
	        .module = frag,
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

	// Alpha-over: premultiplied (One/OneMinusSrcAlpha) by default, straight
	// alpha only swaps the source color factor. Alpha factors are
	// One/OneMinusSrcAlpha in both so the zone's own transparency survives
	// into the atlas (D3D11 blend_premul/blend_alpha parity).
	VkPipelineColorBlendAttachmentState blend_attachment = {
	    .blendEnable = VK_TRUE,
	    .srcColorBlendFactor =
	        unpremultiplied ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_ONE,
	    .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
	    .colorBlendOp = VK_BLEND_OP_ADD,
	    .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
	    .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
	    .alphaBlendOp = VK_BLEND_OP_ADD,
	    .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
	                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
	};

	VkPipelineColorBlendStateCreateInfo blend_state = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
	    .attachmentCount = 1,
	    .pAttachments = &blend_attachment,
	};

	VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dynamic_state = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
	    .dynamicStateCount = 2,
	    .pDynamicStates = dynamic_states,
	};

	VkGraphicsPipelineCreateInfo pipeline_ci = {
	    .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
	    .stageCount = 2,
	    .pStages = stages,
	    .pVertexInputState = &vertex_input,
	    .pInputAssemblyState = &input_assembly,
	    .pViewportState = &viewport_state,
	    .pRasterizationState = &rasterization,
	    .pMultisampleState = &multisample,
	    .pColorBlendState = &blend_state,
	    .pDynamicState = &dynamic_state,
	    .layout = r->zone.pipeline_layout,
	    .renderPass = r->zone.render_pass,
	    .subpass = 0,
	};

	VkResult res =
	    vk->vkCreateGraphicsPipelines(vk->device, VK_NULL_HANDLE, 1, &pipeline_ci, NULL, out_pipeline);
	if (res != VK_SUCCESS) {
		U_LOG_E("VK zones: failed to create %s pipeline: %d",
		        unpremultiplied ? "unpremultiplied" : "premultiplied", res);
		return false;
	}
	return true;
}

//! Lazily create the atlas-independent zone draw bundle. Returns ready state;
//! a failure is sticky (the caller falls back to the blit path for good).
static bool
zone_draw_ensure(struct comp_vk_native_renderer *r)
{
	struct vk_bundle *vk = r->vk;

	if (r->zone.ready) {
		return true;
	}
	if (r->zone.failed) {
		return false;
	}
	r->zone.failed = true; // cleared on full success

	// Render pass: clear the whole atlas (zones frames own the frame's 3D
	// content), end in SHADER_READ_ONLY for the display processor.
	VkAttachmentDescription attachment = {
	    .format = r->format,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
	    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
	    .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
	    .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	    .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
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

	VkSubpassDependency dependencies[2] = {
	    {
	        .srcSubpass = VK_SUBPASS_EXTERNAL,
	        .dstSubpass = 0,
	        .srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	        .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
	        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	    },
	    {
	        .srcSubpass = 0,
	        .dstSubpass = VK_SUBPASS_EXTERNAL,
	        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	        .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
	    },
	};

	VkRenderPassCreateInfo rp_ci = {
	    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
	    .attachmentCount = 1,
	    .pAttachments = &attachment,
	    .subpassCount = 1,
	    .pSubpasses = &subpass,
	    .dependencyCount = 2,
	    .pDependencies = dependencies,
	};

	VkResult res = vk->vkCreateRenderPass(vk->device, &rp_ci, NULL, &r->zone.render_pass);
	if (res != VK_SUCCESS) {
		U_LOG_E("VK zones: failed to create render pass: %d", res);
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

	res = vk->vkCreateDescriptorSetLayout(vk->device, &dsl_ci, NULL, &r->zone.set_layout);
	if (res != VK_SUCCESS) {
		U_LOG_E("VK zones: failed to create descriptor set layout: %d", res);
		zone_draw_destroy(r);
		r->zone.failed = true;
		return false;
	}

	VkPushConstantRange push_range = {
	    .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
	    .offset = 0,
	    .size = 4 * sizeof(float), // normalized src rect
	};

	VkPipelineLayoutCreateInfo pl_ci = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
	    .setLayoutCount = 1,
	    .pSetLayouts = &r->zone.set_layout,
	    .pushConstantRangeCount = 1,
	    .pPushConstantRanges = &push_range,
	};

	res = vk->vkCreatePipelineLayout(vk->device, &pl_ci, NULL, &r->zone.pipeline_layout);
	if (res != VK_SUCCESS) {
		U_LOG_E("VK zones: failed to create pipeline layout: %d", res);
		zone_draw_destroy(r);
		r->zone.failed = true;
		return false;
	}

	VkSamplerCreateInfo sampler_ci = {
	    .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
	    .magFilter = VK_FILTER_LINEAR,
	    .minFilter = VK_FILTER_LINEAR,
	    .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
	    .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	    .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	    .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	    .maxLod = 0.25f,
	};

	res = vk->vkCreateSampler(vk->device, &sampler_ci, NULL, &r->zone.sampler);
	if (res != VK_SUCCESS) {
		U_LOG_E("VK zones: failed to create sampler: %d", res);
		zone_draw_destroy(r);
		r->zone.failed = true;
		return false;
	}

	VkDescriptorPoolSize pool_size = {
	    .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	    .descriptorCount = VK_ZONE_MAX_DRAWS,
	};

	VkDescriptorPoolCreateInfo dp_ci = {
	    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
	    .maxSets = VK_ZONE_MAX_DRAWS,
	    .poolSizeCount = 1,
	    .pPoolSizes = &pool_size,
	};

	res = vk->vkCreateDescriptorPool(vk->device, &dp_ci, NULL, &r->zone.descriptor_pool);
	if (res != VK_SUCCESS) {
		U_LOG_E("VK zones: failed to create descriptor pool: %d", res);
		zone_draw_destroy(r);
		r->zone.failed = true;
		return false;
	}

	VkShaderModule vert = VK_NULL_HANDLE;
	VkShaderModule frag = VK_NULL_HANDLE;
	VkShaderModuleCreateInfo sm_ci = {
	    .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
	    .codeSize = sizeof(shaders_zone_blit_vert),
	    .pCode = shaders_zone_blit_vert,
	};
	res = vk->vkCreateShaderModule(vk->device, &sm_ci, NULL, &vert);
	if (res == VK_SUCCESS) {
		sm_ci.codeSize = sizeof(shaders_zone_blit_frag);
		sm_ci.pCode = shaders_zone_blit_frag;
		res = vk->vkCreateShaderModule(vk->device, &sm_ci, NULL, &frag);
	}
	if (res != VK_SUCCESS) {
		U_LOG_E("VK zones: failed to create shader modules: %d", res);
		if (vert != VK_NULL_HANDLE) {
			vk->vkDestroyShaderModule(vk->device, vert, NULL);
		}
		zone_draw_destroy(r);
		r->zone.failed = true;
		return false;
	}

	bool ok = zone_create_pipeline(r, vert, frag, false, &r->zone.pipeline_premult) &&
	          zone_create_pipeline(r, vert, frag, true, &r->zone.pipeline_unpremult);

	vk->vkDestroyShaderModule(vk->device, vert, NULL);
	vk->vkDestroyShaderModule(vk->device, frag, NULL);

	if (!ok) {
		zone_draw_destroy(r);
		r->zone.failed = true;
		return false;
	}

	r->zone.failed = false;
	r->zone.ready = true;
	U_LOG_W("VK zones: alpha-over draw path ready (premult + unpremult pipelines)");
	return true;
}

//! (Re)create the framebuffer over the current atlas view.
static bool
zone_draw_ensure_framebuffer(struct comp_vk_native_renderer *r)
{
	struct vk_bundle *vk = r->vk;

	if (r->zone.framebuffer != VK_NULL_HANDLE) {
		return true;
	}

	VkFramebufferCreateInfo fb_ci = {
	    .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
	    .renderPass = r->zone.render_pass,
	    .attachmentCount = 1,
	    .pAttachments = &r->atlas_view,
	    .width = r->atlas_alloc_width,
	    .height = r->atlas_alloc_height,
	    .layers = 1,
	};

	VkResult res = vk->vkCreateFramebuffer(vk->device, &fb_ci, NULL, &r->zone.framebuffer);
	if (res != VK_SUCCESS) {
		U_LOG_E("VK zones: failed to create framebuffer: %d", res);
		return false;
	}
	return true;
}

//! Every zone layer must be drawable: plain 2D swapchain (the whole-image
//! view is a 2D view only when array_size == 1) with a valid view.
static bool
zone_pass_usable(struct comp_vk_native_renderer *r,
                 struct comp_layer_accum *layers,
                 const struct comp_vk_native_eff_layout *layout)
{
	if (!zone_draw_ensure(r) || !zone_draw_ensure_framebuffer(r)) {
		return false;
	}

	for (uint32_t i = 0; i < layers->layer_count; i++) {
		struct comp_layer *layer = &layers->layers[i];
		if (layer->data.type != XRT_LAYER_ZONE_3D) {
			continue;
		}
		uint32_t view_count = layer->data.view_count;
		if (view_count > layout->views) {
			view_count = layout->views;
		}
		if (view_count == 0) {
			view_count = 1;
		}
		for (uint32_t eye = 0; eye < view_count; eye++) {
			struct xrt_swapchain *xsc = layer->sc_array[eye];
			if (xsc == NULL) {
				continue;
			}
			if (comp_vk_native_swapchain_get_array_size(xsc) != 1 ||
			    layer->data.proj.v[eye].sub.array_index != 0) {
				static bool layered_warned = false;
				if (!layered_warned) {
					layered_warned = true;
					U_LOG_W("VK zones: layered zone swapchain — falling back to the "
					        "blit path (overlap overwrites; one-time warning)");
				}
				return false;
			}
			uint32_t sc_index = layer->data.proj.v[eye].sub.image_index;
			if (comp_vk_native_swapchain_get_image_view(xsc, sc_index) == 0) {
				return false;
			}
		}
	}
	return true;
}

//! Draw-based projection pass for zones frames: clear via loadOp, draw each
//! zone's view tiles alpha-over in layer-list order, finish in
//! SHADER_READ_ONLY. Mirrors comp_d3d11_renderer's zone branch.
static xrt_result_t
draw_zones_pass(struct comp_vk_native_renderer *r,
                struct comp_layer_accum *layers,
                uint32_t target_width,
                uint32_t target_height,
                const struct comp_vk_native_eff_layout *layout)
{
	struct vk_bundle *vk = r->vk;

	VkCommandBufferAllocateInfo alloc_info = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	    .commandPool = r->cmd_pool,
	    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
	    .commandBufferCount = 1,
	};

	VkCommandBuffer cmd;
	VkResult res = vk->vkAllocateCommandBuffers(vk->device, &alloc_info, &cmd);
	if (res != VK_SUCCESS) {
		U_LOG_E("VK zones: failed to allocate command buffer: %d", res);
		return XRT_ERROR_VULKAN;
	}

	VkCommandBufferBeginInfo begin_info = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	vk->vkBeginCommandBuffer(cmd, &begin_info);

	vk->vkResetDescriptorPool(vk->device, r->zone.descriptor_pool, 0);

	// Transition each unique source image for sampling. A zone's view tiles
	// share one swapchain image, so dedupe — re-transitioning would declare
	// a wrong oldLayout.
	VkImage transitioned[VK_ZONE_MAX_DRAWS];
	uint32_t transitioned_count = 0;
	for (uint32_t i = 0; i < layers->layer_count; i++) {
		struct comp_layer *layer = &layers->layers[i];
		if (layer->data.type != XRT_LAYER_ZONE_3D) {
			continue;
		}
		uint32_t view_count = layer->data.view_count;
		if (view_count > layout->views) {
			view_count = layout->views;
		}
		if (view_count == 0) {
			view_count = 1;
		}
		for (uint32_t eye = 0; eye < view_count; eye++) {
			struct xrt_swapchain *xsc = layer->sc_array[eye];
			if (xsc == NULL) {
				continue;
			}
			uint32_t sc_index = layer->data.proj.v[eye].sub.image_index;
			VkImage img = (VkImage)(uintptr_t)comp_vk_native_swapchain_get_image(xsc, sc_index);
			if (img == VK_NULL_HANDLE) {
				continue;
			}
			bool seen = false;
			for (uint32_t t = 0; t < transitioned_count; t++) {
				if (transitioned[t] == img) {
					seen = true;
					break;
				}
			}
			if (seen || transitioned_count >= VK_ZONE_MAX_DRAWS) {
				continue;
			}
			transitioned[transitioned_count++] = img;
			cmd_image_barrier(vk, cmd, img,
			                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			                   VK_ACCESS_SHADER_READ_BIT,
			                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
		}
	}

	VkClearValue clear_value = {.color = {{0.0f, 0.0f, 0.0f, 0.0f}}};
	VkRenderPassBeginInfo rp_begin = {
	    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
	    .renderPass = r->zone.render_pass,
	    .framebuffer = r->zone.framebuffer,
	    .renderArea = {{0, 0}, {r->atlas_alloc_width, r->atlas_alloc_height}},
	    .clearValueCount = 1,
	    .pClearValues = &clear_value,
	};
	vk->vkCmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

	uint32_t draw_count = 0;
	for (uint32_t i = 0; i < layers->layer_count; i++) {
		struct comp_layer *layer = &layers->layers[i];
		if (layer->data.type != XRT_LAYER_ZONE_3D) {
			continue;
		}

		uint32_t view_count = layer->data.view_count;
		if (view_count > layout->views) {
			view_count = layout->views;
		}
		if (view_count == 0) {
			view_count = 1;
		}

		const bool unpremul =
		    (layer->data.flags & XRT_LAYER_COMPOSITION_UNPREMULTIPLIED_ALPHA_BIT) != 0;

		for (uint32_t eye = 0; eye < view_count; eye++) {
			struct xrt_swapchain *xsc = layer->sc_array[eye];
			if (xsc == NULL || draw_count >= VK_ZONE_MAX_DRAWS) {
				continue;
			}
			uint32_t sc_index = layer->data.proj.v[eye].sub.image_index;
			VkImageView src_view =
			    (VkImageView)(uintptr_t)comp_vk_native_swapchain_get_image_view(xsc, sc_index);
			if (src_view == VK_NULL_HANDLE) {
				continue;
			}

			// Destination: the same tile box + zone-rect scale as the
			// blit path (in zones frames the tile spans the full window).
			// Tile-place by the effective grid (#542); mono content
			// spans the full content region.
			float dx0, dy0, dx1, dy1;
			if (layout->views == 1 || view_count == 1) {
				dx0 = 0.0f;
				dy0 = 0.0f;
				dx1 = (float)(layout->cols * layout->tile_w);
				dy1 = (float)(layout->rows * layout->tile_h);
			} else {
				uint32_t tile_x = eye % layout->cols;
				uint32_t tile_y = eye / layout->cols;
				dx0 = (float)(tile_x * layout->tile_w);
				dy0 = (float)(tile_y * layout->tile_h);
				dx1 = dx0 + (float)layout->tile_w;
				dy1 = dy0 + (float)layout->tile_h;
			}
			if (target_width == 0 || target_height == 0) {
				continue;
			}
			const struct xrt_rect *zr = &layer->data.zone_3d.rect;
			const float zsx = (dx1 - dx0) / (float)target_width;
			const float zsy = (dy1 - dy0) / (float)target_height;
			VkViewport vp = {
			    .x = dx0 + (float)zr->offset.w * zsx,
			    .y = dy0 + (float)zr->offset.h * zsy,
			    .width = (float)zr->extent.w * zsx,
			    .height = (float)zr->extent.h * zsy,
			    .minDepth = 0.0f,
			    .maxDepth = 1.0f,
			};
			if (vp.width <= 0.0f || vp.height <= 0.0f) {
				continue;
			}

			// Scissor: the viewport clamped to the tile box (the zone
			// rect is window-space and may poke past the tile under a
			// stale resize) and to the atlas.
			float sx0f = vp.x < dx0 ? dx0 : vp.x;
			float sy0f = vp.y < dy0 ? dy0 : vp.y;
			float sx1f = vp.x + vp.width > dx1 ? dx1 : vp.x + vp.width;
			float sy1f = vp.y + vp.height > dy1 ? dy1 : vp.y + vp.height;
			if (sx0f < 0.0f) {
				sx0f = 0.0f;
			}
			if (sy0f < 0.0f) {
				sy0f = 0.0f;
			}
			if (sx1f > (float)r->atlas_alloc_width) {
				sx1f = (float)r->atlas_alloc_width;
			}
			if (sy1f > (float)r->atlas_alloc_height) {
				sy1f = (float)r->atlas_alloc_height;
			}
			if (sx1f <= sx0f || sy1f <= sy0f) {
				continue;
			}
			VkRect2D scissor = {
			    .offset = {(int32_t)sx0f, (int32_t)sy0f},
			    .extent = {(uint32_t)(sx1f - sx0f + 0.5f), (uint32_t)(sy1f - sy0f + 0.5f)},
			};

			VkDescriptorSetAllocateInfo ds_ai = {
			    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			    .descriptorPool = r->zone.descriptor_pool,
			    .descriptorSetCount = 1,
			    .pSetLayouts = &r->zone.set_layout,
			};
			VkDescriptorSet set = VK_NULL_HANDLE;
			res = vk->vkAllocateDescriptorSets(vk->device, &ds_ai, &set);
			if (res != VK_SUCCESS) {
				continue;
			}

			VkDescriptorImageInfo image_info = {
			    .sampler = r->zone.sampler,
			    .imageView = src_view,
			    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			};
			VkWriteDescriptorSet write = {
			    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			    .dstSet = set,
			    .dstBinding = 0,
			    .descriptorCount = 1,
			    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			    .pImageInfo = &image_info,
			};
			vk->vkUpdateDescriptorSets(vk->device, 1, &write, 0, NULL);

			// Normalized source rect (the zone's view tile inside its
			// swapchain image).
			uint32_t sc_w = 0;
			uint32_t sc_h = 0;
			comp_vk_native_swapchain_get_dimensions(xsc, &sc_w, &sc_h);
			if (sc_w == 0 || sc_h == 0) {
				continue;
			}
			const struct xrt_rect *sr = &layer->data.proj.v[eye].sub.rect;
			float push[4] = {
			    (float)sr->offset.w / (float)sc_w,
			    (float)sr->offset.h / (float)sc_h,
			    (float)sr->extent.w / (float)sc_w,
			    (float)sr->extent.h / (float)sc_h,
			};

			vk->vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
			                       unpremul ? r->zone.pipeline_unpremult : r->zone.pipeline_premult);
			vk->vkCmdSetViewport(cmd, 0, 1, &vp);
			vk->vkCmdSetScissor(cmd, 0, 1, &scissor);
			vk->vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
			                             r->zone.pipeline_layout, 0, 1, &set, 0, NULL);
			vk->vkCmdPushConstants(cmd, r->zone.pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
			                        sizeof(push), push);
			vk->vkCmdDraw(cmd, 3, 1, 0, 0);
			draw_count++;
		}
	}

	vk->vkCmdEndRenderPass(cmd);

	// Hand the source images back to the apps' steady-state layout.
	for (uint32_t t = 0; t < transitioned_count; t++) {
		cmd_image_barrier(vk, cmd, transitioned[t],
		                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		                   VK_ACCESS_SHADER_READ_BIT,
		                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
	}

	vk->vkEndCommandBuffer(cmd);

	VkSubmitInfo submit_info = {
	    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	    .commandBufferCount = 1,
	    .pCommandBuffers = &cmd,
	};

	res = vk->vkQueueSubmit(vk->main_queue->queue, 1, &submit_info, VK_NULL_HANDLE);
	if (res != VK_SUCCESS) {
		U_LOG_E("VK zones: failed to submit draw commands: %d", res);
		vk->vkFreeCommandBuffers(vk->device, r->cmd_pool, 1, &cmd);
		return XRT_ERROR_VULKAN;
	}

	vk->vkQueueWaitIdle(vk->main_queue->queue);
	vk->vkFreeCommandBuffers(vk->device, r->cmd_pool, 1, &cmd);

	static bool zones_draw_logged = false;
	if (!zones_draw_logged) {
		zones_draw_logged = true;
		U_LOG_W("VK zones: draw-based alpha-over pass active (%u draw(s) this frame)", draw_count);
	}

	return XRT_SUCCESS;
}

xrt_result_t
comp_vk_native_renderer_draw(struct comp_vk_native_renderer *r,
                              struct comp_layer_accum *layers,
                              struct xrt_vec3 *left_eye,
                              struct xrt_vec3 *right_eye,
                              uint32_t target_width,
                              uint32_t target_height,
                              const struct comp_vk_native_eff_layout *layout)
{
	struct vk_bundle *vk = r->vk;
	(void)left_eye;
	(void)right_eye;

	// XR_EXT_display_zones (ADR-027): a zones frame composes N placed zone
	// layers into the window-spanning atlas — the unzoned area must stay
	// transparent so the feathered wish edge blends toward the desktop.
	bool zones_frame = false;
	for (uint32_t i = 0; i < layers->layer_count; i++) {
		if (layers->layers[i].data.type == XRT_LAYER_ZONE_3D) {
			zones_frame = true;
			break;
		}
	}

	// Zones frames take the draw-based pass so overlapping zones composite
	// alpha-over in layer-list order; blits cannot blend. Falls back to the
	// blit path below (overlap overwrites, one-shot WARN) if the pipeline
	// bundle cannot be created or a zone swapchain is layered.
	if (zones_frame && zone_pass_usable(r, layers, layout)) {
		return draw_zones_pass(r, layers, target_width, target_height, layout);
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
	VkClearColorValue clear_color = {
	    {0.0f, 0.0f, 0.0f, (r->transparent_background || zones_frame) ? 0.0f : 1.0f}};
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

	// Blit each projection / zone layer into the atlas texture
	for (uint32_t i = 0; i < layers->layer_count; i++) {
		struct comp_layer *layer = &layers->layers[i];

		const bool is_zone = layer->data.type == XRT_LAYER_ZONE_3D;
		if (layer->data.type != XRT_LAYER_PROJECTION &&
		    layer->data.type != XRT_LAYER_PROJECTION_DEPTH && !is_zone) {
			continue;
		}

		// CONTENT tile count from the SUBMISSION-derived effective layout
		// (#542) — no longer clamped by the hardware weave-state.
		uint32_t view_count = layer->data.view_count;
		if (view_count > layout->views) view_count = layout->views;
		if (view_count == 0) view_count = 1;

		static bool blit_logged = false;
		if (!blit_logged) {
			U_LOG_W("Atlas blit: view_count=%u, eff_tiles=%ux%u, "
			        "eff_view=%ux%u, layer_view_count=%u",
			        view_count,
			        layout->cols, layout->rows,
			        layout->tile_w, layout->tile_h,
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
			if (layout->views == 1 || view_count == 1) {
				// Mono content: stretch across the full content region
				dx0 = 0;
				dy0 = 0;
				dx1 = (int32_t)(layout->cols * layout->tile_w);
				dy1 = (int32_t)(layout->rows * layout->tile_h);
			} else {
				// Tiled layout: place each eye in its effective tile (#542)
				uint32_t tile_x = eye % layout->cols;
				uint32_t tile_y = eye / layout->cols;
				dx0 = (int32_t)(tile_x * layout->tile_w);
				dy0 = (int32_t)(tile_y * layout->tile_h);
				dx1 = dx0 + (int32_t)layout->tile_w;
				dy1 = dy0 + (int32_t)layout->tile_h;
			}

			// XR_EXT_display_zones: scale the zone rect (client-window
			// px) into the tile box — in zones frames the tile spans
			// the full window. FALLBACK ONLY: zones frames normally
			// take draw_zones_pass (alpha-over); this blit leg runs
			// when the pipeline bundle failed or a zone swapchain is
			// layered, and then overlaps OVERWRITE (one-shot WARN).
			if (is_zone && target_width > 0 && target_height > 0) {
				const struct xrt_rect *zr = &layer->data.zone_3d.rect;
				const float bw = (float)(dx1 - dx0);
				const float bh = (float)(dy1 - dy0);
				const float zsx = bw / (float)target_width;
				const float zsy = bh / (float)target_height;
				int32_t zx0 = dx0 + (int32_t)((float)zr->offset.w * zsx);
				int32_t zy0 = dy0 + (int32_t)((float)zr->offset.h * zsy);
				int32_t zx1 = dx0 + (int32_t)((float)(zr->offset.w + zr->extent.w) * zsx);
				int32_t zy1 = dy0 + (int32_t)((float)(zr->offset.h + zr->extent.h) * zsy);
				if (zx0 < dx0) {
					zx0 = dx0;
				}
				if (zy0 < dy0) {
					zy0 = dy0;
				}
				if (zx1 > dx1) {
					zx1 = dx1;
				}
				if (zy1 > dy1) {
					zy1 = dy1;
				}
				if (zx1 <= zx0 || zy1 <= zy0) {
					cmd_image_barrier(vk, cmd, src_image,
					                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
					                   VK_ACCESS_TRANSFER_READ_BIT,
					                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
					                   VK_PIPELINE_STAGE_TRANSFER_BIT,
					                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
					continue;
				}
				dx0 = zx0;
				dy0 = zy0;
				dx1 = zx1;
				dy1 = zy1;

				static bool zone_overlap_warned = false;
				if (!zone_overlap_warned && i > 0) {
					for (uint32_t pz = 0; pz < i && !zone_overlap_warned; pz++) {
						if (layers->layers[pz].data.type != XRT_LAYER_ZONE_3D) {
							continue;
						}
						const struct xrt_rect *pr = &layers->layers[pz].data.zone_3d.rect;
						bool overlap = pr->offset.w < zr->offset.w + zr->extent.w &&
						               zr->offset.w < pr->offset.w + pr->extent.w &&
						               pr->offset.h < zr->offset.h + zr->extent.h &&
						               zr->offset.h < pr->offset.h + pr->extent.h;
						if (overlap) {
							zone_overlap_warned = true;
							U_LOG_W("VK zones: blit FALLBACK in use — overlapping "
							        "zones OVERWRITE in blit order (one-time warning)");
						}
					}
				}
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
	if (new_atlas_width < 64) new_atlas_width = 64;
	if (new_atlas_height < 64) new_atlas_height = 64;

	// #602: decouple the atlas ALLOCATION (worst-case, stable) from the
	// per-frame content VIEW dims. Content-fit display zones (ADR-027)
	// renegotiate view dims ~6x/sec; previously every change tore down and
	// rebuilt the atlas image behind a full vkDeviceWaitIdle — a per-frame GPU
	// stall — and the no-op guard never held while a content-fit canvas was
	// active (it compared the full mode-atlas width against tile_columns *
	// content view_width). Instead allocate the atlas once at the worst case
	// (the mode atlas, passed by layer_commit) and let content pack top-left
	// into a sub-rect; vk_crop_atlas_for_dp rect-crops it back out, and the
	// draw/zone/capture paths place tiles by the per-frame effective layout,
	// not the allocation. While the requested allocation FITS the current one
	// we only update the view bookkeeping in place — no stall, no recreate.
	//
	// The allocation request must be the mode atlas (the stable worst case);
	// the window-resize branch in begin_frame no longer drives a window-derived
	// atlas (that fed a raw window height the high-water then ratcheted on, →
	// out-of-bounds tiles → VK_ERROR_DEVICE_LOST). High-water-mark — never
	// shrink — so it converges and stops firing.
	bool have_atlas = r->atlas_image != VK_NULL_HANDLE;

	uint32_t grow_w = new_atlas_width;
	uint32_t grow_h = new_atlas_height;
	if (have_atlas) {
		if (r->atlas_alloc_width > grow_w) grow_w = r->atlas_alloc_width;
		if (r->atlas_alloc_height > grow_h) grow_h = r->atlas_alloc_height;
	}

	// Defensive: content must fit the allocation — tiles pack into the atlas,
	// so a view wider/taller than alloc/tiles would draw out of bounds. Clamp
	// the stored view dims (these drive the effective layout, crop, and DP
	// handoff). With the mode-atlas allocation this never triggers for normal
	// content-fit (views stay <= the per-eye texture); it's a safety net.
	if (r->tile_columns != 0 && new_view_width * r->tile_columns > grow_w)
		new_view_width = grow_w / r->tile_columns;
	if (r->tile_rows != 0 && new_view_height * r->tile_rows > grow_h)
		new_view_height = grow_h / r->tile_rows;

	bool fits = have_atlas && grow_w == r->atlas_alloc_width && grow_h == r->atlas_alloc_height;
	if (fits) {
		r->view_width = new_view_width;
		r->view_height = new_view_height;
		r->texture_height = new_view_height;
		return XRT_SUCCESS;
	}

	// Genuine grow: first allocation or a display-mode change. Rare, so the
	// one-time device-idle stall here is fine; the steady-state content-fit
	// path fits the branch above and never reaches here.
	if (have_atlas) {
		vk->vkDeviceWaitIdle(vk->device);
		destroy_atlas_resources(r);
	}

	return create_atlas_resources(r, new_view_width, new_view_height, grow_w, grow_h);
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
