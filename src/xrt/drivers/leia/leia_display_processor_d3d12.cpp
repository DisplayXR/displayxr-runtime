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
 * The SR SDK weaver expects side-by-side (SBS) stereo input. When the
 * compositor's atlas uses a different tiling layout (e.g. vertical stacking
 * with tile_columns=1, tile_rows=2), this DP rearranges the atlas into
 * SBS format via CopyTextureRegion before passing to the weaver.
 *
 * @author David Fattal
 * @ingroup drv_leia
 */

#include "leia_display_processor_d3d12.h"
#include "leia_sr_d3d12.h"

#include "xrt/xrt_display_metrics.h"
#include "util/u_logging.h"

#include <d3d12.h>
#include <cstdlib>


/*!
 * Implementation struct wrapping leiasr_d3d12 as xrt_display_processor_d3d12.
 */
struct leia_display_processor_d3d12_impl
{
	struct xrt_display_processor_d3d12 base;
	struct leiasr_d3d12 *leiasr; //!< Owned — destroyed in leia_dp_d3d12_destroy.

	//! @name SBS staging resources for non-SBS atlas layouts
	//! @{
	ID3D12Device *device;              //!< Cached device reference (not owned).
	ID3D12Resource *sbs_texture;       //!< Staging SBS texture (lazy-created).
	uint32_t sbs_width;                //!< Current staging texture width.
	uint32_t sbs_height;               //!< Current staging texture height.
	DXGI_FORMAT sbs_format;            //!< Current staging texture format.
	//! @}

	uint32_t view_count; //!< Active mode view count (1=2D, 2=stereo).
};

static inline struct leia_display_processor_d3d12_impl *
leia_dp_d3d12(struct xrt_display_processor_d3d12 *xdp)
{
	return (struct leia_display_processor_d3d12_impl *)xdp;
}


/*!
 * Ensure the SBS staging texture exists with the right dimensions/format.
 */
static bool
ensure_sbs_staging_d3d12(struct leia_display_processor_d3d12_impl *ldp,
                         uint32_t view_width,
                         uint32_t view_height,
                         DXGI_FORMAT format)
{
	uint32_t sbs_w = 2 * view_width;
	uint32_t sbs_h = view_height;

	if (ldp->sbs_texture != NULL && ldp->sbs_width == sbs_w &&
	    ldp->sbs_height == sbs_h && ldp->sbs_format == format) {
		return true;
	}

	if (ldp->sbs_texture != NULL) {
		ldp->sbs_texture->Release();
		ldp->sbs_texture = NULL;
	}

	if (ldp->device == NULL) {
		return false;
	}

	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = sbs_w;
	desc.Height = sbs_h;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = format;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	desc.Flags = D3D12_RESOURCE_FLAG_NONE;

	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;

	HRESULT hr = ldp->device->CreateCommittedResource(
	    &heap, D3D12_HEAP_FLAG_NONE, &desc,
	    D3D12_RESOURCE_STATE_COPY_DEST, NULL,
	    IID_PPV_ARGS(&ldp->sbs_texture));

	if (FAILED(hr)) {
		U_LOG_E("Leia D3D12 DP: failed to create SBS staging texture (%ux%u): 0x%08x",
		        sbs_w, sbs_h, (unsigned)hr);
		return false;
	}

	ldp->sbs_width = sbs_w;
	ldp->sbs_height = sbs_h;
	ldp->sbs_format = format;

	U_LOG_I("Leia D3D12 DP: created SBS staging texture %ux%u", sbs_w, sbs_h);
	return true;
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
                             uint32_t tile_columns,
                             uint32_t tile_rows,
                             uint32_t format,
                             uint32_t target_width,
                             uint32_t target_height)
{
	(void)atlas_srv_gpu_handle;
	(void)target_rtv_cpu_handle;
	struct leia_display_processor_d3d12_impl *ldp = leia_dp_d3d12(xdp);

	// 2D mode: bypass weaver, use weaver's internal blit shader (lens is off)
	if (ldp->view_count == 1) {
		if (atlas_texture_resource != NULL) {
			leiasr_d3d12_set_input_texture(ldp->leiasr, atlas_texture_resource,
			                               view_width, view_height, format);
		}
		leiasr_d3d12_weave(ldp->leiasr, d3d12_command_list, target_width, target_height);
		return;
	}

	void *weaver_resource = atlas_texture_resource;

	// If atlas is already SBS (tile_columns=2, tile_rows=1), pass directly.
	// Otherwise, rearrange to SBS via CopyTextureRegion.
	if (tile_columns != 2 || tile_rows != 1) {
		DXGI_FORMAT dxgi_format = static_cast<DXGI_FORMAT>(format);
		if (!ensure_sbs_staging_d3d12(ldp, view_width, view_height, dxgi_format) ||
		    atlas_texture_resource == NULL) {
			goto do_weave;
		}

		ID3D12GraphicsCommandList *cmd = static_cast<ID3D12GraphicsCommandList *>(d3d12_command_list);

		// Copy each view from tiled position to SBS position
		for (uint32_t i = 0; i < 2; i++) {
			uint32_t src_x = (i % tile_columns) * view_width;
			uint32_t src_y = (i / tile_columns) * view_height;
			uint32_t dst_x = i * view_width;

			D3D12_TEXTURE_COPY_LOCATION dst_loc = {};
			dst_loc.pResource = ldp->sbs_texture;
			dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			dst_loc.SubresourceIndex = 0;

			D3D12_TEXTURE_COPY_LOCATION src_loc = {};
			src_loc.pResource = static_cast<ID3D12Resource *>(atlas_texture_resource);
			src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			src_loc.SubresourceIndex = 0;

			D3D12_BOX src_box;
			src_box.left = src_x;
			src_box.top = src_y;
			src_box.front = 0;
			src_box.right = src_x + view_width;
			src_box.bottom = src_y + view_height;
			src_box.back = 1;

			cmd->CopyTextureRegion(&dst_loc, dst_x, 0, 0, &src_loc, &src_box);
		}

		// Transition SBS texture to shader resource for weaver to read
		D3D12_RESOURCE_BARRIER barrier = {};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = ldp->sbs_texture;
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		cmd->ResourceBarrier(1, &barrier);

		weaver_resource = ldp->sbs_texture;
	}

do_weave:
	if (weaver_resource != NULL) {
		leiasr_d3d12_set_input_texture(ldp->leiasr, weaver_resource,
		                               view_width, view_height, format);
	}

	leiasr_d3d12_weave(ldp->leiasr, d3d12_command_list, target_width, target_height);

	// Transition SBS texture back to copy dest for next frame
	if (tile_columns != 2 || tile_rows != 1) {
		if (ldp->sbs_texture != NULL) {
			ID3D12GraphicsCommandList *cmd = static_cast<ID3D12GraphicsCommandList *>(d3d12_command_list);
			D3D12_RESOURCE_BARRIER barrier = {};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource = ldp->sbs_texture;
			barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			cmd->ResourceBarrier(1, &barrier);
		}
	}
}

