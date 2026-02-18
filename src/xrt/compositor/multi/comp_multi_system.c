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

// Vulkan helpers needed for Y-flip SBS blit (not Leia-specific)
#include "vk/vk_helpers.h"

#ifdef XRT_HAVE_LEIA_SR_VULKAN
#include "leia/leia_sr.h"
#include "render/render_interface.h"
#endif

#ifdef XRT_OS_WINDOWS
#include "comp_d3d11_window.h"
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
                       VkImageView *out_image_view,
                       VkImage *out_image,
                       uint32_t *out_array_index)
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
	*out_image = sc->vkic.images[vd->sub.image_index].handle;
	*out_array_index = array_index;

	return (*out_image_view != VK_NULL_HANDLE);
}

#ifdef XRT_HAVE_LEIA_SR_VULKAN

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
	// TRANSFER_SRC needed because session_blit_sbs uses these as vkCmdBlitImage source.
	VkExtent2D eye_extent = {.width = width, .height = height};
	VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
	                          VK_IMAGE_USAGE_SAMPLED_BIT |
	                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

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

	// Create pre-blit local copies of shared projection images (Intel CCS workaround).
	// vkCmdBlitImage works for cross-device shared images on Intel Iris Xe; fragment
	// shader sampling does not. We blit shared images into these local copies, then
	// sample the local copies in the compositing render pass.
	{
		VkImageUsageFlags preblit_usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

		for (int eye = 0; eye < 2; eye++) {
			ret = vk_create_image_simple(vk, eye_extent, format, preblit_usage,
			                             &mc->session_render.preblit_memories[eye],
			                             &mc->session_render.preblit_images[eye]);
			if (ret != VK_SUCCESS) {
				U_LOG_E("[composite] Failed to create preblit image %d: %d", eye, ret);
				goto err_images;
			}

			ret = vk_create_view(vk, mc->session_render.preblit_images[eye],
			                     VK_IMAGE_VIEW_TYPE_2D, format, eye_range,
			                     &mc->session_render.preblit_views[eye]);
			if (ret != VK_SUCCESS) {
				U_LOG_E("[composite] Failed to create preblit view %d: %d", eye, ret);
				goto err_images;
			}
		}
	}

	// Create render pass with LOAD_OP_CLEAR - projection layer is drawn first as fullscreen quad
	VkAttachmentDescription attachment = {
	    .format = format,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
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

	// Load per-session shaders on demand (avoids invalid comp_compositor cast)
	if (!mc->session_render.shaders_loaded) {
		if (!render_shaders_load(&mc->session_render.shaders, vk)) {
			U_LOG_E("[composite] Failed to load shaders");
			goto err_pipe_layout;
		}
		mc->session_render.shaders_loaded = true;
	}

	// Create per-session pipeline cache on demand
	if (mc->session_render.pipeline_cache == VK_NULL_HANDLE) {
		ret = vk_create_pipeline_cache(vk, &mc->session_render.pipeline_cache);
		if (ret != VK_SUCCESS) {
			U_LOG_E("[composite] Failed to create pipeline cache: %d", ret);
			goto err_pipe_layout;
		}
	}

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
	    .cullMode = VK_CULL_MODE_NONE, // 2D compositing — no culling (shader Y-flip reverses winding)
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
	        .module = mc->session_render.shaders.layer_quad_vert,
	        .pName = "main",
	    },
	    {
	        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	        .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
	        .module = mc->session_render.shaders.layer_shared_frag,
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

	ret = vk->vkCreateGraphicsPipelines(vk->device, mc->session_render.pipeline_cache, 1,
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
		if (mc->session_render.preblit_views[i] != VK_NULL_HANDLE) {
			vk->vkDestroyImageView(vk->device, mc->session_render.preblit_views[i], NULL);
			mc->session_render.preblit_views[i] = VK_NULL_HANDLE;
		}
		if (mc->session_render.preblit_images[i] != VK_NULL_HANDLE) {
			vk->vkDestroyImage(vk->device, mc->session_render.preblit_images[i], NULL);
			mc->session_render.preblit_images[i] = VK_NULL_HANDLE;
		}
		if (mc->session_render.preblit_memories[i] != VK_NULL_HANDLE) {
			vk->vkFreeMemory(vk->device, mc->session_render.preblit_memories[i], NULL);
			mc->session_render.preblit_memories[i] = VK_NULL_HANDLE;
		}
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
		if (mc->session_render.preblit_views[i] != VK_NULL_HANDLE) {
			vk->vkDestroyImageView(vk->device, mc->session_render.preblit_views[i], NULL);
			mc->session_render.preblit_views[i] = VK_NULL_HANDLE;
		}
		if (mc->session_render.preblit_images[i] != VK_NULL_HANDLE) {
			vk->vkDestroyImage(vk->device, mc->session_render.preblit_images[i], NULL);
			mc->session_render.preblit_images[i] = VK_NULL_HANDLE;
		}
		if (mc->session_render.preblit_memories[i] != VK_NULL_HANDLE) {
			vk->vkFreeMemory(vk->device, mc->session_render.preblit_memories[i], NULL);
			mc->session_render.preblit_memories[i] = VK_NULL_HANDLE;
		}
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

	// Destroy per-session shaders and pipeline cache
	if (mc->session_render.shaders_loaded) {
		render_shaders_fini(&mc->session_render.shaders, vk);
		mc->session_render.shaders_loaded = false;
	}
	if (mc->session_render.pipeline_cache != VK_NULL_HANDLE) {
		vk->vkDestroyPipelineCache(vk->device, mc->session_render.pipeline_cache, NULL);
		mc->session_render.pipeline_cache = VK_NULL_HANDLE;
	}

	mc->session_render.composite_initialized = false;
}

#endif // XRT_HAVE_LEIA_SR_VULKAN (composite resources)

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

#ifdef XRT_HAVE_LEIA_SR_VULKAN

/*!
 * Composite all layers (projection + window-space) into the intermediate stereo
 * targets before weaving. This is the pre-weaving compositing step.
 *
 * Intel CCS workaround (Intel Iris Xe / Gen12 iGPU):
 * On Intel, fragment shader sampling of cross-device shared images produces a
 * black right eye due to CCS (Color Control Surface) metadata not being resolved.
 * Fix: pre-blit shared projection images into compositor-owned local copies via
 * vkCmdBlitImage (which works on Intel), then sample the local copies in the
 * compositing render pass. This avoids shader reads of cross-device images entirely.
 * Cost: 2 extra same-size blits per frame (~microseconds on modern GPUs).
 * On NVIDIA this is a harmless no-op — NVIDIA does not use CCS compression.
 *
 * @param mc  The multi_compositor
 * @param vk  The Vulkan bundle
 * @param cmd The command buffer to record into
 * @param out_left_view  Output: left eye view of composited result
 * @param out_right_view Output: right eye view of composited result
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
	VkImage leftProjImage = VK_NULL_HANDLE, rightProjImage = VK_NULL_HANDLE;
	uint32_t leftProjArray = 0, rightProjArray = 0;

	if (!get_session_layer_view(proj_layer, 0, &imgW, &imgH, &imgFmt, &leftProjView, &leftProjImage,
	                            &leftProjArray) ||
	    !get_session_layer_view(proj_layer, 1, &imgW, &imgH, &imgFmt, &rightProjView, &rightProjImage,
	                            &rightProjArray)) {
		U_LOG_W("[composite] Could not extract projection views");
		return false;
	}

	// Recreate composite resources if projection size changed (window resize)
	if (mc->session_render.composite_initialized &&
	    ((uint32_t)imgW != mc->session_render.composite_width ||
	     (uint32_t)imgH != mc->session_render.composite_height)) {
		U_LOG_W("[composite] Projection size changed %ux%u -> %ux%u, recreating",
		        mc->session_render.composite_width, mc->session_render.composite_height,
		        (uint32_t)imgW, (uint32_t)imgH);
		fini_composite_resources(mc, vk);
	}

	// Lazily init composite resources if needed
	if (!mc->session_render.composite_initialized) {
		if (!init_composite_resources(mc, vk, (uint32_t)imgW, (uint32_t)imgH, imgFmt)) {
			return false;
		}
	}

	uint32_t cw = mc->session_render.composite_width;
	uint32_t ch = mc->session_render.composite_height;

	// Step 1: Transition both composite images UNDEFINED → COLOR_ATTACHMENT
	// (compositor-owned images, safe to transition; LOAD_OP_CLEAR will initialize them)
	VkImageMemoryBarrier barriers_to_attach[2];
	for (int eye = 0; eye < 2; eye++) {
		barriers_to_attach[eye] = (VkImageMemoryBarrier){
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = 0,
		    .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		    .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		    .image = mc->session_render.composite_images[eye],
		    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		};
	}
	vk->vkCmdPipelineBarrier(cmd,
	                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	                         0, 0, NULL, 0, NULL, 2, barriers_to_attach);

	// Pre-blit shared projection images into compositor-owned local copies (Intel CCS workaround).
	// On Intel Iris Xe (Gen12), fragment shader sampling of cross-device shared images produces
	// a black right eye due to CCS (Color Control Surface) metadata not being resolved for shader
	// reads. vkCmdBlitImage (transfer read) works correctly on Intel, so we blit shared images
	// into local preblit copies, then sample those in the compositing render pass.
	// On NVIDIA this is a harmless extra copy (~microseconds for same-size same-format blit).

	{
		VkImage shared_imgs[2] = {leftProjImage, rightProjImage};
		uint32_t shared_layers[2] = {leftProjArray, rightProjArray};

		// Pre-barriers: shared images GENERAL->TRANSFER_SRC, preblit UNDEFINED->TRANSFER_DST
		VkImageMemoryBarrier pre[4] = {
		    {
		        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		        .srcAccessMask = 0,
		        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
		        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		        .image = shared_imgs[0],
		        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, shared_layers[0], 1},
		    },
		    {
		        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		        .srcAccessMask = 0,
		        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
		        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		        .image = shared_imgs[1],
		        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, shared_layers[1], 1},
		    },
		    {
		        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		        .srcAccessMask = 0,
		        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		        .image = mc->session_render.preblit_images[0],
		        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		    },
		    {
		        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		        .srcAccessMask = 0,
		        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		        .image = mc->session_render.preblit_images[1],
		        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		    },
		};
		vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 4, pre);

		// Blit shared images into preblit copies (same size, NEAREST filter, no Y-flip)
		for (int eye = 0; eye < 2; eye++) {
			VkImageBlit region = {
			    .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, shared_layers[eye], 1},
			    .srcOffsets = {{0, 0, 0}, {(int32_t)cw, (int32_t)ch, 1}},
			    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
			    .dstOffsets = {{0, 0, 0}, {(int32_t)cw, (int32_t)ch, 1}},
			};
			vk->vkCmdBlitImage(cmd, shared_imgs[eye], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			                   mc->session_render.preblit_images[eye],
			                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
			                   VK_FILTER_NEAREST);
		}

		// Post-barriers: shared images TRANSFER_SRC->GENERAL (restore for next frame),
		// preblit TRANSFER_DST->SHADER_READ_ONLY (ready for sampling in render pass)
		VkImageMemoryBarrier post[4] = {
		    {
		        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		        .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		        .dstAccessMask = 0,
		        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
		        .image = shared_imgs[0],
		        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, shared_layers[0], 1},
		    },
		    {
		        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		        .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		        .dstAccessMask = 0,
		        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
		        .image = shared_imgs[1],
		        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, shared_layers[1], 1},
		    },
		    {
		        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
		        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		        .image = mc->session_render.preblit_images[0],
		        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		    },
		    {
		        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
		        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		        .image = mc->session_render.preblit_images[1],
		        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		    },
		};
		vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
		                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
		                         0, 0, NULL, 0, NULL, 4, post);
	}

	// UBO stride for sub-allocation from the persistent UBO buffer
	VkDeviceSize ubo_stride = sizeof(struct xrt_normalized_rect) + sizeof(struct xrt_matrix_4x4);

	// Projection layer image views: use preblit copies (compositor-owned, safe for shader sampling)
	VkImageView proj_views[2] = {
	    mc->session_render.preblit_views[0],
	    mc->session_render.preblit_views[1],
	};

	// Step 2: For each eye — begin render pass, draw projection quad, draw overlays, end render pass.
	// Preblit copies are in SHADER_READ_ONLY_OPTIMAL (transitioned above).
	//
	// Descriptor set allocation: ds[0]=eye0 projection, ds[1]=eye1 projection,
	// ds[2..N]=overlay draws (unique per eye×overlay to avoid aliasing, since
	// vkUpdateDescriptorSets is immediate and GPU reads final host state).
	uint32_t ws_desc_index = 2; // overlays start after both projection descriptor sets
	for (int eye = 0; eye < 2; eye++) {
		// --- Projection layer: fullscreen opaque quad (descriptor set 0) ---
		{
			// Fullscreen MVP: scale the [-0.5, 0.5] unit quad to [-1, 1] NDC
			struct xrt_matrix_4x4 mvp;
			// clang-format off
			mvp.v[0]  = 2.0f; mvp.v[1]  = 0.0f; mvp.v[2]  = 0.0f; mvp.v[3]  = 0.0f;
			mvp.v[4]  = 0.0f; mvp.v[5]  = -2.0f; mvp.v[6]  = 0.0f; mvp.v[7]  = 0.0f;
			mvp.v[8]  = 0.0f; mvp.v[9]  = 0.0f; mvp.v[10] = 1.0f; mvp.v[11] = 0.0f;
			mvp.v[12] = 0.0f; mvp.v[13] = 0.0f; mvp.v[14] = 0.5f; mvp.v[15] = 1.0f;
			// clang-format on

			struct
			{
				struct xrt_normalized_rect post_transform;
				struct xrt_matrix_4x4 mvp;
			} ubo_data;

			// UV post_transform: use projection view's norm_rect to sample
			// only the sub-image region of the swapchain. This handles apps
			// that render at a smaller resolution than the swapchain (e.g.
			// GL apps using recommendedViewScale < 1.0).
			const struct xrt_layer_projection_view_data *pvd = &proj_layer->data.proj.v[eye];
			ubo_data.post_transform.x = pvd->sub.norm_rect.x;
			ubo_data.post_transform.y = pvd->sub.norm_rect.y;
			ubo_data.post_transform.w = pvd->sub.norm_rect.w;
			ubo_data.post_transform.h = pvd->sub.norm_rect.h;

			if (proj_layer->data.flip_y) {
				ubo_data.post_transform.y += ubo_data.post_transform.h;
				ubo_data.post_transform.h = -ubo_data.post_transform.h;
			}
			ubo_data.mvp = mvp;

			// Write UBO data — projection uses first slot per eye
			uint32_t ubo_index = eye;
			VkDeviceSize ubo_offset = ubo_index * ubo_stride;
			memcpy((uint8_t *)mc->session_render.composite_ubo_mapped + ubo_offset,
			       &ubo_data, sizeof(ubo_data));

			// Update descriptor set for this eye with projection image
			VkDescriptorSet ds_set = mc->session_render.composite_desc_sets[eye];

			VkDescriptorBufferInfo buf_desc = {
			    .buffer = mc->session_render.composite_ubo_buffer,
			    .offset = ubo_offset,
			    .range = sizeof(ubo_data),
			};

			VkDescriptorImageInfo img_desc = {
			    .sampler = mc->session_render.composite_sampler,
			    .imageView = proj_views[eye],
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
		}

		// Begin render pass (LOAD_OP_CLEAR clears to transparent black)
		VkClearValue clear_value = {.color = {{0.0f, 0.0f, 0.0f, 0.0f}}};
		VkRenderPassBeginInfo rp_begin = {
		    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		    .renderPass = mc->session_render.composite_render_pass,
		    .framebuffer = mc->session_render.composite_framebuffers[eye],
		    .renderArea = {
		        .offset = {0, 0},
		        .extent = {cw, ch},
		    },
		    .clearValueCount = 1,
		    .pClearValues = &clear_value,
		};
		vk->vkCmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

		// Set viewport and scissor
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

		// Draw projection as fullscreen opaque quad
		VkDescriptorSet proj_ds = mc->session_render.composite_desc_sets[eye];
		vk->vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
		                      mc->session_render.composite_pipeline);
		vk->vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
		                            mc->session_render.composite_pipe_layout,
		                            0, 1, &proj_ds, 0, NULL);
		vk->vkCmdDraw(cmd, 4, 1, 0, 0);

		// --- Overlay layers: alpha-blended quads (descriptor sets 2+) ---
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

			// Compute per-eye disparity offset
			float half_disp = ws->disparity / 2.0f;
			float eye_shift = (eye == 0) ? -half_disp : half_disp;

			// Window-space fractional coords → Vulkan NDC [-1, 1]
			// Vulkan NDC: Y=-1 is top, Y=+1 is bottom.
			// Shader flips Y (pos.y = -pos.y), so ndc_sy must be negative
			// to map shader top (+0.5) to Vulkan top (NDC -1).
			float frac_cx = ws->x + ws->width / 2.0f + eye_shift;
			float frac_cy = ws->y + ws->height / 2.0f;
			float ndc_cx = frac_cx * 2.0f - 1.0f;
			float ndc_cy = frac_cy * 2.0f - 1.0f;
			float ndc_sx = ws->width * 2.0f;
			float ndc_sy = -(ws->height * 2.0f);

			// Build orthographic MVP (quad vert shader uses [-0.5, 0.5] unit quad)
			struct xrt_matrix_4x4 mvp;
			// clang-format off
			mvp.v[0]  = ndc_sx; mvp.v[1]  = 0.0f;   mvp.v[2]  = 0.0f; mvp.v[3]  = 0.0f;
			mvp.v[4]  = 0.0f;   mvp.v[5]  = ndc_sy;  mvp.v[6]  = 0.0f; mvp.v[7]  = 0.0f;
			mvp.v[8]  = 0.0f;   mvp.v[9]  = 0.0f;   mvp.v[10] = 1.0f; mvp.v[11] = 0.0f;
			mvp.v[12] = ndc_cx; mvp.v[13] = ndc_cy;  mvp.v[14] = 0.5f; mvp.v[15] = 1.0f;
			// clang-format on

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

			// Write UBO data — each draw has a unique UBO slot matching its descriptor set index
			uint32_t ubo_index = ws_desc_index;
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
			    .imageLayout = VK_IMAGE_LAYOUT_GENERAL, // Overlay shared images stay in GENERAL
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

			// Draw overlay quad (pipeline already bound)
			vk->vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
			                            mc->session_render.composite_pipe_layout,
			                            0, 1, &ds_set, 0, NULL);
			vk->vkCmdDraw(cmd, 4, 1, 0, 0);

			ws_desc_index++;
		}

		vk->vkCmdEndRenderPass(cmd);
	}

	// Step 3: Transition both composite images to SHADER_READ_ONLY for weaver input
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

#endif // XRT_HAVE_LEIA_SR_VULKAN (composite_layers_to_intermediate)

/*!
 * Recreate the per-session swapchain after a window resize.
 * Waits for all GPU work, recreates swapchain images, destroys old framebuffers,
 * creates new framebuffers, and reallocates command buffers/fences if image count changed.
 *
 * @param mc The multi_compositor with per-session rendering
 * @param vk The Vulkan bundle
 */
static void
recreate_session_swapchain(struct multi_compositor *mc, struct vk_bundle *vk)
{
	struct comp_target *ct = mc->session_render.target;
	if (ct == NULL) {
		return;
	}

	U_LOG_W("[per-session] Recreating swapchain (window resized)...");

	// 1. Wait for ALL pending GPU work to complete
	if (mc->session_render.fenced_buffer >= 0) {
		vk->vkWaitForFences(vk->device, 1,
		                    &mc->session_render.fences[mc->session_render.fenced_buffer],
		                    VK_TRUE, UINT64_MAX);
		mc->session_render.fenced_buffer = -1;
	}
	// Also wait for all fences to ensure no in-flight commands reference old swapchain
	for (uint32_t i = 0; i < mc->session_render.buffer_count; i++) {
		if (mc->session_render.fences[i] != VK_NULL_HANDLE) {
			vk->vkWaitForFences(vk->device, 1, &mc->session_render.fences[i], VK_TRUE, UINT64_MAX);
		}
	}

	uint32_t old_image_count = mc->session_render.buffer_count;

	// 2. Recreate swapchain images (queries new surface extent internally)
	struct comp_target_create_images_info info = {
	    .image_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
	    .format_count = 1,
	    .formats = {VK_FORMAT_B8G8R8A8_SRGB},
	    .extent = {ct->width, ct->height}, // Will be overridden by surface caps
	    .color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
	    .present_mode = VK_PRESENT_MODE_FIFO_KHR,
	};
	comp_target_create_images(ct, &info);

	if (!comp_target_has_images(ct)) {
		U_LOG_E("[per-session] Failed to recreate swapchain images");
		mc->session_render.swapchain_needs_recreate = false;
		return;
	}

	uint32_t new_image_count = ct->image_count;

	// 3. Destroy old framebuffers
	if (mc->session_render.framebuffers != NULL) {
		for (uint32_t i = 0; i < old_image_count; i++) {
			if (mc->session_render.framebuffers[i] != VK_NULL_HANDLE) {
				vk->vkDestroyFramebuffer(vk->device, mc->session_render.framebuffers[i], NULL);
			}
		}
	}

	// 4. Handle image_count change - reallocate arrays if needed
	if (new_image_count != old_image_count) {
		U_LOG_W("[per-session] Image count changed: %u -> %u", old_image_count, new_image_count);

		// Free old command buffers from the pool
		vk->vkFreeCommandBuffers(vk->device, mc->session_render.cmd_pool,
		                         old_image_count, mc->session_render.cmd_buffers);

		// Destroy old fences
		for (uint32_t i = 0; i < old_image_count; i++) {
			if (mc->session_render.fences[i] != VK_NULL_HANDLE) {
				vk->vkDestroyFence(vk->device, mc->session_render.fences[i], NULL);
			}
		}

		// Reallocate arrays
		free(mc->session_render.cmd_buffers);
		free(mc->session_render.fences);
		free(mc->session_render.framebuffers);

		mc->session_render.cmd_buffers = U_TYPED_ARRAY_CALLOC(VkCommandBuffer, new_image_count);
		mc->session_render.fences = U_TYPED_ARRAY_CALLOC(VkFence, new_image_count);
		mc->session_render.framebuffers = U_TYPED_ARRAY_CALLOC(VkFramebuffer, new_image_count);

		if (!mc->session_render.cmd_buffers || !mc->session_render.fences || !mc->session_render.framebuffers) {
			U_LOG_E("[per-session] Failed to allocate new arrays after resize");
			mc->session_render.swapchain_needs_recreate = false;
			return;
		}

		// Allocate new command buffers
		VkCommandBufferAllocateInfo cb_alloc = {
		    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		    .commandPool = mc->session_render.cmd_pool,
		    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		    .commandBufferCount = new_image_count,
		};
		VkResult vk_ret = vk->vkAllocateCommandBuffers(vk->device, &cb_alloc, mc->session_render.cmd_buffers);
		if (vk_ret != VK_SUCCESS) {
			U_LOG_E("[per-session] Failed to allocate new command buffers: %s", vk_result_string(vk_ret));
			mc->session_render.swapchain_needs_recreate = false;
			return;
		}

		// Create new fences (signaled)
		VkFenceCreateInfo fence_info = {
		    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
		    .flags = VK_FENCE_CREATE_SIGNALED_BIT,
		};
		for (uint32_t i = 0; i < new_image_count; i++) {
			vk_ret = vk->vkCreateFence(vk->device, &fence_info, NULL, &mc->session_render.fences[i]);
			if (vk_ret != VK_SUCCESS) {
				U_LOG_E("[per-session] Failed to create fence %u: %s", i, vk_result_string(vk_ret));
			}
		}
	} else {
		// Same image count - just reallocate framebuffer array for new images
		free(mc->session_render.framebuffers);
		mc->session_render.framebuffers = U_TYPED_ARRAY_CALLOC(VkFramebuffer, new_image_count);
	}

	// 5. Create new framebuffers bound to new swapchain images
	if (mc->session_render.framebuffers != NULL && mc->session_render.render_pass != VK_NULL_HANDLE) {
		for (uint32_t i = 0; i < new_image_count; i++) {
			VkFramebufferCreateInfo fb_info = {
			    .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
			    .renderPass = mc->session_render.render_pass,
			    .attachmentCount = 1,
			    .pAttachments = &ct->images[i].view,
			    .width = ct->width,
			    .height = ct->height,
			    .layers = 1,
			};
			VkResult vk_ret = vk->vkCreateFramebuffer(vk->device, &fb_info, NULL,
			                                           &mc->session_render.framebuffers[i]);
			if (vk_ret != VK_SUCCESS) {
				U_LOG_E("[per-session] Failed to create framebuffer %u: %s", i, vk_result_string(vk_ret));
				mc->session_render.framebuffers[i] = VK_NULL_HANDLE;
			}
		}
	}

	// 6. Update state
	mc->session_render.buffer_count = new_image_count;
	mc->session_render.fenced_buffer = -1;
	mc->session_render.swapchain_needs_recreate = false;

	U_LOG_W("[per-session] Swapchain recreated: %ux%u, %u images", ct->width, ct->height, new_image_count);

}

/*!
 * Ensure the SBS (side-by-side) flip image exists for Y-flipping GL textures before display processing.
 * Allocates a single image of (eye_width*2, eye_height) with TRANSFER_DST + SAMPLED usage.
 * Both eyes are blitted side-by-side into this image, matching the display processor's SBS stereo mode.
 * Recreates if size or format changed.
 */
static bool
ensure_session_flip_images(struct multi_compositor *mc, struct vk_bundle *vk, int width, int height, VkFormat format)
{
	if (mc->session_render.flip_initialized && mc->session_render.flip_width == width &&
	    mc->session_render.flip_height == height && mc->session_render.flip_format == format) {
		return true;
	}

	if (mc->session_render.flip_initialized) {
		if (mc->session_render.flip_sbs_view != VK_NULL_HANDLE)
			vk->vkDestroyImageView(vk->device, mc->session_render.flip_sbs_view, NULL);
		if (mc->session_render.flip_sbs_image != VK_NULL_HANDLE)
			vk->vkDestroyImage(vk->device, mc->session_render.flip_sbs_image, NULL);
		if (mc->session_render.flip_sbs_memory != VK_NULL_HANDLE)
			vk->vkFreeMemory(vk->device, mc->session_render.flip_sbs_memory, NULL);
		mc->session_render.flip_initialized = false;
	}

	// SBS image: double-wide (left eye in [0,w], right eye in [w,2w])
	VkExtent2D extent = {(uint32_t)(width * 2), (uint32_t)height};
	VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	VkImageSubresourceRange range = {
	    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	    .levelCount = 1,
	    .layerCount = 1,
	};

	VkResult ret = vk_create_image_simple(vk, extent, format, usage,
	                                      &mc->session_render.flip_sbs_memory,
	                                      &mc->session_render.flip_sbs_image);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[per-session] Failed to create SBS flip image: %s", vk_result_string(ret));
		return false;
	}

	ret = vk_create_view(vk, mc->session_render.flip_sbs_image, VK_IMAGE_VIEW_TYPE_2D, format, range,
	                     &mc->session_render.flip_sbs_view);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[per-session] Failed to create SBS flip image view: %s", vk_result_string(ret));
		return false;
	}

	mc->session_render.flip_width = width;
	mc->session_render.flip_height = height;
	mc->session_render.flip_format = format;
	mc->session_render.flip_initialized = true;

	U_LOG_W("[per-session] Created SBS flip image: %dx%d (per-eye %dx%d) format=%d",
	        width * 2, height, width, height, format);
	return true;
}

