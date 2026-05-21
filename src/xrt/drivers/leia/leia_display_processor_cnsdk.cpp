// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Leia CNSDK display processor (Android) — POC stub.
 *
 * Wraps leia_cnsdk as an xrt_display_processor so the runtime has a
 * vendor DP slot filled on Android. process_atlas is intentionally a
 * no-op for now: CNSDK's leia_interlacer_vulkan_do_post_process records
 * its own command buffer and submits internally, which doesn't fit the
 * compositor's "append to my cmd_buffer" contract. Wiring it up
 * properly is #126 (add a self_submitting DP flag) plus #125 followup
 * (per-tile image views over the SBS atlas).
 *
 * Until then this DP:
 *   - lets the factory call succeed (compositor no longer logs
 *     "No VK display processor factory provided")
 *   - returns hardcoded IPD-only eye positions (POC bypass for face
 *     tracking — see android-poc-state memory)
 *   - returns Lume Pad-class display metrics so XR_EXT_display_info
 *     reports something sensible to the test app
 *
 * @author David Fattal
 * @ingroup drv_leia
 */

#include "leia_display_processor_cnsdk.h"
#include "leia_cnsdk.h"

#include "xrt/xrt_display_metrics.h"
#include "util/u_logging.h"

#include <stdlib.h>

namespace {

// Lume Pad 2-class defaults. Used until CNSDK reports the real device
// metrics through leia_core (CNSDK exposes those via leia_core_get_*
// once the async init completes — wiring TBD).
constexpr float kDefaultDisplayWidthM  = 0.1934f;  // ~12.4" diagonal, 16:10
constexpr float kDefaultDisplayHeightM = 0.1209f;
constexpr uint32_t kDefaultDisplayPixelW = 2560;
constexpr uint32_t kDefaultDisplayPixelH = 1600;

// Hardcoded IPD-only eye positions. Origin = display center; z is
// toward the user. 65 mm IPD, ~50 cm viewing distance.
constexpr float kIpdHalfM       = 0.0325f;
constexpr float kEyeViewerDistM = 0.5f;

struct leia_dp_cnsdk
{
	struct xrt_display_processor base;
	struct leia_cnsdk *cnsdk;          //!< Owned.
};

inline leia_dp_cnsdk *
as_impl(struct xrt_display_processor *xdp)
{
	return reinterpret_cast<leia_dp_cnsdk *>(xdp);
}

void
process_atlas_noop(struct xrt_display_processor *xdp,
                   VkCommandBuffer cmd_buffer,
                   VkImage_XDP atlas_image,
                   VkImageView atlas_view,
                   uint32_t view_width,
                   uint32_t view_height,
                   uint32_t tile_columns,
                   uint32_t tile_rows,
                   VkFormat_XDP view_format,
                   VkFramebuffer target_fb,
                   VkImage_XDP target_image,
                   uint32_t target_width,
                   uint32_t target_height,
                   VkFormat_XDP target_format,
                   int32_t canvas_offset_x,
                   int32_t canvas_offset_y,
                   uint32_t canvas_width,
                   uint32_t canvas_height)
{
	(void)xdp; (void)cmd_buffer; (void)atlas_image; (void)atlas_view;
	(void)view_width; (void)view_height; (void)tile_columns; (void)tile_rows;
	(void)view_format; (void)target_fb; (void)target_image;
	(void)target_width; (void)target_height; (void)target_format;
	(void)canvas_offset_x; (void)canvas_offset_y;
	(void)canvas_width; (void)canvas_height;
	// TODO(#126 + #125): once the self_submitting flag exists and we have
	// per-tile VkImageViews over the SBS atlas, call leia_cnsdk_weave()
	// here with (left_view, right_view, target_fb, target_image).
}

bool
get_predicted_eye_positions_ipd(struct xrt_display_processor *xdp,
                                 struct xrt_eye_positions *out_eye_pos)
{
	(void)xdp;
	out_eye_pos->eyes[0].x = -kIpdHalfM;
	out_eye_pos->eyes[0].y = 0.0f;
	out_eye_pos->eyes[0].z = kEyeViewerDistM;
	out_eye_pos->eyes[1].x = +kIpdHalfM;
	out_eye_pos->eyes[1].y = 0.0f;
	out_eye_pos->eyes[1].z = kEyeViewerDistM;
	out_eye_pos->count = 2;
	out_eye_pos->valid = true;
	out_eye_pos->is_tracking = false;
	return true;
}

bool
get_display_dimensions_default(struct xrt_display_processor *xdp,
                                float *out_width_m,
                                float *out_height_m)
{
	(void)xdp;
	*out_width_m = kDefaultDisplayWidthM;
	*out_height_m = kDefaultDisplayHeightM;
	return true;
}

bool
get_display_pixel_info_default(struct xrt_display_processor *xdp,
                                uint32_t *out_pixel_width,
                                uint32_t *out_pixel_height,
                                int32_t *out_screen_left,
                                int32_t *out_screen_top)
{
	(void)xdp;
	*out_pixel_width = kDefaultDisplayPixelW;
	*out_pixel_height = kDefaultDisplayPixelH;
	*out_screen_left = 0;
	*out_screen_top = 0;
	return true;
}

void
destroy_impl(struct xrt_display_processor *xdp)
{
	leia_dp_cnsdk *impl = as_impl(xdp);
	if (impl->cnsdk != nullptr) {
		leia_cnsdk_destroy(&impl->cnsdk);
	}
	free(impl);
}

} // namespace

extern "C" xrt_result_t
leia_dp_factory_cnsdk(void *vk_bundle,
                      void *vk_cmd_pool,
                      void *window_handle,
                      int32_t target_format,
                      struct xrt_display_processor **out_xdp)
{
	// vk_bundle / vk_cmd_pool / window_handle / target_format are unused
	// today — CNSDK looks up its own VkDevice via leia_core. They become
	// relevant when process_atlas() actually calls leia_cnsdk_weave (#126).
	(void)vk_bundle; (void)vk_cmd_pool; (void)window_handle; (void)target_format;

	struct leia_cnsdk *cnsdk = nullptr;
	xrt_result_t ret = leia_cnsdk_create(&cnsdk);
	if (ret != XRT_SUCCESS || cnsdk == nullptr) {
		U_LOG_W("leia_cnsdk_create failed (%d), falling back to no-DP path", (int)ret);
		return ret != XRT_SUCCESS ? ret : XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	leia_dp_cnsdk *impl = static_cast<leia_dp_cnsdk *>(calloc(1, sizeof(*impl)));
	if (impl == nullptr) {
		leia_cnsdk_destroy(&cnsdk);
		return XRT_ERROR_ALLOCATION;
	}

	impl->cnsdk = cnsdk;
	impl->base.process_atlas = process_atlas_noop;
	impl->base.get_predicted_eye_positions = get_predicted_eye_positions_ipd;
	impl->base.get_display_dimensions = get_display_dimensions_default;
	impl->base.get_display_pixel_info = get_display_pixel_info_default;
	impl->base.destroy = destroy_impl;
	// All other vtable entries left NULL — inline helpers in
	// xrt_display_processor.h NULL-check and return false / no-op.

	*out_xdp = &impl->base;

	U_LOG_W("Leia CNSDK DP created (POC: process_atlas is a no-op until #126)");
	return XRT_SUCCESS;
}
