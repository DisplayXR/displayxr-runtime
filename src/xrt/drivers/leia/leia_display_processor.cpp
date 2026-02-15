// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Leia display processor: wraps SR SDK Vulkan weaver
 *         as an @ref xrt_display_processor.
 * @author David Fattal
 * @ingroup drv_leia
 */

#include "leia_display_processor.h"
#include "leia_sr.h"

#include "util/u_logging.h"

#include <vulkan/vulkan.h>
#include <cstdlib>


/*!
 * Implementation struct wrapping leiasr as xrt_display_processor.
 */
struct leia_display_processor
{
	struct xrt_display_processor base;
	struct leiasr *leiasr;
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

static void
leia_dp_destroy(struct xrt_display_processor *xdp)
{
	struct leia_display_processor *ldp = leia_display_processor(xdp);
	// Does NOT destroy the leiasr instance — caller owns it.
	free(ldp);
}


/*
 *
 * Exported creation function.
 *
 */

extern "C" xrt_result_t
leia_display_processor_create(struct leiasr *leiasr,
                               struct xrt_display_processor **out_xdp)
{
	if (leiasr == NULL || out_xdp == NULL) {
		return XRT_ERROR_DEVICE_NOT_FOUND;
	}

	struct leia_display_processor *ldp =
	    (struct leia_display_processor *)calloc(1, sizeof(*ldp));
	if (ldp == NULL) {
		return XRT_ERROR_ALLOCATION;
	}

	ldp->base.process_views = leia_dp_process_views;
	ldp->base.destroy = leia_dp_destroy;
	ldp->leiasr = leiasr;

	*out_xdp = &ldp->base;

	U_LOG_W("Created Leia SR display processor");

	return XRT_SUCCESS;
}
