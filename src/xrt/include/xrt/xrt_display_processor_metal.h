// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Header for @ref xrt_display_processor_metal interface.
 *
 * Metal variant of the display processor abstraction for vendor-specific
 * atlas-to-display output processing (interlacing, SBS, anaglyph, etc.).
 *
 * Unlike the D3D11 variant, this interface operates on Metal resources:
 * - Input is an atlas texture (id<MTLTexture>)
 * - Output goes to a provided render command encoder or texture
 * - Uses Metal command buffers rather than immediate-mode context
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

// Forward declarations for types used by optional vtable methods.
struct xrt_eye_positions;
struct xrt_window_metrics;

/*!
 * @interface xrt_display_processor_metal
 *
 * Metal display output processor that converts an atlas
 * texture into the final display output format.
 *
 * The compositor calls process_atlas() after rendering the view
 * pair into an atlas texture. The display processor writes the final
 * output to the provided render target texture.
 *
 * @ingroup xrt_iface
 */
struct xrt_display_processor_metal
{
	/*!
	 * Process an atlas texture into the final display output.
	 *
	 * @param      xdp              Pointer to self.
	 * @param      command_buffer   Metal command buffer (id<MTLCommandBuffer>).
	 * @param      atlas_texture   Atlas texture (id<MTLTexture>).
	 * @param      view_width       Width of one eye view in pixels.
	 * @param      view_height      Height of one eye view in pixels.
	 * @param      tile_columns     Number of tile columns in the atlas layout.
	 * @param      tile_rows        Number of tile rows in the atlas layout.
	 * @param      format           MTLPixelFormat of the atlas texture (as uint32_t).
	 * @param      target_texture   Output render target (id<MTLTexture>).
	 * @param      target_width     Width of the output render target in pixels.
	 * @param      target_height    Height of the output render target in pixels.
	 * @param      canvas_offset_x  Canvas left edge in window client-area pixels (0 = no offset).
	 * @param      canvas_offset_y  Canvas top edge in window client-area pixels (0 = no offset).
	 * @param      canvas_width     Canvas width in pixels (0 = fills full window/target).
	 * @param      canvas_height    Canvas height in pixels (0 = fills full window/target).
	 */
	void (*process_atlas)(struct xrt_display_processor_metal *xdp,
	                       void *command_buffer,
	                       void *atlas_texture,
	                       uint32_t view_width,
	                       uint32_t view_height,
	                       uint32_t tile_columns,
	                       uint32_t tile_rows,
	                       uint32_t format,
	                       void *target_texture,
	                       uint32_t target_width,
	                       uint32_t target_height,
	                       int32_t canvas_offset_x,
	                       int32_t canvas_offset_y,
	                       uint32_t canvas_width,
	                       uint32_t canvas_height);

	/*!
	 * Get predicted eye positions from vendor eye tracking SDK.
	 * Optional — NULL means not supported.
	 */
	bool (*get_predicted_eye_positions)(struct xrt_display_processor_metal *xdp,
	                                    struct xrt_eye_positions *out_eye_pos);

	/*!
	 * Get window metrics for adaptive FOV calculation.
	 * Optional — NULL means not supported.
	 */
	bool (*get_window_metrics)(struct xrt_display_processor_metal *xdp,
	                           struct xrt_window_metrics *out_metrics);

	/*!
	 * Request a display mode switch (2D/3D).
	 * Optional — NULL means not supported.
	 */
	bool (*request_display_mode)(struct xrt_display_processor_metal *xdp,
	                             bool enable_3d);

	/*!
	 * Get physical display dimensions in meters.
	 * Optional — NULL means not supported.
	 */
	bool (*get_display_dimensions)(struct xrt_display_processor_metal *xdp,
	                               float *out_width_m,
	                               float *out_height_m);

	/*!
	 * Get native display pixel info (resolution and screen position).
	 * Optional — NULL means not supported.
	 */
	bool (*get_display_pixel_info)(struct xrt_display_processor_metal *xdp,
	                               uint32_t *out_pixel_width,
	                               uint32_t *out_pixel_height,
	                               int32_t *out_screen_left,
	                               int32_t *out_screen_top);

	/*!
	 * Destroy this display processor and free all resources.
	 *
	 * @param xdp Pointer to self.
	 */
	void (*destroy)(struct xrt_display_processor_metal *xdp);
};

/*!
 * @copydoc xrt_display_processor_metal::process_atlas
 *
 * Helper for calling through the function pointer.
 *
 * @public @memberof xrt_display_processor_metal
 */
