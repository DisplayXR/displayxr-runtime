// Copyright 2019-2024, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Multi client wrapper compositor.
 * @author Pete Black <pblack@collabora.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Korcan Hussein <korcan.hussein@collabora.com>
 * @ingroup comp_multi
 *
 * @note DisplayXR-specific: this is the Monado-legacy multi-client orchestrator.
 * DisplayXR's workspace mode uses a separate per-client compositor
 * (`d3d11_service_compositor`) and its own multi-client orchestration
 * (`d3d11_multi_compositor`) inside `compositor/d3d11_service/comp_d3d11_service.cpp`.
 * The code in this file is reachable only via `compositor/null/null_compositor.c`
 * (headless testing) and `targets/sdl_test/sdl_compositor.c` (dev harness).
 * Modifying it does NOT affect workspace-mode performance or behavior.
 */

#include "xrt/xrt_config_os.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_handles.h"
#include "xrt/xrt_session.h"
#include "xrt/xrt_display_metrics.h"

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
#include "main/comp_target.h"

// Per-session rendering support (Phase 4)
#include "util/comp_swapchain.h"
#include "util/comp_render_helpers.h"
#include "util/u_tiling.h"

// Vulkan helpers needed for Y-flip atlas blit (not Leia-specific)
#include "vk/vk_helpers.h"
#include "vk/vk_hud_blend.h"

#include "xrt/xrt_system.h"
#include "math/m_api.h"

#include "render/render_interface.h"

#ifdef XRT_OS_WINDOWS
#include "comp_d3d11_window.h"
#endif

#ifdef XRT_BUILD_DRIVER_QWERTY
#include "qwerty/qwerty_interface.h"
#include "xrt/xrt_system.h"
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

	// Get the swapchain for this view (xscs[] is indexed by view index)
	struct xrt_swapchain *xsc = layer->xscs[view_index];
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

/*!
 * Initialize intermediate composite resources for pre-display-processing layer compositing.
 * Creates a tiled atlas image, per-eye views, render pass, framebuffers,
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

	// Create per-eye composite images (separate images for clean display processor input)
	// TRANSFER_SRC needed for potential vkCmdBlitImage operations.
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

	// Porter-Duff "over" for both color and alpha so dst.a is preserved
	// through layered composition (see issue #225). The previous
	// (srcA=ONE_MINUS_SRC_ALPHA, dstA=ONE) was quadratic in src.a and
	// clobbered opaque destinations when a semi-transparent layer landed
	// on top — breaking compose-under-bg downstream.
	VkPipelineColorBlendAttachmentState blend_att = {
	    .blendEnable = VK_TRUE,
	    .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
	                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
	    .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
	    .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
	    .colorBlendOp = VK_BLEND_OP_ADD,
	    .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
	    .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
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
 * Composite all layers (projection + window-space) into the intermediate atlas
 * targets before display processing. This is the pre-display-processing compositing step.
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
	                            &leftProjArray)) {
		U_LOG_W("[composite] Could not extract left projection view");
		return false;
	}

	// Mono mode (viewCount=1): duplicate left view for right eye so the
	// compositing pipeline can run unchanged.  The right composite output
	// is identical to the left and is simply discarded by the caller.
	bool mono_composite = (proj_layer->data.view_count < 2);
	if (mono_composite) {
		rightProjView = leftProjView;
		rightProjImage = leftProjImage;
		rightProjArray = leftProjArray;
	} else if (!get_session_layer_view(proj_layer, 1, &imgW, &imgH, &imgFmt, &rightProjView,
	                                    &rightProjImage, &rightProjArray)) {
		U_LOG_W("[composite] Could not extract right projection view");
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
		bool same_source = (shared_imgs[0] == shared_imgs[1] &&
		                    shared_layers[0] == shared_layers[1]);

		// Per-eye source offsets for single-swapchain SBS apps.
		// Mono mode: right eye duplicates left eye offsets.
		int src_off_x[2], src_off_y[2];
		src_off_x[0] = proj_layer->data.proj.v[0].sub.rect.offset.w;
		src_off_y[0] = proj_layer->data.proj.v[0].sub.rect.offset.h;
		src_off_x[1] = mono_composite ? src_off_x[0] : proj_layer->data.proj.v[1].sub.rect.offset.w;
		src_off_y[1] = mono_composite ? src_off_y[0] : proj_layer->data.proj.v[1].sub.rect.offset.h;

		// Pre-barriers: shared images GENERAL->TRANSFER_SRC, preblit UNDEFINED->TRANSFER_DST
		// Deduplicate source barrier when both eyes share the same image.
		VkImageMemoryBarrier pre[4];
		uint32_t pre_count = 0;
		pre[pre_count++] = (VkImageMemoryBarrier){
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = 0,
		    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		    .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
		    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		    .image = shared_imgs[0],
		    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, shared_layers[0], 1},
		};
		if (!same_source) {
			pre[pre_count++] = (VkImageMemoryBarrier){
			    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			    .srcAccessMask = 0,
			    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
			    .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
			    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			    .image = shared_imgs[1],
			    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, shared_layers[1], 1},
			};
		}
		pre[pre_count++] = (VkImageMemoryBarrier){
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = 0,
		    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		    .image = mc->session_render.preblit_images[0],
		    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		};
		pre[pre_count++] = (VkImageMemoryBarrier){
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = 0,
		    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		    .image = mc->session_render.preblit_images[1],
		    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		};
		vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
		                         pre_count, pre);

		// Blit shared images into preblit copies with per-eye source offsets
		for (int eye = 0; eye < 2; eye++) {
			VkImageBlit region = {
			    .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, shared_layers[eye], 1},
			    .srcOffsets = {{src_off_x[eye], src_off_y[eye], 0},
			                  {src_off_x[eye] + (int32_t)cw, src_off_y[eye] + (int32_t)ch, 1}},
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
		// Deduplicate source barrier when both eyes share the same image.
		VkImageMemoryBarrier post[4];
		uint32_t post_count = 0;
		post[post_count++] = (VkImageMemoryBarrier){
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		    .dstAccessMask = 0,
		    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
		    .image = shared_imgs[0],
		    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, shared_layers[0], 1},
		};
		if (!same_source) {
			post[post_count++] = (VkImageMemoryBarrier){
			    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			    .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
			    .dstAccessMask = 0,
			    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
			    .image = shared_imgs[1],
			    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, shared_layers[1], 1},
			};
		}
		post[post_count++] = (VkImageMemoryBarrier){
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
		    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		    .image = mc->session_render.preblit_images[0],
		    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		};
		post[post_count++] = (VkImageMemoryBarrier){
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
		    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		    .image = mc->session_render.preblit_images[1],
		    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		};
		vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
		                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
		                         0, 0, NULL, 0, NULL, post_count, post);
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

			// UV post_transform: identity {0,0,1,1} because the preblit
			// images already contain the correctly cropped per-eye content.
			// The original norm_rect maps into the full SBS swapchain UV
			// space, but the preblit step (above) extracted each eye's
			// sub-region into a full-extent local image. Using norm_rect
			// here would double-crop (e.g. sampling only UVs [0,0.5] of
			// an already-cropped 960px image → showing only 480px).
			ubo_data.post_transform.x = 0.0f;
			ubo_data.post_transform.y = 0.0f;
			ubo_data.post_transform.w = 1.0f;
			ubo_data.post_transform.h = 1.0f;
			if (proj_layer->data.flip_y) {
				// GL textures are Y-flipped: remap UV y from [0,1]→[1,0]
				// to flip the preblit content. Same pattern as overlay layers.
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
 * Get tile layout from the active rendering mode of the head device.
 * Defaults to 2 columns, 1 row (stereo) if not available.
 */
