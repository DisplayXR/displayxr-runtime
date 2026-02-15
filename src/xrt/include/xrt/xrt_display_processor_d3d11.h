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

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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
