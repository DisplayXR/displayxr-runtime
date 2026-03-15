// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Simulation D3D12 display processor: SBS, anaglyph, alpha-blend output.
 *
 * Implements SBS, anaglyph, alpha-blend, squeezed SBS, and quad atlas output
 * modes using HLSL shaders compiled at runtime via D3DCompile. All 5 PSOs are
 * pre-compiled at init for instant runtime switching via 1/2/3/4/5 keys.
 *
 * Unlike the D3D11 variant, this records draw commands onto a provided
 * command list rather than executing immediately.
 *
 * @author David Fattal
 * @ingroup drv_sim_display
 */

#include "sim_display_interface.h"

#include "xrt/xrt_display_processor_d3d12.h"
#include "xrt/xrt_display_metrics.h"

#include "util/u_debug.h"
#include "util/u_logging.h"
#include "os/os_time.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>

#include <cstdlib>
#include <cstring>

DEBUG_GET_ONCE_FLOAT_OPTION(sim_display_nominal_z_m_d3d12, "SIM_DISPLAY_NOMINAL_Z_M", 0.60f)


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
	float2 left_uv = float2(uv.x * tile_cols_inv, uv.y * tile_rows_inv);
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
	float2 left_uv = float2(uv.x * tile_cols_inv, uv.y * tile_rows_inv);
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
 * Implementation struct for the D3D12 simulation display processor.
 */
struct sim_display_processor_d3d12_impl
{
	struct xrt_display_processor_d3d12 base;
	ID3D12RootSignature *root_signature;
	ID3D12PipelineState *psos[6]; //!< One per output mode (SBS, anaglyph, blend, squeezed SBS, quad, passthrough)

	//! Nominal viewer parameters for faked eye positions.
	float ipd_m;
	float nominal_x_m;
	float nominal_y_m;
	float nominal_z_m;
};

static inline struct sim_display_processor_d3d12_impl *
sim_dp_d3d12(struct xrt_display_processor_d3d12 *xdp)
{
	return reinterpret_cast<struct sim_display_processor_d3d12_impl *>(xdp);
}


/*
 *
 * Fullscreen quad with runtime-switchable PSO.
 *
 */

static void
sim_dp_d3d12_process_atlas(struct xrt_display_processor_d3d12 *xdp,
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
	struct sim_display_processor_d3d12_impl *sdp = sim_dp_d3d12(xdp);
	ID3D12GraphicsCommandList *cmd_list = static_cast<ID3D12GraphicsCommandList *>(d3d12_command_list);
	ID3D12Resource *atlas_res = static_cast<ID3D12Resource *>(atlas_texture_resource);

	if (cmd_list == nullptr) {
		return;
	}

	// Read the current mode (may change at runtime via 1/2/3 keys)
	enum sim_display_output_mode mode = sim_display_get_output_mode();
	ID3D12PipelineState *active_pso = sdp->psos[mode];
	if (active_pso == nullptr) {
		return;
	}

	// Set root signature and PSO
	cmd_list->SetGraphicsRootSignature(sdp->root_signature);
	cmd_list->SetPipelineState(active_pso);

	// Set render target
	D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle;
	rtv_handle.ptr = static_cast<SIZE_T>(target_rtv_cpu_handle);
	cmd_list->OMSetRenderTargets(1, &rtv_handle, FALSE, nullptr);

	// Set viewport
	D3D12_VIEWPORT viewport = {};
	viewport.TopLeftX = 0.0f;
	viewport.TopLeftY = 0.0f;
	viewport.Width = static_cast<float>(target_width);
	viewport.Height = static_cast<float>(target_height);
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	cmd_list->RSSetViewports(1, &viewport);

	// Set scissor rect
	D3D12_RECT scissor = {};
	scissor.left = 0;
	scissor.top = 0;
	scissor.right = static_cast<LONG>(target_width);
	scissor.bottom = static_cast<LONG>(target_height);
	cmd_list->RSSetScissorRects(1, &scissor);

	// Set SRV via root descriptor table
	D3D12_GPU_DESCRIPTOR_HANDLE srv_handle;
	srv_handle.ptr = atlas_srv_gpu_handle;
	cmd_list->SetGraphicsRootDescriptorTable(0, srv_handle);

	// Compute UV scale from view/atlas dimensions (not 1/tile_columns)
	// so mapping is correct when atlas is larger than the tiled region.
	uint32_t atlas_w = tile_columns * view_width;
	uint32_t atlas_h = tile_rows * view_height;
	if (atlas_res != nullptr) {
		D3D12_RESOURCE_DESC desc = atlas_res->GetDesc();
		atlas_w = static_cast<uint32_t>(desc.Width);
		atlas_h = static_cast<uint32_t>(desc.Height);
	}
	float tile_cols_inv = (atlas_w > 0) ? (static_cast<float>(view_width) / static_cast<float>(atlas_w)) : 0.5f;
	float tile_rows_inv = (atlas_h > 0) ? (static_cast<float>(view_height) / static_cast<float>(atlas_h)) : 1.0f;
	float tile_cols_f = static_cast<float>(tile_columns);
	float tile_rows_f = static_cast<float>(tile_rows);
	uint32_t tile_constants[4];
	memcpy(&tile_constants[0], &tile_cols_inv, sizeof(float));
	memcpy(&tile_constants[1], &tile_rows_inv, sizeof(float));
	memcpy(&tile_constants[2], &tile_cols_f, sizeof(float));
	memcpy(&tile_constants[3], &tile_rows_f, sizeof(float));
	cmd_list->SetGraphicsRoot32BitConstants(1, 4, tile_constants, 0);

	// Draw fullscreen quad (4 vertices, triangle strip)
	cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	cmd_list->IASetVertexBuffers(0, 0, nullptr);
	cmd_list->DrawInstanced(4, 1, 0, 0);
}