static inline void
xrt_display_processor_metal_process_atlas(struct xrt_display_processor_metal *xdp,
                                           void *command_buffer,
                                           void *atlas_texture,
                                           uint32_t view_width,
                                           uint32_t view_height,
                                           uint32_t tile_columns,
                                           uint32_t tile_rows,
                                           uint32_t format,
                                           void *target_texture,
                                           uint32_t target_width,
                                           uint32_t target_height,
                                           int32_t canvas_offset_x,
                                           int32_t canvas_offset_y,
                                           uint32_t canvas_width,
                                           uint32_t canvas_height)
{
	xdp->process_atlas(xdp, command_buffer, atlas_texture, view_width, view_height, tile_columns, tile_rows,
	                    format, target_texture, target_width, target_height, canvas_offset_x, canvas_offset_y,
	                    canvas_width, canvas_height);
}

/*!
 * @copydoc xrt_display_processor_metal::get_predicted_eye_positions
 * Returns false if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor_metal
 */
static inline bool
xrt_display_processor_metal_get_predicted_eye_positions(struct xrt_display_processor_metal *xdp,
                                                        struct xrt_eye_positions *out_eye_pos)
{
	if (xdp == NULL || xdp->get_predicted_eye_positions == NULL) {
		return false;
	}
	return xdp->get_predicted_eye_positions(xdp, out_eye_pos);
}

/*!
 * @copydoc xrt_display_processor_metal::get_window_metrics
 * Returns false if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor_metal
 */
static inline bool
xrt_display_processor_metal_get_window_metrics(struct xrt_display_processor_metal *xdp,
                                               struct xrt_window_metrics *out_metrics)
{
	if (xdp == NULL || xdp->get_window_metrics == NULL) {
		return false;
	}
	return xdp->get_window_metrics(xdp, out_metrics);
}

/*!
 * @copydoc xrt_display_processor_metal::request_display_mode
 * Returns false if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor_metal
 */
static inline bool
xrt_display_processor_metal_request_display_mode(struct xrt_display_processor_metal *xdp, bool enable_3d)
{
	if (xdp == NULL || xdp->request_display_mode == NULL) {
		return false;
	}
	return xdp->request_display_mode(xdp, enable_3d);
}

/*!
 * @copydoc xrt_display_processor_metal::get_display_dimensions
 * Returns false if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor_metal
 */
static inline bool
xrt_display_processor_metal_get_display_dimensions(struct xrt_display_processor_metal *xdp,
                                                   float *out_width_m,
                                                   float *out_height_m)
{
	if (xdp == NULL || xdp->get_display_dimensions == NULL) {
		return false;
	}
	return xdp->get_display_dimensions(xdp, out_width_m, out_height_m);
}

/*!
 * @copydoc xrt_display_processor_metal::get_display_pixel_info
 * Returns false if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor_metal
 */
static inline bool
xrt_display_processor_metal_get_display_pixel_info(struct xrt_display_processor_metal *xdp,
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
 * Factory function type for creating a Metal display processor.
 *
 * Called by the compositor to create a display processor for a session.
 * The factory is set by the target builder at init time and stored in
 * xrt_system_compositor_info.
 *
 * @param metal_device    Metal device (id<MTLDevice>).
 * @param command_queue   Metal command queue (id<MTLCommandQueue>).
 * @param window_handle   Native window handle (NSView*), may be NULL.
 * @param[out] out_xdp    Created display processor on success.
 * @return XRT_SUCCESS on success.
 */
typedef xrt_result_t (*xrt_dp_factory_metal_fn_t)(void *metal_device,
                                                   void *command_queue,
                                                   void *window_handle,
                                                   struct xrt_display_processor_metal **out_xdp);

/*!
 * Destroy an xrt_display_processor_metal — helper function.
 *
 * @param[in,out] xdp_ptr  A pointer to your display processor pointer.
 *
 * Will destroy the processor if *xdp_ptr is not NULL.
 * Will then set *xdp_ptr to NULL.
 *
 * @public @memberof xrt_display_processor_metal
 */
static inline void
xrt_display_processor_metal_destroy(struct xrt_display_processor_metal **xdp_ptr)
{
	struct xrt_display_processor_metal *xdp = *xdp_ptr;
	if (xdp == NULL) {
		return;
	}

	xdp->destroy(xdp);
	*xdp_ptr = NULL;
}


#ifdef __cplusplus
}
#endif