/*!
 * Blit both eyes with Y-flip into a single SBS (side-by-side) image.
 * Left eye goes to [0, eye_width], right eye to [eye_width, 2*eye_width].
 * Sources: GENERAL -> TRANSFER_SRC -> GENERAL (imported images, must return to GENERAL)
 * Dest:    UNDEFINED -> TRANSFER_DST -> SHADER_READ_ONLY (SBS image, ready for weaver sampling)
 *
 * @param flip_y If true, flip Y axis during blit (needed for GL textures which are Y-up).
 *               If false, copy without flip (for VK textures which are already Y-down).
 */
static void
session_blit_sbs(struct vk_bundle *vk,
                 VkCommandBuffer cmd,
                 VkImage left_src,
                 uint32_t left_array_index,
                 VkImage right_src,
                 uint32_t right_array_index,
                 VkImage sbs_dst,
                 int eye_width,
                 int eye_height,
                 bool flip_y)
{
	// Pre-barriers: sources GENERAL->TRANSFER_SRC, SBS dest UNDEFINED->TRANSFER_DST
	VkImageMemoryBarrier pre_barriers[3] = {
	    {
	        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	        .srcAccessMask = 0,
	        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
	        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
	        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	        .image = left_src,
	        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, left_array_index, 1},
	    },
	    {
	        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	        .srcAccessMask = 0,
	        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
	        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
	        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	        .image = right_src,
	        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, right_array_index, 1},
	    },
	    {
	        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	        .srcAccessMask = 0,
	        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	        .image = sbs_dst,
	        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	    },
	};
	vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL,
	                         0, NULL, 3, pre_barriers);

	// Blit left eye into left half of SBS image [0, eye_width]
	// When flip_y: src Y is inverted (GL Y-up -> VK Y-down)
	int src_top = flip_y ? eye_height : 0;
	int src_bot = flip_y ? 0 : eye_height;

	VkImageBlit left_blit = {
	    .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, left_array_index, 1},
	    .srcOffsets = {{0, src_top, 0}, {eye_width, src_bot, 1}},
	    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	    .dstOffsets = {{0, 0, 0}, {eye_width, eye_height, 1}},
	};
	vk->vkCmdBlitImage(cmd, left_src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sbs_dst,
	                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &left_blit, VK_FILTER_NEAREST);

	// Blit right eye into right half of SBS image [eye_width, 2*eye_width]
	VkImageBlit right_blit = {
	    .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, right_array_index, 1},
	    .srcOffsets = {{0, src_top, 0}, {eye_width, src_bot, 1}},
	    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	    .dstOffsets = {{eye_width, 0, 0}, {eye_width * 2, eye_height, 1}},
	};
	vk->vkCmdBlitImage(cmd, right_src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sbs_dst,
	                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &right_blit, VK_FILTER_NEAREST);

	// Post-barriers: sources TRANSFER_SRC->GENERAL, SBS dest TRANSFER_DST->SHADER_READ_ONLY
	VkImageMemoryBarrier post_barriers[3] = {
	    {
	        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	        .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
	        .dstAccessMask = 0,
	        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
	        .image = left_src,
	        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, left_array_index, 1},
	    },
	    {
	        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	        .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
	        .dstAccessMask = 0,
	        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
	        .image = right_src,
	        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, right_array_index, 1},
	    },
	    {
	        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
	        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	        .image = sbs_dst,
	        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	    },
	};
	vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
	                         NULL, 0, NULL, 3, post_barriers);
}