static void
leia_dp_d3d12_set_output_format(struct xrt_display_processor_d3d12 *xdp, uint32_t format)
{
	struct leia_display_processor_d3d12_impl *ldp = leia_dp_d3d12(xdp);
	leiasr_d3d12_set_output_format(ldp->leiasr, format);
}

static bool
leia_dp_d3d12_get_predicted_eye_positions(struct xrt_display_processor_d3d12 *xdp,
                                          struct xrt_eye_positions *out_eye_pos)
{
	struct leia_display_processor_d3d12_impl *ldp = leia_dp_d3d12(xdp);
	float left[3], right[3];
	if (!leiasr_d3d12_get_predicted_eye_positions(ldp->leiasr, left, right)) {
		return false;
	}
	out_eye_pos->eyes[0].x = left[0];
	out_eye_pos->eyes[0].y = left[1];
	out_eye_pos->eyes[0].z = left[2];
	out_eye_pos->eyes[1].x = right[0];
	out_eye_pos->eyes[1].y = right[1];
	out_eye_pos->eyes[1].z = right[2];
	out_eye_pos->count = 2;
	out_eye_pos->valid = true;
	out_eye_pos->is_tracking = true;
	// In 2D mode, average L/R to a single midpoint eye.
	if (ldp->view_count == 1 && out_eye_pos->count >= 2) {
		out_eye_pos->eyes[0].x = (out_eye_pos->eyes[0].x + out_eye_pos->eyes[1].x) * 0.5f;
		out_eye_pos->eyes[0].y = (out_eye_pos->eyes[0].y + out_eye_pos->eyes[1].y) * 0.5f;
		out_eye_pos->eyes[0].z = (out_eye_pos->eyes[0].z + out_eye_pos->eyes[1].z) * 0.5f;
		out_eye_pos->count = 1;
	}
	return true;
}

static bool
leia_dp_d3d12_request_display_mode(struct xrt_display_processor_d3d12 *xdp, bool enable_3d)
{
	struct leia_display_processor_d3d12_impl *ldp = leia_dp_d3d12(xdp);
	bool ok = leiasr_d3d12_request_display_mode(ldp->leiasr, enable_3d);
	if (ok) {
		ldp->view_count = enable_3d ? 2 : 1;
	}
	return ok;
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

	if (ldp->sbs_texture != NULL) {
		ldp->sbs_texture->Release();
	}

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
	ldp->device = static_cast<ID3D12Device *>(d3d12_device);
	ldp->view_count = 2;

	*out_xdp = &ldp->base;

	U_LOG_W("Created Leia SR D3D12 display processor (factory, owns weaver)");

	return XRT_SUCCESS;
}
