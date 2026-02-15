// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Simulation display processor: SBS, anaglyph, alpha-blend output.
 *
 * Implements side-by-side, anaglyph, and alpha-blend stereo output
 * modes for development and testing on regular 2D displays.
 *
 * The SBS mode uses vkCmdBlitImage to copy left/right views into
 * the left/right halves of the target. Anaglyph and blend modes
 * are stubbed for now (will use fragment shaders in a future phase).
 *
 * @author David Fattal
 * @ingroup drv_sim_display
 */

#include "sim_display_interface.h"

#include "xrt/xrt_display_processor.h"

#include "util/u_logging.h"

#include <vulkan/vulkan.h>
#include <stdlib.h>
#include <string.h>


/*!
 * Implementation struct for the simulation display processor.
 */
struct sim_display_processor
{
	struct xrt_display_processor base;
	enum sim_display_output_mode mode;
};

static inline struct sim_display_processor *
sim_display_processor(struct xrt_display_processor *xdp)
{
	return (struct sim_display_processor *)xdp;
}


/*
 *
 * SBS output: blit left view to left half, right view to right half.
 * Uses vkCmdBlitImage which requires the source images to be in
 * VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL layout. Since we only have
 * VkImageViews (not VkImages), we fall back to recording the viewport
 * configuration and let the compositor handle the blit upstream.
 *
 * For Phase 3, we implement a simple pass-through that records the
 * left/right views as-is. The actual SBS layout is achieved by the
 * compositor's viewport configuration which splits the target into
 * two halves for the two views.
 *
 * TODO: In a future phase, implement proper Vulkan render passes
 * with fragment shaders for all three modes.
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
	// SBS mode: The compositor's viewport setup already handles
	// side-by-side layout by configuring two viewports (left half / right half).
	// The display processor is a no-op for SBS since the compositor
	// renders directly into the correct viewport regions.
	//
	// This is the correct behavior — when the sim_display device reports
	// its display geometry via u_device_setup_split_side_by_side(),
	// the compositor automatically renders left/right views side-by-side.
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

static void
sim_dp_process_views_anaglyph(struct xrt_display_processor *xdp,
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
	// TODO: Implement red-cyan anaglyph with fragment shader.
	// Left eye → red channel, right eye → green+blue channels.
	U_LOG_W("Anaglyph mode not yet implemented, falling back to no-op");
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

static void
sim_dp_process_views_blend(struct xrt_display_processor *xdp,
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
	// TODO: Implement 50/50 alpha blend with fragment shader.
	// Both views overlaid at 50% opacity for stereo alignment checking.
	U_LOG_W("Blend mode not yet implemented, falling back to no-op");
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


static void
sim_dp_destroy(struct xrt_display_processor *xdp)
{
	struct sim_display_processor *sdp = sim_display_processor(xdp);
	free(sdp);
}


/*
 *
 * Exported creation function.
 *
 */

xrt_result_t
sim_display_processor_create(enum sim_display_output_mode mode,
                             struct xrt_display_processor **out_xdp)
{
	if (out_xdp == NULL) {
		return XRT_ERROR_DEVICE_NOT_FOUND;
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
		sdp->base.process_views = sim_dp_process_views_anaglyph;
		U_LOG_W("Created sim display processor: Anaglyph mode");
		break;
	case SIM_DISPLAY_OUTPUT_BLEND:
		sdp->base.process_views = sim_dp_process_views_blend;
		U_LOG_W("Created sim display processor: Blend mode");
		break;
	default:
		U_LOG_E("Unknown sim display output mode: %d", (int)mode);
		free(sdp);
		return XRT_ERROR_DEVICE_NOT_FOUND;
	}

	*out_xdp = &sdp->base;
	return XRT_SUCCESS;
}