static void
get_active_tile_layout(struct multi_compositor *mc, uint32_t *out_tile_columns, uint32_t *out_tile_rows)
{
	*out_tile_columns = 2;
	*out_tile_rows = 1;

	struct xrt_device *xdev_head = (mc->xsysd != NULL) ? mc->xsysd->static_roles.head : NULL;
	if (xdev_head != NULL && xdev_head->hmd != NULL) {
		uint32_t idx = xdev_head->hmd->active_rendering_mode_index;
		if (idx < xdev_head->rendering_mode_count) {
			uint32_t tc = xdev_head->rendering_modes[idx].tile_columns;
			uint32_t tr = xdev_head->rendering_modes[idx].tile_rows;
			if (tc > 0 && tr > 0) {
				*out_tile_columns = tc;
				*out_tile_rows = tr;
			}
		}
	}
}

/*!
 * Ensure the atlas intermediate image exists for display processors
 * that prefer packed atlas input. Creates a single image at
 * (tile_columns * per_eye_width x tile_rows * height) so all views can be
 * blitted into a tiled atlas before passing to the display processor.
 * Reuses the flip_sbs_* fields in session_render.
 * Recreates if size or format changed.
 */
static bool
ensure_session_atlas_image(struct multi_compositor *mc, struct vk_bundle *vk, int per_eye_width, int height,
                         uint32_t tile_columns, uint32_t tile_rows, VkFormat format)
{
	if (mc->session_render.flip_initialized && mc->session_render.flip_width == per_eye_width &&
	    mc->session_render.flip_height == height && mc->session_render.flip_format == format) {
		return true;
	}

	// Destroy old if resizing
	if (mc->session_render.flip_initialized) {
		if (mc->session_render.flip_sbs_view != VK_NULL_HANDLE)
			vk->vkDestroyImageView(vk->device, mc->session_render.flip_sbs_view, NULL);
		if (mc->session_render.flip_sbs_image != VK_NULL_HANDLE)
			vk->vkDestroyImage(vk->device, mc->session_render.flip_sbs_image, NULL);
		if (mc->session_render.flip_sbs_memory != VK_NULL_HANDLE)
			vk->vkFreeMemory(vk->device, mc->session_render.flip_sbs_memory, NULL);
		mc->session_render.flip_initialized = false;
	}

	uint32_t atlas_width = tile_columns * (uint32_t)per_eye_width;
	uint32_t atlas_height = tile_rows * (uint32_t)height;
	VkExtent2D extent = {atlas_width, atlas_height};
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
		U_LOG_E("[per-session] Failed to create atlas image: %s", vk_result_string(ret));
		return false;
	}

	ret = vk_create_view(vk, mc->session_render.flip_sbs_image, VK_IMAGE_VIEW_TYPE_2D, format, range,
	                     &mc->session_render.flip_sbs_view);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[per-session] Failed to create atlas view: %s", vk_result_string(ret));
		return false;
	}

	mc->session_render.flip_width = per_eye_width;
	mc->session_render.flip_height = height;
	mc->session_render.flip_format = format;
	mc->session_render.flip_initialized = true;

	U_LOG_W("[per-session] Created atlas image: %ux%u (per-eye %dx%d, tiles %ux%u) format=%d",
	        atlas_width, atlas_height, per_eye_width, height, tile_columns, tile_rows, format);
	return true;
}

/*!
 * Initialize Vulkan GPU resources for the HUD overlay (image + staging buffer).
 * Called lazily on first HUD render.
 */
