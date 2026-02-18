// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Simulation D3D11 display processor: SBS, anaglyph, alpha-blend output.
 *
 * Implements SBS, anaglyph, and alpha-blend stereo output modes using HLSL
 * shaders compiled at runtime via D3DCompile. All 3 shaders are pre-compiled
 * at init for instant runtime switching via 1/2/3 keys.
 *
 * @author David Fattal
 * @ingroup drv_sim_display
 */

#include "sim_display_interface.h"

#include "xrt/xrt_display_processor_d3d11.h"

#include "util/u_logging.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <cstdlib>
#include <cstring>


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
Texture2D stereo_tex : register(t0);
SamplerState samp : register(s0);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
	return stereo_tex.Sample(samp, uv);
}
)";

// Anaglyph pixel shader: SBS texture, left=red, right=green+blue
static const char *ps_anaglyph_source = R"(
Texture2D stereo_tex : register(t0);
SamplerState samp : register(s0);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
	float4 left  = stereo_tex.Sample(samp, float2(uv.x * 0.5, uv.y));
	float4 right = stereo_tex.Sample(samp, float2(uv.x * 0.5 + 0.5, uv.y));
	return float4(left.r, right.g, right.b, 1.0);
}
)";

// Blend pixel shader: SBS texture, 50/50 mix
static const char *ps_blend_source = R"(
Texture2D stereo_tex : register(t0);
SamplerState samp : register(s0);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
	float4 left  = stereo_tex.Sample(samp, float2(uv.x * 0.5, uv.y));
	float4 right = stereo_tex.Sample(samp, float2(uv.x * 0.5 + 0.5, uv.y));
	return lerp(left, right, 0.5);
}
)";


/*!
 * Implementation struct for the D3D11 simulation display processor.
 */
struct sim_display_processor_d3d11_impl
{
	struct xrt_display_processor_d3d11 base;
	ID3D11VertexShader *vs;
	ID3D11PixelShader *ps_shaders[3]; //!< One per output mode (SBS, anaglyph, blend)
	ID3D11SamplerState *sampler;
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

static void
sim_dp_d3d11_process_stereo(struct xrt_display_processor_d3d11 *xdp,
                             void *d3d11_context,
                             void *stereo_srv,
                             uint32_t view_width,
                             uint32_t view_height,
                             uint32_t format,
                             uint32_t target_width,
                             uint32_t target_height)
{
	struct sim_display_processor_d3d11_impl *sdp = sim_dp_d3d11(xdp);
	ID3D11DeviceContext *ctx = static_cast<ID3D11DeviceContext *>(d3d11_context);
	ID3D11ShaderResourceView *srv = static_cast<ID3D11ShaderResourceView *>(stereo_srv);

	if (ctx == nullptr || srv == nullptr) {
		return;
	}

	// Read the current mode (may change at runtime via 1/2/3 keys)
	enum sim_display_output_mode mode = sim_display_get_output_mode();
	ID3D11PixelShader *active_ps = sdp->ps_shaders[mode];
	if (active_ps == nullptr) {
		return;
	}

	// Set viewport
	D3D11_VIEWPORT viewport = {};
	viewport.TopLeftX = 0.0f;
	viewport.TopLeftY = 0.0f;
	viewport.Width = static_cast<float>(target_width);
	viewport.Height = static_cast<float>(target_height);
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	ctx->RSSetViewports(1, &viewport);

	// Bind shaders, sampler, and SRV
	ctx->VSSetShader(sdp->vs, nullptr, 0);
	ctx->PSSetShader(active_ps, nullptr, 0);
	ctx->PSSetSamplers(0, 1, &sdp->sampler);
	ctx->PSSetShaderResources(0, 1, &srv);

	// Set topology and draw fullscreen quad (4 vertices, triangle strip)
	ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	ctx->IASetInputLayout(nullptr);
	ctx->Draw(4, 0);

	// Unbind SRV to prevent D3D11 hazard warnings
	ID3D11ShaderResourceView *null_srv = nullptr;
	ctx->PSSetShaderResources(0, 1, &null_srv);
}


static void
sim_dp_d3d11_destroy(struct xrt_display_processor_d3d11 *xdp)
{
	struct sim_display_processor_d3d11_impl *sdp = sim_dp_d3d11(xdp);

	if (sdp->vs != nullptr) {
		sdp->vs->Release();
	}
	for (int i = 0; i < 3; i++) {
		if (sdp->ps_shaders[i] != nullptr) {
			sdp->ps_shaders[i]->Release();
		}
	}
	if (sdp->sampler != nullptr) {
		sdp->sampler->Release();
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
	sdp->base.process_stereo = sim_dp_d3d11_process_stereo;

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

	// Compile all 3 pixel shaders so runtime switching is instant
	const char *ps_sources[3] = {ps_sbs_source, ps_anaglyph_source, ps_blend_source};
	const char *ps_names[3] = {"SBS", "Anaglyph", "Blend"};

	for (int i = 0; i < 3; i++) {
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

	// Set the initial output mode (atomic global read by process_stereo each frame)
	sim_display_set_output_mode(mode);

	U_LOG_W("Created sim display D3D11 processor (all 3 shaders), initial mode: %s",
	        mode == SIM_DISPLAY_OUTPUT_SBS       ? "SBS" :
	        mode == SIM_DISPLAY_OUTPUT_ANAGLYPH   ? "Anaglyph" : "Blend");

	*out_xdp = &sdp->base;
	return XRT_SUCCESS;
}
