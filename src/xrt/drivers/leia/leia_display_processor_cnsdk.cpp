// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Leia CNSDK display processor (Android), atlas-mode variant.
 *
 * Wraps leia_cnsdk as an xrt_display_processor. Uses CNSDK's atlas mode:
 * the compositor's pre-composited SBS atlas VkImage+View is passed
 * directly to CNSDK each frame via
 * leia_interlacer_vulkan_set_interlace_view_texture_atlas, and CNSDK
 * splits L/R internally. No per-view image management, no per-tile
 * blit, no blit cmd buffer / semaphore / fence on our side.
 *
 * Display metrics + face-tracked eye positions come from CNSDK once the
 * async core init completes (lazy on first query). Falls back to
 * hardcoded Lume Pad 2 metrics and IPD-only eyes while the core is
 * still booting.
 *
 * @author David Fattal
 * @ingroup drv_leia
 */

#include "leia_display_processor_cnsdk.h"
#include "leia_cnsdk.h"

#include "xrt/xrt_display_metrics.h"
#include "vk/vk_helpers.h"
#include "util/u_logging.h"

#include <stdlib.h>

namespace {

// Lume Pad 2-class defaults. Used until CNSDK reports the real device
// metrics through leia_core via the leia_cnsdk wrapper's cached values.
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
	struct vk_bundle *vk;              //!< Borrowed from compositor.
};

inline leia_dp_cnsdk *
as_impl(struct xrt_display_processor *xdp)
{
	return reinterpret_cast<leia_dp_cnsdk *>(xdp);
}

void
process_atlas_weave(struct xrt_display_processor *xdp,
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
	(void)cmd_buffer;     // self-submitting: compositor passes VK_NULL_HANDLE
	(void)view_format;
	(void)canvas_offset_x; (void)canvas_offset_y;
	(void)canvas_width; (void)canvas_height;

	leia_dp_cnsdk *impl = as_impl(xdp);

	if (tile_columns != 2 || tile_rows != 1) {
		static bool warned = false;
		if (!warned) {
			U_LOG_W("CNSDK DP expects 2x1 SBS atlas, got %ux%u; skipping weave",
			        tile_columns, tile_rows);
			warned = true;
		}
		return;
	}

	if (impl->vk == nullptr) {
		return;
	}

	// Gate the weave on CNSDK interlacer readiness. Async core init may
	// take several frames; until then the interlacer doesn't exist and
	// leia_cnsdk_weave is a no-op. The compositor's pre-DP submit has
	// already happened, so target is in COLOR_ATTACHMENT_OPTIMAL with
	// undefined content for those frames — acceptable for POC.
	if (!leia_cnsdk_ensure_interlacer(impl->cnsdk,
	                                   impl->vk->device,
	                                   impl->vk->physical_device,
	                                   (VkFormat)target_format)) {
		return;
	}

	const uint32_t atlas_w = view_width * tile_columns;
	const uint32_t atlas_h = view_height * tile_rows;

	leia_cnsdk_weave(impl->cnsdk,
	                 impl->vk->device,
	                 impl->vk->physical_device,
	                 (VkImage)(uintptr_t)atlas_image,
	                 atlas_view,
	                 atlas_w,
	                 atlas_h,
	                 (VkFormat)target_format,
	                 target_width,
	                 target_height,
	                 target_fb,
	                 (VkImage)(uintptr_t)target_image);
}

bool
is_self_submitting_true(struct xrt_display_processor *xdp)
{
	(void)xdp;
	return true;
}

// Try to fetch CNSDK's predicted face position and derive L/R eyes from
// it (face X ± IPD/2). Falls back to a hardcoded IPD-only stub if face
// tracking isn't running yet (CNSDK core still async-initializing, or
// no face lock).
bool
get_predicted_eye_positions_ipd(struct xrt_display_processor *xdp,
                                 struct xrt_eye_positions *out_eye_pos)
{
	leia_dp_cnsdk *impl = as_impl(xdp);

	bool tracked = false;
	float fx = 0.0f, fy = 0.0f, fz = kEyeViewerDistM;
	if (impl->cnsdk != nullptr) {
		if (leia_cnsdk_ensure_face_tracking_started(impl->cnsdk) &&
		    leia_cnsdk_get_primary_face(impl->cnsdk, &fx, &fy, &fz)) {
			tracked = true;
		}
	}

	out_eye_pos->eyes[0].x = fx - kIpdHalfM;
	out_eye_pos->eyes[0].y = fy;
	out_eye_pos->eyes[0].z = fz;
	out_eye_pos->eyes[1].x = fx + kIpdHalfM;
	out_eye_pos->eyes[1].y = fy;
	out_eye_pos->eyes[1].z = fz;
	out_eye_pos->count = 2;
	out_eye_pos->valid = true;
	out_eye_pos->is_tracking = tracked;
	return true;
}

bool
get_display_dimensions_default(struct xrt_display_processor *xdp,
                                float *out_width_m,
                                float *out_height_m)
{
	leia_dp_cnsdk *impl = as_impl(xdp);

	if (impl->cnsdk != nullptr &&
	    leia_cnsdk_get_display_metrics(impl->cnsdk, out_width_m, out_height_m,
	                                    nullptr, nullptr)) {
		return true;
	}

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
	leia_dp_cnsdk *impl = as_impl(xdp);

	*out_screen_left = 0;
	*out_screen_top = 0;

	if (impl->cnsdk != nullptr &&
	    leia_cnsdk_get_display_metrics(impl->cnsdk, nullptr, nullptr,
	                                    out_pixel_width, out_pixel_height)) {
		return true;
	}

	*out_pixel_width = kDefaultDisplayPixelW;
	*out_pixel_height = kDefaultDisplayPixelH;
	return true;
}

void
destroy_impl(struct xrt_display_processor *xdp)
{
	leia_dp_cnsdk *impl = as_impl(xdp);

	// Drain all in-flight GPU work (especially CNSDK's interlacer
	// submits) before destroying any handles CNSDK might still be
	// reading. Same defensive idiom as audit B6 — applies in atlas mode
	// because CNSDK still owns its own queue submit even though we
	// don't have a per-view image / blit cmd buffer to wait on.
	if (impl->vk != nullptr) {
		impl->vk->vkDeviceWaitIdle(impl->vk->device);
	}

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
	(void)vk_cmd_pool;     // atlas mode owns no cmd buffer — CNSDK records its own
	(void)window_handle; (void)target_format;

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
	impl->vk = static_cast<struct vk_bundle *>(vk_bundle);

	impl->base.process_atlas = process_atlas_weave;
	impl->base.is_self_submitting = is_self_submitting_true;
	impl->base.get_predicted_eye_positions = get_predicted_eye_positions_ipd;
	impl->base.get_display_dimensions = get_display_dimensions_default;
	impl->base.get_display_pixel_info = get_display_pixel_info_default;
	impl->base.destroy = destroy_impl;

	*out_xdp = &impl->base;

	U_LOG_W("Leia CNSDK DP created (atlas mode)");
	return XRT_SUCCESS;
}