/*!
 * Render a single per-session client to its own comp_target using display processing.
 *
 * @param mc The multi_compositor with per-session rendering
 * @param vk The Vulkan bundle
 * @param display_time_ns The display timestamp
 */
static void
render_session_to_own_target(struct multi_compositor *mc, struct vk_bundle *vk, int64_t display_time_ns)
{
	struct comp_target *ct = mc->session_render.target;

	if (ct == NULL) {
		U_LOG_E("[per-session] Per-session target not initialized");
		return;
	}

	// Recreate swapchain if flagged (from previous frame's VK_SUBOPTIMAL_KHR)
#ifdef XRT_OS_WINDOWS
	if (mc->session_render.swapchain_needs_recreate &&
	    mc->session_render.owns_window && mc->session_render.own_window != NULL &&
	    comp_d3d11_window_is_in_size_move(mc->session_render.own_window)) {
		// Defer recreation until drag ends — avoids stutter from texture reallocation
	} else
#endif
	if (mc->session_render.swapchain_needs_recreate) {
		recreate_session_swapchain(mc, vk);
		// Re-read ct since create_images updates it in place
		ct = mc->session_render.target;
	}

	// Must have at least one layer
	if (mc->delivered.layer_count == 0) {
		U_LOG_W("[per-session] No layers delivered, skipping");
		return;
	}

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

	// Detect mono (2D) vs stereo (3D) submission
	bool is_mono = (layer->data.view_count == 1);

	// Extract left and right view info
	int imageWidth = 0, imageHeight = 0;
	VkFormat imageFormat = VK_FORMAT_UNDEFINED;
	VkImageView leftImageView = VK_NULL_HANDLE;
	VkImageView rightImageView = VK_NULL_HANDLE;
	VkImage leftImage = VK_NULL_HANDLE, rightImage = VK_NULL_HANDLE;
	uint32_t leftArrayIndex = 0, rightArrayIndex = 0;

	bool leftOk = get_session_layer_view(layer, 0, &imageWidth, &imageHeight, &imageFormat, &leftImageView,
	                                     &leftImage, &leftArrayIndex);
	bool rightOk = false;
	if (!is_mono) {
		rightOk = get_session_layer_view(layer, 1, &imageWidth, &imageHeight, &imageFormat, &rightImageView,
		                                 &rightImage, &rightArrayIndex);
	}

	if (!leftOk || (!is_mono && !rightOk)) {
		U_LOG_W("[per-session] Could not extract views for per-session rendering (mono=%d)", is_mono);
		return;
	}

	// Wait for pending fence if exists (from previous frame using same buffer)
	if (mc->session_render.fenced_buffer >= 0) {
		VkResult fence_ret = vk->vkWaitForFences(vk->device, 1,
		                                         &mc->session_render.fences[mc->session_render.fenced_buffer],
		                                         VK_TRUE, UINT64_MAX);
		if (fence_ret != VK_SUCCESS) {
			U_LOG_E("[per-session] Failed to wait for fence: %s", vk_result_string(fence_ret));
		}
		mc->session_render.fenced_buffer = -1;
	}

#ifdef XRT_OS_WINDOWS
	// During drag of self-owned window, synchronize with WM_PAINT cycle.
	if (mc->session_render.owns_window && mc->session_render.own_window != NULL &&
	    comp_d3d11_window_is_in_size_move(mc->session_render.own_window)) {
		comp_d3d11_window_wait_for_paint(mc->session_render.own_window);
	}
#endif

	// Acquire the next swapchain image from the per-session target
	uint32_t buffer_index = 0;
	VkResult ret = comp_target_acquire(ct, &buffer_index);
	if (ret == VK_ERROR_OUT_OF_DATE_KHR || ret == VK_SUBOPTIMAL_KHR) {
		U_LOG_W("[per-session] Swapchain out of date/suboptimal (%s), recreating now",
		        vk_result_string(ret));
		recreate_session_swapchain(mc, vk);
		ct = mc->session_render.target;
		mc->session_render.swapchain_needs_recreate = false;

		// Retry acquire after recreation
		ret = comp_target_acquire(ct, &buffer_index);
		if (ret != VK_SUCCESS) {
			U_LOG_E("[per-session] Failed to acquire after swapchain recreation: %s",
			        vk_result_string(ret));
			return;
		}
	} else if (ret != VK_SUCCESS) {
		U_LOG_E("[per-session] Failed to acquire per-session target image: %s", vk_result_string(ret));
		return;
	}

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

	// Mono (2D) path: blit single view directly to target, skip SBS and weaving.
	// In 2D mode the app submits only one view (viewCount=1). We blit it
	// directly to the presentation target without side-by-side packing or
	// light-field interlacing (weaving).
	if (is_mono) {
		bool flip_y = layer->data.flip_y;
		int src_top = flip_y ? imageHeight : 0;
		int src_bot = flip_y ? 0 : imageHeight;

		// Pre-barriers: source GENERAL → TRANSFER_SRC, target UNDEFINED → TRANSFER_DST
		VkImageMemoryBarrier mono_pre[2] = {
		    {
		        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		        .srcAccessMask = 0,
		        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
		        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		        .image = leftImage,
		        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, leftArrayIndex, 1},
		    },
		    {
		        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		        .srcAccessMask = 0,
		        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		        .image = ct->images[buffer_index].handle,
		        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		    },
		};
		vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, mono_pre);

		// Blit mono view to fill entire target
		VkImageBlit mono_blit = {
		    .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, leftArrayIndex, 1},
		    .srcOffsets = {{0, src_top, 0}, {imageWidth, src_bot, 1}},
		    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
		    .dstOffsets = {{0, 0, 0}, {(int32_t)framebufferWidth, (int32_t)framebufferHeight, 1}},
		};
		vk->vkCmdBlitImage(cmd, leftImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		                   ct->images[buffer_index].handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
		                   &mono_blit, VK_FILTER_LINEAR);

		// Post-barriers: source → GENERAL, target → PRESENT_SRC_KHR
		VkImageMemoryBarrier mono_post[2] = {
		    {
		        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		        .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		        .dstAccessMask = 0,
		        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
		        .image = leftImage,
		        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, leftArrayIndex, 1},
		    },
		    {
		        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		        .dstAccessMask = 0,
		        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		        .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		        .image = ct->images[buffer_index].handle,
		        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		    },
		};
		vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
		                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 2,
		                         mono_post);

		static bool mono_logged = false;
		if (!mono_logged) {
			U_LOG_W("[per-session] Mono (2D) blit: %dx%d -> %ux%u (flip_y=%d)", imageWidth,
			        imageHeight, framebufferWidth, framebufferHeight, flip_y);
			mono_logged = true;
		}
		goto submit_and_present;
	}

	// Stereo (3D) path: SBS blit + weaving/display processing
	VkImageView weaveLeft = leftImageView;
	VkImageView weaveRight = rightImageView;
	int weaveWidth = imageWidth;
	int weaveHeight = imageHeight;

	bool blit_flip_y = layer->data.flip_y;

