// Copyright 2019-2024, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Multi client wrapper compositor.
 * @author Pete Black <pblack@collabora.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Korcan Hussein <korcan.hussein@collabora.com>
 * @ingroup comp_multi
 */

#include "xrt/xrt_config_os.h"
#include "xrt/xrt_config_have.h"
#include "xrt/xrt_handles.h"
#include "xrt/xrt_session.h"

#include "os/os_time.h"
#include "os/os_threading.h"

#include "util/u_var.h"
#include "util/u_misc.h"
#include "util/u_time.h"
#include "util/u_wait.h"
#include "util/u_debug.h"
#include "util/u_trace_marker.h"
#include "util/u_distortion_mesh.h"

#ifdef XRT_OS_LINUX
#include "util/u_linux.h"
#endif

#include "multi/comp_multi_private.h"
#include "multi/comp_multi_interface.h"
#include "main/comp_compositor.h"
#include "main/comp_target.h"

// Per-session rendering support (Phase 4)
#include "util/comp_swapchain.h"
#include "util/comp_render_helpers.h"

#ifdef XRT_HAVE_LEIA_SR_VULKAN
#include "leiasr/leiasr.h"
#include "render/render_interface.h"
#include "vk/vk_helpers.h"
#endif

#include <math.h>
#include <stdio.h>
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#ifdef XRT_GRAPHICS_SYNC_HANDLE_IS_FD
#include <unistd.h>
#endif


/*
 *
 * Render thread.
 *
 */

static void
do_projection_layer(struct xrt_compositor *xc, struct multi_compositor *mc, struct multi_layer_entry *layer, uint32_t i)
{
	struct xrt_device *xdev = layer->xdev;

	// Cast away
	struct xrt_layer_data *data = (struct xrt_layer_data *)&layer->data;

	// Do not need to copy the reference, but should verify the pointers for consistency
	for (uint32_t j = 0; j < data->view_count; j++) {
		if (layer->xscs[j] == NULL) {
			U_LOG_E("Invalid swap chain for projection layer #%u!", i);
			return;
		}
	}

	if (xdev == NULL) {
		U_LOG_E("Invalid xdev for projection layer #%u!", i);
		return;
	}

	xrt_comp_layer_projection(xc, xdev, layer->xscs, data);
}

static void
do_projection_layer_depth(struct xrt_compositor *xc,
                          struct multi_compositor *mc,
                          struct multi_layer_entry *layer,
                          uint32_t i)
{
	struct xrt_device *xdev = layer->xdev;

	struct xrt_swapchain *xsc[XRT_MAX_VIEWS];
	struct xrt_swapchain *d_xsc[XRT_MAX_VIEWS];
	// Cast away
	struct xrt_layer_data *data = (struct xrt_layer_data *)&layer->data;

	for (uint32_t j = 0; j < data->view_count; j++) {
		xsc[j] = layer->xscs[j];
		d_xsc[j] = layer->xscs[j + data->view_count];

		if (xsc[j] == NULL || d_xsc[j] == NULL) {
			U_LOG_E("Invalid swap chain for projection layer #%u!", i);
			return;
		}
	}

	if (xdev == NULL) {
		U_LOG_E("Invalid xdev for projection layer #%u!", i);
		return;
	}


	xrt_comp_layer_projection_depth(xc, xdev, xsc, d_xsc, data);
}

static bool
do_single(struct xrt_compositor *xc,
          struct multi_compositor *mc,
          struct multi_layer_entry *layer,
          uint32_t i,
          const char *name,
          struct xrt_device **out_xdev,
          struct xrt_swapchain **out_xcs,
          struct xrt_layer_data **out_data)
{
	struct xrt_device *xdev = layer->xdev;
	struct xrt_swapchain *xcs = layer->xscs[0];

	if (xcs == NULL) {
		U_LOG_E("Invalid swapchain for layer #%u '%s'!", i, name);
		return false;
	}

	if (xdev == NULL) {
		U_LOG_E("Invalid xdev for layer #%u '%s'!", i, name);
		return false;
	}

	// Cast away
	struct xrt_layer_data *data = (struct xrt_layer_data *)&layer->data;

	*out_xdev = xdev;
	*out_xcs = xcs;
	*out_data = data;

	return true;
}

static void
do_quad_layer(struct xrt_compositor *xc, struct multi_compositor *mc, struct multi_layer_entry *layer, uint32_t i)
{
	struct xrt_device *xdev = NULL;
	struct xrt_swapchain *xcs = NULL;
	struct xrt_layer_data *data = NULL;

	if (!do_single(xc, mc, layer, i, "quad", &xdev, &xcs, &data)) {
		return;
	}

	xrt_comp_layer_quad(xc, xdev, xcs, data);
}

static void
do_cube_layer(struct xrt_compositor *xc, struct multi_compositor *mc, struct multi_layer_entry *layer, uint32_t i)
{
	struct xrt_device *xdev = NULL;
	struct xrt_swapchain *xcs = NULL;
	struct xrt_layer_data *data = NULL;

	if (!do_single(xc, mc, layer, i, "cube", &xdev, &xcs, &data)) {
		return;
	}

	xrt_comp_layer_cube(xc, xdev, xcs, data);
}

static void
do_cylinder_layer(struct xrt_compositor *xc, struct multi_compositor *mc, struct multi_layer_entry *layer, uint32_t i)
{
	struct xrt_device *xdev = NULL;
	struct xrt_swapchain *xcs = NULL;
	struct xrt_layer_data *data = NULL;

	if (!do_single(xc, mc, layer, i, "cylinder", &xdev, &xcs, &data)) {
		return;
	}

	xrt_comp_layer_cylinder(xc, xdev, xcs, data);
}

static void
do_equirect1_layer(struct xrt_compositor *xc, struct multi_compositor *mc, struct multi_layer_entry *layer, uint32_t i)
{
	struct xrt_device *xdev = NULL;
	struct xrt_swapchain *xcs = NULL;
	struct xrt_layer_data *data = NULL;

	if (!do_single(xc, mc, layer, i, "equirect1", &xdev, &xcs, &data)) {
		return;
	}

	xrt_comp_layer_equirect1(xc, xdev, xcs, data);
}

static void
do_equirect2_layer(struct xrt_compositor *xc, struct multi_compositor *mc, struct multi_layer_entry *layer, uint32_t i)
{
	struct xrt_device *xdev = NULL;
	struct xrt_swapchain *xcs = NULL;
	struct xrt_layer_data *data = NULL;

	if (!do_single(xc, mc, layer, i, "equirect2", &xdev, &xcs, &data)) {
		return;
	}

	xrt_comp_layer_equirect2(xc, xdev, xcs, data);
}

static int
overlay_sort_func(const void *a, const void *b)
{
	struct multi_compositor *mc_a = *(struct multi_compositor **)a;
	struct multi_compositor *mc_b = *(struct multi_compositor **)b;

	if (mc_a->state.z_order < mc_b->state.z_order) {
		return -1;
	}

	if (mc_a->state.z_order > mc_b->state.z_order) {
		return 1;
	}

	return 0;
}

static enum xrt_blend_mode
find_active_blend_mode(struct multi_compositor **overlay_sorted_clients, size_t size)
{
	if (overlay_sorted_clients == NULL)
		return XRT_BLEND_MODE_OPAQUE;

	const struct multi_compositor *first_visible = NULL;
	for (size_t k = 0; k < size; ++k) {
		const struct multi_compositor *mc = overlay_sorted_clients[k];
		assert(mc != NULL);

		// if a focused client is found just return, "first_visible" has lower priority and can be ignored.
		if (mc->state.focused) {
			assert(mc->state.visible);
			return mc->delivered.data.env_blend_mode;
		}

		if (first_visible == NULL && mc->state.visible) {
			first_visible = mc;
		}
	}
	if (first_visible != NULL)
		return first_visible->delivered.data.env_blend_mode;
	return XRT_BLEND_MODE_OPAQUE;
}


/*
 *
 * Per-session rendering (Phase 4)
 *
 */

#ifdef XRT_HAVE_LEIA_SR_VULKAN

/*!
 * Extract VkImageView and dimensions from a multi_layer_entry for a specific view.
 * Similar to getLayerInfo() in comp_renderer.c but adapted for multi_layer_entry.
 *
 * @param layer The layer entry to extract from
 * @param view_index 0 for left eye, 1 for right eye
 * @param[out] out_width Image width
 * @param[out] out_height Image height
 * @param[out] out_format Image format
 * @param[out] out_image_view The VkImageView for rendering
 * @return true if extraction successful
 */
