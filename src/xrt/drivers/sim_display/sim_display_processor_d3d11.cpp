// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Simulation D3D11 display processor: SBS, anaglyph, alpha-blend output.
 *
 * Implements SBS, anaglyph, alpha-blend, squeezed SBS, and quad atlas output
 * modes using HLSL shaders compiled at runtime via D3DCompile. All 5 shaders
 * are pre-compiled at init for instant runtime switching via 1/2/3/4/5 keys.
 *
 * @author David Fattal
 * @ingroup drv_sim_display
 */

#include "sim_display_interface.h"

#include "xrt/xrt_display_processor_d3d11.h"
#include "xrt/xrt_display_metrics.h"

#include "util/u_debug.h"
#include "util/u_logging.h"
#include "os/os_time.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <cstdlib>
#include <cstring>

DEBUG_GET_ONCE_FLOAT_OPTION(sim_display_nominal_z_m_d3d11, "SIM_DISPLAY_NOMINAL_Z_M", 0.60f)


// Fullscreen quad vertex shader (4 vertices, triangle strip via SV_VertexID)
static const char *vs_source = R"(
struct VS_OUTPUT {
	float4 pos : SV_Position;
	float2 uv  : TEXCOORD0;
};

VS_OUTPUT main(uint id : SV_VertexID) {
	VS_OUTPUT o;
	// Triangle strip: 0=(0,0), 1=(1,0), 2=(0,1), 3=(1,1)
	o.uv = float2(id & 1, id >> 1);
	o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
	return o;
}
)";

// SBS pixel shader: pass-through (identity) so all modes go through the same path
static const char *ps_sbs_source = R"(
Texture2D atlas_tex : register(t0);
SamplerState samp : register(s0);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
	return atlas_tex.Sample(samp, uv);
}
)";

// Anaglyph pixel shader: tiled atlas texture, left=red, right=green+blue
static const char *ps_anaglyph_source = R"(
Texture2D atlas_tex : register(t0);
SamplerState samp : register(s0);

cbuffer TileParams : register(b0) {
	float tile_cols_inv;
	float tile_rows_inv;
	float tile_cols;
	float tile_rows;
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
	float2 left_uv  = float2(uv.x * tile_cols_inv, uv.y * tile_rows_inv);
	uint col1 = 1 % (uint)tile_cols;
	uint row1 = 1 / (uint)tile_cols;
	float2 right_uv = float2((uv.x + col1) * tile_cols_inv, (uv.y + row1) * tile_rows_inv);
	float4 left  = atlas_tex.Sample(samp, left_uv);
	float4 right = atlas_tex.Sample(samp, right_uv);
	return float4(left.r, right.g, right.b, 1.0);
}
)";

// Squeezed SBS pixel shader: left tile on left half, right tile on right half, no crop
static const char *ps_squeezed_sbs_source = R"(
Texture2D atlas_tex : register(t0);
SamplerState samp : register(s0);

cbuffer TileParams : register(b0) {
	float tile_cols_inv;
	float tile_rows_inv;
	float tile_cols;
	float tile_rows;
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
	float eye_index = (uv.x < 0.5) ? 0.0 : 1.0;
	float eye_u = (uv.x < 0.5) ? (uv.x / 0.5) : ((uv.x - 0.5) / 0.5);
	uint col = (uint)eye_index % (uint)tile_cols;
	uint row = (uint)eye_index / (uint)tile_cols;
	float src_u = (eye_u + col) * tile_cols_inv;
	float src_v = (uv.y + row) * tile_rows_inv;
	return atlas_tex.Sample(samp, float2(src_u, src_v));
}
)";

// Quad pixel shader: 2x2 grid — TL=view0, TR=view1, BL=view2, BR=view3
static const char *ps_quad_source = R"(
Texture2D atlas_tex : register(t0);
SamplerState samp : register(s0);

cbuffer TileParams : register(b0) {
	float tile_cols_inv;
	float tile_rows_inv;
	float tile_cols;
	float tile_rows;
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
	float col_idx = (uv.x < 0.5) ? 0.0 : 1.0;
	float row_idx = (uv.y < 0.5) ? 0.0 : 1.0;
	float view_index = row_idx * 2.0 + col_idx;
	float local_u = frac(uv.x * 2.0);
	float local_v = frac(uv.y * 2.0);
	float col = fmod(view_index, tile_cols);
	float row = floor(view_index / tile_cols);
	float atlas_u = (local_u + col) * tile_cols_inv;
	float atlas_v = (local_v + row) * tile_rows_inv;
	return atlas_tex.Sample(samp, float2(atlas_u, atlas_v));
}
)";

