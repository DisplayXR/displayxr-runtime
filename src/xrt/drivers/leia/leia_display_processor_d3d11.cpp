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
 * The SR SDK weaver expects side-by-side (SBS) stereo input. The Leia
 * device defines its 3D mode as tile_columns=2, tile_rows=1, so the
 * compositor always delivers SBS. The compositor crop-blit guarantees
 * the atlas texture dimensions match exactly 2*view_width x view_height.
 *
 * @author David Fattal
 * @ingroup drv_leia
 */

#include "leia_display_processor_d3d11.h"
#include "leia_sr_d3d11.h"

#include "xrt/xrt_display_metrics.h"
#include "util/u_logging.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdlib>
#include <cstring>


// Fullscreen quad vertex shader (4 vertices, triangle strip via SV_VertexID)
static const char *blit_vs_source = R"(
struct VS_OUTPUT {
	float4 pos : SV_Position;
	float2 uv  : TEXCOORD0;
};

VS_OUTPUT main(uint id : SV_VertexID) {
	VS_OUTPUT o;
	o.uv = float2(id & 1, id >> 1);
	o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
	return o;
}
)";

// Passthrough pixel shader: samples first tile from atlas, stretches to fill target
static const char *blit_ps_source = R"(
Texture2D atlas_tex : register(t0);
SamplerState samp : register(s0);

cbuffer BlitParams : register(b0) {
	float u_scale;
	float v_scale;
	float pad0;
	float pad1;
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
	return atlas_tex.Sample(samp, float2(uv.x * u_scale, uv.y * v_scale));
}
)";


/*!
 * Implementation struct wrapping leiasr_d3d11 as xrt_display_processor_d3d11.
 */
struct leia_display_processor_d3d11_impl
{
	struct xrt_display_processor_d3d11 base;
	struct leiasr_d3d11 *leiasr; //!< Owned — destroyed in leia_dp_d3d11_destroy.

	ID3D11Device *device;              //!< Cached device reference (not owned, for blit init).

	//! @name 2D blit shader resources (passthrough stretch-blit)
	//! @{
	ID3D11VertexShader *blit_vs;
	ID3D11PixelShader *blit_ps;
	ID3D11SamplerState *blit_sampler;
	ID3D11Buffer *blit_cb; //!< 16 bytes: u_scale, v_scale, pad, pad
	//! @}