static bool
get_session_layer_view(struct multi_layer_entry *layer,
                       int view_index,
                       int *out_width,
                       int *out_height,
                       VkFormat *out_format,
                       VkImageView *out_image_view)
{
	const struct xrt_layer_data *layer_data = &layer->data;

	// Only support projection layers for SR weaving
	if (layer_data->type != XRT_LAYER_PROJECTION && layer_data->type != XRT_LAYER_PROJECTION_DEPTH) {
		return false;
	}

	// Get the swapchain for this view
	const uint32_t sc_index = (view_index == 0) ? 0 : 1;
	struct xrt_swapchain *xsc = layer->xscs[sc_index];
	if (xsc == NULL) {
		return false;
	}

	// Cast to comp_swapchain to access Vulkan resources
	struct comp_swapchain *sc = comp_swapchain(xsc);

	// Get the projection view data
	const struct xrt_layer_projection_view_data *vd = &layer_data->proj.v[view_index];
	const uint32_t array_index = vd->sub.array_index;
	const struct comp_swapchain_image *image = &sc->images[vd->sub.image_index];

	// Extract dimensions
	*out_width = vd->sub.rect.extent.w;
	*out_height = vd->sub.rect.extent.h;
	*out_format = (VkFormat)sc->vkic.info.format;
	*out_image_view = get_image_view(image, layer_data->flags, array_index);

	return (*out_image_view != VK_NULL_HANDLE);
}

/*!
 * Initialize intermediate composite resources for pre-weaving layer compositing.
 * Creates a side-by-side stereo image, per-eye views, render pass, framebuffers,
 * pipeline, and descriptor resources.
 *
 * @param mc    The multi_compositor with per-session rendering already initialized
 * @param vk    The Vulkan bundle
 * @param width Single eye width
 * @param height Eye height
 * @param format Image format (should match swapchain format)
 * @return true on success
 */
static bool
init_composite_resources(struct multi_compositor *mc, struct vk_bundle *vk, uint32_t width, uint32_t height, VkFormat format)
{
	VkResult ret;

	if (mc->session_render.composite_initialized) {
		return true;
	}

	mc->session_render.composite_width = width;
	mc->session_render.composite_height = height;

	U_LOG_W("[composite] Initializing composite resources: %ux%u per eye, format=%d", width, height, format);

	// Create per-eye composite images (separate images for clean weaver input)
	VkExtent2D eye_extent = {.width = width, .height = height};
	VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
	                          VK_IMAGE_USAGE_SAMPLED_BIT |
	                          VK_IMAGE_USAGE_TRANSFER_DST_BIT;

	VkImageSubresourceRange eye_range = {
	    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	    .baseMipLevel = 0,
	    .levelCount = 1,
	    .baseArrayLayer = 0,
	    .layerCount = 1,
	};

	for (int eye = 0; eye < 2; eye++) {
		ret = vk_create_image_simple(vk, eye_extent, format, usage,
		                             &mc->session_render.composite_memories[eye],
		                             &mc->session_render.composite_images[eye]);
		if (ret != VK_SUCCESS) {
			U_LOG_E("[composite] Failed to create composite image %d: %d", eye, ret);
			goto err_images;
		}

		ret = vk_create_view(vk, mc->session_render.composite_images[eye],
		                     VK_IMAGE_VIEW_TYPE_2D, format, eye_range,
		                     &mc->session_render.composite_eye_views[eye]);
		if (ret != VK_SUCCESS) {
			U_LOG_E("[composite] Failed to create eye view %d: %d", eye, ret);
			goto err_images;
		}
	}

	// Create render pass with LOAD_OP_LOAD for overlay compositing
	VkAttachmentDescription attachment = {
	    .format = format,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
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
	    .pAttachments = &attachment,
	    .subpassCount = 1,
	    .pSubpasses = &subpass,
	};

	ret = vk->vkCreateRenderPass(vk->device, &rp_info, NULL,
	                             &mc->session_render.composite_render_pass);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[composite] Failed to create render pass: %d", ret);
		goto err_images;
	}

	// Create framebuffers - one per eye, each using its own image view
	for (int eye = 0; eye < 2; eye++) {
		VkImageView attachments[1] = {mc->session_render.composite_eye_views[eye]};
		VkFramebufferCreateInfo fb_info = {
		    .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		    .renderPass = mc->session_render.composite_render_pass,
		    .attachmentCount = 1,
		    .pAttachments = attachments,
		    .width = width,
		    .height = height,
		    .layers = 1,
		};
		ret = vk->vkCreateFramebuffer(vk->device, &fb_info, NULL,
		                              &mc->session_render.composite_framebuffers[eye]);
		if (ret != VK_SUCCESS) {
			U_LOG_E("[composite] Failed to create framebuffer %d: %d", eye, ret);
			goto err_framebuffers;
		}
	}

	// Create descriptor set layout (UBO + combined image sampler, same as render_resources)
	VkDescriptorSetLayoutBinding bindings[2] = {
	    {
	        .binding = 0, // UBO
	        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
	        .descriptorCount = 1,
	        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
	    },
	    {
	        .binding = 1, // Combined image sampler
	        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	        .descriptorCount = 1,
	        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
	    },
	};

	VkDescriptorSetLayoutCreateInfo dsl_info = {
	    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
	    .bindingCount = 2,
	    .pBindings = bindings,
	};

	ret = vk->vkCreateDescriptorSetLayout(vk->device, &dsl_info, NULL,
	                                      &mc->session_render.composite_desc_layout);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[composite] Failed to create descriptor set layout: %d", ret);
		goto err_framebuffers;
	}

	// Create pipeline layout
	ret = vk_create_pipeline_layout(vk, mc->session_render.composite_desc_layout,
	                                &mc->session_render.composite_pipe_layout);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[composite] Failed to create pipeline layout: %d", ret);
		goto err_desc_layout;
	}

	// Create pipeline using quad shaders from comp_compositor
	struct comp_compositor *c = comp_compositor(&mc->msc->xcn->base);

	// Build a pipeline compatible with our render pass
	// Triangle strip, no vertex input, dynamic viewport/scissor, alpha blending
	VkPipelineInputAssemblyStateCreateInfo ia = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
	    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
	};

	VkPipelineVertexInputStateCreateInfo vi = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
	};

	VkPipelineViewportStateCreateInfo vp = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
	    .viewportCount = 1,
	    .scissorCount = 1,
	};

	VkPipelineRasterizationStateCreateInfo rs = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
	    .polygonMode = VK_POLYGON_MODE_FILL,
	    .cullMode = VK_CULL_MODE_BACK_BIT,
	    .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
	    .lineWidth = 1.0f,
	};

	VkPipelineMultisampleStateCreateInfo ms = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
	    .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
	};

	VkPipelineColorBlendAttachmentState blend_att = {
	    .blendEnable = VK_TRUE,
	    .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
	                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
	    .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
	    .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
	    .colorBlendOp = VK_BLEND_OP_ADD,
	    .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
	    .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
	    .alphaBlendOp = VK_BLEND_OP_ADD,
	};

	VkPipelineColorBlendStateCreateInfo cb = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
	    .attachmentCount = 1,
	    .pAttachments = &blend_att,
	};

	VkPipelineDepthStencilStateCreateInfo ds = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
	};

	VkDynamicState dyn_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dyn = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
	    .dynamicStateCount = 2,
	    .pDynamicStates = dyn_states,
	};

	VkPipelineShaderStageCreateInfo stages[2] = {
	    {
	        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	        .stage = VK_SHADER_STAGE_VERTEX_BIT,
	        .module = c->shaders.layer_quad_vert,
	        .pName = "main",
	    },
	    {
	        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	        .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
	        .module = c->shaders.layer_shared_frag,
	        .pName = "main",
	    },
	};

	VkGraphicsPipelineCreateInfo pipe_info = {
	    .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
	    .stageCount = 2,
	    .pStages = stages,
	    .pVertexInputState = &vi,
	    .pInputAssemblyState = &ia,
	    .pViewportState = &vp,
	    .pRasterizationState = &rs,
	    .pMultisampleState = &ms,
	    .pDepthStencilState = &ds,
	    .pColorBlendState = &cb,
	    .pDynamicState = &dyn,
	    .layout = mc->session_render.composite_pipe_layout,
	    .renderPass = mc->session_render.composite_render_pass,
	    .subpass = 0,
	};

	ret = vk->vkCreateGraphicsPipelines(vk->device, c->nr.pipeline_cache, 1,
	                                    &pipe_info, NULL,
	                                    &mc->session_render.composite_pipeline);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[composite] Failed to create composite pipeline: %d", ret);
		goto err_pipe_layout;
	}

	// Create sampler
	ret = vk_create_sampler(vk, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	                        &mc->session_render.composite_sampler);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[composite] Failed to create sampler: %d", ret);
		goto err_pipeline;
	}

	// Create descriptor pool (enough for XRT_MAX_LAYERS descriptors)
	struct vk_descriptor_pool_info pool_info = {
	    .uniform_per_descriptor_count = 1,
	    .sampler_per_descriptor_count = 1,
	    .descriptor_count = XRT_MAX_LAYERS,
	    .freeable = false,
	};
	ret = vk_create_descriptor_pool(vk, &pool_info,
	                                &mc->session_render.composite_desc_pool);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[composite] Failed to create descriptor pool: %d", ret);
		goto err_sampler;
	}

	// Pre-allocate descriptor sets
	for (uint32_t i = 0; i < XRT_MAX_LAYERS; i++) {
		ret = vk_create_descriptor_set(vk, mc->session_render.composite_desc_pool,
		                               mc->session_render.composite_desc_layout,
		                               &mc->session_render.composite_desc_sets[i]);
		if (ret != VK_SUCCESS) {
			U_LOG_E("[composite] Failed to create descriptor set %u: %d", i, ret);
			goto err_desc_pool;
		}
	}

	// Create persistent UBO buffer for window-space layer data
	// Size: per-eye quad UBO (post_transform + mvp = 80 bytes) × 2 eyes × XRT_MAX_LAYERS
	VkDeviceSize ubo_size = sizeof(struct xrt_normalized_rect) + sizeof(struct xrt_matrix_4x4);
	VkDeviceSize total_ubo_size = ubo_size * 2 * XRT_MAX_LAYERS;
	{
		VkBufferCreateInfo buf_ci = {
		    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		    .size = total_ubo_size,
		    .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
		};
		ret = vk->vkCreateBuffer(vk->device, &buf_ci, NULL, &mc->session_render.composite_ubo_buffer);
		if (ret != VK_SUCCESS) {
			U_LOG_E("[composite] Failed to create UBO buffer: %d", ret);
			goto err_desc_pool;
		}

		VkMemoryRequirements mem_reqs;
		vk->vkGetBufferMemoryRequirements(vk->device, mc->session_render.composite_ubo_buffer, &mem_reqs);

		VkPhysicalDeviceMemoryProperties mem_props;
		vk->vkGetPhysicalDeviceMemoryProperties(vk->physical_device, &mem_props);
		uint32_t mem_type = 0;
		VkMemoryPropertyFlags desired = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		for (uint32_t mi = 0; mi < mem_props.memoryTypeCount; mi++) {
			if ((mem_reqs.memoryTypeBits & (1u << mi)) &&
			    (mem_props.memoryTypes[mi].propertyFlags & desired) == desired) {
				mem_type = mi;
				break;
			}
		}

		VkMemoryAllocateInfo alloc_ci = {
		    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		    .allocationSize = mem_reqs.size,
		    .memoryTypeIndex = mem_type,
		};
		ret = vk->vkAllocateMemory(vk->device, &alloc_ci, NULL, &mc->session_render.composite_ubo_memory);
		if (ret != VK_SUCCESS) {
			U_LOG_E("[composite] Failed to allocate UBO memory: %d", ret);
			vk->vkDestroyBuffer(vk->device, mc->session_render.composite_ubo_buffer, NULL);
			mc->session_render.composite_ubo_buffer = VK_NULL_HANDLE;
			goto err_desc_pool;
		}

		vk->vkBindBufferMemory(vk->device, mc->session_render.composite_ubo_buffer,
		                       mc->session_render.composite_ubo_memory, 0);

		ret = vk->vkMapMemory(vk->device, mc->session_render.composite_ubo_memory,
		                      0, total_ubo_size, 0, &mc->session_render.composite_ubo_mapped);
		if (ret != VK_SUCCESS) {
			U_LOG_E("[composite] Failed to map UBO memory: %d", ret);
			vk->vkDestroyBuffer(vk->device, mc->session_render.composite_ubo_buffer, NULL);
			vk->vkFreeMemory(vk->device, mc->session_render.composite_ubo_memory, NULL);
			mc->session_render.composite_ubo_buffer = VK_NULL_HANDLE;
			mc->session_render.composite_ubo_memory = VK_NULL_HANDLE;
			goto err_desc_pool;
		}
	}

	mc->session_render.composite_initialized = true;
	U_LOG_W("[composite] Composite resources initialized: %ux%u per eye", width, height);
	return true;

	// Error cleanup (reverse order)
