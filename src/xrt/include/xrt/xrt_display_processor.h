// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Header for @ref xrt_display_processor interface.
 *
 * Abstracts vendor-specific atlas-to-display output processing
 * (interlacing for light field displays, SBS layout, anaglyph, etc.)
 * so the compositor remains vendor-agnostic.
 *
 * @author David Fattal
 * @ingroup xrt_iface
 */

#pragma once

#include "xrt/xrt_compiler.h"
#include "xrt/xrt_results.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations — avoid pulling in full Vulkan headers.
typedef struct VkCommandBuffer_T *VkCommandBuffer;

#ifdef XRT_64_BIT
typedef struct VkImageView_T *VkImageView;
typedef struct VkFramebuffer_T *VkFramebuffer;
typedef struct VkRenderPass_T *VkRenderPass;
#else
typedef uint64_t VkImageView;
typedef uint64_t VkFramebuffer;
typedef uint64_t VkRenderPass;
#endif

// Re-use Vulkan enum values without including vulkan.h.
typedef int32_t VkFormat_XDP;

// Forward declarations for types used by optional vtable methods.
struct xrt_eye_pair;
struct xrt_window_metrics;


/*!
 * @interface xrt_display_processor
 *
 * Generic display output processor that converts a tiled atlas texture
 * into the final display output format. Each vendor (Leia SR SDK, CNSDK,
 * simulation, etc.) provides its own implementation.
 *
 * The compositor calls process_atlas() after compositing all views
 * into an atlas texture, and the display processor produces the final output
 * (interlaced light field pattern, side-by-side, anaglyph, etc.).
 *
 * Lifecycle:
 * - Created by the vendor driver or builder
 * - Passed to the compositor at init time
 * - Compositor calls process_atlas() each frame
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
	 * Process a tiled atlas texture into the final display output.
	 *
	 * Called by the compositor after layer compositing is complete.
	 * The implementation records Vulkan commands into @p cmd_buffer
	 * that transform the atlas texture into the target framebuffer
	 * in the display's native format.
	 *
	 * @param      xdp              Pointer to self.
	 * @param      cmd_buffer       Vulkan command buffer to record into.
	 * @param      atlas_view       Atlas image view (tiled views).
	 * @param      view_width       Width of one view in the atlas in pixels.
	 * @param      view_height      Height of one view in the atlas in pixels.
	 * @param      tile_columns     Number of tile columns in the atlas layout.
	 * @param      tile_rows        Number of tile rows in the atlas layout.
	 * @param      view_format      Vulkan format of the atlas texture.
	 * @param      target_fb        Target framebuffer to render into.
	 * @param      target_width     Width of the target framebuffer in pixels.
	 * @param      target_height    Height of the target framebuffer in pixels.
	 * @param      target_format    Vulkan format of the target framebuffer.
	 */
	void (*process_atlas)(struct xrt_display_processor *xdp,
	                      VkCommandBuffer cmd_buffer,
	                      VkImageView atlas_view,
	                      uint32_t view_width,
	                      uint32_t view_height,
	                      uint32_t tile_columns,
	                      uint32_t tile_rows,
	                      VkFormat_XDP view_format,
	                      VkFramebuffer target_fb,
	                      uint32_t target_width,
	                      uint32_t target_height,
	                      VkFormat_XDP target_format);

	/*!
	 * Get predicted eye positions from vendor eye tracking SDK.
	 * Optional — NULL means not supported.
	 *
	 * @param      xdp           Pointer to self.
	 * @param[out] out_eye_pair  Predicted left/right eye positions.
	 * @return true if eye positions are valid.
	 */
	bool (*get_predicted_eye_positions)(struct xrt_display_processor *xdp,
	                                    struct xrt_eye_pair *out_eye_pair);

	/*!
	 * Get window metrics for adaptive FOV calculation.
	 * Optional — NULL means not supported.
	 *
	 * @param      xdp          Pointer to self.
	 * @param[out] out_metrics  Window and display geometry.
	 * @return true if metrics are valid.
	 */
	bool (*get_window_metrics)(struct xrt_display_processor *xdp,
	                           struct xrt_window_metrics *out_metrics);

	/*!
	 * Request a display mode switch (2D/3D).
	 * Optional — NULL means not supported.
	 *
	 * @param xdp        Pointer to self.
	 * @param enable_3d  true for 3D mode, false for 2D mode.
	 * @return true if the request was accepted.
	 */
	bool (*request_display_mode)(struct xrt_display_processor *xdp,
	                             bool enable_3d);

	/*!
	 * Get the Vulkan render pass used by this display processor.
	 * Required by compositors that need to create VkFramebuffers
	 * for the display processor's render pass.
	 * Optional — NULL means not supported.
	 *
	 * @param xdp  Pointer to self.
	 * @return VkRenderPass handle, or VK_NULL_HANDLE if not available.
	 */
	VkRenderPass (*get_render_pass)(struct xrt_display_processor *xdp);

	/*!
	 * Get physical display dimensions in meters.
	 * Optional — NULL means not supported.
	 *
	 * @param      xdp           Pointer to self.
	 * @param[out] out_width_m   Physical width in meters.
	 * @param[out] out_height_m  Physical height in meters.
	 * @return true if dimensions are valid.
	 */
	bool (*get_display_dimensions)(struct xrt_display_processor *xdp,
	                               float *out_width_m,
	                               float *out_height_m);

	/*!
	 * Get native display pixel info (resolution and screen position).
	 * Optional — NULL means not supported.
	 *
	 * @param      xdp               Pointer to self.
	 * @param[out] out_pixel_width   Native panel width in pixels.
	 * @param[out] out_pixel_height  Native panel height in pixels.
	 * @param[out] out_screen_left   Display left edge in screen coordinates.
	 * @param[out] out_screen_top    Display top edge in screen coordinates.
	 * @return true if info is valid.
	 */
	bool (*get_display_pixel_info)(struct xrt_display_processor *xdp,
	                               uint32_t *out_pixel_width,
	                               uint32_t *out_pixel_height,
	                               int32_t *out_screen_left,
	                               int32_t *out_screen_top);

	/*!
	 * Destroy this display processor and free all resources.
	 *
	 * @param xdp Pointer to self.
	 */
	void (*destroy)(struct xrt_display_processor *xdp);

	/*! @} */
};