// Blend pixel shader: tiled atlas texture, 50/50 mix
static const char *ps_blend_source = R"(
Texture2D atlas_tex : register(t0);
SamplerState samp : register(s0);

cbuffer TileParams : register(b0) {
	float tile_cols_inv;
	float tile_rows_inv;
	float tile_cols;
	float tile_rows;
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
	float2 left_uv  = float2(uv.x * tile_cols_inv, uv.y * tile_rows_inv);
	uint col1 = 1 % (uint)tile_cols;
	uint row1 = 1 / (uint)tile_cols;
	float2 right_uv = float2((uv.x + col1) * tile_cols_inv, (uv.y + row1) * tile_rows_inv);
	float4 left  = atlas_tex.Sample(samp, left_uv);
	float4 right = atlas_tex.Sample(samp, right_uv);
	return lerp(left, right, 0.5);
}
)";

static const char *ps_passthrough_source = R"(
Texture2D atlas_tex : register(t0);
SamplerState samp : register(s0);

cbuffer TileParams : register(b0) {
	float tile_cols_inv;
	float tile_rows_inv;
	float tile_cols;
	float tile_rows;
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
	float2 atlas_uv = float2(uv.x * tile_cols_inv, uv.y * tile_rows_inv);
	return atlas_tex.Sample(samp, atlas_uv);
}
)";


/*!
 * Implementation struct for the D3D11 simulation display processor.
 */
struct sim_display_processor_d3d11_impl
{
	struct xrt_display_processor_d3d11 base;
	ID3D11VertexShader *vs;
	ID3D11PixelShader *ps_shaders[6]; //!< One per output mode (SBS, anaglyph, blend, squeezed SBS, quad, passthrough)
	ID3D11SamplerState *sampler;
	ID3D11Buffer *tile_cb; //!< Constant buffer for tile parameters

	//! Nominal viewer parameters for faked eye positions.
	float ipd_m;
	float nominal_x_m;
	float nominal_y_m;
	float nominal_z_m;
};

static inline struct sim_display_processor_d3d11_impl *
sim_dp_d3d11(struct xrt_display_processor_d3d11 *xdp)
{
	return reinterpret_cast<struct sim_display_processor_d3d11_impl *>(xdp);
}


/*
 *
 * Fullscreen quad with runtime-switchable pixel shader.
 *
 */

/*!
 * Tile parameter constant buffer layout — must match cbuffer TileParams in HLSL.
 */
struct tile_params_cb
{
	float tile_cols_inv;
	float tile_rows_inv;
	float tile_cols;
	float tile_rows;
};

static void
sim_dp_d3d11_process_atlas(struct xrt_display_processor_d3d11 *xdp,
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

	struct sim_display_processor_d3d11_impl *sdp = sim_dp_d3d11(xdp);
	ID3D11DeviceContext *ctx = static_cast<ID3D11DeviceContext *>(d3d11_context);
	ID3D11ShaderResourceView *srv = static_cast<ID3D11ShaderResourceView *>(atlas_srv);

	if (ctx == nullptr || srv == nullptr) {
		return;
	}

	// Read the current mode (may change at runtime via 1/2/3 keys).
	// Single-view input forces passthrough — 3D shaders need ≥2 views.
	enum sim_display_output_mode mode = sim_display_get_output_mode();
	if (tile_columns * tile_rows <= 1) {
		mode = SIM_DISPLAY_OUTPUT_PASSTHROUGH;
	}
	ID3D11PixelShader *active_ps = sdp->ps_shaders[mode];
	if (active_ps == nullptr) {
		return;
	}

	// Update tile parameter constant buffer.
	// Atlas is guaranteed content-sized by compositor crop-blit.
	struct tile_params_cb tile_data = {};
	tile_data.tile_cols_inv = (tile_columns > 0) ? (1.0f / static_cast<float>(tile_columns)) : 0.5f;
	tile_data.tile_rows_inv = (tile_rows > 0) ? (1.0f / static_cast<float>(tile_rows)) : 1.0f;
	tile_data.tile_cols = static_cast<float>(tile_columns);
	tile_data.tile_rows = static_cast<float>(tile_rows);
	ctx->UpdateSubresource(sdp->tile_cb, 0, nullptr, &tile_data, 0, 0);

	// Set viewport
	D3D11_VIEWPORT viewport = {};
	viewport.TopLeftX = 0.0f;
	viewport.TopLeftY = 0.0f;
	viewport.Width = static_cast<float>(target_width);
	viewport.Height = static_cast<float>(target_height);
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	ctx->RSSetViewports(1, &viewport);

	// Bind shaders, sampler, SRV, and constant buffer
	ctx->VSSetShader(sdp->vs, nullptr, 0);
	ctx->PSSetShader(active_ps, nullptr, 0);
	ctx->PSSetSamplers(0, 1, &sdp->sampler);
	ctx->PSSetShaderResources(0, 1, &srv);
	ctx->PSSetConstantBuffers(0, 1, &sdp->tile_cb);

	// Set topology and draw fullscreen quad (4 vertices, triangle strip)
	ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	ctx->IASetInputLayout(nullptr);
	ctx->Draw(4, 0);

	// Unbind SRV to prevent D3D11 hazard warnings
	ID3D11ShaderResourceView *null_srv = nullptr;
	ctx->PSSetShaderResources(0, 1, &null_srv);
}


