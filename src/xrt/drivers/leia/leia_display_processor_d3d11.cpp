// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Leia D3D11 display processor: wraps SR SDK D3D11 weaver
 *         as an @ref xrt_display_processor_d3d11.
 * @author David Fattal
 * @ingroup drv_leia
 */

#include "leia_display_processor_d3d11.h"
#include "leia_sr_d3d11.h"

#include "util/u_logging.h"

#include <d3d11.h>
#include <cstdlib>


/*!
 * Implementation struct wrapping leiasr_d3d11 as xrt_display_processor_d3d11.
 */
struct leia_display_processor_d3d11_impl
{
	struct xrt_display_processor_d3d11 base;
	struct leiasr_d3d11 *leiasr;
};

static inline struct leia_display_processor_d3d11_impl *
leia_dp_d3d11(struct xrt_display_processor_d3d11 *xdp)
{
	return (struct leia_display_processor_d3d11_impl *)xdp;
}


/*
 *
 * xrt_display_processor_d3d11 interface methods.
 *
 */

static void
leia_dp_d3d11_process_stereo(struct xrt_display_processor_d3d11 *xdp,
                             void *d3d11_context,
                             void *stereo_srv,
                             uint32_t view_width,
                             uint32_t view_height,
                             uint32_t format,
                             uint32_t target_width,
                             uint32_t target_height)
{
	struct leia_display_processor_d3d11_impl *ldp = leia_dp_d3d11(xdp);
	ID3D11DeviceContext *ctx = static_cast<ID3D11DeviceContext *>(d3d11_context);

	// Set input texture for weaving (view_width is single eye, weaver handles SBS)
	leiasr_d3d11_set_input_texture(ldp->leiasr, stereo_srv, view_width, view_height, format);

	// Set viewport for target dimensions
	D3D11_VIEWPORT viewport = {};
	viewport.TopLeftX = 0.0f;
	viewport.TopLeftY = 0.0f;
	viewport.Width = static_cast<float>(target_width);
	viewport.Height = static_cast<float>(target_height);
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	ctx->RSSetViewports(1, &viewport);

	// Perform weaving to the currently bound render target
	leiasr_d3d11_weave(ldp->leiasr);
}

static void
leia_dp_d3d11_destroy(struct xrt_display_processor_d3d11 *xdp)
{
	struct leia_display_processor_d3d11_impl *ldp = leia_dp_d3d11(xdp);
	// Does NOT destroy the leiasr_d3d11 instance — caller owns it.
	free(ldp);
}


/*
 *
 * Exported creation function.
 *
 */

extern "C" xrt_result_t
leia_display_processor_d3d11_create(struct leiasr_d3d11 *leiasr,
                                    struct xrt_display_processor_d3d11 **out_xdp)
{
	if (leiasr == NULL || out_xdp == NULL) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	struct leia_display_processor_d3d11_impl *ldp =
	    (struct leia_display_processor_d3d11_impl *)calloc(1, sizeof(*ldp));
	if (ldp == NULL) {
		return XRT_ERROR_ALLOCATION;
	}

	ldp->base.process_stereo = leia_dp_d3d11_process_stereo;
	ldp->base.destroy = leia_dp_d3d11_destroy;
	ldp->leiasr = leiasr;

	*out_xdp = &ldp->base;

	U_LOG_W("Created Leia SR D3D11 display processor");

	return XRT_SUCCESS;
}