/*!
 * @copydoc xrt_display_processor::process_atlas
 *
 * Helper for calling through the function pointer.
 *
 * @public @memberof xrt_display_processor
 */
static inline void
xrt_display_processor_process_atlas(struct xrt_display_processor *xdp,
                                    VkCommandBuffer cmd_buffer,
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
	xdp->process_atlas(xdp, cmd_buffer, atlas_view,
	                   view_width, view_height,
	                   tile_columns, tile_rows,
	                   view_format, target_fb,
	                   target_width, target_height,
	                   target_format);
}

/*!
 * @copydoc xrt_display_processor::get_predicted_eye_positions
 * Returns false if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor
 */
static inline bool
xrt_display_processor_get_predicted_eye_positions(struct xrt_display_processor *xdp,
                                                  struct xrt_eye_pair *out_eye_pair)
{
	if (xdp == NULL || xdp->get_predicted_eye_positions == NULL) {
		return false;
	}
	return xdp->get_predicted_eye_positions(xdp, out_eye_pair);
}

/*!
 * @copydoc xrt_display_processor::get_window_metrics
 * Returns false if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor
 */
static inline bool
xrt_display_processor_get_window_metrics(struct xrt_display_processor *xdp,
                                         struct xrt_window_metrics *out_metrics)
{
	if (xdp == NULL || xdp->get_window_metrics == NULL) {
		return false;
	}
	return xdp->get_window_metrics(xdp, out_metrics);
}

/*!
 * @copydoc xrt_display_processor::request_display_mode
 * Returns false if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor
 */
static inline bool
xrt_display_processor_request_display_mode(struct xrt_display_processor *xdp, bool enable_3d)
{
	if (xdp == NULL || xdp->request_display_mode == NULL) {
		return false;
	}
	return xdp->request_display_mode(xdp, enable_3d);
}

/*!
 * @copydoc xrt_display_processor::get_render_pass
 * Returns VK_NULL_HANDLE if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor
 */
static inline VkRenderPass
xrt_display_processor_get_render_pass(struct xrt_display_processor *xdp)
{
	if (xdp == NULL || xdp->get_render_pass == NULL) {
		return (VkRenderPass)0;
	}
	return xdp->get_render_pass(xdp);
}

/*!
 * @copydoc xrt_display_processor::get_display_dimensions
 * Returns false if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor
 */
static inline bool
xrt_display_processor_get_display_dimensions(struct xrt_display_processor *xdp,
                                             float *out_width_m,
                                             float *out_height_m)
{
	if (xdp == NULL || xdp->get_display_dimensions == NULL) {
		return false;
	}
	return xdp->get_display_dimensions(xdp, out_width_m, out_height_m);
}

/*!
 * @copydoc xrt_display_processor::get_display_pixel_info
 * Returns false if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor
 */
static inline bool
xrt_display_processor_get_display_pixel_info(struct xrt_display_processor *xdp,
                                             uint32_t *out_pixel_width,
                                             uint32_t *out_pixel_height,
                                             int32_t *out_screen_left,
                                             int32_t *out_screen_top)
{
	if (xdp == NULL || xdp->get_display_pixel_info == NULL) {
		return false;
	}
	return xdp->get_display_pixel_info(xdp, out_pixel_width, out_pixel_height, out_screen_left, out_screen_top);
}

/*!
 * Factory function type for creating a Vulkan display processor.
 *
 * Called by the compositor to create a display processor for a session.
 * The factory is set by the target builder at init time and stored in
 * xrt_system_compositor_info. The implementation creates and owns all
 * vendor-specific resources internally.
 *
 * @param vk_bundle      Opaque pointer to the compositor's struct vk_bundle.
 * @param vk_cmd_pool    Vulkan command pool (VkCommandPool as void*) for recording.
 * @param window_handle  Native window handle (HWND on Windows, etc.), may be NULL.
 * @param target_format  Target framebuffer format (VkFormat as int32_t).
 * @param[out] out_xdp   Created display processor on success.
 * @return XRT_SUCCESS on success.
 */
typedef xrt_result_t (*xrt_dp_factory_vk_fn_t)(void *vk_bundle,
                                               void *vk_cmd_pool,
                                               void *window_handle,
                                               int32_t target_format,
                                               struct xrt_display_processor **out_xdp);

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