static bool
sim_dp_d3d11_get_predicted_eye_positions(struct xrt_display_processor_d3d11 *xdp,
                                          struct xrt_eye_positions *out)
{
	struct sim_display_processor_d3d11_impl *sdp = sim_dp_d3d11(xdp);
	float half_ipd = sdp->ipd_m / 2.0f;
	uint32_t vc = sim_display_get_view_count();

	if (vc == 1) {
		out->eyes[0] = {sdp->nominal_x_m, sdp->nominal_y_m, sdp->nominal_z_m};
		out->count = 1;
	} else if (vc >= 4) {
		out->eyes[0] = {sdp->nominal_x_m - half_ipd, sdp->nominal_y_m - 0.032f, sdp->nominal_z_m};
		out->eyes[1] = {sdp->nominal_x_m + half_ipd, sdp->nominal_y_m - 0.032f, sdp->nominal_z_m};
		out->eyes[2] = {sdp->nominal_x_m - half_ipd, sdp->nominal_y_m + 0.032f, sdp->nominal_z_m};
		out->eyes[3] = {sdp->nominal_x_m + half_ipd, sdp->nominal_y_m + 0.032f, sdp->nominal_z_m};
		out->count = 4;
	} else {
		out->eyes[0] = {sdp->nominal_x_m - half_ipd, sdp->nominal_y_m, sdp->nominal_z_m};
		out->eyes[1] = {sdp->nominal_x_m + half_ipd, sdp->nominal_y_m, sdp->nominal_z_m};
		out->count = 2;
	}
	out->timestamp_ns = os_monotonic_get_ns();
	out->valid = true;
	out->is_tracking = false; // Nominal, not real tracking
	return true;
}

static void
sim_dp_d3d11_destroy(struct xrt_display_processor_d3d11 *xdp)
{
	struct sim_display_processor_d3d11_impl *sdp = sim_dp_d3d11(xdp);

	if (sdp->vs != nullptr) {
		sdp->vs->Release();
	}
	for (int i = 0; i < 6; i++) {
		if (sdp->ps_shaders[i] != nullptr) {
			sdp->ps_shaders[i]->Release();
		}
	}
	if (sdp->sampler != nullptr) {
		sdp->sampler->Release();
	}
	if (sdp->tile_cb != nullptr) {
		sdp->tile_cb->Release();
	}

	free(sdp);
}


/*
 *
 * Helper: compile HLSL shader and create D3D11 shader object.
 *
 */

static HRESULT
compile_shader(const char *source, const char *entry, const char *target, ID3DBlob **out_blob)
{
	ID3DBlob *error_blob = nullptr;
	HRESULT hr = D3DCompile(source, strlen(source), nullptr, nullptr, nullptr, entry, target, 0, 0, out_blob,
	                        &error_blob);
	if (FAILED(hr)) {
		if (error_blob != nullptr) {
			U_LOG_E("sim_display D3D11: shader compile error: %s",
			        static_cast<const char *>(error_blob->GetBufferPointer()));
			error_blob->Release();
		}
	}
	return hr;
}


/*
 *
 * Exported creation function.
 *
 */