err_desc_pool:
	vk->vkDestroyDescriptorPool(vk->device, mc->session_render.composite_desc_pool, NULL);
	mc->session_render.composite_desc_pool = VK_NULL_HANDLE;
err_sampler:
	vk->vkDestroySampler(vk->device, mc->session_render.composite_sampler, NULL);
	mc->session_render.composite_sampler = VK_NULL_HANDLE;
err_pipeline:
	vk->vkDestroyPipeline(vk->device, mc->session_render.composite_pipeline, NULL);
	mc->session_render.composite_pipeline = VK_NULL_HANDLE;
err_pipe_layout:
	vk->vkDestroyPipelineLayout(vk->device, mc->session_render.composite_pipe_layout, NULL);
	mc->session_render.composite_pipe_layout = VK_NULL_HANDLE;
err_desc_layout:
	vk->vkDestroyDescriptorSetLayout(vk->device, mc->session_render.composite_desc_layout, NULL);
	mc->session_render.composite_desc_layout = VK_NULL_HANDLE;
err_framebuffers:
	for (int i = 0; i < 2; i++) {
		if (mc->session_render.composite_framebuffers[i] != VK_NULL_HANDLE) {
			vk->vkDestroyFramebuffer(vk->device, mc->session_render.composite_framebuffers[i], NULL);
			mc->session_render.composite_framebuffers[i] = VK_NULL_HANDLE;
		}
	}
	vk->vkDestroyRenderPass(vk->device, mc->session_render.composite_render_pass, NULL);
	mc->session_render.composite_render_pass = VK_NULL_HANDLE;
err_images:
	for (int i = 0; i < 2; i++) {
		if (mc->session_render.composite_eye_views[i] != VK_NULL_HANDLE) {
			vk->vkDestroyImageView(vk->device, mc->session_render.composite_eye_views[i], NULL);
			mc->session_render.composite_eye_views[i] = VK_NULL_HANDLE;
		}
		if (mc->session_render.composite_images[i] != VK_NULL_HANDLE) {
			vk->vkDestroyImage(vk->device, mc->session_render.composite_images[i], NULL);
			mc->session_render.composite_images[i] = VK_NULL_HANDLE;
		}
		if (mc->session_render.composite_memories[i] != VK_NULL_HANDLE) {
			vk->vkFreeMemory(vk->device, mc->session_render.composite_memories[i], NULL);
			mc->session_render.composite_memories[i] = VK_NULL_HANDLE;
		}
	}
	return false;
}

/*!
 * Destroy intermediate composite resources.
 */
static void
fini_composite_resources(struct multi_compositor *mc, struct vk_bundle *vk)
{
	if (!mc->session_render.composite_initialized) {
		return;
	}

	// Destroy UBO buffer and memory
	if (mc->session_render.composite_ubo_buffer != VK_NULL_HANDLE) {
		vk->vkDestroyBuffer(vk->device, mc->session_render.composite_ubo_buffer, NULL);
		mc->session_render.composite_ubo_buffer = VK_NULL_HANDLE;
	}
	if (mc->session_render.composite_ubo_memory != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, mc->session_render.composite_ubo_memory, NULL);
		mc->session_render.composite_ubo_memory = VK_NULL_HANDLE;
	}
	mc->session_render.composite_ubo_mapped = NULL;

	vk->vkDestroyDescriptorPool(vk->device, mc->session_render.composite_desc_pool, NULL);
	mc->session_render.composite_desc_pool = VK_NULL_HANDLE;

	vk->vkDestroySampler(vk->device, mc->session_render.composite_sampler, NULL);
	mc->session_render.composite_sampler = VK_NULL_HANDLE;

	vk->vkDestroyPipeline(vk->device, mc->session_render.composite_pipeline, NULL);
	mc->session_render.composite_pipeline = VK_NULL_HANDLE;

	vk->vkDestroyPipelineLayout(vk->device, mc->session_render.composite_pipe_layout, NULL);
	mc->session_render.composite_pipe_layout = VK_NULL_HANDLE;

	vk->vkDestroyDescriptorSetLayout(vk->device, mc->session_render.composite_desc_layout, NULL);
	mc->session_render.composite_desc_layout = VK_NULL_HANDLE;

	for (int i = 0; i < 2; i++) {
		if (mc->session_render.composite_framebuffers[i] != VK_NULL_HANDLE) {
			vk->vkDestroyFramebuffer(vk->device, mc->session_render.composite_framebuffers[i], NULL);
			mc->session_render.composite_framebuffers[i] = VK_NULL_HANDLE;
		}
	}

	vk->vkDestroyRenderPass(vk->device, mc->session_render.composite_render_pass, NULL);
	mc->session_render.composite_render_pass = VK_NULL_HANDLE;

	for (int i = 0; i < 2; i++) {
		if (mc->session_render.composite_eye_views[i] != VK_NULL_HANDLE) {
			vk->vkDestroyImageView(vk->device, mc->session_render.composite_eye_views[i], NULL);
			mc->session_render.composite_eye_views[i] = VK_NULL_HANDLE;
		}
		if (mc->session_render.composite_images[i] != VK_NULL_HANDLE) {
			vk->vkDestroyImage(vk->device, mc->session_render.composite_images[i], NULL);
			mc->session_render.composite_images[i] = VK_NULL_HANDLE;
		}
		if (mc->session_render.composite_memories[i] != VK_NULL_HANDLE) {
			vk->vkFreeMemory(vk->device, mc->session_render.composite_memories[i], NULL);
			mc->session_render.composite_memories[i] = VK_NULL_HANDLE;
		}
	}

	mc->session_render.composite_initialized = false;
}