static bool
sim_dp_d3d12_get_predicted_eye_positions(struct xrt_display_processor_d3d12 *xdp,
                                          struct xrt_eye_positions *out)
{
	struct sim_display_processor_d3d12_impl *sdp = sim_dp_d3d12(xdp);
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
sim_dp_d3d12_destroy(struct xrt_display_processor_d3d12 *xdp)
{
	struct sim_display_processor_d3d12_impl *sdp = sim_dp_d3d12(xdp);

	if (sdp->root_signature != nullptr) {
		sdp->root_signature->Release();
	}
	for (int i = 0; i < 6; i++) {
		if (sdp->psos[i] != nullptr) {
			sdp->psos[i]->Release();
		}
	}

	free(sdp);
}


/*
 *
 * Helper: compile HLSL shader.
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
			U_LOG_E("sim_display D3D12: shader compile error: %s",
			        static_cast<const char *>(error_blob->GetBufferPointer()));
			error_blob->Release();
		}
	}
	return hr;
}


/*
 *
 * Helper: create root signature with 1 SRV descriptor table + 2 root constants + 1 static sampler.
 *
 */

static HRESULT
create_root_signature(ID3D12Device *device, ID3D12RootSignature **out_rs)
{
	// Descriptor range: 1 SRV at register(t0)
	D3D12_DESCRIPTOR_RANGE srv_range = {};
	srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	srv_range.NumDescriptors = 1;
	srv_range.BaseShaderRegister = 0;
	srv_range.RegisterSpace = 0;
	srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

	// Root parameter 0: 1 descriptor table (SRV)
	D3D12_ROOT_PARAMETER root_params[2] = {};
	root_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	root_params[0].DescriptorTable.NumDescriptorRanges = 1;
	root_params[0].DescriptorTable.pDescriptorRanges = &srv_range;
	root_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	// Root parameter 1: 4 x 32-bit constants (tile_cols_inv, tile_rows_inv, tile_cols, tile_rows) -> cbuffer b0
	root_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	root_params[1].Constants.ShaderRegister = 0;
	root_params[1].Constants.RegisterSpace = 0;
	root_params[1].Constants.Num32BitValues = 4;
	root_params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	// Static sampler: linear, clamp (register s0)
	D3D12_STATIC_SAMPLER_DESC sampler = {};
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.MipLODBias = 0.0f;
	sampler.MaxAnisotropy = 0;
	sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
	sampler.MinLOD = 0.0f;
	sampler.MaxLOD = D3D12_FLOAT32_MAX;
	sampler.ShaderRegister = 0;
	sampler.RegisterSpace = 0;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_ROOT_SIGNATURE_DESC rs_desc = {};
	rs_desc.NumParameters = 2;
	rs_desc.pParameters = root_params;
	rs_desc.NumStaticSamplers = 1;
	rs_desc.pStaticSamplers = &sampler;
	rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

	ID3DBlob *sig_blob = nullptr;
	ID3DBlob *error_blob = nullptr;
	HRESULT hr = D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1,
	                                          &sig_blob, &error_blob);
	if (FAILED(hr)) {
		if (error_blob != nullptr) {
			U_LOG_E("sim_display D3D12: root signature serialize error: %s",
			        static_cast<const char *>(error_blob->GetBufferPointer()));
			error_blob->Release();
		}
		return hr;
	}

	hr = device->CreateRootSignature(0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(),
	                                  __uuidof(ID3D12RootSignature), reinterpret_cast<void **>(out_rs));
	sig_blob->Release();
	return hr;
}


/*
 *
 * Exported creation function.
 *
 */