#ifdef XRT_HAVE_LEIA_SR_VULKAN
	// If window-space overlay layers are present, composite all layers into
	// intermediate per-eye images first, then use those for the SBS blit.
	// Shared projection images are pre-blitted into compositor-owned local copies
	// to work around Intel CCS issues. See composite_layers_to_intermediate() docs.
	if (has_window_space_layers(mc)) {
		VkImageView comp_left_view = VK_NULL_HANDLE, comp_right_view = VK_NULL_HANDLE;
		if (composite_layers_to_intermediate(mc, vk, cmd, &comp_left_view, &comp_right_view)) {
			// Use composited images as SBS blit sources instead of raw projection images.
			// Compositing already applied flip_y, so disable it for the blit.
			leftImage = mc->session_render.composite_images[0];
			rightImage = mc->session_render.composite_images[1];
			leftArrayIndex = 0;
			rightArrayIndex = 0;
			blit_flip_y = false;

			// Transition composited images SHADER_READ_ONLY → GENERAL
			// so session_blit_sbs can transition them GENERAL → TRANSFER_SRC.
			VkImageMemoryBarrier readToGeneral[2];
			for (int e = 0; e < 2; e++) {
				readToGeneral[e] = (VkImageMemoryBarrier){
				    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				    .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
				    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
				    .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
				    .image = mc->session_render.composite_images[e],
				    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
				};
			}
			vk->vkCmdPipelineBarrier(cmd,
			                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			                         VK_PIPELINE_STAGE_TRANSFER_BIT,
			                         0, 0, NULL, 0, NULL, 2, readToGeneral);

		}
	}