/*!
 * Check if any window-space layers exist in the delivered frame.
 */
static bool
has_window_space_layers(struct multi_compositor *mc)
{
	for (uint32_t i = 0; i < mc->delivered.layer_count; i++) {
		if (mc->delivered.layers[i].data.type == XRT_LAYER_WINDOW_SPACE) {
			return true;
		}
	}
	return false;
}

/*!
 * Composite all layers (projection + window-space) into the intermediate stereo
 * targets before weaving. This is the pre-weaving compositing step.
 *
 * @param mc  The multi_compositor
 * @param vk  The Vulkan bundle
 * @param cmd The command buffer to record into
 * @param leftImageView  Output: left eye view of composited result
 * @param rightImageView Output: right eye view of composited result
 * @return true if compositing was performed
 */
static bool
composite_layers_to_intermediate(struct multi_compositor *mc,
                                 struct vk_bundle *vk,
                                 VkCommandBuffer cmd,
                                 VkImageView *out_left_view,
                                 VkImageView *out_right_view)
{
	// Find the projection layer first
	struct multi_layer_entry *proj_layer = NULL;
	for (uint32_t i = 0; i < mc->delivered.layer_count; i++) {
		enum xrt_layer_type type = mc->delivered.layers[i].data.type;
		if (type == XRT_LAYER_PROJECTION || type == XRT_LAYER_PROJECTION_DEPTH) {
			proj_layer = &mc->delivered.layers[i];
			break;
		}
	}

	if (proj_layer == NULL) {
		U_LOG_W("[composite] No projection layer found");
		return false;
	}

	// Get projection layer image info
	int imgW = 0, imgH = 0;
	VkFormat imgFmt = VK_FORMAT_UNDEFINED;
	VkImageView leftProjView = VK_NULL_HANDLE, rightProjView = VK_NULL_HANDLE;

	if (!get_session_layer_view(proj_layer, 0, &imgW, &imgH, &imgFmt, &leftProjView) ||
	    !get_session_layer_view(proj_layer, 1, &imgW, &imgH, &imgFmt, &rightProjView)) {
		U_LOG_W("[composite] Could not extract projection views");
		return false;
	}

	// Lazily init composite resources if needed
	if (!mc->session_render.composite_initialized) {
		if (!init_composite_resources(mc, vk, (uint32_t)imgW, (uint32_t)imgH, imgFmt)) {
			return false;
		}
	}

	uint32_t cw = mc->session_render.composite_width;
	uint32_t ch = mc->session_render.composite_height;

	// Step 1: Transition both composite images to TRANSFER_DST
	VkImageMemoryBarrier barriers_to_dst[2];
	for (int eye = 0; eye < 2; eye++) {
		barriers_to_dst[eye] = (VkImageMemoryBarrier){
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = 0,
		    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		    .image = mc->session_render.composite_images[eye],
		    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		};
	}
	vk->vkCmdPipelineBarrier(cmd,
	                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	                         VK_PIPELINE_STAGE_TRANSFER_BIT,
	                         0, 0, NULL, 0, NULL, 2, barriers_to_dst);

	// Step 2: Blit projection layer into per-eye composite images
	for (int eye = 0; eye < 2; eye++) {
		struct xrt_swapchain *xsc = proj_layer->xscs[eye];
		if (xsc == NULL)
			continue;

		struct comp_swapchain *sc = comp_swapchain(xsc);
		const struct xrt_layer_projection_view_data *vd = &proj_layer->data.proj.v[eye];
		VkImage src_vk_image = sc->vkic.images[vd->sub.image_index].handle;

		// Transition source image to TRANSFER_SRC
		VkImageMemoryBarrier src_barrier = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		    .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		    .image = src_vk_image,
		    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		};
		vk->vkCmdPipelineBarrier(cmd,
		                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		                         VK_PIPELINE_STAGE_TRANSFER_BIT,
		                         0, 0, NULL, 0, NULL, 1, &src_barrier);

		// Blit from source to per-eye composite target
		VkImageBlit blit = {
		    .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, vd->sub.array_index, 1},
		    .srcOffsets = {
		        {vd->sub.rect.offset.w, vd->sub.rect.offset.h, 0},
		        {vd->sub.rect.offset.w + (int32_t)vd->sub.rect.extent.w,
		         vd->sub.rect.offset.h + (int32_t)vd->sub.rect.extent.h, 1},
		    },
		    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
		    .dstOffsets = {
		        {0, 0, 0},
		        {(int32_t)cw, (int32_t)ch, 1},
		    },
		};
		vk->vkCmdBlitImage(cmd,
		                   src_vk_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		                   mc->session_render.composite_images[eye], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		                   1, &blit, VK_FILTER_LINEAR);

		// Transition source back to SHADER_READ_ONLY
		VkImageMemoryBarrier src_restore = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
		    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		    .image = src_vk_image,
		    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		};
		vk->vkCmdPipelineBarrier(cmd,
		                         VK_PIPELINE_STAGE_TRANSFER_BIT,
		                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		                         0, 0, NULL, 0, NULL, 1, &src_restore);
	}

	// Step 3: Transition both composite images to COLOR_ATTACHMENT_OPTIMAL
	VkImageMemoryBarrier barriers_to_attach[2];
	for (int eye = 0; eye < 2; eye++) {
		barriers_to_attach[eye] = (VkImageMemoryBarrier){
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		    .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		    .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		    .image = mc->session_render.composite_images[eye],
		    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		};
	}
	vk->vkCmdPipelineBarrier(cmd,
	                         VK_PIPELINE_STAGE_TRANSFER_BIT,
	                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	                         0, 0, NULL, 0, NULL, 2, barriers_to_attach);

	// Step 4: Render window-space layers as alpha-blended quads
	uint32_t ws_desc_index = 0;
	for (uint32_t li = 0; li < mc->delivered.layer_count; li++) {
		struct multi_layer_entry *layer = &mc->delivered.layers[li];
		if (layer->data.type != XRT_LAYER_WINDOW_SPACE) {
			continue;
		}
		if (ws_desc_index >= XRT_MAX_LAYERS) {
			break;
		}

		const struct xrt_layer_window_space_data *ws = &layer->data.window_space;
		struct xrt_swapchain *xsc = layer->xscs[0];
		if (xsc == NULL) {
			continue;
		}

		struct comp_swapchain *sc = comp_swapchain(xsc);
		uint32_t img_idx = ws->sub.image_index;
		const struct comp_swapchain_image *ws_image = &sc->images[img_idx];
		VkImageView ws_view = get_image_view(ws_image, layer->data.flags, ws->sub.array_index);
		if (ws_view == VK_NULL_HANDLE) {
			continue;
		}
		VkImage ws_vk_image = sc->vkic.images[img_idx].handle;

		// Transition window-space swapchain image to SHADER_READ_ONLY
		VkImageMemoryBarrier ws_barrier = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
		    .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		    .image = ws_vk_image,
		    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		};
		vk->vkCmdPipelineBarrier(cmd,
		                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		                         0, 0, NULL, 0, NULL, 1, &ws_barrier);

		// Compute per-eye disparity offset
		float half_disp = ws->disparity / 2.0f;

		// UBO stride for sub-allocation from the persistent UBO buffer
		VkDeviceSize ubo_stride = sizeof(struct xrt_normalized_rect) + sizeof(struct xrt_matrix_4x4);

		// Render to both eyes
		for (int eye = 0; eye < 2; eye++) {
			float eye_shift = (eye == 0) ? -half_disp : half_disp;

			// Window-space fractional coords → NDC [-1, 1]
			float frac_cx = ws->x + ws->width / 2.0f + eye_shift;
			float frac_cy = ws->y + ws->height / 2.0f;
			float ndc_cx = frac_cx * 2.0f - 1.0f;
			float ndc_cy = 1.0f - frac_cy * 2.0f;
			float ndc_sx = ws->width * 2.0f;
			float ndc_sy = ws->height * 2.0f;

			// Build orthographic MVP (quad vert shader uses [-0.5, 0.5] unit quad)
			struct xrt_matrix_4x4 mvp;
			// clang-format off
			mvp.v[0]  = ndc_sx; mvp.v[1]  = 0.0f;   mvp.v[2]  = 0.0f; mvp.v[3]  = 0.0f;
			mvp.v[4]  = 0.0f;   mvp.v[5]  = ndc_sy;  mvp.v[6]  = 0.0f; mvp.v[7]  = 0.0f;
			mvp.v[8]  = 0.0f;   mvp.v[9]  = 0.0f;   mvp.v[10] = 1.0f; mvp.v[11] = 0.0f;
			mvp.v[12] = ndc_cx; mvp.v[13] = ndc_cy;  mvp.v[14] = 0.5f; mvp.v[15] = 1.0f;
			// clang-format on

			// UBO data: post_transform (xrt_normalized_rect) + mvp (xrt_matrix_4x4)
			struct
			{
				struct xrt_normalized_rect post_transform;
				struct xrt_matrix_4x4 mvp;
			} ubo_data;

			ubo_data.post_transform.x = ws->sub.norm_rect.x;
			ubo_data.post_transform.y = ws->sub.norm_rect.y;
			ubo_data.post_transform.w = ws->sub.norm_rect.w;
			ubo_data.post_transform.h = ws->sub.norm_rect.h;

			if (layer->data.flip_y) {
				ubo_data.post_transform.y += ubo_data.post_transform.h;
				ubo_data.post_transform.h = -ubo_data.post_transform.h;
			}

			ubo_data.mvp = mvp;

			// Write UBO data to persistent buffer at the right offset
			// Layout: [layer0_eye0, layer0_eye1, layer1_eye0, layer1_eye1, ...]
			uint32_t ubo_index = ws_desc_index * 2 + eye;
			VkDeviceSize ubo_offset = ubo_index * ubo_stride;
			memcpy((uint8_t *)mc->session_render.composite_ubo_mapped + ubo_offset,
			       &ubo_data, sizeof(ubo_data));

			// Update descriptor set
			VkDescriptorSet ds_set = mc->session_render.composite_desc_sets[ws_desc_index];

			VkDescriptorBufferInfo buf_desc = {
			    .buffer = mc->session_render.composite_ubo_buffer,
			    .offset = ubo_offset,
			    .range = sizeof(ubo_data),
			};

			VkDescriptorImageInfo img_desc = {
			    .sampler = mc->session_render.composite_sampler,
			    .imageView = ws_view,
			    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			};

			VkWriteDescriptorSet writes[2] = {
			    {
			        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet = ds_set,
			        .dstBinding = 0,
			        .descriptorCount = 1,
			        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			        .pBufferInfo = &buf_desc,
			    },
			    {
			        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			        .dstSet = ds_set,
			        .dstBinding = 1,
			        .descriptorCount = 1,
			        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			        .pImageInfo = &img_desc,
			    },
			};
			vk->vkUpdateDescriptorSets(vk->device, 2, writes, 0, NULL);

			// Begin render pass on the eye's framebuffer (per-eye image)
			VkRenderPassBeginInfo rp_begin = {
			    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			    .renderPass = mc->session_render.composite_render_pass,
			    .framebuffer = mc->session_render.composite_framebuffers[eye],
			    .renderArea = {
			        .offset = {0, 0},
			        .extent = {cw, ch},
			    },
			    .clearValueCount = 0,
			    .pClearValues = NULL,
			};
			vk->vkCmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

			// Set viewport and scissor for the full per-eye image
			VkViewport eye_viewport = {
			    .x = 0.0f,
			    .y = 0.0f,
			    .width = (float)cw,
			    .height = (float)ch,
			    .minDepth = 0.0f,
			    .maxDepth = 1.0f,
			};
			vk->vkCmdSetViewport(cmd, 0, 1, &eye_viewport);

			VkRect2D scissor = {
			    .offset = {0, 0},
			    .extent = {cw, ch},
			};
			vk->vkCmdSetScissor(cmd, 0, 1, &scissor);

			// Bind pipeline and descriptor set, draw quad
			vk->vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
			                      mc->session_render.composite_pipeline);
			vk->vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
			                            mc->session_render.composite_pipe_layout,
			                            0, 1, &ds_set, 0, NULL);
			vk->vkCmdDraw(cmd, 4, 1, 0, 0);

			vk->vkCmdEndRenderPass(cmd);
		}

		ws_desc_index++;
	}

	// Step 5: Transition both composite images to SHADER_READ_ONLY for weaver input
	VkImageMemoryBarrier barriers_to_read[2];
	for (int eye = 0; eye < 2; eye++) {
		barriers_to_read[eye] = (VkImageMemoryBarrier){
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
		    .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		    .image = mc->session_render.composite_images[eye],
		    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		};
	}
	vk->vkCmdPipelineBarrier(cmd,
	                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	                         0, 0, NULL, 0, NULL, 2, barriers_to_read);

	// Output the per-eye views
	*out_left_view = mc->session_render.composite_eye_views[0];
	*out_right_view = mc->session_render.composite_eye_views[1];

	return true;
}

