// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Header for @ref xrt_display_processor_d3d11 interface.
 *
 * D3D11 variant of the display processor abstraction for vendor-specific
 * stereo-to-display output processing (interlacing, SBS, anaglyph, etc.).
 *
 * Unlike the Vulkan variant, this interface operates on D3D11 resources:
 * - Input is a side-by-side stereo SRV (not separate left/right views)
 * - Output goes to the currently bound render target (no framebuffer param)
 * - No command buffer — D3D11 uses immediate-mode device context
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
struct xrt_eye_pair;
struct xrt_window_metrics;

/*!
 * @interface xrt_display_processor_d3d11
 *
 * D3D11 display output processor that converts a side-by-side stereo
 * texture into the final display output format.
 *
 * The compositor calls process_stereo() after rendering the stereo
 * pair into an SBS texture. The display processor writes the final
 * output (interlaced pattern, etc.) to the currently bound render target.
 *
 * @ingroup xrt_iface
 */
struct xrt_display_processor_d3d11
{
	/*!
	 * Process a side-by-side stereo texture into the final display output.
	 *
	 * The output render target must already be bound via OMSetRenderTargets.
	 * The implementation will set the viewport and perform the display-
	 * specific processing (interlacing, etc.).
	 *
	 * @param      xdp              Pointer to self.
	 * @param      d3d11_context    D3D11 device context (ID3D11DeviceContext*).
	 * @param      stereo_srv       SBS stereo texture SRV (ID3D11ShaderResourceView*).
	 * @param      view_width       Width of one eye view (half of SBS texture width).
	 * @param      view_height      Height of the views.
	 * @param      format           DXGI format of the stereo texture (DXGI_FORMAT as uint32_t).
	 * @param      target_width     Width of the output render target in pixels.
	 * @param      target_height    Height of the output render target in pixels.
	 */
	void (*process_stereo)(struct xrt_display_processor_d3d11 *xdp,
	                       void *d3d11_context,
	                       void *stereo_srv,
	                       uint32_t view_width,
	                       uint32_t view_height,
	                       uint32_t format,
	                       uint32_t target_width,
	                       uint32_t target_height);

	/*!
	 * Get predicted eye positions from vendor eye tracking SDK.
	 * Optional — NULL means not supported.
	 */
	bool (*get_predicted_eye_positions)(struct xrt_display_processor_d3d11 *xdp,
	                                    struct xrt_eye_pair *out_eye_pair);

	/*!
	 * Get window metrics for adaptive FOV calculation.
	 * Optional — NULL means not supported.
	 */
	bool (*get_window_metrics)(struct xrt_display_processor_d3d11 *xdp,
	                           struct xrt_window_metrics *out_metrics);

	/*!
	 * Request a display mode switch (2D/3D).
	 * Optional — NULL means not supported.
	 */
	bool (*request_display_mode)(struct xrt_display_processor_d3d11 *xdp,
	                             bool enable_3d);

	/*!
	 * Get physical display dimensions in meters.
	 * Optional — NULL means not supported.
	 */
	bool (*get_display_dimensions)(struct xrt_display_processor_d3d11 *xdp,
	                               float *out_width_m,
	                               float *out_height_m);

	/*!
	 * Get native display pixel info (resolution and screen position).
	 * Optional — NULL means not supported.
	 */
	bool (*get_display_pixel_info)(struct xrt_display_processor_d3d11 *xdp,
	                               uint32_t *out_pixel_width,
	                               uint32_t *out_pixel_height,
	                               int32_t *out_screen_left,
	                               int32_t *out_screen_top);

	/*!
	 * Destroy this display processor and free all resources.
	 *
	 * @param xdp Pointer to self.
	 */
	void (*destroy)(struct xrt_display_processor_d3d11 *xdp);
};

/*!
 * @copydoc xrt_display_processor_d3d11::process_stereo
 *
 * Helper for calling through the function pointer.
 *
 * @public @memberof xrt_display_processor_d3d11
 */
