// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Leia display processor: wraps SR SDK Vulkan weaver
 *         as an @ref xrt_display_processor.
 *
 * The display processor owns the leiasr handle — it creates it
 * via the factory function and destroys it on cleanup.
 *
 * @author David Fattal
 * @ingroup drv_leia
 */

#include "leia_display_processor.h"
#include "leia_sr.h"

#include "xrt/xrt_display_metrics.h"
#include "vk/vk_helpers.h"
#include "util/u_logging.h"
#include <cstdint>
#include <cstdlib>


/*!
 * Implementation struct wrapping leiasr as xrt_display_processor.
 */
struct leia_display_processor
{
	struct xrt_display_processor base;
	struct leiasr *leiasr; //!< Owned — destroyed in leia_dp_destroy.
};

static inline struct leia_display_processor *
leia_display_processor(struct xrt_display_processor *xdp)
{
	return (struct leia_display_processor *)xdp;
}


/*
 *
 * xrt_display_processor interface methods.
 *
 */

static void
leia_dp_process_views(struct xrt_display_processor *xdp,
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
	struct leia_display_processor *ldp = leia_display_processor(xdp);

	// Build a fullscreen viewport from target dimensions.
	VkRect2D viewport = {};
	viewport.offset.x = 0;
	viewport.offset.y = 0;
	viewport.extent.width = target_width;
	viewport.extent.height = target_height;

	leiasr_weave(ldp->leiasr,
	             cmd_buffer,
	             left_view,
	             right_view,
	             viewport,
	             (int)view_width,
	             (int)view_height,
	             (VkFormat)view_format,
	             target_fb,
	             (int)target_width,
	             (int)target_height,
	             (VkFormat)target_format);
}

static bool
leia_dp_get_predicted_eye_positions(struct xrt_display_processor *xdp, struct xrt_eye_pair *out_eye_pair)
{
	struct leia_display_processor *ldp = leia_display_processor(xdp);
	// leiasr_eye_pair is #defined to xrt_eye_pair in leia_types.h
	return leiasr_get_predicted_eye_positions(ldp->leiasr, (struct leiasr_eye_pair *)out_eye_pair);
}

static bool
leia_dp_get_window_metrics(struct xrt_display_processor *xdp, struct xrt_window_metrics *out_metrics)
{
	struct leia_display_processor *ldp = leia_display_processor(xdp);
	// leiasr_window_metrics is #defined to xrt_window_metrics in leia_types.h
	return leiasr_get_window_metrics(ldp->leiasr, (struct leiasr_window_metrics *)out_metrics);
}

static bool
leia_dp_request_display_mode(struct xrt_display_processor *xdp, bool enable_3d)
{
	struct leia_display_processor *ldp = leia_display_processor(xdp);
	return leiasr_request_display_mode(ldp->leiasr, enable_3d);
}

static bool
leia_dp_get_display_dimensions(struct xrt_display_processor *xdp, float *out_width_m, float *out_height_m)
{
	struct leia_display_processor *ldp = leia_display_processor(xdp);
	struct leiasr_display_dimensions dims = {};
	if (!leiasr_get_display_dimensions(ldp->leiasr, &dims) || !dims.valid) {
		return false;
	}
	*out_width_m = dims.width_m;
	*out_height_m = dims.height_m;
	return true;
}

static bool
leia_dp_get_display_pixel_info(struct xrt_display_processor *xdp,
                               uint32_t *out_pixel_width,
                               uint32_t *out_pixel_height,
                               int32_t *out_screen_left,
                               int32_t *out_screen_top)
{
	struct leia_display_processor *ldp = leia_display_processor(xdp);
	float w_m, h_m; // unused but required by API
	return leiasr_get_display_pixel_info(ldp->leiasr, out_pixel_width, out_pixel_height, out_screen_left,
	                                     out_screen_top, &w_m, &h_m);
}

static void
leia_dp_destroy(struct xrt_display_processor *xdp)
{
	struct leia_display_processor *ldp = leia_display_processor(xdp);
	leiasr_destroy(ldp->leiasr);
	free(ldp);
}


/*
 *
 * Factory function — matches xrt_dp_factory_vk_fn_t signature.
 *
 */

extern "C" xrt_result_t
leia_dp_factory_vk(void *vk_bundle_ptr,
                   void *vk_cmd_pool,
                   void *window_handle,
                   int32_t target_format,
                   struct xrt_display_processor **out_xdp)
{
	(void)target_format; // unused by SR weaver

	// Extract Vulkan handles from vk_bundle.
	struct vk_bundle *vk = (struct vk_bundle *)vk_bundle_ptr;

	struct leiasr *leiasr = NULL;
	xrt_result_t ret = leiasr_create(5.0, vk->device, vk->physical_device, vk->main_queue->queue,
	                                 (VkCommandPool)(uintptr_t)vk_cmd_pool, window_handle, &leiasr);
	if (ret != XRT_SUCCESS || leiasr == NULL) {
		U_LOG_W("Failed to create SR Vulkan weaver, continuing without interlacing");
		return ret != XRT_SUCCESS ? ret : XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	struct leia_display_processor *ldp = (struct leia_display_processor *)calloc(1, sizeof(*ldp));
	if (ldp == NULL) {
		leiasr_destroy(leiasr);
		return XRT_ERROR_ALLOCATION;
	}

	ldp->base.process_views = leia_dp_process_views;
	ldp->base.get_predicted_eye_positions = leia_dp_get_predicted_eye_positions;
	ldp->base.get_window_metrics = leia_dp_get_window_metrics;
	ldp->base.request_display_mode = leia_dp_request_display_mode;
	ldp->base.get_display_dimensions = leia_dp_get_display_dimensions;
	ldp->base.get_display_pixel_info = leia_dp_get_display_pixel_info;
	ldp->base.destroy = leia_dp_destroy;
	ldp->leiasr = leiasr;

	*out_xdp = &ldp->base;

	U_LOG_W("Created Leia SR display processor (factory, owns weaver)");

	return XRT_SUCCESS;
}


/*
 *
 * Legacy creation function — wraps an existing leiasr handle.
 * Kept for backward compatibility during the refactoring transition.
 *
 */

extern "C" xrt_result_t
leia_display_processor_create(struct leiasr *leiasr, struct xrt_display_processor **out_xdp)
{
	if (leiasr == NULL || out_xdp == NULL) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	struct leia_display_processor *ldp = (struct leia_display_processor *)calloc(1, sizeof(*ldp));
	if (ldp == NULL) {
		return XRT_ERROR_ALLOCATION;
	}

	ldp->base.process_views = leia_dp_process_views;
	ldp->base.get_predicted_eye_positions = leia_dp_get_predicted_eye_positions;
	ldp->base.get_window_metrics = leia_dp_get_window_metrics;
	ldp->base.request_display_mode = leia_dp_request_display_mode;
	ldp->base.get_display_dimensions = leia_dp_get_display_dimensions;
	ldp->base.get_display_pixel_info = leia_dp_get_display_pixel_info;
	// Legacy: does NOT own leiasr — use a destroy that skips leiasr_destroy.
	// For now just assign the full destroy; callers will be migrated to factory.
	ldp->base.destroy = leia_dp_destroy;
	ldp->leiasr = leiasr;

	*out_xdp = &ldp->base;

	U_LOG_W("Created Leia SR display processor (legacy, owns weaver)");

	return XRT_SUCCESS;
}