/*!
 * Render a single per-session client to its own comp_target using SR weaving.
 *
 * @param mc The multi_compositor with per-session rendering
 * @param vk The Vulkan bundle
 * @param display_time_ns The display timestamp
 */
static void
render_session_to_own_target(struct multi_compositor *mc, struct vk_bundle *vk, int64_t display_time_ns)
{
	U_LOG_W("[per-session] render_session_to_own_target: START");

	struct comp_target *ct = mc->session_render.target;
	struct leiasr *weaver = mc->session_render.weaver;

	if (ct == NULL || weaver == NULL) {
		U_LOG_E("[per-session] Per-session target or weaver not initialized");
		return;
	}

	// Must have at least one layer
	if (mc->delivered.layer_count == 0) {
		U_LOG_W("[per-session] No layers delivered, skipping");
		return;
	}

	U_LOG_W("[per-session] Have %u layers, extracting stereo views...", mc->delivered.layer_count);

	// Check if we need compositing (window-space layers present)
	bool needs_compositing = has_window_space_layers(mc);

	// Get the first projection layer
	struct multi_layer_entry *layer = NULL;
	for (uint32_t i = 0; i < mc->delivered.layer_count; i++) {
		enum xrt_layer_type type = mc->delivered.layers[i].data.type;
		if (type == XRT_LAYER_PROJECTION || type == XRT_LAYER_PROJECTION_DEPTH) {
			layer = &mc->delivered.layers[i];
			break;
		}
	}

	if (layer == NULL) {
		U_LOG_W("[per-session] No projection layer found, skipping");
		return;
	}

	// Extract left and right view info
	int imageWidth = 0, imageHeight = 0;
	VkFormat imageFormat = VK_FORMAT_UNDEFINED;
	VkImageView leftImageView = VK_NULL_HANDLE;
	VkImageView rightImageView = VK_NULL_HANDLE;

	bool leftOk = get_session_layer_view(layer, 0, &imageWidth, &imageHeight, &imageFormat, &leftImageView);
	bool rightOk = get_session_layer_view(layer, 1, &imageWidth, &imageHeight, &imageFormat, &rightImageView);

	if (!leftOk || !rightOk) {
		U_LOG_W("[per-session] Could not extract stereo views for per-session rendering");
		return;
	}

	U_LOG_W("[per-session] Got stereo views: %dx%d, left=%p, right=%p, needs_compositing=%d",
	        imageWidth, imageHeight, (void *)leftImageView, (void *)rightImageView, needs_compositing);

	// Wait for pending fence if exists (from previous frame using same buffer)
	if (mc->session_render.fenced_buffer >= 0) {
		U_LOG_W("[per-session] Waiting for pending fence (buffer %d)...", mc->session_render.fenced_buffer);
		VkResult fence_ret = vk->vkWaitForFences(vk->device, 1,
		                                         &mc->session_render.fences[mc->session_render.fenced_buffer],
		                                         VK_TRUE, UINT64_MAX);
		if (fence_ret != VK_SUCCESS) {
			U_LOG_E("[per-session] Failed to wait for fence: %s", vk_result_string(fence_ret));
		}
		mc->session_render.fenced_buffer = -1;
	}

	// Acquire the next swapchain image from the per-session target
	U_LOG_W("[per-session] Acquiring target swapchain image...");
	uint32_t buffer_index = 0;
	VkResult ret = comp_target_acquire(ct, &buffer_index);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[per-session] Failed to acquire per-session target image: %s", vk_result_string(ret));
		return;
	}
	U_LOG_W("[per-session] Acquired buffer_index=%u", buffer_index);

	// Validate buffer_index is in range
	if (buffer_index >= mc->session_render.buffer_count) {
		U_LOG_E("[per-session] buffer_index %u out of range (max %u)", buffer_index, mc->session_render.buffer_count);
		return;
	}

	// Reset fence for current buffer
	ret = vk->vkResetFences(vk->device, 1, &mc->session_render.fences[buffer_index]);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[per-session] Failed to reset fence: %s", vk_result_string(ret));
		return;
	}

	// Get target framebuffer info
	uint32_t framebufferWidth = ct->width;
	uint32_t framebufferHeight = ct->height;
	VkFormat framebufferFormat = ct->format;

	// Set up viewport (fullscreen)
	VkRect2D viewport = {0};
	viewport.offset.x = 0;
	viewport.offset.y = 0;
	viewport.extent.width = framebufferWidth;
	viewport.extent.height = framebufferHeight;

	// Use pre-allocated command buffer for this swapchain image
	VkCommandBuffer cmd = mc->session_render.cmd_buffers[buffer_index];
	ret = vk->vkResetCommandBuffer(cmd, 0);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[per-session] Failed to reset command buffer: %s", vk_result_string(ret));
		return;
	}

	// Begin command buffer
	VkCommandBufferBeginInfo begin_info = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};

	ret = vk->vkBeginCommandBuffer(cmd, &begin_info);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[per-session] Failed to begin command buffer: %s", vk_result_string(ret));
		return;
	}
	U_LOG_W("[per-session] Command buffer started");

	// If we have window-space layers, composite all layers into intermediate targets first
	VkImageView weaveLeft = leftImageView;
	VkImageView weaveRight = rightImageView;
	int weaveWidth = imageWidth;
	int weaveHeight = imageHeight;

	if (needs_compositing) {
		U_LOG_W("[per-session] Compositing layers to intermediate targets...");
		VkImageView compLeft = VK_NULL_HANDLE, compRight = VK_NULL_HANDLE;
		if (composite_layers_to_intermediate(mc, vk, cmd, &compLeft, &compRight)) {
			weaveLeft = compLeft;
			weaveRight = compRight;
			weaveWidth = (int)mc->session_render.composite_width;
			weaveHeight = (int)mc->session_render.composite_height;
			U_LOG_W("[per-session] Using composited per-eye views: %dx%d", weaveWidth, weaveHeight);
		} else {
			U_LOG_W("[per-session] Compositing failed, falling back to direct projection views");
		}
	}

	// Perform SR weaving
	U_LOG_W("[per-session] Calling leiasr_weave: weaver=%p, cmd=%p, fb=%ux%u",
	        (void *)weaver, (void *)cmd, framebufferWidth, framebufferHeight);
	leiasr_weave(weaver, cmd, weaveLeft, weaveRight, viewport, weaveWidth, weaveHeight, imageFormat,
	             VK_NULL_HANDLE, // framebuffer - SR Runtime handles this internally
	             (int)framebufferWidth, (int)framebufferHeight, framebufferFormat);
	U_LOG_W("[per-session] leiasr_weave returned");

	// End command buffer
	U_LOG_W("[per-session] Ending command buffer...");
	ret = vk->vkEndCommandBuffer(cmd);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[per-session] Failed to end command buffer: %s", vk_result_string(ret));
		return;
	}
	U_LOG_W("[per-session] Command buffer ended");

	// Submit command buffer with fence for async completion
	// Signal the render_complete semaphore so comp_target_present can wait on it
	VkSemaphore signal_sem = ct->semaphores.render_complete;
	VkSubmitInfo submit_info = {
	    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	    .commandBufferCount = 1,
	    .pCommandBuffers = &cmd,
	    .signalSemaphoreCount = (signal_sem != VK_NULL_HANDLE) ? 1 : 0,
	    .pSignalSemaphores = (signal_sem != VK_NULL_HANDLE) ? &signal_sem : NULL,
	};

	U_LOG_W("[per-session] Submitting command buffer with fence (signal_sem=%p)...", (void *)signal_sem);
	ret = vk->vkQueueSubmit(vk->main_queue->queue, 1, &submit_info, mc->session_render.fences[buffer_index]);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[per-session] Failed to submit per-session render: %s", vk_result_string(ret));
		return;
	}
	mc->session_render.fenced_buffer = (int32_t)buffer_index;
	U_LOG_W("[per-session] Queue submit succeeded, fenced_buffer=%d", mc->session_render.fenced_buffer);

	// Present the image (fence handles GPU sync - no vkQueueWaitIdle needed)
	U_LOG_W("[per-session] Presenting image (buffer_index=%u)...", buffer_index);
	ret = comp_target_present(ct, vk->main_queue->queue, buffer_index, 0, display_time_ns, 0);
	if (ret != VK_SUCCESS && ret != VK_SUBOPTIMAL_KHR) {
		U_LOG_E("[per-session] Failed to present per-session target: %s", vk_result_string(ret));
	}
	U_LOG_W("[per-session] render_session_to_own_target: END (present result=%d)", ret);
}