static bool
session_render_hud_init_gpu(struct multi_compositor *mc, struct vk_bundle *vk)
{
	if (mc->session_render.hud_gpu_initialized) {
		return true;
	}

	uint32_t hud_w = u_hud_get_width(mc->session_render.hud);
	uint32_t hud_h = u_hud_get_height(mc->session_render.hud);
	uint32_t pixel_size = hud_w * hud_h * 4;
	VkResult ret;

	// Create HUD image (TRANSFER_DST for upload, SAMPLED for blend pipeline)
	VkImageCreateInfo image_info = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .imageType = VK_IMAGE_TYPE_2D,
	    .format = VK_FORMAT_R8G8B8A8_UNORM,
	    .extent = {hud_w, hud_h, 1},
	    .mipLevels = 1,
	    .arrayLayers = 1,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .tiling = VK_IMAGE_TILING_OPTIMAL,
	    .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	ret = vk->vkCreateImage(vk->device, &image_info, NULL, &mc->session_render.hud_image);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[HUD] Failed to create image: %s", vk_result_string(ret));
		return false;
	}

	// Allocate image memory
	VkMemoryRequirements mem_reqs;
	vk->vkGetImageMemoryRequirements(vk->device, mc->session_render.hud_image, &mem_reqs);

	VkMemoryAllocateInfo alloc_info = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .allocationSize = mem_reqs.size,
	};
	if (!vk_get_memory_type(vk, mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
	                        &alloc_info.memoryTypeIndex)) {
		U_LOG_E("[HUD] Failed to find device-local memory type");
		vk->vkDestroyImage(vk->device, mc->session_render.hud_image, NULL);
		mc->session_render.hud_image = VK_NULL_HANDLE;
		return false;
	}

	ret = vk->vkAllocateMemory(vk->device, &alloc_info, NULL, &mc->session_render.hud_memory);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[HUD] Failed to allocate image memory");
		vk->vkDestroyImage(vk->device, mc->session_render.hud_image, NULL);
		mc->session_render.hud_image = VK_NULL_HANDLE;
		return false;
	}
	vk->vkBindImageMemory(vk->device, mc->session_render.hud_image, mc->session_render.hud_memory, 0);

	// Create staging buffer
	VkBufferCreateInfo buf_info = {
	    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
	    .size = pixel_size,
	    .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	};
	ret = vk->vkCreateBuffer(vk->device, &buf_info, NULL, &mc->session_render.hud_staging_buffer);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[HUD] Failed to create staging buffer");
		return false;
	}

	VkMemoryRequirements buf_reqs;
	vk->vkGetBufferMemoryRequirements(vk->device, mc->session_render.hud_staging_buffer, &buf_reqs);

	VkMemoryAllocateInfo buf_alloc = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .allocationSize = buf_reqs.size,
	};
	if (!vk_get_memory_type(vk, buf_reqs.memoryTypeBits,
	                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	                        &buf_alloc.memoryTypeIndex)) {
		U_LOG_E("[HUD] Failed to find host-visible memory type");
		return false;
	}

	ret = vk->vkAllocateMemory(vk->device, &buf_alloc, NULL, &mc->session_render.hud_staging_memory);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[HUD] Failed to allocate staging memory");
		return false;
	}
	vk->vkBindBufferMemory(vk->device, mc->session_render.hud_staging_buffer,
	                       mc->session_render.hud_staging_memory, 0);

	// Persistently map staging buffer
	ret = vk->vkMapMemory(vk->device, mc->session_render.hud_staging_memory, 0, pixel_size, 0,
	                      &mc->session_render.hud_staging_mapped);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[HUD] Failed to map staging buffer");
		return false;
	}

	// Init alpha-blend pipeline for semi-transparent HUD overlay (image is
	// passed per-draw now so the helper can be reused with rotating swapchain
	// images and per-layer destination rects).
	if (!vk_hud_blend_init(&mc->session_render.hud_blend, vk, mc->session_render.target->format)) {
		U_LOG_E("[HUD] Failed to init alpha-blend pipeline");
		return false;
	}

	mc->session_render.hud_gpu_initialized = true;
	U_LOG_W("[HUD] Vulkan GPU resources initialized (%ux%u)", hud_w, hud_h);
	return true;
}

/*!
 * Alpha-blend HUD overlay onto swapchain image (post-weave).
 * Transitions: PRESENT_SRC -> COLOR_ATTACHMENT -> PRESENT_SRC
 */
