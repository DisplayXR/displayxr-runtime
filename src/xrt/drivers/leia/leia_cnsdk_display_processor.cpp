// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  CNSDK display processor: wraps CNSDK Vulkan interlacer
 *         as an @ref xrt_display_processor.
 *
 * The display processor owns the leia_cnsdk handle — it creates it
 * via the factory function and destroys it on cleanup.
 *
 * CNSDK manages its own Vulkan command buffers and queue submission,
 * so self_submitting = true and cmd_buffer is always NULL.
 *
 * @author David Fattal
 * @ingroup drv_leia
 */

#include "leia_cnsdk_display_processor.h"
#include "leia_cnsdk.h"

#include "vk/vk_helpers.h"
#include "util/u_logging.h"

#include <cstdlib>


/*!
 * Implementation struct wrapping CNSDK as xrt_display_processor.
 */
struct leia_cnsdk_display_processor
{
	struct xrt_display_processor base;
	struct leia_cnsdk *cnsdk; //!< Owned — destroyed in cnsdk_dp_destroy.
	struct vk_bundle *vk;     //!< Cached vk_bundle (not owned).

	VkRenderPass render_pass;   //!< Render pass for framebuffer compatibility.
	uint32_t view_count;        //!< Active mode (1=2D, 2=stereo).
};

static inline struct leia_cnsdk_display_processor *
leia_cnsdk_display_processor(struct xrt_display_processor *xdp)
{
	return (struct leia_cnsdk_display_processor *)xdp;
}


/*
 *
 * xrt_display_processor interface methods.
 *
 */

static void
cnsdk_dp_process_atlas(struct xrt_display_processor *xdp,
                       VkCommandBuffer cmd_buffer,
                       VkImage_XDP atlas_image,
                       VkImageView atlas_view,
                       uint32_t view_width,
                       uint32_t view_height,
                       uint32_t tile_columns,
                       uint32_t tile_rows,
                       VkFormat_XDP view_format,
                       VkFramebuffer target_fb,
                       VkImage_XDP target_image,
                       uint32_t target_width,
                       uint32_t target_height,
                       VkFormat_XDP target_format,
                       int32_t canvas_offset_x,
                       int32_t canvas_offset_y,
                       uint32_t canvas_width,
                       uint32_t canvas_height)
{
	// TODO(#85): Pass canvas_offset_x/y to CNSDK for interlacing phase
	// correction once CNSDK supports sub-rect offset.
	(void)canvas_offset_x;
	(void)canvas_offset_y;
	(void)canvas_width;
	(void)canvas_height;

	struct leia_cnsdk_display_processor *ldp = leia_cnsdk_display_processor(xdp);

	// 2D mode: no interlacing needed.
	// TODO: Implement 2D blit passthrough (like leia_display_processor.cpp).
	// For now the compositor fallback handles 2D rendering.
	if (ldp->view_count == 1) {
		return;
	}

	// CNSDK manages its own command buffers — cmd_buffer is NULL (self_submitting).
	// Atlas is guaranteed content-sized SBS (2*view_width x view_height)
	// by compositor crop-blit. Pass directly to CNSDK atlas-mode interlacer.
	leia_cnsdk_weave_atlas(ldp->cnsdk,
	                       ldp->vk->device,
	                       ldp->vk->physical_device,
	                       atlas_view,
	                       (VkFormat)target_format,
	                       view_width * tile_columns,
	                       view_height * tile_rows,
	                       tile_columns * tile_rows,
	                       target_fb,
	                       (VkImage)target_image);
}

static bool
cnsdk_dp_request_display_mode(struct xrt_display_processor *xdp, bool enable_3d)
{
	struct leia_cnsdk_display_processor *ldp = leia_cnsdk_display_processor(xdp);
	ldp->view_count = enable_3d ? 2 : 1;
	// TODO: Call CNSDK backlight control API for 2D/3D switching.
	return true;
}

static bool
cnsdk_dp_get_hardware_3d_state(struct xrt_display_processor *xdp, bool *out_is_3d)
{
	struct leia_cnsdk_display_processor *ldp = leia_cnsdk_display_processor(xdp);
	// CNSDK does not yet expose a hardware 3D state query.
	// Report based on our tracked view_count.
	*out_is_3d = (ldp->view_count == 2);
	return true;
}