/*!
 * Render all per-session clients to their own targets.
 * Called after xrt_comp_layer_commit() for sessions with external window handles.
 *
 * @param msc The multi system compositor
 * @param display_time_ns The predicted display time
 */
static void
render_per_session_clients_locked(struct multi_system_compositor *msc, int64_t display_time_ns)
{
	COMP_TRACE_MARKER();

	U_LOG_W("[per-session] render_per_session_clients_locked: START");

	struct vk_bundle *vk = comp_target_service_get_vk(msc->target_service);
	if (vk == NULL) {
		U_LOG_E("[per-session] No Vulkan bundle available from target service");
		return;
	}

	int session_count = 0;
	for (size_t k = 0; k < ARRAY_SIZE(msc->clients); k++) {
		struct multi_compositor *mc = msc->clients[k];

		if (mc == NULL || !mc->session_render.initialized) {
			continue;
		}

		// Skip if no active/delivered frame
		if (!mc->delivered.active || mc->delivered.layer_count == 0) {
			U_LOG_W("[per-session] Client %zu: skipping (no active frame)", k);
			continue;
		}

		U_LOG_W("[per-session] Client %zu: rendering session to own target...", k);
		session_count++;

		// Render this session to its own target
		render_session_to_own_target(mc, vk, display_time_ns);

		U_LOG_W("[per-session] Client %zu: retiring delivered frame...", k);
		// Retire the delivered frame for this session
		int64_t now_ns = os_monotonic_get_ns();
		multi_compositor_retire_delivered_locked(mc, now_ns);
		U_LOG_W("[per-session] Client %zu: done", k);
	}

	U_LOG_W("[per-session] render_per_session_clients_locked: END (processed %d sessions)", session_count);
}

#endif // XRT_HAVE_LEIA_SR_VULKAN


static void
transfer_layers_locked(struct multi_system_compositor *msc, int64_t display_time_ns, int64_t system_frame_id)
{
	COMP_TRACE_MARKER();

	struct xrt_compositor *xc = &msc->xcn->base;

	struct multi_compositor *array[MULTI_MAX_CLIENTS] = {0};

	// To mark latching.
	int64_t now_ns = os_monotonic_get_ns();

	size_t count = 0;
	for (size_t k = 0; k < ARRAY_SIZE(array); k++) {
		struct multi_compositor *mc = msc->clients[k];

		// Array can be empty
		if (mc == NULL) {
			continue;
		}

		// Lazily initialize per-session render resources if session has external HWND
		// This creates the per-session comp_target and SR weaver for multi-app support
		if (multi_compositor_has_session_render(mc) && !mc->session_render.initialized) {
			U_LOG_W("Calling multi_compositor_init_session_render...");
			bool init_result = multi_compositor_init_session_render(mc);
			U_LOG_W("multi_compositor_init_session_render returned %d", init_result);
		}

		U_LOG_W("About to call multi_compositor_deliver_any_frames...");
		// Even if it's not shown, make sure that frames are delivered.
		multi_compositor_deliver_any_frames(mc, display_time_ns);
		U_LOG_W("multi_compositor_deliver_any_frames completed");

		// None of the data in this slot is valid, don't check access it.
		if (!mc->delivered.active) {
			continue;
		}

		// The client isn't visible, do not submit it's layers.
		if (!mc->state.visible) {
			// Need to drop delivered frame as it shouldn't be reused.
			multi_compositor_retire_delivered_locked(mc, now_ns);
			continue;
		}

		// Just in case.
		if (!mc->state.session_active) {
			U_LOG_W("Session is visible but not active.");

			// Need to drop delivered frame as it shouldn't be reused.
			multi_compositor_retire_delivered_locked(mc, now_ns);
			continue;
		}

		// The list_and_timing_lock is held when callign this function.
		multi_compositor_latch_frame_locked(mc, now_ns, system_frame_id);

		array[count++] = msc->clients[k];
	}

	// Sort the stack array
	qsort(array, count, sizeof(struct multi_compositor *), overlay_sort_func);

	// find first (ordered by bottom to top) active client to retrieve xrt_layer_frame_data
	const enum xrt_blend_mode blend_mode = find_active_blend_mode(array, count);

	const struct xrt_layer_frame_data data = {
	    .frame_id = system_frame_id,
	    .display_time_ns = display_time_ns,
	    .env_blend_mode = blend_mode,
	};
	xrt_comp_layer_begin(xc, &data);

	// Copy all active layers (skip sessions with per-session rendering - Phase 4).
	for (size_t k = 0; k < count; k++) {
		struct multi_compositor *mc = array[k];
		assert(mc != NULL);

		// Skip sessions with per-session rendering - they render separately to their own targets
		if (mc->session_render.initialized) {
			continue;
		}

		for (uint32_t i = 0; i < mc->delivered.layer_count; i++) {
			struct multi_layer_entry *layer = &mc->delivered.layers[i];

			switch (layer->data.type) {
			case XRT_LAYER_PROJECTION: do_projection_layer(xc, mc, layer, i); break;
			case XRT_LAYER_PROJECTION_DEPTH: do_projection_layer_depth(xc, mc, layer, i); break;
			case XRT_LAYER_QUAD: do_quad_layer(xc, mc, layer, i); break;
			case XRT_LAYER_CUBE: do_cube_layer(xc, mc, layer, i); break;
			case XRT_LAYER_CYLINDER: do_cylinder_layer(xc, mc, layer, i); break;
			case XRT_LAYER_EQUIRECT1: do_equirect1_layer(xc, mc, layer, i); break;
			case XRT_LAYER_EQUIRECT2: do_equirect2_layer(xc, mc, layer, i); break;
			default: U_LOG_E("Unhandled layer type '%i'!", layer->data.type); break;
			}
		}
	}
}

