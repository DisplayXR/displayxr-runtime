// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Leia D3D11 display processor: wraps SR SDK D3D11 weaver
 *         as an @ref xrt_display_processor_d3d11.
 *
 * The display processor owns the leiasr_d3d11 handle — it creates it
 * via the factory function and destroys it on cleanup.
 *
 * @author David Fattal
 * @ingroup drv_leia
 */

#include "leia_display_processor_d3d11.h"
#include "leia_sr_d3d11.h"

#include "xrt/xrt_display_metrics.h"
#include "util/u_logging.h"

#include <d3d11.h>
#include <cstdlib>


/*!
 * Implementation struct wrapping leiasr_d3d11 as xrt_display_processor_d3d11.
 */
struct leia_display_processor_d3d11_impl
{
	struct xrt_display_processor_d3d11 base;
	struct leiasr_d3d11 *leiasr; //!< Owned — destroyed in leia_dp_d3d11_destroy.
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

static bool
leia_dp_d3d11_get_predicted_eye_positions(struct xrt_display_processor_d3d11 *xdp,
                                          struct xrt_eye_pair *out_eye_pair)
{
	struct leia_display_processor_d3d11_impl *ldp = leia_dp_d3d11(xdp);
	float left[3], right[3];
	if (!leiasr_d3d11_get_predicted_eye_positions(ldp->leiasr, left, right)) {
		return false;
	}
	out_eye_pair->left.x = left[0];
	out_eye_pair->left.y = left[1];
	out_eye_pair->left.z = left[2];
	out_eye_pair->right.x = right[0];
	out_eye_pair->right.y = right[1];
	out_eye_pair->right.z = right[2];
	out_eye_pair->valid = true;
	out_eye_pair->is_tracking = true;
	return true;
}

static bool
leia_dp_d3d11_get_window_metrics(struct xrt_display_processor_d3d11 *xdp,
                                 struct xrt_window_metrics *out_metrics)
{
	// D3D11 path: compute window metrics from display pixel info + Win32 GetClientRect.
	// The Leia D3D11 weaver doesn't have a direct get_window_metrics equivalent
	// like the Vulkan path. The compositor handles this via display pixel info
	// and Win32 calls. Return false to let the compositor use its fallback.
	(void)xdp;
	(void)out_metrics;
	return false;
}

static bool
leia_dp_d3d11_request_display_mode(struct xrt_display_processor_d3d11 *xdp, bool enable_3d)
{
	struct leia_display_processor_d3d11_impl *ldp = leia_dp_d3d11(xdp);
	return leiasr_d3d11_request_display_mode(ldp->leiasr, enable_3d);
}

static bool
leia_dp_d3d11_get_display_dimensions(struct xrt_display_processor_d3d11 *xdp,
                                     float *out_width_m,
                                     float *out_height_m)
{
	struct leia_display_processor_d3d11_impl *ldp = leia_dp_d3d11(xdp);
	struct leiasr_display_dimensions dims = {};
	if (!leiasr_d3d11_get_display_dimensions(ldp->leiasr, &dims) || !dims.valid) {
		return false;
	}
	*out_width_m = dims.width_m;
	*out_height_m = dims.height_m;
	return true;
}

static bool
leia_dp_d3d11_get_display_pixel_info(struct xrt_display_processor_d3d11 *xdp,
                                     uint32_t *out_pixel_width,
                                     uint32_t *out_pixel_height,
                                     int32_t *out_screen_left,
                                     int32_t *out_screen_top)
{
	struct leia_display_processor_d3d11_impl *ldp = leia_dp_d3d11(xdp);
	float w_m, h_m; // unused but required by API
	return leiasr_d3d11_get_display_pixel_info(ldp->leiasr, out_pixel_width, out_pixel_height, out_screen_left,
	                                           out_screen_top, &w_m, &h_m);
}

static void
leia_dp_d3d11_destroy(struct xrt_display_processor_d3d11 *xdp)
{
	struct leia_display_processor_d3d11_impl *ldp = leia_dp_d3d11(xdp);
	if (ldp->leiasr != NULL) {
		leiasr_d3d11_destroy(&ldp->leiasr);
	}
	free(ldp);
}


/*
 *
 * Helper to populate vtable entries on an impl struct.
 *
 */

static void
leia_dp_d3d11_init_vtable(struct leia_display_processor_d3d11_impl *ldp)
{
	ldp->base.process_stereo = leia_dp_d3d11_process_stereo;
	ldp->base.get_predicted_eye_positions = leia_dp_d3d11_get_predicted_eye_positions;
	ldp->base.get_window_metrics = leia_dp_d3d11_get_window_metrics;
	ldp->base.request_display_mode = leia_dp_d3d11_request_display_mode;
	ldp->base.get_display_dimensions = leia_dp_d3d11_get_display_dimensions;
	ldp->base.get_display_pixel_info = leia_dp_d3d11_get_display_pixel_info;
	ldp->base.destroy = leia_dp_d3d11_destroy;
}


/*
 *
 * Factory function — matches xrt_dp_factory_d3d11_fn_t signature.
 *
 */

extern "C" xrt_result_t
leia_dp_factory_d3d11(void *d3d11_device,
                      void *d3d11_context,
                      void *window_handle,
                      struct xrt_display_processor_d3d11 **out_xdp)
{
	// Create weaver — view dimensions are set per-frame via setInputViewTexture,
	// so we pass 0,0 here (avoids creating a redundant temp SR context just to
	// query recommended dims that leiasr_d3d11_create queries again internally).
	struct leiasr_d3d11 *weaver = NULL;
	xrt_result_t ret = leiasr_d3d11_create(5.0, d3d11_device, d3d11_context,
	                                       window_handle, 0, 0, &weaver);
	if (ret != XRT_SUCCESS || weaver == NULL) {
		U_LOG_W("Failed to create SR D3D11 weaver");
		return ret != XRT_SUCCESS ? ret : XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	struct leia_display_processor_d3d11_impl *ldp =
	    (struct leia_display_processor_d3d11_impl *)calloc(1, sizeof(*ldp));
	if (ldp == NULL) {
		leiasr_d3d11_destroy(&weaver);
		return XRT_ERROR_ALLOCATION;
	}

	leia_dp_d3d11_init_vtable(ldp);
	ldp->leiasr = weaver;

	*out_xdp = &ldp->base;

	U_LOG_W("Created Leia SR D3D11 display processor (factory, owns weaver)");

	return XRT_SUCCESS;
}


/*
 *
 * Legacy creation function — wraps an existing leiasr_d3d11 handle.
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

	leia_dp_d3d11_init_vtable(ldp);
	ldp->leiasr = leiasr;

	*out_xdp = &ldp->base;

	U_LOG_W("Created Leia SR D3D11 display processor (legacy, owns weaver)");

	return XRT_SUCCESS;
}
