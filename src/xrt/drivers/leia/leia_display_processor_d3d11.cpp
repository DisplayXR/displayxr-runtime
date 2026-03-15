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
 * The SR SDK weaver expects side-by-side (SBS) stereo input. When the
 * compositor's atlas uses a different tiling layout (e.g. vertical stacking
 * with tile_columns=1, tile_rows=2), this DP rearranges the atlas into
 * SBS format via CopySubresourceRegion before passing to the weaver.
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

	//! @name SBS staging resources for non-SBS atlas layouts
	//! @{
	ID3D11Device *device;              //!< Cached device reference (not owned).
	ID3D11Texture2D *sbs_texture;      //!< Staging SBS texture (lazy-created).
	ID3D11ShaderResourceView *sbs_srv; //!< SRV for sbs_texture.
	uint32_t sbs_width;                //!< Current staging texture width.
	uint32_t sbs_height;               //!< Current staging texture height.
	DXGI_FORMAT sbs_format;            //!< Current staging texture format.
	//! @}

	uint32_t view_count; //!< Active mode view count (1=2D, 2=stereo).
};

static inline struct leia_display_processor_d3d11_impl *
leia_dp_d3d11(struct xrt_display_processor_d3d11 *xdp)
{
	return (struct leia_display_processor_d3d11_impl *)xdp;
}


/*!
 * Ensure the SBS staging texture exists with the right dimensions/format.
 * Returns true if the staging texture is ready to use.
 */
static bool
ensure_sbs_staging(struct leia_display_processor_d3d11_impl *ldp,
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

	// Release old resources
	if (ldp->sbs_srv != NULL) {
		ldp->sbs_srv->Release();
		ldp->sbs_srv = NULL;
	}
	if (ldp->sbs_texture != NULL) {
		ldp->sbs_texture->Release();
		ldp->sbs_texture = NULL;
	}

	if (ldp->device == NULL) {
		return false;
	}

	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = sbs_w;
	desc.Height = sbs_h;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = format;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

	HRESULT hr = ldp->device->CreateTexture2D(&desc, NULL, &ldp->sbs_texture);
	if (FAILED(hr)) {
		U_LOG_E("Leia D3D11 DP: failed to create SBS staging texture (%ux%u): 0x%08x",
		        sbs_w, sbs_h, (unsigned)hr);
		return false;
	}

	hr = ldp->device->CreateShaderResourceView(ldp->sbs_texture, NULL, &ldp->sbs_srv);
	if (FAILED(hr)) {
		U_LOG_E("Leia D3D11 DP: failed to create SBS staging SRV: 0x%08x", (unsigned)hr);
		ldp->sbs_texture->Release();
		ldp->sbs_texture = NULL;
		return false;
	}

	ldp->sbs_width = sbs_w;
	ldp->sbs_height = sbs_h;
	ldp->sbs_format = format;

	U_LOG_I("Leia D3D11 DP: created SBS staging texture %ux%u", sbs_w, sbs_h);
	return true;
}


/*
 *
 * xrt_display_processor_d3d11 interface methods.
 *
 */

static void
leia_dp_d3d11_process_atlas(struct xrt_display_processor_d3d11 *xdp,
                             void *d3d11_context,
                             void *atlas_srv,
                             uint32_t view_width,
                             uint32_t view_height,
                             uint32_t tile_columns,
                             uint32_t tile_rows,
                             uint32_t format,
                             uint32_t target_width,
                             uint32_t target_height)
{
	struct leia_display_processor_d3d11_impl *ldp = leia_dp_d3d11(xdp);
	ID3D11DeviceContext *ctx = static_cast<ID3D11DeviceContext *>(d3d11_context);

	// 2D mode: bypass weaver, use weaver's internal blit shader (lens is off)
	if (ldp->view_count == 1) {
		// Weaver in passthrough mode: lens is off (request_display_mode(false) already called),
		// so canWeaveInternal() returns false and weaver uses blit shader.
		// Pass full content dimensions so blit shader doesn't halve.
		leiasr_d3d11_set_input_texture(ldp->leiasr, atlas_srv, view_width, view_height, format);

		D3D11_VIEWPORT viewport = {};
		viewport.TopLeftX = 0.0f;
		viewport.TopLeftY = 0.0f;
		viewport.Width = static_cast<float>(target_width);
		viewport.Height = static_cast<float>(target_height);
		viewport.MinDepth = 0.0f;
		viewport.MaxDepth = 1.0f;
		ctx->RSSetViewports(1, &viewport);

		leiasr_d3d11_weave(ldp->leiasr);
		return;
	}

	void *weaver_srv = atlas_srv;
	uint32_t weaver_view_width = view_width;

	// If atlas is already SBS (tile_columns=2, tile_rows=1), pass directly.
	// Otherwise, rearrange to SBS via CopySubresourceRegion.
	if (tile_columns != 2 || tile_rows != 1) {
		DXGI_FORMAT dxgi_format = static_cast<DXGI_FORMAT>(format);
		if (!ensure_sbs_staging(ldp, view_width, view_height, dxgi_format)) {
			// Fallback: pass atlas as-is (will look wrong but won't crash)
			goto do_weave;
		}

		// Get the atlas texture resource from the SRV
		ID3D11Resource *atlas_resource = NULL;
		static_cast<ID3D11ShaderResourceView *>(atlas_srv)->GetResource(&atlas_resource);

		// Copy each view from tiled position to SBS position
		for (uint32_t i = 0; i < 2; i++) {
			uint32_t src_x = (i % tile_columns) * view_width;
			uint32_t src_y = (i / tile_columns) * view_height;
			uint32_t dst_x = i * view_width;

			D3D11_BOX src_box;
			src_box.left = src_x;
			src_box.top = src_y;
			src_box.front = 0;
			src_box.right = src_x + view_width;
			src_box.bottom = src_y + view_height;
			src_box.back = 1;

			ctx->CopySubresourceRegion(ldp->sbs_texture, 0, dst_x, 0, 0,
			                           atlas_resource, 0, &src_box);
		}

		atlas_resource->Release();
		weaver_srv = ldp->sbs_srv;
	}

do_weave:
	// Set input texture for weaving
	leiasr_d3d11_set_input_texture(ldp->leiasr, weaver_srv, view_width, view_height, format);

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
                                          struct xrt_eye_positions *out_eye_pos)
{
	struct leia_display_processor_d3d11_impl *ldp = leia_dp_d3d11(xdp);
	float left[3], right[3];
	if (!leiasr_d3d11_get_predicted_eye_positions(ldp->leiasr, left, right)) {
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
	bool ok = leiasr_d3d11_request_display_mode(ldp->leiasr, enable_3d);
	if (ok) {
		ldp->view_count = enable_3d ? 2 : 1;
	}
	return ok;
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

	if (ldp->sbs_srv != NULL) {
		ldp->sbs_srv->Release();
	}
	if (ldp->sbs_texture != NULL) {
		ldp->sbs_texture->Release();
	}

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
	ldp->base.process_atlas = leia_dp_d3d11_process_atlas;
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
	ldp->device = static_cast<ID3D11Device *>(d3d11_device);
	ldp->view_count = 2;

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
	ldp->view_count = 2;
	// Legacy path: no device reference, SBS rearrangement not available.
	// Atlas must already be SBS.

	*out_xdp = &ldp->base;

	U_LOG_W("Created Leia SR D3D11 display processor (legacy, owns weaver)");

	return XRT_SUCCESS;
}