static void
session_render_hud_overlay(struct multi_compositor *mc,
                           struct vk_bundle *vk,
                           VkCommandBuffer cmd,
                           VkImage swapchain_image,
                           VkImageView swapchain_view,
                           uint32_t fb_width,
                           uint32_t fb_height)
{
	if (mc->session_render.hud == NULL || !u_hud_is_visible()) {
		return;
	}

	// Compute FPS
	uint64_t now_ns = os_monotonic_get_ns();
	if (mc->session_render.hud_last_frame_time_ns != 0) {
		float dt_ms = (float)(now_ns - mc->session_render.hud_last_frame_time_ns) / 1e6f;
		mc->session_render.hud_smoothed_frame_time_ms =
		    mc->session_render.hud_smoothed_frame_time_ms * 0.9f + dt_ms * 0.1f;
	}
	mc->session_render.hud_last_frame_time_ns = now_ns;

	float fps = 0.0f;
	if (mc->session_render.hud_smoothed_frame_time_ms > 0.0f) {
		fps = 1000.0f / mc->session_render.hud_smoothed_frame_time_ms;
	}

	// Get eye positions and display dims via display processor vtable
	struct xrt_eye_positions eye_pos = {0};
	float disp_w_mm = 0.0f, disp_h_mm = 0.0f;
	float nom_x = 0.0f, nom_y = 0.0f, nom_z = 600.0f;

	if (mc->session_render.display_processor != NULL) {
		if (xrt_display_processor_get_predicted_eye_positions(
		        mc->session_render.display_processor, &eye_pos) && eye_pos.valid) {
			// eye_pos populated with N-view positions
		}

		float dim_w = 0.0f, dim_h = 0.0f;
		if (xrt_display_processor_get_display_dimensions(
		        mc->session_render.display_processor, &dim_w, &dim_h)) {
			disp_w_mm = dim_w * 1000.0f;
			disp_h_mm = dim_h * 1000.0f;
		}
	}

	// Get head device for device name and forward vector
	struct xrt_device *xdev = (mc->xsysd != NULL) ? mc->xsysd->static_roles.head : NULL;

	// Fallback: display info from xrt_system_compositor_info (populated at init)
	float zoom_scale = 0.0f;
	if (disp_w_mm == 0.0f && disp_h_mm == 0.0f) {
		const struct xrt_system_compositor_info *info = &mc->msc->base.info;
		if (info->display_width_m > 0.0f) {
			disp_w_mm = info->display_width_m * 1000.0f;
			disp_h_mm = info->display_height_m * 1000.0f;
			nom_y = info->nominal_viewer_y_m * 1000.0f;
			nom_z = info->nominal_viewer_z_m * 1000.0f;
		}
	}

	// Get render dimensions from last delivered layer
	uint32_t render_w = 0, render_h = 0;
	for (uint32_t i = 0; i < mc->delivered.layer_count; i++) {
		enum xrt_layer_type type = mc->delivered.layers[i].data.type;
		if (type == XRT_LAYER_PROJECTION || type == XRT_LAYER_PROJECTION_DEPTH) {
			render_w = mc->delivered.layers[i].data.proj.v[0].sub.rect.extent.w;
			render_h = mc->delivered.layers[i].data.proj.v[0].sub.rect.extent.h;
			break;
		}
	}

	// Virtual display position + forward vector from qwerty device pose.
	float vdisp_x = 0.0f, vdisp_y = 0.0f, vdisp_z = 0.0f;
	float fwd_x = 0.0f, fwd_y = 0.0f, fwd_z = -1.0f;
#ifdef XRT_BUILD_DRIVER_QWERTY
	if (mc->xsysd != NULL) {
		struct xrt_pose qwerty_pose;
		if (qwerty_get_hmd_pose(mc->xsysd->xdevs, mc->xsysd->xdev_count, &qwerty_pose)) {
			vdisp_x = qwerty_pose.position.x;
			vdisp_y = qwerty_pose.position.y;
			vdisp_z = qwerty_pose.position.z;
			struct xrt_vec3 fwd_in = {0, 0, -1};
			struct xrt_vec3 fwd_out;
			math_quat_rotate_vec3(&qwerty_pose.orientation, &fwd_in, &fwd_out);
			fwd_x = fwd_out.x;
			fwd_y = fwd_out.y;
			fwd_z = fwd_out.z;
		}
	}
#endif

	// Device name includes mode suffix (set by device's set_property handler)
	const char *device_name = (xdev != NULL) ? xdev->str : NULL;

	struct u_hud_data data = {0};
	data.device_name = device_name;
	data.fps = fps;
	data.frame_time_ms = mc->session_render.hud_smoothed_frame_time_ms;
	data.mode_3d = mc->hardware_display_3d;
	if (xdev != NULL && xdev->hmd != NULL) {
		uint32_t idx = xdev->hmd->active_rendering_mode_index;
		if (idx < xdev->rendering_mode_count) {
			data.rendering_mode_name = xdev->rendering_modes[idx].mode_name;
		}
	}
	data.render_width = render_w;
	data.render_height = render_h;
	if (xdev != NULL && xdev->rendering_mode_count > 0) {
		u_tiling_compute_system_atlas(xdev->rendering_modes, xdev->rendering_mode_count,
		                              &data.swapchain_width, &data.swapchain_height);
	}
	data.window_width = fb_width;
	data.window_height = fb_height;
	data.display_width_mm = disp_w_mm;
	data.display_height_mm = disp_h_mm;
	data.nominal_x = nom_x;
	data.nominal_y = nom_y;
	data.nominal_z = nom_z;
	if (!eye_pos.valid) {
		// Fallback: nominal stereo (in mm)
		eye_pos.count = 2;
		eye_pos.eyes[0] = (struct xrt_eye_position){-0.032f, nom_y / 1000.0f, nom_z / 1000.0f};
		eye_pos.eyes[1] = (struct xrt_eye_position){ 0.032f, nom_y / 1000.0f, nom_z / 1000.0f};
	}
	data.eye_count = eye_pos.count;
	for (uint32_t e = 0; e < eye_pos.count && e < 8; e++) {
		data.eyes[e].x = eye_pos.eyes[e].x * 1000.0f;
		data.eyes[e].y = eye_pos.eyes[e].y * 1000.0f;
		data.eyes[e].z = eye_pos.eyes[e].z * 1000.0f;
	}
	data.eye_tracking_active = eye_pos.is_tracking;

	data.zoom_scale = zoom_scale;
	data.vdisp_x = vdisp_x;
	data.vdisp_y = vdisp_y;
	data.vdisp_z = vdisp_z;
	data.forward_x = fwd_x;
	data.forward_y = fwd_y;
	data.forward_z = fwd_z;

#ifdef XRT_BUILD_DRIVER_QWERTY
	if (mc->xsysd != NULL) {
		struct qwerty_view_state ss;
		if (qwerty_get_view_state(mc->xsysd->xdevs, mc->xsysd->xdev_count, &ss)) {
			data.camera_mode = ss.camera_mode;
			data.cam_spread_factor = ss.cam_spread_factor;
			data.cam_parallax_factor = ss.cam_parallax_factor;
			data.cam_convergence = ss.cam_convergence;
			data.cam_half_tan_vfov = ss.cam_half_tan_vfov;
			data.disp_spread_factor = ss.disp_spread_factor;
			data.disp_parallax_factor = ss.disp_parallax_factor;
			data.disp_vHeight = ss.disp_vHeight;
			data.nominal_viewer_z = ss.nominal_viewer_z;
			data.screen_height_m = ss.screen_height_m;
		}
	}
#endif

	bool dirty = u_hud_update(mc->session_render.hud, &data);

	// Lazy-init GPU resources
	if (!session_render_hud_init_gpu(mc, vk)) {
		return;
	}

	uint32_t hud_w = u_hud_get_width(mc->session_render.hud);
	uint32_t hud_h = u_hud_get_height(mc->session_render.hud);

	// Upload pixels to staging buffer if changed
	if (dirty) {
		memcpy(mc->session_render.hud_staging_mapped,
		       u_hud_get_pixels(mc->session_render.hud),
		       hud_w * hud_h * 4);
	}

	// Transition HUD image: UNDEFINED -> TRANSFER_DST (for copy from staging)
	VkImageMemoryBarrier hud_to_dst = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = 0,
	    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	    .image = mc->session_render.hud_image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &hud_to_dst);

	// Copy staging buffer -> HUD image
	VkBufferImageCopy copy_region = {
	    .bufferOffset = 0,
	    .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	    .imageExtent = {hud_w, hud_h, 1},
	};
	vk->vkCmdCopyBufferToImage(cmd, mc->session_render.hud_staging_buffer,
	                           mc->session_render.hud_image,
	                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                           1, &copy_region);

	// Transition HUD image: TRANSFER_DST -> SHADER_READ_ONLY (for sampling in blend pipeline)
	VkImageMemoryBarrier hud_to_src = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	    .image = mc->session_render.hud_image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &hud_to_src);

	// Alpha-blend HUD onto swapchain (PRESENT_SRC -> COLOR_ATTACHMENT -> PRESENT_SRC)
	static bool mono_hud_blend_logged = false;
	if (!mc->hardware_display_3d && !mono_hud_blend_logged) {
		U_LOG_W("[HUD] Mono (2D) mode: blend draw fb=%ux%u hud=%ux%u init=%d",
		        fb_width, fb_height, hud_w, hud_h,
		        mc->session_render.hud_blend.initialized);
		mono_hud_blend_logged = true;
	}
	// Bottom-left placement with a 10px margin (matches the previous behavior
	// when destination rect was hardcoded inside vk_hud_blend_draw).
	int32_t hud_dst_x = 10;
	int32_t hud_dst_y = (fb_height > hud_h + 10u) ? (int32_t)(fb_height - hud_h - 10u) : 0;
	vk_hud_blend_draw(&mc->session_render.hud_blend, vk, cmd, swapchain_view, swapchain_image, fb_width,
	                   fb_height, mc->session_render.hud_image, hud_w, hud_h,
	                   hud_dst_x, hud_dst_y, hud_w, hud_h);
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

	// Sync hardware_display_3d from device's active rendering mode
	struct xrt_device *xdev_head = (mc->xsysd != NULL) ? mc->xsysd->static_roles.head : NULL;
	if (xdev_head != NULL && xdev_head->hmd != NULL) {
		uint32_t idx = xdev_head->hmd->active_rendering_mode_index;
		if (idx < xdev_head->rendering_mode_count) {
			mc->hardware_display_3d = xdev_head->rendering_modes[idx].hardware_display_3d;
		}
	}

	// App-submitted mono (viewCount=1) forces 2D path regardless of device mode
	if (layer->data.view_count == 1) {
		mc->hardware_display_3d = false;
	}

	// Runtime-side 2D/3D toggle from qwerty V key
