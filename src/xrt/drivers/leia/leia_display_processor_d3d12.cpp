// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Leia D3D12 display processor: wraps SR SDK D3D12 weaver
 *         as an @ref xrt_display_processor_d3d12.
 *
 * The display processor owns the leiasr_d3d12 handle — it creates it
 * via the factory function and destroys it on cleanup.
 *
 * @author David Fattal
 * @ingroup drv_leia
 */

#include "leia_display_processor_d3d12.h"
#include "leia_sr_d3d12.h"

#include "xrt/xrt_display_metrics.h"
#include "util/u_logging.h"

#include <cstdlib>


/*!
 * Implementation struct wrapping leiasr_d3d12 as xrt_display_processor_d3d12.
 */
struct leia_display_processor_d3d12_impl
{
	struct xrt_display_processor_d3d12 base;
	struct leiasr_d3d12 *leiasr; //!< Owned — destroyed in leia_dp_d3d12_destroy.
};

static inline struct leia_display_processor_d3d12_impl *
leia_dp_d3d12(struct xrt_display_processor_d3d12 *xdp)
{
	return (struct leia_display_processor_d3d12_impl *)xdp;
}


/*
 *
 * xrt_display_processor_d3d12 interface methods.
 *
 */

static void
leia_dp_d3d12_process_atlas(struct xrt_display_processor_d3d12 *xdp,
                             void *d3d12_command_list,
                             void *atlas_texture_resource,
                             uint64_t atlas_srv_gpu_handle,
                             uint64_t target_rtv_cpu_handle,
                             uint32_t view_width,
                             uint32_t view_height,
                             uint32_t format,
                             uint32_t target_width,
                             uint32_t target_height)
{
	(void)atlas_srv_gpu_handle;
	(void)target_rtv_cpu_handle;
	struct leia_display_processor_d3d12_impl *ldp = leia_dp_d3d12(xdp);

	// Set input texture — SR DX12 weaver needs the ID3D12Resource*
	if (atlas_texture_resource != NULL) {
		leiasr_d3d12_set_input_texture(ldp->leiasr, atlas_texture_resource,
		                               view_width, view_height, format);
	}

	// Record weaving commands onto the command list
	leiasr_d3d12_weave(ldp->leiasr, d3d12_command_list, target_width, target_height);
}

static void
leia_dp_d3d12_set_output_format(struct xrt_display_processor_d3d12 *xdp, uint32_t format)
{
	struct leia_display_processor_d3d12_impl *ldp = leia_dp_d3d12(xdp);
	leiasr_d3d12_set_output_format(ldp->leiasr, format);
}

static bool
leia_dp_d3d12_get_predicted_eye_positions(struct xrt_display_processor_d3d12 *xdp,
                                          struct xrt_eye_pair *out_eye_pair)
{
	struct leia_display_processor_d3d12_impl *ldp = leia_dp_d3d12(xdp);
	float left[3], right[3];
	if (!leiasr_d3d12_get_predicted_eye_positions(ldp->leiasr, left, right)) {
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
leia_dp_d3d12_request_display_mode(struct xrt_display_processor_d3d12 *xdp, bool enable_3d)
{
	struct leia_display_processor_d3d12_impl *ldp = leia_dp_d3d12(xdp);
	return leiasr_d3d12_request_display_mode(ldp->leiasr, enable_3d);
}

static bool
leia_dp_d3d12_get_display_dimensions(struct xrt_display_processor_d3d12 *xdp,
                                     float *out_width_m,
                                     float *out_height_m)
{
	struct leia_display_processor_d3d12_impl *ldp = leia_dp_d3d12(xdp);
	struct leiasr_display_dimensions dims = {};
	if (!leiasr_d3d12_get_display_dimensions(ldp->leiasr, &dims) || !dims.valid) {
		return false;
	}
	*out_width_m = dims.width_m;
	*out_height_m = dims.height_m;
	return true;
}

static bool
leia_dp_d3d12_get_display_pixel_info(struct xrt_display_processor_d3d12 *xdp,
                                     uint32_t *out_pixel_width,
                                     uint32_t *out_pixel_height,
                                     int32_t *out_screen_left,
                                     int32_t *out_screen_top)
{
	struct leia_display_processor_d3d12_impl *ldp = leia_dp_d3d12(xdp);
	float w_m, h_m; // unused but required by API
	return leiasr_d3d12_get_display_pixel_info(ldp->leiasr, out_pixel_width, out_pixel_height,
	                                           out_screen_left, out_screen_top, &w_m, &h_m);
}

static void
leia_dp_d3d12_destroy(struct xrt_display_processor_d3d12 *xdp)
{
	struct leia_display_processor_d3d12_impl *ldp = leia_dp_d3d12(xdp);
	if (ldp->leiasr != NULL) {
		leiasr_d3d12_destroy(&ldp->leiasr);
	}
	free(ldp);
}


/*
 *
 * Factory function — matches xrt_dp_factory_d3d12_fn_t signature.
 *
 */

extern "C" xrt_result_t
leia_dp_factory_d3d12(void *d3d12_device,
                      void *d3d12_command_queue,
                      void *window_handle,
                      struct xrt_display_processor_d3d12 **out_xdp)
{
	// Create weaver — view dimensions are set per-frame via setInputViewTexture,
	// so we pass 0,0 here.
	struct leiasr_d3d12 *weaver = NULL;
	xrt_result_t ret = leiasr_d3d12_create(5.0, d3d12_device, d3d12_command_queue,
	                                       window_handle, 0, 0, &weaver);
	if (ret != XRT_SUCCESS || weaver == NULL) {
		U_LOG_W("Failed to create SR D3D12 weaver");
		return ret != XRT_SUCCESS ? ret : XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	struct leia_display_processor_d3d12_impl *ldp =
	    (struct leia_display_processor_d3d12_impl *)calloc(1, sizeof(*ldp));
	if (ldp == NULL) {
		leiasr_d3d12_destroy(&weaver);
		return XRT_ERROR_ALLOCATION;
	}

	ldp->base.process_atlas = leia_dp_d3d12_process_atlas;
	ldp->base.set_output_format = leia_dp_d3d12_set_output_format;
	ldp->base.get_predicted_eye_positions = leia_dp_d3d12_get_predicted_eye_positions;
	ldp->base.get_window_metrics = NULL;
	ldp->base.request_display_mode = leia_dp_d3d12_request_display_mode;
	ldp->base.get_display_dimensions = leia_dp_d3d12_get_display_dimensions;
	ldp->base.get_display_pixel_info = leia_dp_d3d12_get_display_pixel_info;
	ldp->base.destroy = leia_dp_d3d12_destroy;
	ldp->leiasr = weaver;

	*out_xdp = &ldp->base;

	U_LOG_W("Created Leia SR D3D12 display processor (factory, owns weaver)");

	return XRT_SUCCESS;
}