#endif // XRT_HAVE_LEIA_SR_VULKAN

	// Both GL and VK paths blit shared images into a local SBS (side-by-side) image.
	// This creates a compositor-owned local copy for the weaver to read from.
	// GL path flips Y (GL is Y-up, VK is Y-down). VK path copies without flip.
	// When composited, flip_y is already applied so blit_flip_y is false.
	// The weaver's SBS mode (right=VK_NULL_HANDLE) reads left half as left eye,
	// right half as right eye.
	if (ensure_session_flip_images(mc, vk, imageWidth, imageHeight, imageFormat)) {
		session_blit_sbs(vk, cmd, leftImage, leftArrayIndex, rightImage, rightArrayIndex,
		                 mc->session_render.flip_sbs_image, imageWidth, imageHeight, blit_flip_y);
		weaveLeft = mc->session_render.flip_sbs_view;
		weaveRight = VK_NULL_HANDLE;
		weaveWidth = imageWidth * 2;
	} else {
		U_LOG_W("[per-session] Failed to create SBS image, using raw views");
	}

	// Get the framebuffer for the current swapchain image
	VkFramebuffer framebuffer = VK_NULL_HANDLE;
	if (mc->session_render.framebuffers != NULL) {
		framebuffer = mc->session_render.framebuffers[buffer_index];
	}

	// Transition swapchain image to COLOR_ATTACHMENT_OPTIMAL before weaving
	// (matches Vulkan weaving example: weaver expects this layout)
	{
		VkImageMemoryBarrier pre_weave_barrier = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = 0,
		    .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		    .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		    .image = ct->images[buffer_index].handle,
		    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		};
		vk->vkCmdPipelineBarrier(cmd,
		                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		                         0, 0, NULL, 0, NULL, 1, &pre_weave_barrier);
	}

	// Log one-time diagnostic info
	{
		static bool diag_logged = false;
		if (!diag_logged) {
			U_LOG_W("[per-session] Weave params: viewport=(%d,%d,%u,%u), input=%dx%d fmt=%d, fb=%ux%u fmt=%d",
			        viewport.offset.x, viewport.offset.y,
			        viewport.extent.width, viewport.extent.height,
			        weaveWidth, weaveHeight, imageFormat,
			        framebufferWidth, framebufferHeight, framebufferFormat);
			diag_logged = true;
		}
	}