#ifdef XRT_BUILD_DRIVER_QWERTY
	if (mc->xsysd != NULL) {
		bool force_2d = false;
		bool toggled =
		    qwerty_check_display_mode_toggle(mc->xsysd->xdevs, mc->xsysd->xdev_count, &force_2d);
		if (toggled) {
			struct xrt_device *head = mc->xsysd->static_roles.head;
			if (head != NULL && head->hmd != NULL) {
				if (force_2d) {
					// Save current 3D mode index before switching to 2D
					mc->last_3d_mode_index = head->hmd->active_rendering_mode_index;
					// Find first 2D mode
					for (uint32_t i = 0; i < head->rendering_mode_count; i++) {
						if (!head->rendering_modes[i].hardware_display_3d) {
							head->hmd->active_rendering_mode_index = i;
							break;
						}
					}
				} else {
					// Restore last known 3D mode index
					head->hmd->active_rendering_mode_index = mc->last_3d_mode_index;
				}
				mc->hardware_display_3d = !force_2d;
			}
			multi_compositor_request_display_mode(mc, !force_2d);
		}

		// Rendering mode change from qwerty 1/2/3 keys (disabled for legacy apps).
		if (!mc->msc->base.info.legacy_app_tile_scaling) {
			int render_mode = -1;
			if (qwerty_check_rendering_mode_change(mc->xsysd->xdevs, mc->xsysd->xdev_count, &render_mode)) {
				struct xrt_device *head = mc->xsysd->static_roles.head;
				if (head != NULL) {
					xrt_device_set_property(head, XRT_DEVICE_PROPERTY_OUTPUT_MODE, render_mode);
				}
			}
		}
	}