extern "C" xrt_result_t
sim_display_processor_d3d12_create(enum sim_display_output_mode mode,
                                   void *d3d12_device,
                                   struct xrt_display_processor_d3d12 **out_xdp)
{
	if (out_xdp == nullptr) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	if (d3d12_device == nullptr) {
		U_LOG_E("sim_display D3D12: device required for display processor");
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	struct sim_display_processor_d3d12_impl *sdp =
	    static_cast<struct sim_display_processor_d3d12_impl *>(calloc(1, sizeof(*sdp)));
	if (sdp == nullptr) {
		return XRT_ERROR_ALLOCATION;
	}

	sdp->base.destroy = sim_dp_d3d12_destroy;
	sdp->base.process_atlas = sim_dp_d3d12_process_atlas;
	sdp->base.get_predicted_eye_positions = sim_dp_d3d12_get_predicted_eye_positions;

	// Nominal viewer parameters (same defaults as sim_display_hmd_create)
	sdp->ipd_m = 0.06f;
	sdp->nominal_x_m = 0.0f;
	sdp->nominal_y_m = 0.1f;
	sdp->nominal_z_m = debug_get_float_option_sim_display_nominal_z_m_d3d12();

	ID3D12Device *device = static_cast<ID3D12Device *>(d3d12_device);

	// Create root signature
	HRESULT hr = create_root_signature(device, &sdp->root_signature);
	if (FAILED(hr)) {
		U_LOG_E("sim_display D3D12: failed to create root signature: 0x%08x", hr);
		free(sdp);
		return XRT_ERROR_D3D;
	}

	// Compile shared vertex shader
	ID3DBlob *vs_blob = nullptr;
	hr = compile_shader(vs_source, "main", "vs_5_0", &vs_blob);
	if (FAILED(hr)) {
		U_LOG_E("sim_display D3D12: failed to compile vertex shader");
		sim_dp_d3d12_destroy(&sdp->base);
		return XRT_ERROR_D3D;
	}

	// Compile all 6 pixel shaders and create PSOs
	const char *ps_sources[6] = {ps_sbs_source, ps_anaglyph_source, ps_blend_source, ps_squeezed_sbs_source, ps_quad_source, ps_passthrough_source};
	const char *ps_names[6] = {"SBS", "Anaglyph", "Blend", "Squeezed SBS", "Quad", "Passthrough"};

	for (int i = 0; i < 6; i++) {
		ID3DBlob *ps_blob = nullptr;
		hr = compile_shader(ps_sources[i], "main", "ps_5_0", &ps_blob);
		if (FAILED(hr)) {
			U_LOG_E("sim_display D3D12: failed to compile %s pixel shader", ps_names[i]);
			vs_blob->Release();
			sim_dp_d3d12_destroy(&sdp->base);
			return XRT_ERROR_D3D;
		}

		// Create PSO
		D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
		pso_desc.pRootSignature = sdp->root_signature;
		pso_desc.VS.pShaderBytecode = vs_blob->GetBufferPointer();
		pso_desc.VS.BytecodeLength = vs_blob->GetBufferSize();
		pso_desc.PS.pShaderBytecode = ps_blob->GetBufferPointer();
		pso_desc.PS.BytecodeLength = ps_blob->GetBufferSize();

		// Blend state: no blending
		pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

		// Rasterizer state: no culling
		pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		pso_desc.RasterizerState.DepthClipEnable = TRUE;

		pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		pso_desc.NumRenderTargets = 1;
		pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
		pso_desc.SampleDesc.Count = 1;
		pso_desc.SampleMask = UINT_MAX;

		hr = device->CreateGraphicsPipelineState(&pso_desc, __uuidof(ID3D12PipelineState),
		                                          reinterpret_cast<void **>(&sdp->psos[i]));
		ps_blob->Release();
		if (FAILED(hr)) {
			U_LOG_E("sim_display D3D12: failed to create %s PSO: 0x%08x", ps_names[i], hr);
			vs_blob->Release();
			sim_dp_d3d12_destroy(&sdp->base);
			return XRT_ERROR_D3D;
		}
	}

	vs_blob->Release();

	// Set the initial output mode
	sim_display_set_output_mode(mode);

	U_LOG_W("Created sim display D3D12 processor (all 6 PSOs), initial mode: %s",
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
 * Factory function — matches xrt_dp_factory_d3d12_fn_t signature.
 *
 */

extern "C" xrt_result_t
sim_display_dp_factory_d3d12(void *d3d12_device,
                              void *d3d12_command_queue,
                              void *window_handle,
                              struct xrt_display_processor_d3d12 **out_xdp)
{
	(void)d3d12_command_queue;
	(void)window_handle;

	enum sim_display_output_mode mode = sim_display_get_output_mode();

	return sim_display_processor_d3d12_create(mode, d3d12_device, out_xdp);
}