	uint32_t view_count; //!< Active mode view count (1=2D, 2=stereo).
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
leia_dp_d3d11_process_atlas(struct xrt_display_processor_d3d11 *xdp,
                             void *d3d11_context,
                             void *atlas_srv,
                             uint32_t view_width,
                             uint32_t view_height,
                             uint32_t tile_columns,
                             uint32_t tile_rows,
                             uint32_t format,
                             uint32_t target_width,
                             uint32_t target_height,
                             int32_t canvas_offset_x,
                             int32_t canvas_offset_y,
                             uint32_t canvas_width,
                             uint32_t canvas_height)
{
	// TODO(#85): Pass canvas_offset_x/y to vendor weaver for interlacing
	// phase correction once Leia SR SDK supports sub-rect offset.
	(void)canvas_offset_x;
	(void)canvas_offset_y;
	(void)canvas_width;
	(void)canvas_height;

	struct leia_display_processor_d3d11_impl *ldp = leia_dp_d3d11(xdp);
	ID3D11DeviceContext *ctx = static_cast<ID3D11DeviceContext *>(d3d11_context);

	// 2D mode: passthrough stretch-blit (first tile fills target)
	if (ldp->view_count == 1) {
		if (ldp->blit_vs == NULL || ldp->blit_ps == NULL) {
			return;
		}

		ID3D11ShaderResourceView *srv = static_cast<ID3D11ShaderResourceView *>(atlas_srv);

		// Atlas is guaranteed content-sized by compositor crop-blit.
		// In 2D mode, content occupies min(target, atlas) of the atlas.
		uint32_t atlas_w = tile_columns * view_width;
		uint32_t atlas_h = tile_rows * view_height;
		uint32_t content_w = (target_width < atlas_w) ? target_width : atlas_w;
		uint32_t content_h = (target_height < atlas_h) ? target_height : atlas_h;
		struct { float u_scale; float v_scale; float pad0; float pad1; } cb_data;
		cb_data.u_scale = (atlas_w > 0) ? (float)content_w / (float)atlas_w : 1.0f;
		cb_data.v_scale = (atlas_h > 0) ? (float)content_h / (float)atlas_h : 1.0f;
		cb_data.pad0 = 0.0f;
		cb_data.pad1 = 0.0f;
		ctx->UpdateSubresource(ldp->blit_cb, 0, NULL, &cb_data, 0, 0);

		// Set viewport
		D3D11_VIEWPORT viewport = {};
		viewport.Width = static_cast<float>(target_width);
		viewport.Height = static_cast<float>(target_height);
		viewport.MaxDepth = 1.0f;
		ctx->RSSetViewports(1, &viewport);

		// Bind shaders, sampler, SRV, and constant buffer
		ctx->VSSetShader(ldp->blit_vs, NULL, 0);
		ctx->PSSetShader(ldp->blit_ps, NULL, 0);
		ctx->PSSetSamplers(0, 1, &ldp->blit_sampler);
		ctx->PSSetShaderResources(0, 1, &srv);
		ctx->PSSetConstantBuffers(0, 1, &ldp->blit_cb);

		// Draw fullscreen quad (4 vertices, triangle strip)
		ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		ctx->IASetInputLayout(NULL);
		ctx->Draw(4, 0);

		// Unbind SRV to prevent D3D11 hazard warnings
		ID3D11ShaderResourceView *null_srv = NULL;
		ctx->PSSetShaderResources(0, 1, &null_srv);
		return;
	}

	// Atlas is guaranteed content-sized SBS (2*view_width x view_height)
	// by compositor crop-blit. Pass directly to weaver.
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

	if (ldp->blit_vs != NULL) {
		ldp->blit_vs->Release();
	}
	if (ldp->blit_ps != NULL) {
		ldp->blit_ps->Release();
	}
	if (ldp->blit_sampler != NULL) {
		ldp->blit_sampler->Release();
	}
	if (ldp->blit_cb != NULL) {
		ldp->blit_cb->Release();
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
 * Helper: compile blit shaders for 2D passthrough mode.
 *
 */

static bool
leia_dp_d3d11_init_blit(struct leia_display_processor_d3d11_impl *ldp)
{
	if (ldp->device == NULL) {
		return false;
	}

	// Compile vertex shader
	ID3DBlob *vs_blob = NULL;
	ID3DBlob *error_blob = NULL;
	HRESULT hr = D3DCompile(blit_vs_source, strlen(blit_vs_source), NULL, NULL, NULL,
	                        "main", "vs_5_0", 0, 0, &vs_blob, &error_blob);
	if (FAILED(hr)) {
		if (error_blob != NULL) {
			U_LOG_E("Leia D3D11 DP: blit VS compile error: %s",
			        (const char *)error_blob->GetBufferPointer());
			error_blob->Release();
		}
		return false;
	}

	hr = ldp->device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
	                                     NULL, &ldp->blit_vs);
	vs_blob->Release();
	if (FAILED(hr)) {
		U_LOG_E("Leia D3D11 DP: failed to create blit VS: 0x%08x", (unsigned)hr);
		return false;
	}

	// Compile pixel shader
	ID3DBlob *ps_blob = NULL;
	hr = D3DCompile(blit_ps_source, strlen(blit_ps_source), NULL, NULL, NULL,
	                "main", "ps_5_0", 0, 0, &ps_blob, &error_blob);
	if (FAILED(hr)) {
		if (error_blob != NULL) {
			U_LOG_E("Leia D3D11 DP: blit PS compile error: %s",
			        (const char *)error_blob->GetBufferPointer());
			error_blob->Release();
		}
		return false;
	}

	hr = ldp->device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(),
	                                    NULL, &ldp->blit_ps);
	ps_blob->Release();
	if (FAILED(hr)) {
		U_LOG_E("Leia D3D11 DP: failed to create blit PS: 0x%08x", (unsigned)hr);
		return false;
	}

	// Create sampler state (linear, clamp)
	D3D11_SAMPLER_DESC sampler_desc = {};
	sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;

	hr = ldp->device->CreateSamplerState(&sampler_desc, &ldp->blit_sampler);
	if (FAILED(hr)) {
		U_LOG_E("Leia D3D11 DP: failed to create blit sampler: 0x%08x", (unsigned)hr);
		return false;
	}

	// Create constant buffer (16 bytes: u_scale, v_scale, pad, pad)
	D3D11_BUFFER_DESC cb_desc = {};
	cb_desc.ByteWidth = 16;
	cb_desc.Usage = D3D11_USAGE_DEFAULT;
	cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

	hr = ldp->device->CreateBuffer(&cb_desc, NULL, &ldp->blit_cb);
	if (FAILED(hr)) {
		U_LOG_E("Leia D3D11 DP: failed to create blit CB: 0x%08x", (unsigned)hr);
		return false;
	}

	U_LOG_I("Leia D3D11 DP: compiled 2D blit shaders");
	return true;
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

	// Compile blit shaders for 2D passthrough mode
	if (!leia_dp_d3d11_init_blit(ldp)) {
		U_LOG_W("Leia D3D11 DP: blit shader init failed — 2D mode will be unavailable");
	}

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