static inline void
xrt_display_processor_d3d11_process_stereo(struct xrt_display_processor_d3d11 *xdp,
                                           void *d3d11_context,
                                           void *stereo_srv,
                                           uint32_t view_width,
                                           uint32_t view_height,
                                           uint32_t format,
                                           uint32_t target_width,
                                           uint32_t target_height)
{
	xdp->process_stereo(xdp, d3d11_context, stereo_srv, view_width, view_height, format, target_width,
	                    target_height);
}

/*!
 * @copydoc xrt_display_processor_d3d11::get_predicted_eye_positions
 * Returns false if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor_d3d11
 */
static inline bool
xrt_display_processor_d3d11_get_predicted_eye_positions(struct xrt_display_processor_d3d11 *xdp,
                                                        struct xrt_eye_pair *out_eye_pair)
{
	if (xdp == NULL || xdp->get_predicted_eye_positions == NULL) {
		return false;
	}
	return xdp->get_predicted_eye_positions(xdp, out_eye_pair);
}

/*!
 * @copydoc xrt_display_processor_d3d11::get_window_metrics
 * Returns false if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor_d3d11
 */
static inline bool
xrt_display_processor_d3d11_get_window_metrics(struct xrt_display_processor_d3d11 *xdp,
                                               struct xrt_window_metrics *out_metrics)
{
	if (xdp == NULL || xdp->get_window_metrics == NULL) {
		return false;
	}
	return xdp->get_window_metrics(xdp, out_metrics);
}

/*!
 * @copydoc xrt_display_processor_d3d11::request_display_mode
 * Returns false if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor_d3d11
 */
static inline bool
xrt_display_processor_d3d11_request_display_mode(struct xrt_display_processor_d3d11 *xdp, bool enable_3d)
{
	if (xdp == NULL || xdp->request_display_mode == NULL) {
		return false;
	}
	return xdp->request_display_mode(xdp, enable_3d);
}

/*!
 * @copydoc xrt_display_processor_d3d11::get_display_dimensions
 * Returns false if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor_d3d11
 */
static inline bool
xrt_display_processor_d3d11_get_display_dimensions(struct xrt_display_processor_d3d11 *xdp,
                                                   float *out_width_m,
                                                   float *out_height_m)
{
	if (xdp == NULL || xdp->get_display_dimensions == NULL) {
		return false;
	}
	return xdp->get_display_dimensions(xdp, out_width_m, out_height_m);
}

/*!
 * @copydoc xrt_display_processor_d3d11::get_display_pixel_info
 * Returns false if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor_d3d11
 */
static inline bool
xrt_display_processor_d3d11_get_display_pixel_info(struct xrt_display_processor_d3d11 *xdp,
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
 * Factory function type for creating a D3D11 display processor.
 *
 * Called by the compositor to create a display processor for a session.
 * The factory is set by the target builder at init time and stored in
 * xrt_system_compositor_info.
 *
 * @param d3d11_device   D3D11 device (ID3D11Device*).
 * @param d3d11_context  D3D11 immediate context (ID3D11DeviceContext*).
 * @param window_handle  Native window handle (HWND), may be NULL.
 * @param[out] out_xdp   Created display processor on success.
 * @return XRT_SUCCESS on success.
 */
typedef xrt_result_t (*xrt_dp_factory_d3d11_fn_t)(void *d3d11_device,
                                                   void *d3d11_context,
                                                   void *window_handle,
                                                   struct xrt_display_processor_d3d11 **out_xdp);

/*!
 * Destroy an xrt_display_processor_d3d11 — helper function.
 *
 * @param[in,out] xdp_ptr  A pointer to your display processor pointer.
 *
 * Will destroy the processor if *xdp_ptr is not NULL.
 * Will then set *xdp_ptr to NULL.
 *
 * @public @memberof xrt_display_processor_d3d11
 */
static inline void
xrt_display_processor_d3d11_destroy(struct xrt_display_processor_d3d11 **xdp_ptr)
{
	struct xrt_display_processor_d3d11 *xdp = *xdp_ptr;
	if (xdp == NULL) {
		return;
	}

	xdp->destroy(xdp);
	*xdp_ptr = NULL;
}


#ifdef __cplusplus
}
#endif