#endif

	// Extract per-view info (N-view generalized)
	uint32_t view_count = mc->hardware_display_3d ? layer->data.view_count : 1;
	if (view_count > XRT_MAX_VIEWS)
		view_count = XRT_MAX_VIEWS;

	int imageWidth = 0, imageHeight = 0;
	VkFormat imageFormat = VK_FORMAT_UNDEFINED;
	VkImageView viewImageViews[XRT_MAX_VIEWS];
	VkImage viewImages[XRT_MAX_VIEWS];
	uint32_t viewArrayIndices[XRT_MAX_VIEWS];
	int viewOffsetX[XRT_MAX_VIEWS], viewOffsetY[XRT_MAX_VIEWS];

	bool views_ok = true;
	for (uint32_t v = 0; v < view_count; v++) {
		viewImageViews[v] = VK_NULL_HANDLE;
		viewImages[v] = VK_NULL_HANDLE;
		viewArrayIndices[v] = 0;
		if (!get_session_layer_view(layer, v, &imageWidth, &imageHeight, &imageFormat,
		                            &viewImageViews[v], &viewImages[v], &viewArrayIndices[v])) {
			views_ok = false;
		}
		viewOffsetX[v] = layer->data.proj.v[v].sub.rect.offset.w;
		viewOffsetY[v] = layer->data.proj.v[v].sub.rect.offset.h;
	}

	if (!views_ok) {
		U_LOG_W("[per-session] Could not extract views for per-session rendering (3d=%d, count=%u)",
		        mc->hardware_display_3d, view_count);
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
	if (ret == VK_ERROR_OUT_OF_DATE_KHR) {
		U_LOG_W("[per-session] Swapchain out of date, recreating now");
		recreate_session_swapchain(mc, vk);
		ct = mc->session_render.target;
		mc->session_render.swapchain_needs_recreate = false;

		// Retry acquire after recreation.
		// Accept VK_SUBOPTIMAL_KHR — the image IS acquired, it's just not
		// optimal (e.g. overlay window still resizing asynchronously).
		ret = comp_target_acquire(ct, &buffer_index);
		if (ret != VK_SUCCESS && ret != VK_SUBOPTIMAL_KHR) {
			U_LOG_E("[per-session] Failed to acquire after swapchain recreation: %s",
			        vk_result_string(ret));
			return;
		}
	} else if (ret != VK_SUCCESS && ret != VK_SUBOPTIMAL_KHR) {
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

	// Mono (2D) path: blit single view directly to target, skip atlas and weaving.
	// In 2D mode the app submits only one view (viewCount=1). We blit it
	// directly to the presentation target without atlas packing or
	// light-field interlacing (weaving).
	if (!mc->hardware_display_3d) {
		// Mono + window-space overlays: composite projection + HUD overlays
		// into an intermediate image, then blit left eye to target.
		// Works for both app-submitted mono (viewCount=1) and runtime-forced
		// 2D mode (viewCount=2): composite_layers_to_intermediate() duplicates
		// the left view for the right eye when only 1 view is submitted.
		if (has_window_space_layers(mc)) {
			VkImageView comp_left = VK_NULL_HANDLE, comp_right = VK_NULL_HANDLE;
			if (composite_layers_to_intermediate(mc, vk, cmd, &comp_left, &comp_right)) {
				// composite_images[0] is in SHADER_READ_ONLY_OPTIMAL.
				// Transition it to TRANSFER_SRC, target to TRANSFER_DST.
				VkImageMemoryBarrier hud_mono_pre[2] = {
				    {
				        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				        .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
				        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
				        .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				        .image = mc->session_render.composite_images[0],
				        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
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
				vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
				                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
				                         2, hud_mono_pre);

				uint32_t cw = mc->session_render.composite_width;
				uint32_t ch = mc->session_render.composite_height;

				VkImageBlit hud_mono_blit = {
				    .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
				    .srcOffsets = {{0, 0, 0}, {(int32_t)cw, (int32_t)ch, 1}},
				    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
				    .dstOffsets = {{0, 0, 0},
				                  {(int32_t)framebufferWidth, (int32_t)framebufferHeight, 1}},
				};
				vk->vkCmdBlitImage(cmd, mc->session_render.composite_images[0],
				                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				                   ct->images[buffer_index].handle,
				                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &hud_mono_blit,
				                   VK_FILTER_LINEAR);

				// Post: target TRANSFER_DST → PRESENT_SRC
				VkImageMemoryBarrier hud_mono_post = {
				    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
				    .dstAccessMask = 0,
				    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				    .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
				    .image = ct->images[buffer_index].handle,
				    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
				};
				vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
				                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL,
				                         0, NULL, 1, &hud_mono_post);

				static bool mono_hud_logged = false;
				if (!mono_hud_logged) {
					U_LOG_W("[per-session] Mono (2D) + window-space HUD: "
					        "composited %ux%u -> %ux%u (viewCount=%u)",
					        cw, ch, framebufferWidth, framebufferHeight,
					        layer->data.view_count);
					mono_hud_logged = true;
				}
				goto submit_and_present;
			}
			// Fallthrough to direct blit if compositing fails
		}

		bool flip_y = layer->data.flip_y;
		int src_top = viewOffsetY[0] + (flip_y ? imageHeight : 0);
		int src_bot = viewOffsetY[0] + (flip_y ? 0 : imageHeight);

		// Pre-barriers: source GENERAL → TRANSFER_SRC, target UNDEFINED → TRANSFER_DST
		VkImageMemoryBarrier mono_pre[2] = {
		    {
		        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		        .srcAccessMask = 0,
		        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
		        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		        .image = viewImages[0],
		        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, viewArrayIndices[0], 1},
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
		    .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, viewArrayIndices[0], 1},
		    .srcOffsets = {{viewOffsetX[0], src_top, 0}, {viewOffsetX[0] + imageWidth, src_bot, 1}},
		    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
		    .dstOffsets = {{0, 0, 0}, {(int32_t)framebufferWidth, (int32_t)framebufferHeight, 1}},
		};
		vk->vkCmdBlitImage(cmd, viewImages[0], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
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
		        .image = viewImages[0],
		        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, viewArrayIndices[0], 1},
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
		                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 2,
		                         mono_post);

		static bool mono_logged = false;
		if (!mono_logged) {
			U_LOG_W("[per-session] Mono (2D) blit: %dx%d -> %ux%u (flip_y=%d)", imageWidth,
			        imageHeight, framebufferWidth, framebufferHeight, flip_y);
			mono_logged = true;
		}
		goto submit_and_present;
	}

	// Stereo (3D) path: display processing or SR weaving

	// Get the framebuffer for the current swapchain image
	VkFramebuffer framebuffer = VK_NULL_HANDLE;
	if (mc->session_render.framebuffers != NULL) {
		framebuffer = mc->session_render.framebuffers[buffer_index];
	}

	// Display processor path: blit source sub-regions into a tiled atlas,
	// then pass to the display processor for final output.
	// All display processors now accept atlas input with explicit tile params.
	if (mc->session_render.display_processor != NULL) {
		static bool dp_logged = false;
		if (!dp_logged) {
			U_LOG_W("[per-session] Vulkan display processor: input=%dx%d fmt=%d, fb=%ux%u fmt=%d",
			        imageWidth, imageHeight, imageFormat,
			        framebufferWidth, framebufferHeight, framebufferFormat);
			dp_logged = true;
		}

		// ================================================================
		// ATLAS INPUT PATH: blit eyes into tiled atlas image, then weave
		// with explicit pre/post layout barriers on the target image.
		// Tile layout comes from the active rendering mode (default 2x1
		// for stereo).
		// ================================================================
		{
			// Get tile layout from active rendering mode
			uint32_t tile_columns, tile_rows;
			get_active_tile_layout(mc, &tile_columns, &tile_rows);

			// Determine blit sources — either composited overlay images or
			// direct swapchain sub-regions.
			VkImage blit_sources[XRT_MAX_VIEWS];
			uint32_t blit_arrays[XRT_MAX_VIEWS];
			int blit_off_x[XRT_MAX_VIEWS], blit_off_y[XRT_MAX_VIEWS];
			int blit_eye_w = imageWidth, blit_eye_h = imageHeight;
			bool blit_flip_y = layer->data.flip_y;
			VkImageLayout blit_src_old_layout = VK_IMAGE_LAYOUT_GENERAL;

			for (uint32_t v = 0; v < view_count; v++) {
				blit_sources[v] = viewImages[v];
				blit_arrays[v] = viewArrayIndices[v];
				blit_off_x[v] = viewOffsetX[v];
				blit_off_y[v] = viewOffsetY[v];
			}

			if (has_window_space_layers(mc)) {
				VkImageView comp_left = VK_NULL_HANDLE, comp_right = VK_NULL_HANDLE;
				if (composite_layers_to_intermediate(mc, vk, cmd, &comp_left, &comp_right)) {
					// Composite path currently supports 2 views
					uint32_t comp_count = view_count < 2 ? view_count : 2;
					for (uint32_t v = 0; v < comp_count; v++) {
						blit_sources[v] = mc->session_render.composite_images[v];
						blit_arrays[v] = 0;
						blit_off_x[v] = 0;
						blit_off_y[v] = 0;
					}
					blit_eye_w = (int)mc->session_render.composite_width;
					blit_eye_h = (int)mc->session_render.composite_height;
					blit_flip_y = false;
					blit_src_old_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				}
			}

			// Ensure atlas intermediate image exists (tile_columns*eye_w x tile_rows*eye_h)
			if (!ensure_session_atlas_image(mc, vk, blit_eye_w, blit_eye_h,
			                              tile_columns, tile_rows, imageFormat)) {
				U_LOG_E("[per-session] Failed to ensure atlas image");
				goto submit_and_present;
			}

			// Collect unique source images for barriers
			VkImage unique_sources[XRT_MAX_VIEWS];
			uint32_t unique_arrays[XRT_MAX_VIEWS];
			uint32_t unique_count = 0;
			for (uint32_t v = 0; v < view_count; v++) {
				bool found = false;
				for (uint32_t u = 0; u < unique_count; u++) {
					if (unique_sources[u] == blit_sources[v] &&
					    unique_arrays[u] == blit_arrays[v]) {
						found = true;
						break;
					}
				}
				if (!found) {
					unique_sources[unique_count] = blit_sources[v];
					unique_arrays[unique_count] = blit_arrays[v];
					unique_count++;
				}
			}

			// Pre-barriers: unique sources → TRANSFER_SRC, atlas → TRANSFER_DST
			VkImageMemoryBarrier pre_barriers[XRT_MAX_VIEWS + 1];
			uint32_t pre_count = 0;
			for (uint32_t u = 0; u < unique_count; u++) {
				pre_barriers[pre_count++] = (VkImageMemoryBarrier){
				    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				    .srcAccessMask = 0,
				    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
				    .oldLayout = blit_src_old_layout,
				    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				    .image = unique_sources[u],
				    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, unique_arrays[u], 1},
				};
			}
			pre_barriers[pre_count++] = (VkImageMemoryBarrier){
			    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			    .srcAccessMask = 0,
			    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			    .image = mc->session_render.flip_sbs_image,
			    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
			};
			vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
			                         pre_count, pre_barriers);

			// Blit each view into its tile position in the atlas
			for (uint32_t v = 0; v < view_count; v++) {
				uint32_t tile_x, tile_y;
				u_tiling_view_origin(v, tile_columns, (uint32_t)blit_eye_w, (uint32_t)blit_eye_h,
				                     &tile_x, &tile_y);
				int src_top = blit_off_y[v] + (blit_flip_y ? blit_eye_h : 0);
				int src_bot = blit_off_y[v] + (blit_flip_y ? 0 : blit_eye_h);
				VkImageBlit blit = {
				    .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, blit_arrays[v], 1},
				    .srcOffsets = {{blit_off_x[v], src_top, 0},
				                   {blit_off_x[v] + blit_eye_w, src_bot, 1}},
				    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
				    .dstOffsets = {{(int32_t)tile_x, (int32_t)tile_y, 0},
				                   {(int32_t)(tile_x + blit_eye_w), (int32_t)(tile_y + blit_eye_h), 1}},
				};
				vk->vkCmdBlitImage(cmd, blit_sources[v], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				                   mc->session_render.flip_sbs_image,
				                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				                   1, &blit, VK_FILTER_NEAREST);
			}

			// Post-barriers: unique sources → original layout, atlas → SHADER_READ_ONLY
			VkImageMemoryBarrier post_barriers[XRT_MAX_VIEWS + 1];
			uint32_t post_count = 0;
			for (uint32_t u = 0; u < unique_count; u++) {
				post_barriers[post_count++] = (VkImageMemoryBarrier){
				    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				    .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
				    .dstAccessMask = 0,
				    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				    .newLayout = blit_src_old_layout,
				    .image = unique_sources[u],
				    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, unique_arrays[u], 1},
				};
			}
			post_barriers[post_count++] = (VkImageMemoryBarrier){
			    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
			    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			    .image = mc->session_render.flip_sbs_image,
			    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
			};
			vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
			                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL,
			                         post_count, post_barriers);

			// Pre-weave barrier: target → COLOR_ATTACHMENT_OPTIMAL
			VkImageMemoryBarrier pre_weave = {
			    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			    .srcAccessMask = 0,
			    .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			    .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			    .image = ct->images[buffer_index].handle,
			    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
			};
			vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			                         0, 0, NULL, 0, NULL, 1, &pre_weave);

			// Call display processor with atlas input
			xrt_display_processor_process_atlas(
			    mc->session_render.display_processor, cmd,
			    (VkImage_XDP)mc->session_render.flip_sbs_image, // atlas image (for copy/blit)
			    mc->session_render.flip_sbs_view,  // atlas view (tiled views)
			    (uint32_t)blit_eye_w,              // per-view width
			    (uint32_t)blit_eye_h,              // per-view height
			    tile_columns,                       // tile layout columns
			    tile_rows,                          // tile layout rows
			    (VkFormat_XDP)imageFormat,
			    framebuffer,
			    (VkImage_XDP)ct->images[buffer_index].handle,
			    framebufferWidth, framebufferHeight,
			    (VkFormat_XDP)framebufferFormat,
			    0, 0, 0, 0);

			// Post-weave barrier: target → PRESENT_SRC_KHR (for HUD overlay + present)
			VkImageMemoryBarrier post_weave = {
			    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			    .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			    .dstAccessMask = 0,
			    .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			    .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
			    .image = ct->images[buffer_index].handle,
			    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
			};
			vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			                         0, 0, NULL, 0, NULL, 1, &post_weave);

			goto submit_and_present;
		}

	}