static void
broadcast_timings_to_clients(struct multi_system_compositor *msc, int64_t predicted_display_time_ns)
{
	COMP_TRACE_MARKER();

	os_mutex_lock(&msc->list_and_timing_lock);

	for (size_t i = 0; i < ARRAY_SIZE(msc->clients); i++) {
		struct multi_compositor *mc = msc->clients[i];
		if (mc == NULL) {
			continue;
		}

		os_mutex_lock(&mc->slot_lock);
		mc->slot_next_frame_display = predicted_display_time_ns;
		os_mutex_unlock(&mc->slot_lock);
	}

	os_mutex_unlock(&msc->list_and_timing_lock);
}

static void
broadcast_timings_to_pacers(struct multi_system_compositor *msc,
                            int64_t predicted_display_time_ns,
                            int64_t predicted_display_period_ns,
                            int64_t diff_ns)
{
	COMP_TRACE_MARKER();

	os_mutex_lock(&msc->list_and_timing_lock);

	for (size_t i = 0; i < ARRAY_SIZE(msc->clients); i++) {
		struct multi_compositor *mc = msc->clients[i];
		if (mc == NULL) {
			continue;
		}

		u_pa_info(                       //
		    mc->upa,                     //
		    predicted_display_time_ns,   //
		    predicted_display_period_ns, //
		    diff_ns);                    //

		os_mutex_lock(&mc->slot_lock);
		mc->slot_next_frame_display = predicted_display_time_ns;
		os_mutex_unlock(&mc->slot_lock);
	}

	msc->last_timings.predicted_display_time_ns = predicted_display_time_ns;
	msc->last_timings.predicted_display_period_ns = predicted_display_period_ns;
	msc->last_timings.diff_ns = diff_ns;

	os_mutex_unlock(&msc->list_and_timing_lock);
}

static void
wait_frame(struct os_precise_sleeper *sleeper, struct xrt_compositor *xc, int64_t frame_id, int64_t wake_up_time_ns)
{
	COMP_TRACE_MARKER();

	// Wait until the given wake up time.
	u_wait_until(sleeper, wake_up_time_ns);

	int64_t now_ns = os_monotonic_get_ns();

	// Signal that we woke up.
	xrt_comp_mark_frame(xc, frame_id, XRT_COMPOSITOR_FRAME_POINT_WOKE, now_ns);
}

static void
update_session_state_locked(struct multi_system_compositor *msc)
{
	struct xrt_compositor *xc = &msc->xcn->base;

	//! @todo Make this not be hardcoded.
	const struct xrt_begin_session_info begin_session_info = {
	    .view_type = XRT_VIEW_TYPE_STEREO,
	    .ext_hand_tracking_enabled = false,
	    .ext_hand_tracking_data_source_enabled = false,
	    .ext_eye_gaze_interaction_enabled = false,
	    .ext_hand_interaction_enabled = false,
	    .htc_facial_tracking_enabled = false,
	    .fb_body_tracking_enabled = false,
	    .fb_face_tracking2_enabled = false,
	    .meta_body_tracking_full_body_enabled = false,
	    .meta_body_tracking_calibration_enabled = false,
	};

	switch (msc->sessions.state) {
	case MULTI_SYSTEM_STATE_INIT_WARM_START:
		// Produce at least one frame on init.
		msc->sessions.state = MULTI_SYSTEM_STATE_STOPPING;
		xrt_comp_begin_session(xc, &begin_session_info);
		U_LOG_I("Doing warm start, %u active app session(s).", (uint32_t)msc->sessions.active_count);
		break;

	case MULTI_SYSTEM_STATE_STOPPED:
		if (msc->sessions.active_count == 0) {
			break;
		}

		msc->sessions.state = MULTI_SYSTEM_STATE_RUNNING;
		xrt_comp_begin_session(xc, &begin_session_info);
		U_LOG_I("Started native session, %u active app session(s).", (uint32_t)msc->sessions.active_count);
		break;

	case MULTI_SYSTEM_STATE_RUNNING:
		if (msc->sessions.active_count > 0) {
			break;
		}

		msc->sessions.state = MULTI_SYSTEM_STATE_STOPPING;
		U_LOG_D("Stopping native session, %u active app session(s).", (uint32_t)msc->sessions.active_count);
		break;

	case MULTI_SYSTEM_STATE_STOPPING:
		// Just in case
		if (msc->sessions.active_count > 0) {
			msc->sessions.state = MULTI_SYSTEM_STATE_RUNNING;
			U_LOG_D("Restarting native session, %u active app session(s).",
			        (uint32_t)msc->sessions.active_count);
			break;
		}

		msc->sessions.state = MULTI_SYSTEM_STATE_STOPPED;
		xrt_comp_end_session(xc);
		U_LOG_I("Stopped native session, %u active app session(s).", (uint32_t)msc->sessions.active_count);
		break;

	case MULTI_SYSTEM_STATE_INVALID:
	default:
		U_LOG_E("Got invalid state %u", msc->sessions.state);
		msc->sessions.state = MULTI_SYSTEM_STATE_STOPPING;
		assert(false);
	}
}

static int
multi_main_loop(struct multi_system_compositor *msc)
{
	U_TRACE_SET_THREAD_NAME("Multi Client Module");
	os_thread_helper_name(&msc->oth, "Multi Client Module");

#ifdef XRT_OS_LINUX
	// Try to raise priority of this thread.
	u_linux_try_to_set_realtime_priority_on_thread(U_LOGGING_INFO, "Multi Client Module");
#endif

	struct xrt_compositor *xc = &msc->xcn->base;

	// For wait frame.
	struct os_precise_sleeper sleeper = {0};
	os_precise_sleeper_init(&sleeper);

	// Protect the thread state and the sessions state.
	os_thread_helper_lock(&msc->oth);

	while (os_thread_helper_is_running_locked(&msc->oth)) {

		// Updates msc->sessions.active depending on active client sessions.
		update_session_state_locked(msc);

		if (msc->sessions.state == MULTI_SYSTEM_STATE_STOPPED) {
			// Sleep and wait to be signaled.
			os_thread_helper_wait_locked(&msc->oth);

			// Loop back to running and session check.
			continue;
		}

		// Unlock the thread after the checks has been done.
		os_thread_helper_unlock(&msc->oth);

		int64_t frame_id = -1;
		int64_t wake_up_time_ns = 0;
		int64_t predicted_gpu_time_ns = 0;
		int64_t predicted_display_time_ns = 0;
		int64_t predicted_display_period_ns = 0;

		// Get the information for the next frame.
		xrt_comp_predict_frame(            //
		    xc,                            //
		    &frame_id,                     //
		    &wake_up_time_ns,              //
		    &predicted_gpu_time_ns,        //
		    &predicted_display_time_ns,    //
		    &predicted_display_period_ns); //

		// Do this as soon as we have the new display time.
		broadcast_timings_to_clients(msc, predicted_display_time_ns);

		// Now we can wait.
		wait_frame(&sleeper, xc, frame_id, wake_up_time_ns);

		int64_t now_ns = os_monotonic_get_ns();
		int64_t diff_ns = predicted_display_time_ns - now_ns;

		// Now we know the diff, broadcast to pacers.
		broadcast_timings_to_pacers(msc, predicted_display_time_ns, predicted_display_period_ns, diff_ns);

		xrt_comp_begin_frame(xc, frame_id);

		// Make sure that the clients doesn't go away while we transfer layers.
		os_mutex_lock(&msc->list_and_timing_lock);
		transfer_layers_locked(msc, predicted_display_time_ns, frame_id);
		os_mutex_unlock(&msc->list_and_timing_lock);

		xrt_comp_layer_commit(xc, XRT_GRAPHICS_SYNC_HANDLE_INVALID);

#ifdef XRT_HAVE_LEIA_SR_VULKAN
		// Render per-session clients to their own targets (Phase 4)
		// These sessions were skipped in transfer_layers_locked and render separately
		os_mutex_lock(&msc->list_and_timing_lock);
		render_per_session_clients_locked(msc, predicted_display_time_ns);
		os_mutex_unlock(&msc->list_and_timing_lock);
#endif

		// Re-lock the thread for check in while statement.
		os_thread_helper_lock(&msc->oth);
	}

	// Clean up the sessions state.
	switch (msc->sessions.state) {
	case MULTI_SYSTEM_STATE_RUNNING:
	case MULTI_SYSTEM_STATE_STOPPING:
		U_LOG_I("Stopped native session, shutting down.");
		xrt_comp_end_session(xc);
		break;
	case MULTI_SYSTEM_STATE_STOPPED: break;
	default: assert(false);
	}

	os_thread_helper_unlock(&msc->oth);

	os_precise_sleeper_deinit(&sleeper);

	return 0;
}

