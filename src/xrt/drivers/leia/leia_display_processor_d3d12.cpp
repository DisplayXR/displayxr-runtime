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
 * The SR SDK weaver expects side-by-side (SBS) stereo input. The Leia
 * device defines its 3D mode as tile_columns=2, tile_rows=1, so the
 * compositor always delivers SBS. The compositor crop-blit guarantees
 * the atlas texture dimensions match exactly 2*view_width x view_height.
 *
 * @author David Fattal
 * @ingroup drv_leia
 */

#include "leia_display_processor_d3d12.h"
#include "leia_sr_d3d12.h"

#include "xrt/xrt_display_metrics.h"
#include "util/u_logging.h"

#include <d3d12.h>
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
 * Implementation struct wrapping leiasr_d3d12 as xrt_display_processor_d3d12.
 */
struct leia_display_processor_d3d12_impl
{
	struct xrt_display_processor_d3d12 base;
	struct leiasr_d3d12 *leiasr; //!< Owned — destroyed in leia_dp_d3d12_destroy.

	ID3D12Device *device;              //!< Cached device reference (not owned, for blit init).

	//! @name 2D blit pipeline resources (passthrough stretch-blit)
	//! @{
	ID3D12RootSignature *blit_root_sig;
	ID3D12PipelineState *blit_pso;
	ID3D12DescriptorHeap *blit_srv_heap; //!< Shader-visible, 1 SRV
	DXGI_FORMAT blit_output_format;
	//! @}

	uint32_t view_count; //!< Active mode view count (1=2D, 2=stereo).
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
                             void *target_resource,
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
	struct leia_display_processor_d3d12_impl *ldp = leia_dp_d3d12(xdp);

	// Compute effective viewport: canvas sub-rect when set, else full target.
	// The SR SDK weaver uses viewport offset in its phase calculation:
	//   xOffset = window_WeavingX + vpX
	//   yOffset = window_WeavingY + vpY
	int32_t vp_x = 0;
	int32_t vp_y = 0;
	uint32_t vp_w = target_width;
	uint32_t vp_h = target_height;
	if (canvas_width > 0 && canvas_height > 0) {
		vp_x = canvas_offset_x;
		vp_y = canvas_offset_y;
		vp_w = canvas_width;
		vp_h = canvas_height;
	}

	// 2D mode: passthrough stretch-blit (first tile fills target)
	if (ldp->view_count == 1) {
		if (ldp->blit_pso == NULL || ldp->blit_root_sig == NULL ||
		    ldp->blit_srv_heap == NULL || atlas_texture_resource == NULL) {
			return;
		}

		ID3D12GraphicsCommandList *cmd = static_cast<ID3D12GraphicsCommandList *>(d3d12_command_list);
		ID3D12Resource *atlas_res = static_cast<ID3D12Resource *>(atlas_texture_resource);

		// Create SRV for the atlas resource in our shader-visible heap
		D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
		srv_desc.Format = static_cast<DXGI_FORMAT>(format);
		srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv_desc.Texture2D.MipLevels = 1;
		ldp->device->CreateShaderResourceView(
		    atlas_res, &srv_desc,
		    ldp->blit_srv_heap->GetCPUDescriptorHandleForHeapStart());

		// Set descriptor heap, root sig, PSO
		ID3D12DescriptorHeap *heaps[] = {ldp->blit_srv_heap};
		cmd->SetDescriptorHeaps(1, heaps);
		cmd->SetGraphicsRootSignature(ldp->blit_root_sig);
		cmd->SetPipelineState(ldp->blit_pso);

		// Set render target
		D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle;
		rtv_handle.ptr = static_cast<SIZE_T>(target_rtv_cpu_handle);
		cmd->OMSetRenderTargets(1, &rtv_handle, FALSE, nullptr);

		// Set viewport and scissor to canvas sub-rect (or full target)
		D3D12_VIEWPORT viewport = {};
		viewport.TopLeftX = static_cast<float>(vp_x);
		viewport.TopLeftY = static_cast<float>(vp_y);
		viewport.Width = static_cast<float>(vp_w);
		viewport.Height = static_cast<float>(vp_h);
		viewport.MaxDepth = 1.0f;
		cmd->RSSetViewports(1, &viewport);

		D3D12_RECT scissor = {};
		scissor.left = static_cast<LONG>(vp_x);
		scissor.top = static_cast<LONG>(vp_y);
		scissor.right = static_cast<LONG>(vp_x) + static_cast<LONG>(vp_w);
		scissor.bottom = static_cast<LONG>(vp_y) + static_cast<LONG>(vp_h);
		cmd->RSSetScissorRects(1, &scissor);

		// Set SRV descriptor table
		cmd->SetGraphicsRootDescriptorTable(
		    0, ldp->blit_srv_heap->GetGPUDescriptorHandleForHeapStart());

		// Atlas is guaranteed content-sized by compositor crop-blit.
		// In 2D mode, content occupies min(target, atlas) of the atlas.
		uint32_t atlas_w = tile_columns * view_width;
		uint32_t atlas_h = tile_rows * view_height;
		uint32_t content_w = (target_width < atlas_w) ? target_width : atlas_w;
		uint32_t content_h = (target_height < atlas_h) ? target_height : atlas_h;
		float u_scale = (atlas_w > 0) ? (float)content_w / (float)atlas_w : 1.0f;
		float v_scale = (atlas_h > 0) ? (float)content_h / (float)atlas_h : 1.0f;
		uint32_t constants[4];
		memcpy(&constants[0], &u_scale, sizeof(float));
		memcpy(&constants[1], &v_scale, sizeof(float));
		constants[2] = 0;
		constants[3] = 0;
		cmd->SetGraphicsRoot32BitConstants(1, 4, constants, 0);

		// Draw fullscreen quad
		cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		cmd->IASetVertexBuffers(0, 0, nullptr);
		cmd->DrawInstanced(4, 1, 0, 0);
		return;
	}