#ifdef XRT_HAVE_LEIA_SR_VULKAN
	// Log SR window position diagnostics (once)
	if (mc->session_render.weaver != NULL) {
		static bool sr_diag_logged = false;
		if (!sr_diag_logged) {
			leiasr_log_window_diagnostics(mc->session_render.weaver,
			                              mc->session_render.external_window_handle);
			sr_diag_logged = true;
		}
	}
#endif

	// Perform display processing via generic display processor interface
	if (mc->session_render.display_processor != NULL) {
		static bool dp_logged = false;
		if (!dp_logged) {
			U_LOG_W("[per-session] Vulkan rendering via display processor interface");
			dp_logged = true;
		}

		xrt_display_processor_process_views(
		    mc->session_render.display_processor,
		    cmd,
		    weaveLeft,
		    weaveRight,
		    (uint32_t)weaveWidth,
		    (uint32_t)weaveHeight,
		    (VkFormat_XDP)imageFormat,
		    framebuffer,
		    framebufferWidth,
		    framebufferHeight,
		    (VkFormat_XDP)framebufferFormat);
	}
#ifdef XRT_HAVE_LEIA_SR_VULKAN
	else if (mc->session_render.weaver != NULL) {
		static bool fallback_logged = false;
		if (!fallback_logged) {
			U_LOG_W("[per-session] Vulkan weaving via direct SR call (display processor unavailable)");
			fallback_logged = true;
		}

		leiasr_weave(mc->session_render.weaver, cmd, weaveLeft, weaveRight, viewport, weaveWidth, weaveHeight,
		             imageFormat, framebuffer, (int)framebufferWidth, (int)framebufferHeight,
		             framebufferFormat);
	}