submit_and_present:
	// HUD overlay (post-weave, always readable).
	// Render for all per-session windows (self-owned and app-provided),
	// matching the D3D11 compositor which renders HUD for all sessions.
	session_render_hud_overlay(mc, vk, cmd, ct->images[buffer_index].handle,
	                           ct->images[buffer_index].view,
	                           framebufferWidth, framebufferHeight);

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
	if (ret == VK_ERROR_OUT_OF_DATE_KHR) {
		U_LOG_W("[per-session] Present returned OUT_OF_DATE, flagging for recreation");
		mc->session_render.swapchain_needs_recreate = true;
	} else if (ret == VK_SUBOPTIMAL_KHR) {
		// Presentation succeeded — swapchain size differs from surface but content is shown.
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

		// Lazily initialize per-session render resources if session has external HWND or readback
		// This creates the per-session comp_target and display processor for multi-app support
		// Skip re-init if window close is in progress (avoids VK_ERROR_SURFACE_LOST_KHR on dead HWND)
		if (multi_compositor_has_session_render(mc) && !mc->session_render.initialized &&
		    !mc->session_render.window_close_exit_sent) {
			multi_compositor_init_session_render(mc);
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

	// Runtime-side 2D/3D toggle from qwerty V key for shared compositor path.
	// Per-session clients handle this in render_session_to_own_target().
#ifdef XRT_BUILD_DRIVER_QWERTY
	for (size_t k = 0; k < count; k++) {
		struct multi_compositor *mc = array[k];
		if (mc->session_render.initialized) {
			continue; // skip per-session clients
		}
		if (mc->xsysd != NULL) {
			bool force_2d = false;
			bool toggled =
			    qwerty_check_display_mode_toggle(mc->xsysd->xdevs, mc->xsysd->xdev_count, &force_2d);
			if (toggled) {
				struct xrt_device *head = mc->xsysd->static_roles.head;
				if (head != NULL && head->hmd != NULL) {
					if (force_2d) {
						mc->last_3d_mode_index = head->hmd->active_rendering_mode_index;
						for (uint32_t i = 0; i < head->rendering_mode_count; i++) {
							if (!head->rendering_modes[i].hardware_display_3d) {
								head->hmd->active_rendering_mode_index = i;
								break;
							}
						}
					} else {
						head->hmd->active_rendering_mode_index = mc->last_3d_mode_index;
					}
				}
				multi_compositor_request_display_mode(mc, !force_2d);
			}

			// Rendering mode change from qwerty 1/2/3 keys (disabled for legacy apps).
			if (!mc->msc->base.info.legacy_app_tile_scaling) {
				int render_mode = -1;
				if (qwerty_check_rendering_mode_change(mc->xsysd->xdevs, mc->xsysd->xdev_count,
				                                       &render_mode)) {
					struct xrt_device *head = mc->xsysd->static_roles.head;
					if (head != NULL) {
						xrt_device_set_property(head, XRT_DEVICE_PROPERTY_OUTPUT_MODE,
						                        render_mode);
					}
				}
			}

			break; // one check per frame is enough
		}
	}
#endif

	// Note: qwerty input forwarding to main compositor removed (comp_main no longer exists).
	// Per-session windows handle their own input forwarding.

	// Copy all active layers (skip sessions with per-session rendering - Phase 4).
	for (size_t k = 0; k < count; k++) {
		struct multi_compositor *mc = array[k];
		assert(mc != NULL);

		// Skip sessions with per-session rendering - they render separately to their own targets
		if (mc->session_render.initialized) {
			continue;
		}

		// Sync hardware_display_3d for shared clients
		struct xrt_device *shared_head = (mc->xsysd != NULL) ? mc->xsysd->static_roles.head : NULL;
		if (shared_head != NULL && shared_head->hmd != NULL) {
			uint32_t idx = shared_head->hmd->active_rendering_mode_index;
			if (idx < shared_head->rendering_mode_count) {
				mc->hardware_display_3d = shared_head->rendering_modes[idx].hardware_display_3d;
			}
		}

		for (uint32_t i = 0; i < mc->delivered.layer_count; i++) {
			struct multi_layer_entry *layer = &mc->delivered.layers[i];

			// When 2D mode is active, override projection layers to mono (view_count=1)
			// so the main compositor's dispatch_graphics() uses full-width viewport
			// and skips weaving.
#ifdef XRT_BUILD_DRIVER_QWERTY
			bool override_mono = !mc->hardware_display_3d &&
			                     (layer->data.type == XRT_LAYER_PROJECTION ||
			                      layer->data.type == XRT_LAYER_PROJECTION_DEPTH) &&
			                     layer->data.view_count > 1;
			if (override_mono) {
				struct multi_layer_entry mono_layer = *layer;
				mono_layer.data.view_count = 1;
				switch (layer->data.type) {
				case XRT_LAYER_PROJECTION:
					do_projection_layer(xc, mc, &mono_layer, i);
					break;
				case XRT_LAYER_PROJECTION_DEPTH:
					do_projection_layer_depth(xc, mc, &mono_layer, i);
					break;
				default: break;
				}
				continue;
			}
#endif

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