	(void)atlas_srv_gpu_handle;
	(void)target_rtv_cpu_handle;

	// Atlas is guaranteed content-sized SBS (2*view_width x view_height)
	// by compositor crop-blit. Pass directly to weaver.
	if (atlas_texture_resource != NULL) {
		leiasr_d3d12_set_input_texture(ldp->leiasr, atlas_texture_resource,
		                               view_width, view_height, format);
	}

	// vp_x/vp_y/vp_w/vp_h carry the canvas sub-rect. leiasr_d3d12_weave
	// applies them via RSSetViewports/RSSetScissorRects on the cmd list —
	// the weaver's setViewport/setScissorRect alone do NOT scope the draw.
	// See gotcha at leiasr_d3d12_weave().
	leiasr_d3d12_weave(ldp->leiasr, d3d12_command_list, vp_x, vp_y, vp_w, vp_h);
}

static void
leia_dp_d3d12_ensure_blit_pso(struct leia_display_processor_d3d12_impl *ldp, DXGI_FORMAT fmt)
{
	if (ldp->blit_root_sig == NULL || ldp->device == NULL) {
		return;
	}
	if (ldp->blit_pso != NULL && ldp->blit_output_format == fmt) {
		return;
	}

	if (ldp->blit_pso != NULL) {
		ldp->blit_pso->Release();
		ldp->blit_pso = NULL;
	}

	// Compile shaders
	ID3DBlob *vs_blob = NULL;
	ID3DBlob *ps_blob = NULL;
	ID3DBlob *error_blob = NULL;

	HRESULT hr = D3DCompile(blit_vs_source, strlen(blit_vs_source), NULL, NULL, NULL,
	                        "main", "vs_5_0", 0, 0, &vs_blob, &error_blob);
	if (FAILED(hr)) {
		if (error_blob) { error_blob->Release(); }
		U_LOG_E("Leia D3D12 DP: blit VS compile failed: 0x%08x", (unsigned)hr);
		return;
	}

	hr = D3DCompile(blit_ps_source, strlen(blit_ps_source), NULL, NULL, NULL,
	                "main", "ps_5_0", 0, 0, &ps_blob, &error_blob);
	if (FAILED(hr)) {
		vs_blob->Release();
		if (error_blob) { error_blob->Release(); }
		U_LOG_E("Leia D3D12 DP: blit PS compile failed: 0x%08x", (unsigned)hr);
		return;
	}

	D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
	pso_desc.pRootSignature = ldp->blit_root_sig;
	pso_desc.VS.pShaderBytecode = vs_blob->GetBufferPointer();
	pso_desc.VS.BytecodeLength = vs_blob->GetBufferSize();
	pso_desc.PS.pShaderBytecode = ps_blob->GetBufferPointer();
	pso_desc.PS.BytecodeLength = ps_blob->GetBufferSize();
	pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	pso_desc.RasterizerState.DepthClipEnable = TRUE;
	pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pso_desc.NumRenderTargets = 1;
	pso_desc.RTVFormats[0] = fmt;
	pso_desc.SampleDesc.Count = 1;
	pso_desc.SampleMask = UINT_MAX;

	hr = ldp->device->CreateGraphicsPipelineState(
	    &pso_desc, __uuidof(ID3D12PipelineState),
	    reinterpret_cast<void **>(&ldp->blit_pso));

	vs_blob->Release();
	ps_blob->Release();

	if (FAILED(hr)) {
		U_LOG_E("Leia D3D12 DP: blit PSO creation failed: 0x%08x", (unsigned)hr);
		return;
	}

	ldp->blit_output_format = fmt;
	U_LOG_I("Leia D3D12 DP: created 2D blit PSO for format %u", (unsigned)fmt);
}