static void *
thread_func(void *ptr)
{
	return (void *)(intptr_t)multi_main_loop((struct multi_system_compositor *)ptr);
}


/*
 *
 * System multi compositor functions.
 *
 */

static xrt_result_t
system_compositor_set_state(struct xrt_system_compositor *xsc, struct xrt_compositor *xc, bool visible, bool focused)
{
	struct multi_system_compositor *msc = multi_system_compositor(xsc);
	struct multi_compositor *mc = multi_compositor(xc);
	(void)msc;

	//! @todo Locking?
	if (mc->state.visible != visible || mc->state.focused != focused) {
		mc->state.visible = visible;
		mc->state.focused = focused;

		union xrt_session_event xse = XRT_STRUCT_INIT;
		xse.type = XRT_SESSION_EVENT_STATE_CHANGE;
		xse.state.visible = visible;
		xse.state.focused = focused;

		return multi_compositor_push_event(mc, &xse);
	}

	return XRT_SUCCESS;
}

static xrt_result_t
system_compositor_set_z_order(struct xrt_system_compositor *xsc, struct xrt_compositor *xc, int64_t z_order)
{
	struct multi_system_compositor *msc = multi_system_compositor(xsc);
	struct multi_compositor *mc = multi_compositor(xc);
	(void)msc;

	//! @todo Locking?
	mc->state.z_order = z_order;

	return XRT_SUCCESS;
}

static xrt_result_t
system_compositor_set_main_app_visibility(struct xrt_system_compositor *xsc, struct xrt_compositor *xc, bool visible)
{
	struct multi_system_compositor *msc = multi_system_compositor(xsc);
	struct multi_compositor *mc = multi_compositor(xc);
	(void)msc;

	union xrt_session_event xse = XRT_STRUCT_INIT;
	xse.type = XRT_SESSION_EVENT_OVERLAY_CHANGE;
	xse.overlay.visible = visible;

	return multi_compositor_push_event(mc, &xse);
}

static xrt_result_t
system_compositor_notify_loss_pending(struct xrt_system_compositor *xsc,
                                      struct xrt_compositor *xc,
                                      int64_t loss_time_ns)
{
	struct multi_system_compositor *msc = multi_system_compositor(xsc);
	struct multi_compositor *mc = multi_compositor(xc);
	(void)msc;

	union xrt_session_event xse = XRT_STRUCT_INIT;
	xse.type = XRT_SESSION_EVENT_LOSS_PENDING;
	xse.loss_pending.loss_time_ns = loss_time_ns;

	return multi_compositor_push_event(mc, &xse);
}

static xrt_result_t
system_compositor_notify_lost(struct xrt_system_compositor *xsc, struct xrt_compositor *xc)
{
	struct multi_system_compositor *msc = multi_system_compositor(xsc);
	struct multi_compositor *mc = multi_compositor(xc);
	(void)msc;

	union xrt_session_event xse = XRT_STRUCT_INIT;
	xse.type = XRT_SESSION_EVENT_LOST;

	return multi_compositor_push_event(mc, &xse);
}

static xrt_result_t
system_compositor_notify_display_refresh_changed(struct xrt_system_compositor *xsc,
                                                 struct xrt_compositor *xc,
                                                 float from_display_refresh_rate_hz,
                                                 float to_display_refresh_rate_hz)
{
	struct multi_system_compositor *msc = multi_system_compositor(xsc);
	struct multi_compositor *mc = multi_compositor(xc);
	(void)msc;

	union xrt_session_event xse = XRT_STRUCT_INIT;
	xse.type = XRT_SESSION_EVENT_DISPLAY_REFRESH_RATE_CHANGE;
	xse.display.from_display_refresh_rate_hz = from_display_refresh_rate_hz;
	xse.display.to_display_refresh_rate_hz = to_display_refresh_rate_hz;

	return multi_compositor_push_event(mc, &xse);
}


/*
 *
 * System compositor functions.
 *
 */

static xrt_result_t
system_compositor_create_native_compositor(struct xrt_system_compositor *xsc,
                                           const struct xrt_session_info *xsi,
                                           struct xrt_session_event_sink *xses,
                                           struct xrt_compositor_native **out_xcn)
{
	struct multi_system_compositor *msc = multi_system_compositor(xsc);

	return multi_compositor_create(msc, xsi, xses, out_xcn);
}

static void
system_compositor_destroy(struct xrt_system_compositor *xsc)
{
	struct multi_system_compositor *msc = multi_system_compositor(xsc);

	// Destroy the render thread first, destroy also stops the thread.
	os_thread_helper_destroy(&msc->oth);

	u_paf_destroy(&msc->upaf);

	xrt_comp_native_destroy(&msc->xcn);

	os_mutex_destroy(&msc->list_and_timing_lock);

	free(msc);
}


/*
 *
 * 'Exported' functions.
 *
 */

void
multi_system_compositor_update_session_status(struct multi_system_compositor *msc, bool active)
{
	os_thread_helper_lock(&msc->oth);

	if (active) {
		assert(msc->sessions.active_count < UINT32_MAX);
		msc->sessions.active_count++;

		// If the thread is sleeping wake it up.
		os_thread_helper_signal_locked(&msc->oth);
	} else {
		assert(msc->sessions.active_count > 0);
		msc->sessions.active_count--;
	}

	os_thread_helper_unlock(&msc->oth);
}

xrt_result_t
comp_multi_create_system_compositor(struct xrt_compositor_native *xcn,
                                    struct u_pacing_app_factory *upaf,
                                    const struct xrt_system_compositor_info *xsci,
                                    bool do_warm_start,
                                    struct comp_target_service *target_service,
                                    struct xrt_system_compositor **out_xsysc)
{
	struct multi_system_compositor *msc = U_TYPED_CALLOC(struct multi_system_compositor);
	msc->base.create_native_compositor = system_compositor_create_native_compositor;
	msc->base.destroy = system_compositor_destroy;
	msc->xmcc.set_state = system_compositor_set_state;
	msc->xmcc.set_z_order = system_compositor_set_z_order;
	msc->xmcc.set_main_app_visibility = system_compositor_set_main_app_visibility;
	msc->xmcc.notify_loss_pending = system_compositor_notify_loss_pending;
	msc->xmcc.notify_lost = system_compositor_notify_lost;
	msc->xmcc.notify_display_refresh_changed = system_compositor_notify_display_refresh_changed;
	msc->base.xmcc = &msc->xmcc;
	msc->base.info = *xsci;
	msc->upaf = upaf;
	msc->xcn = xcn;

	// Store the target service for per-session rendering (Phase 3)
	msc->target_service = target_service;

	msc->sessions.active_count = 0;
	msc->sessions.state = do_warm_start ? MULTI_SYSTEM_STATE_INIT_WARM_START : MULTI_SYSTEM_STATE_STOPPED;

	os_mutex_init(&msc->list_and_timing_lock);

	//! @todo Make the clients not go from IDLE to READY before we have completed a first frame.
	// Make sure there is at least some sort of valid frame data here.
	msc->last_timings.predicted_display_time_ns = os_monotonic_get_ns();   // As good as any time.
	msc->last_timings.predicted_display_period_ns = U_TIME_1MS_IN_NS * 16; // Just a wild guess.
	msc->last_timings.diff_ns = U_TIME_1MS_IN_NS * 5;                      // Make sure it's not zero at least.

	int ret = os_thread_helper_init(&msc->oth);
	if (ret < 0) {
		return XRT_ERROR_THREADING_INIT_FAILURE;
	}

	os_thread_helper_start(&msc->oth, thread_func, msc);

	*out_xsysc = &msc->base;

	return XRT_SUCCESS;
}