extern "C" xrt_result_t
sim_display_processor_d3d11_create(enum sim_display_output_mode mode,
                                   void *d3d11_device,
                                   struct xrt_display_processor_d3d11 **out_xdp)
{
	if (out_xdp == nullptr) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	if (d3d11_device == nullptr) {
		U_LOG_E("sim_display D3D11: device required for display processor");
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	struct sim_display_processor_d3d11_impl *sdp =
	    static_cast<struct sim_display_processor_d3d11_impl *>(calloc(1, sizeof(*sdp)));
	if (sdp == nullptr) {
		return XRT_ERROR_ALLOCATION;
	}

	sdp->base.destroy = sim_dp_d3d11_destroy;
	sdp->base.process_atlas = sim_dp_d3d11_process_atlas;
	sdp->base.get_predicted_eye_positions = sim_dp_d3d11_get_predicted_eye_positions;

	// Nominal viewer parameters (same defaults as sim_display_hmd_create)
	sdp->ipd_m = 0.06f;
	sdp->nominal_x_m = 0.0f;
	sdp->nominal_y_m = 0.1f;
	sdp->nominal_z_m = debug_get_float_option_sim_display_nominal_z_m_d3d11();

	ID3D11Device *device = static_cast<ID3D11Device *>(d3d11_device);

	// Compile shared vertex shader
	ID3DBlob *vs_blob = nullptr;
	HRESULT hr = compile_shader(vs_source, "main", "vs_5_0", &vs_blob);
	if (FAILED(hr)) {
		U_LOG_E("sim_display D3D11: failed to compile vertex shader");
		free(sdp);
		return XRT_ERROR_VULKAN;
	}

	hr = device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &sdp->vs);
	vs_blob->Release();
	if (FAILED(hr)) {
		U_LOG_E("sim_display D3D11: failed to create vertex shader");
		free(sdp);
		return XRT_ERROR_VULKAN;
	}

	// Compile all 6 pixel shaders so runtime switching is instant
	const char *ps_sources[6] = {ps_sbs_source, ps_anaglyph_source, ps_blend_source, ps_squeezed_sbs_source, ps_quad_source, ps_passthrough_source};
	const char *ps_names[6] = {"SBS", "Anaglyph", "Blend", "Squeezed SBS", "Quad", "Passthrough"};

	for (int i = 0; i < 6; i++) {
		ID3DBlob *ps_blob = nullptr;
		hr = compile_shader(ps_sources[i], "main", "ps_5_0", &ps_blob);
		if (FAILED(hr)) {
			U_LOG_E("sim_display D3D11: failed to compile %s pixel shader", ps_names[i]);
			sim_dp_d3d11_destroy(&sdp->base);
			return XRT_ERROR_VULKAN;
		}

		hr = device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr,
		                               &sdp->ps_shaders[i]);
		ps_blob->Release();
		if (FAILED(hr)) {
			U_LOG_E("sim_display D3D11: failed to create %s pixel shader", ps_names[i]);
			sim_dp_d3d11_destroy(&sdp->base);
			return XRT_ERROR_VULKAN;
		}
	}

	// Create sampler state (linear, clamp)
	D3D11_SAMPLER_DESC sampler_desc = {};
	sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;

	hr = device->CreateSamplerState(&sampler_desc, &sdp->sampler);
	if (FAILED(hr)) {
		U_LOG_E("sim_display D3D11: failed to create sampler state");
		sim_dp_d3d11_destroy(&sdp->base);
		return XRT_ERROR_VULKAN;
	}

	// Create constant buffer for tile parameters (16-byte aligned)
	D3D11_BUFFER_DESC cb_desc = {};
	cb_desc.ByteWidth = sizeof(struct tile_params_cb);
	cb_desc.Usage = D3D11_USAGE_DEFAULT;
	cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

	hr = device->CreateBuffer(&cb_desc, nullptr, &sdp->tile_cb);
	if (FAILED(hr)) {
		U_LOG_E("sim_display D3D11: failed to create tile constant buffer");
		sim_dp_d3d11_destroy(&sdp->base);
		return XRT_ERROR_VULKAN;
	}

	// Set the initial output mode (atomic global read by process_atlas each frame)
	sim_display_set_output_mode(mode);

	U_LOG_W("Created sim display D3D11 processor (all 6 shaders), initial mode: %s",
	        mode == SIM_DISPLAY_OUTPUT_SBS           ? "SBS" :
	        mode == SIM_DISPLAY_OUTPUT_ANAGLYPH       ? "Anaglyph" :
	        mode == SIM_DISPLAY_OUTPUT_SQUEEZED_SBS   ? "Squeezed SBS" :
	        mode == SIM_DISPLAY_OUTPUT_QUAD           ? "Quad" :
	        mode == SIM_DISPLAY_OUTPUT_PASSTHROUGH    ? "Passthrough" : "Blend");

	*out_xdp = &sdp->base;
	return XRT_SUCCESS;
}


/*
 *
 * Factory function — matches xrt_dp_factory_d3d11_fn_t signature.
 *
 */

extern "C" xrt_result_t
sim_display_dp_factory_d3d11(void *d3d11_device,
                              void *d3d11_context,
                              void *window_handle,
                              struct xrt_display_processor_d3d11 **out_xdp)
{
	(void)d3d11_context;
	(void)window_handle;

	enum sim_display_output_mode mode = sim_display_get_output_mode();

	return sim_display_processor_d3d11_create(mode, d3d11_device, out_xdp);
}
