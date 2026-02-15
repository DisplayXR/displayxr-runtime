// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Header for @ref xrt_display_processor interface.
 *
 * Abstracts vendor-specific stereo-to-display output processing
 * (interlacing for light field displays, SBS layout, anaglyph, etc.)
 * so the compositor remains vendor-agnostic.
 *
 * @author David Fattal
 * @ingroup xrt_iface
 */

#pragma once

#include "xrt/xrt_compiler.h"

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations — avoid pulling in full Vulkan headers.
typedef struct VkCommandBuffer_T *VkCommandBuffer;

#ifdef XRT_64_BIT
typedef struct VkImageView_T *VkImageView;
typedef struct VkFramebuffer_T *VkFramebuffer;
#else
typedef uint64_t VkImageView;
typedef uint64_t VkFramebuffer;
#endif

// Re-use Vulkan enum values without including vulkan.h.
typedef int32_t VkFormat_XDP;


/*!
 * @interface xrt_display_processor
 *
 * Generic display output processor that converts rendered stereo views
 * into the final display output format. Each vendor (Leia SR SDK, CNSDK,
 * simulation, etc.) provides its own implementation.
 *
 * The compositor calls process_views() after compositing the left/right
 * eye layers, and the display processor produces the final output
 * (interlaced light field pattern, side-by-side, anaglyph, etc.).
 *
 * Lifecycle:
 * - Created by the vendor driver or builder
 * - Passed to the compositor at init time
 * - Compositor calls process_views() each frame
 * - Compositor calls xrt_display_processor_destroy() at shutdown
 *
 * @ingroup xrt_iface
 */
struct xrt_display_processor
{
	/*!
	 * @name Interface Methods
	 * @{
	 */

	/*!
	 * Process left and right eye views into the final display output.
	 *
	 * Called by the compositor after layer compositing is complete.
	 * The implementation records Vulkan commands into @p cmd_buffer
	 * that transform the stereo views into the target framebuffer
	 * in the display's native format.
	 *
	 * @param      xdp              Pointer to self.
	 * @param      cmd_buffer       Vulkan command buffer to record into.
	 * @param      left_view        Left eye image view.
	 * @param      right_view       Right eye image view.
	 * @param      view_width       Width of each eye view in pixels.
	 * @param      view_height      Height of each eye view in pixels.
	 * @param      view_format      Vulkan format of the eye views.
	 * @param      target_fb        Target framebuffer to render into.
	 * @param      target_width     Width of the target framebuffer in pixels.
	 * @param      target_height    Height of the target framebuffer in pixels.
	 * @param      target_format    Vulkan format of the target framebuffer.
	 */
	void (*process_views)(struct xrt_display_processor *xdp,
	                      VkCommandBuffer cmd_buffer,
	                      VkImageView left_view,
	                      VkImageView right_view,
	                      uint32_t view_width,
	                      uint32_t view_height,
	                      VkFormat_XDP view_format,
	                      VkFramebuffer target_fb,
	                      uint32_t target_width,
	                      uint32_t target_height,
	                      VkFormat_XDP target_format);

	/*!
	 * Destroy this display processor and free all resources.
	 *
	 * @param xdp Pointer to self.
	 */
	void (*destroy)(struct xrt_display_processor *xdp);

	/*! @} */
};

/*!
 * @copydoc xrt_display_processor::process_views
 *
 * Helper for calling through the function pointer.
 *
 * @public @memberof xrt_display_processor
 */
static inline void
xrt_display_processor_process_views(struct xrt_display_processor *xdp,
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
	xdp->process_views(xdp, cmd_buffer, left_view, right_view,
	                   view_width, view_height, view_format,
	                   target_fb, target_width, target_height,
	                   target_format);
}

/*!
 * Destroy an xrt_display_processor — helper function.
 *
 * @param[in,out] xdp_ptr  A pointer to your display processor pointer.
 *
 * Will destroy the processor if *xdp_ptr is not NULL.
 * Will then set *xdp_ptr to NULL.
 *
 * @public @memberof xrt_display_processor
 */
static inline void
xrt_display_processor_destroy(struct xrt_display_processor **xdp_ptr)
{
	struct xrt_display_processor *xdp = *xdp_ptr;
	if (xdp == NULL) {
		return;
	}

	xdp->destroy(xdp);
	*xdp_ptr = NULL;
}


#ifdef __cplusplus
}
#endif