#endif

	// Transition swapchain image to PRESENT_SRC_KHR after weaving
	// (matches Vulkan weaving example: image must be presentable)
	{
		VkImageMemoryBarrier post_weave_barrier = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		    .dstAccessMask = 0,
		    .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		    .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		    .image = ct->images[buffer_index].handle,
		    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		};
		vk->vkCmdPipelineBarrier(cmd,
		                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
		                         0, 0, NULL, 0, NULL, 1, &post_weave_barrier);
	}

submit_and_present:
	// End command buffer
	ret = vk->vkEndCommandBuffer(cmd);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[per-session] Failed to end command buffer: %s", vk_result_string(ret));
		return;
	}

	// Submit command buffer with fence for async completion.
	// Wait on present_complete (signaled by vkAcquireNextImageKHR) before writing
	// to the swapchain image, and signal render_complete for comp_target_present.
	VkSemaphore wait_sem = ct->semaphores.present_complete;
	VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkSemaphore signal_sem = ct->semaphores.render_complete;
	VkSubmitInfo submit_info = {
	    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	    .waitSemaphoreCount = (wait_sem != VK_NULL_HANDLE) ? 1 : 0,
	    .pWaitSemaphores = (wait_sem != VK_NULL_HANDLE) ? &wait_sem : NULL,
	    .pWaitDstStageMask = (wait_sem != VK_NULL_HANDLE) ? &wait_stage : NULL,
	    .commandBufferCount = 1,
	    .pCommandBuffers = &cmd,
	    .signalSemaphoreCount = (signal_sem != VK_NULL_HANDLE) ? 1 : 0,
	    .pSignalSemaphores = (signal_sem != VK_NULL_HANDLE) ? &signal_sem : NULL,
	};

	ret = vk->vkQueueSubmit(vk->main_queue->queue, 1, &submit_info, mc->session_render.fences[buffer_index]);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[per-session] Failed to submit per-session render: %s", vk_result_string(ret));
		return;
	}

	// CRITICAL: Wait for GPU work to complete before returning.
	// With cross-device external memory sharing (null compositor + VK app),
	// there is no GPU-level synchronization between Device A (compositor) and
	// Device B (app). Without this wait, the compositor's GPU read of shared
	// images may still be in-flight when the app starts writing to the same
	// images for the next frame, causing VK_ERROR_DEVICE_LOST on Intel.
	// GL apps don't hit this because GL has implicit driver-level sync.
	ret = vk->vkWaitForFences(vk->device, 1, &mc->session_render.fences[buffer_index], VK_TRUE, UINT64_MAX);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[per-session] Failed to wait for render fence: %s", vk_result_string(ret));
	}
	mc->session_render.fenced_buffer = -1; // Fence already waited, no deferred wait needed

	// Present the image (GPU work is complete, semaphore already signaled)
	ret = comp_target_present(ct, vk->main_queue->queue, buffer_index, 0, display_time_ns, 0);
	if (ret == VK_ERROR_OUT_OF_DATE_KHR || ret == VK_SUBOPTIMAL_KHR) {
		U_LOG_W("[per-session] Present returned %s, flagging for recreation",
		        vk_result_string(ret));
		mc->session_render.swapchain_needs_recreate = true;
	} else if (ret != VK_SUCCESS) {
		U_LOG_E("[per-session] Failed to present per-session target: %s", vk_result_string(ret));
	}