static void
leia_dp_d3d12_set_output_format(struct xrt_display_processor_d3d12 *xdp, uint32_t format)
{
	struct leia_display_processor_d3d12_impl *ldp = leia_dp_d3d12(xdp);
	leiasr_d3d12_set_output_format(ldp->leiasr, format);

	// Create/recreate blit PSO to match the output format
	leia_dp_d3d12_ensure_blit_pso(ldp, static_cast<DXGI_FORMAT>(format));
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
leia_dp_d3d12_get_hardware_3d_state(struct xrt_display_processor_d3d12 *xdp, bool *out_is_3d)
{
	struct leia_display_processor_d3d12_impl *ldp = leia_dp_d3d12(xdp);
	return leiasr_d3d12_get_hardware_3d_state(ldp->leiasr, out_is_3d);
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

	if (ldp->blit_pso != NULL) {
		ldp->blit_pso->Release();
	}
	if (ldp->blit_root_sig != NULL) {
		ldp->blit_root_sig->Release();
	}
	if (ldp->blit_srv_heap != NULL) {
		ldp->blit_srv_heap->Release();
	}

	if (ldp->leiasr != NULL) {
		leiasr_d3d12_destroy(&ldp->leiasr);
	}
	free(ldp);
}


/*
 *
 * Helper: create blit root signature and SRV heap for 2D passthrough mode.
 *
 */

static bool
leia_dp_d3d12_init_blit(struct leia_display_processor_d3d12_impl *ldp)
{
	if (ldp->device == NULL) {
		return false;
	}

	// Create root signature: 1 SRV descriptor table (t0) + 4 root constants (b0) + 1 static sampler (s0)
	D3D12_DESCRIPTOR_RANGE srv_range = {};
	srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	srv_range.NumDescriptors = 1;
	srv_range.BaseShaderRegister = 0;
	srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

	D3D12_ROOT_PARAMETER root_params[2] = {};
	root_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	root_params[0].DescriptorTable.NumDescriptorRanges = 1;
	root_params[0].DescriptorTable.pDescriptorRanges = &srv_range;
	root_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	root_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	root_params[1].Constants.ShaderRegister = 0;
	root_params[1].Constants.Num32BitValues = 4;
	root_params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_STATIC_SAMPLER_DESC sampler = {};
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
	sampler.MaxLOD = D3D12_FLOAT32_MAX;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_ROOT_SIGNATURE_DESC rs_desc = {};
	rs_desc.NumParameters = 2;
	rs_desc.pParameters = root_params;
	rs_desc.NumStaticSamplers = 1;
	rs_desc.pStaticSamplers = &sampler;
	rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

	ID3DBlob *sig_blob = NULL;
	ID3DBlob *error_blob = NULL;
	HRESULT hr = D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1,
	                                          &sig_blob, &error_blob);
	if (FAILED(hr)) {
		if (error_blob) { error_blob->Release(); }
		U_LOG_E("Leia D3D12 DP: blit root sig serialize failed: 0x%08x", (unsigned)hr);
		return false;
	}

	hr = ldp->device->CreateRootSignature(0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(),
	                                       __uuidof(ID3D12RootSignature),
	                                       reinterpret_cast<void **>(&ldp->blit_root_sig));
	sig_blob->Release();
	if (FAILED(hr)) {
		U_LOG_E("Leia D3D12 DP: blit root sig creation failed: 0x%08x", (unsigned)hr);
		return false;
	}

	// Create shader-visible SRV heap (1 descriptor)
	D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
	heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heap_desc.NumDescriptors = 1;
	heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

	hr = ldp->device->CreateDescriptorHeap(&heap_desc, __uuidof(ID3D12DescriptorHeap),
	                                        reinterpret_cast<void **>(&ldp->blit_srv_heap));
	if (FAILED(hr)) {
		U_LOG_E("Leia D3D12 DP: blit SRV heap creation failed: 0x%08x", (unsigned)hr);
		return false;
	}

	// PSO is created lazily in set_output_format when the format is known
	U_LOG_I("Leia D3D12 DP: initialized 2D blit root signature and SRV heap");
	return true;
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
	ldp->base.get_hardware_3d_state = leia_dp_d3d12_get_hardware_3d_state;
	ldp->base.get_display_dimensions = leia_dp_d3d12_get_display_dimensions;
	ldp->base.get_display_pixel_info = leia_dp_d3d12_get_display_pixel_info;
	ldp->base.destroy = leia_dp_d3d12_destroy;
	ldp->leiasr = weaver;
	ldp->device = static_cast<ID3D12Device *>(d3d12_device);
	ldp->view_count = 2;

	// Init blit root signature and SRV heap for 2D passthrough mode
	if (!leia_dp_d3d12_init_blit(ldp)) {
		U_LOG_W("Leia D3D12 DP: blit init failed — 2D mode will be unavailable");
	}

	*out_xdp = &ldp->base;

	U_LOG_W("Created Leia SR D3D12 display processor (factory, owns weaver)");

	return XRT_SUCCESS;
}