static VkRenderPass
cnsdk_dp_get_render_pass(struct xrt_display_processor *xdp)
{
	struct leia_cnsdk_display_processor *ldp = leia_cnsdk_display_processor(xdp);
	return ldp->render_pass;
}

static void
cnsdk_dp_destroy(struct xrt_display_processor *xdp)
{
	struct leia_cnsdk_display_processor *ldp = leia_cnsdk_display_processor(xdp);
	struct vk_bundle *vk = ldp->vk;

	if (vk != NULL) {
		if (ldp->render_pass != VK_NULL_HANDLE) {
			vk->vkDestroyRenderPass(vk->device, ldp->render_pass, NULL);
		}
	}

	leia_cnsdk_destroy(&ldp->cnsdk);
	free(ldp);
}


/*
 *
 * Factory function — matches xrt_dp_factory_vk_fn_t signature.
 *
 */

extern "C" xrt_result_t
leia_cnsdk_dp_factory_vk(void *vk_bundle_ptr,
                          void *vk_cmd_pool,
                          void *window_handle,
                          int32_t target_format,
                          struct xrt_display_processor **out_xdp)
{
	(void)vk_cmd_pool;
	(void)window_handle;

	// Extract Vulkan handles from vk_bundle.
	struct vk_bundle *vk = (struct vk_bundle *)vk_bundle_ptr;

	struct leia_cnsdk *cnsdk = NULL;
	xrt_result_t ret = leia_cnsdk_create(&cnsdk);
	if (ret != XRT_SUCCESS || cnsdk == NULL) {
		U_LOG_E("Failed to create CNSDK core, continuing without interlacing");
		return ret != XRT_SUCCESS ? ret : XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	struct leia_cnsdk_display_processor *ldp =
	    (struct leia_cnsdk_display_processor *)calloc(1, sizeof(*ldp));
	if (ldp == NULL) {
		leia_cnsdk_destroy(&cnsdk);
		return XRT_ERROR_ALLOCATION;
	}

	// Create a render pass compatible with the CNSDK interlacer's output.
	// The interlacer renders to a single color attachment (no depth).
	// Use the target_format passed by the compositor, or B8G8R8A8_UNORM as default.
	VkFormat rp_format = (target_format != 0) ? (VkFormat)target_format : VK_FORMAT_B8G8R8A8_UNORM;
	VkAttachmentDescription color_attachment = {
	    .format = rp_format,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
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
	VkRenderPass render_pass = VK_NULL_HANDLE;
	VkResult vk_ret = vk->vkCreateRenderPass(vk->device, &rp_info, NULL, &render_pass);
	if (vk_ret != VK_SUCCESS) {
		U_LOG_E("CNSDK VK DP: failed to create render pass: %d", vk_ret);
		leia_cnsdk_destroy(&cnsdk);
		free(ldp);
		return XRT_ERROR_VULKAN;
	}

	ldp->base.self_submitting = true;
	ldp->base.process_atlas = cnsdk_dp_process_atlas;
	ldp->base.get_render_pass = cnsdk_dp_get_render_pass;
	ldp->base.get_predicted_eye_positions = NULL; // TODO: CNSDK face tracking
	ldp->base.get_window_metrics = NULL;          // Not applicable on Android
	ldp->base.request_display_mode = cnsdk_dp_request_display_mode;
	ldp->base.get_hardware_3d_state = cnsdk_dp_get_hardware_3d_state;
	ldp->base.get_display_dimensions = NULL;      // TODO: Query from CNSDK
	ldp->base.get_display_pixel_info = NULL;      // TODO: Query from CNSDK
	ldp->base.destroy = cnsdk_dp_destroy;

	ldp->cnsdk = cnsdk;
	ldp->vk = vk;
	ldp->render_pass = render_pass;
	ldp->view_count = 2;

	*out_xdp = &ldp->base;

	U_LOG_W("Created CNSDK display processor (factory, self_submitting, render_pass=%p)",
	        (void *)render_pass);

	return XRT_SUCCESS;
}