#ifdef XRT_OS_WINDOWS
	// Signal WM_PAINT handler that frame is done
	if (mc->session_render.owns_window && mc->session_render.own_window != NULL) {
		comp_d3d11_window_signal_paint_done(mc->session_render.own_window);
	}
#endif

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
			continue;
		}

#ifdef XRT_OS_WINDOWS
		// Skip rendering if self-owned window was closed
		if (mc->session_render.owns_window && mc->session_render.own_window != NULL &&
		    !comp_d3d11_window_is_valid(mc->session_render.own_window)) {
			U_LOG_W("[per-session] Client %zu: skipping (window closed)", k);
			int64_t now_ns = os_monotonic_get_ns();
			multi_compositor_retire_delivered_locked(mc, now_ns);
			continue;
		}
#endif
#ifdef XRT_OS_MACOS
		// Skip rendering if macOS window was closed
		{
			extern bool oxr_macos_window_closed(void);
			if (oxr_macos_window_closed()) {
				int64_t now_ns = os_monotonic_get_ns();
				multi_compositor_retire_delivered_locked(mc, now_ns);
				continue;
			}
		}
#endif

		session_count++;

		// Render this session to its own target
		render_session_to_own_target(mc, vk, display_time_ns);

		// Retire the delivered frame for this session
		int64_t now_ns = os_monotonic_get_ns();
		multi_compositor_retire_delivered_locked(mc, now_ns);
	}
}



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

		// Even if it's not shown, make sure that frames are delivered.
		multi_compositor_deliver_any_frames(mc, display_time_ns);

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

		// Render per-session clients to their own targets (Phase 4)
		// These sessions were skipped in transfer_layers_locked and render separately
		os_mutex_lock(&msc->list_and_timing_lock);
		render_per_session_clients_locked(msc, predicted_display_time_ns);
		os_mutex_unlock(&msc->list_and_timing_lock);

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
                                    bool xcn_is_comp_compositor,
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
	msc->xcn_is_comp_compositor = xcn_is_comp_compositor;

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
