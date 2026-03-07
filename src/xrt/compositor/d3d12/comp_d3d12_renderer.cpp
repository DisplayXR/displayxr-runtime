// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  D3D12 renderer implementation for layer compositing.
 * @author David Fattal
 * @ingroup comp_d3d12
 */

#include "comp_d3d12_renderer.h"
#include "comp_d3d12_compositor.h"
#include "comp_d3d12_swapchain.h"

#include "util/comp_layer_accum.h"
#include "util/u_logging.h"
#include "math/m_api.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>

// Access compositor internals for the device
extern "C" {
struct comp_d3d12_compositor_internals
{
	struct xrt_compositor_native base;
	struct xrt_device *xdev;
	ID3D12Device *device;
	ID3D12CommandQueue *command_queue;
};
}

static inline struct comp_d3d12_compositor_internals *
get_internals(struct comp_d3d12_compositor *c)
{
	return reinterpret_cast<struct comp_d3d12_compositor_internals *>(c);
}

/*!
 * Blit shader constant: source rect (xy=offset, zw=scale) passed as root constants.
 */
struct BlitConstants
{
	float src_rect[4]; // x_offset, y_offset, x_scale, y_scale
};

// Fullscreen blit vertex shader (generates quad from SV_VertexID)
static const char *blit_vs_source = R"(
cbuffer BlitCB : register(b0) {
	float4 src_rect; // xy=offset, zw=scale
};

struct VS_OUTPUT {
	float4 pos : SV_Position;
	float2 uv  : TEXCOORD0;
};

VS_OUTPUT main(uint id : SV_VertexID) {
	VS_OUTPUT o;
	float2 uv = float2(id & 1, id >> 1);
	o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
	o.uv = uv * src_rect.zw + src_rect.xy;
	return o;
}
)";

// Simple texture sampling pixel shader
static const char *blit_ps_source = R"(
Texture2D tex : register(t0);
SamplerState samp : register(s0);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
	return tex.Sample(samp, uv);
}
)";

/*!
 * D3D12 renderer structure.
 */
struct comp_d3d12_renderer
{
	//! Parent compositor.
	struct comp_d3d12_compositor *c;

	//! Side-by-side stereo texture.
	ID3D12Resource *stereo_texture;

	//! RTV descriptor heap for stereo texture.
	ID3D12DescriptorHeap *rtv_heap;

	//! SRV/CBV descriptor heap (shader visible).
	ID3D12DescriptorHeap *srv_heap;

	//! Root signature for blit operations.
	ID3D12RootSignature *root_signature;

	//! Blit PSO.
	ID3D12PipelineState *blit_pso;

	//! Descriptor sizes.
	uint32_t rtv_descriptor_size;
	uint32_t srv_descriptor_size;

	//! View dimensions.
	uint32_t view_width;
	uint32_t view_height;

	//! Actual stereo texture height (may be max of view_height and target_height).
	uint32_t texture_height;
};


static HRESULT
compile_shader(const char *source, const char *entry, const char *target, ID3DBlob **out_blob)
{
	ID3DBlob *error_blob = nullptr;
	HRESULT hr = D3DCompile(source, strlen(source), nullptr, nullptr, nullptr, entry, target, 0, 0, out_blob,
	                        &error_blob);
	if (FAILED(hr)) {
		if (error_blob != nullptr) {
			U_LOG_E("D3D12 renderer: shader compile error: %s",
			        static_cast<const char *>(error_blob->GetBufferPointer()));
			error_blob->Release();
		}
	}
	return hr;
}


static xrt_result_t
create_stereo_texture(struct comp_d3d12_renderer *r, ID3D12Device *device, uint32_t width, uint32_t height)
{
	D3D12_HEAP_PROPERTIES heap_props = {};
	heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC res_desc = {};
	res_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	res_desc.Width = width * 2; // SBS: two views side by side
	res_desc.Height = height;
	res_desc.DepthOrArraySize = 1;
	res_desc.MipLevels = 1;
	res_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	res_desc.SampleDesc.Count = 1;
	res_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

	D3D12_CLEAR_VALUE clear_value = {};
	clear_value.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

	HRESULT hr = device->CreateCommittedResource(
	    &heap_props, D3D12_HEAP_FLAG_NONE, &res_desc,
	    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear_value,
	    __uuidof(ID3D12Resource), reinterpret_cast<void **>(&r->stereo_texture));
	if (FAILED(hr)) {
		U_LOG_E("Failed to create stereo texture: 0x%08x", hr);
		return XRT_ERROR_D3D;
	}

	// Create RTV for stereo texture
	D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = r->rtv_heap->GetCPUDescriptorHandleForHeapStart();
	device->CreateRenderTargetView(r->stereo_texture, nullptr, rtv_handle);

	// Create SRV for stereo texture
	D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
	srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv_desc.Texture2D.MipLevels = 1;
	srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

	D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu = r->srv_heap->GetCPUDescriptorHandleForHeapStart();
	device->CreateShaderResourceView(r->stereo_texture, &srv_desc, srv_cpu);

	return XRT_SUCCESS;
}


extern "C" xrt_result_t
comp_d3d12_renderer_create(struct comp_d3d12_compositor *c,
                           uint32_t view_width,
                           uint32_t view_height,
                           uint32_t target_height,
                           struct comp_d3d12_renderer **out_renderer)
{
	auto internals = get_internals(c);
	ID3D12Device *device = internals->device;

	comp_d3d12_renderer *r = new comp_d3d12_renderer();
	memset(r, 0, sizeof(*r));
	r->c = c;
	r->view_width = view_width;
	r->view_height = view_height;
	r->texture_height = (std::max)(view_height, target_height);

	r->rtv_descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	r->srv_descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	// Create RTV descriptor heap (1 for stereo texture)
	D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {};
	rtv_heap_desc.NumDescriptors = 1;
	rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

	HRESULT hr = device->CreateDescriptorHeap(
	    &rtv_heap_desc, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void **>(&r->rtv_heap));
	if (FAILED(hr)) {
		U_LOG_E("Failed to create renderer RTV heap: 0x%08x", hr);
		delete r;
		return XRT_ERROR_D3D;
	}

	// Create SRV descriptor heap (shader visible, 1 SRV for stereo texture)
	D3D12_DESCRIPTOR_HEAP_DESC srv_heap_desc = {};
	srv_heap_desc.NumDescriptors = 1;
	srv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	srv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

	hr = device->CreateDescriptorHeap(
	    &srv_heap_desc, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void **>(&r->srv_heap));
	if (FAILED(hr)) {
		U_LOG_E("Failed to create renderer SRV heap: 0x%08x", hr);
		r->rtv_heap->Release();
		delete r;
		return XRT_ERROR_D3D;
	}

	// Create stereo texture
	xrt_result_t xret = create_stereo_texture(r, device, view_width, r->texture_height);
	if (xret != XRT_SUCCESS) {
		r->srv_heap->Release();
		r->rtv_heap->Release();
		delete r;
		return xret;
	}

	// Create root signature: 1 descriptor table (SRV) + 1 root constant (vec4 src_rect) + 1 static sampler
	D3D12_DESCRIPTOR_RANGE srv_range = {};
	srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	srv_range.NumDescriptors = 1;
	srv_range.BaseShaderRegister = 0;
	srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

	D3D12_ROOT_PARAMETER root_params[2] = {};

	// Param 0: SRV descriptor table
	root_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	root_params[0].DescriptorTable.NumDescriptorRanges = 1;
	root_params[0].DescriptorTable.pDescriptorRanges = &srv_range;
	root_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	// Param 1: root constants (4 floats = src_rect)
	root_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	root_params[1].Constants.ShaderRegister = 0;
	root_params[1].Constants.RegisterSpace = 0;
	root_params[1].Constants.Num32BitValues = 4;
	root_params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

	// Static sampler
	D3D12_STATIC_SAMPLER_DESC sampler = {};
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	sampler.MaxLOD = D3D12_FLOAT32_MAX;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_ROOT_SIGNATURE_DESC rs_desc = {};
	rs_desc.NumParameters = 2;
	rs_desc.pParameters = root_params;
	rs_desc.NumStaticSamplers = 1;
	rs_desc.pStaticSamplers = &sampler;
	rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

	ID3DBlob *sig_blob = nullptr;
	ID3DBlob *error_blob = nullptr;
	hr = D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &sig_blob, &error_blob);
	if (FAILED(hr)) {
		if (error_blob != nullptr) {
			U_LOG_E("Root signature serialize error: %s",
			        static_cast<const char *>(error_blob->GetBufferPointer()));
			error_blob->Release();
		}
		comp_d3d12_renderer_destroy(&r);
		return XRT_ERROR_D3D;
	}

	hr = device->CreateRootSignature(0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(),
	                                  __uuidof(ID3D12RootSignature), reinterpret_cast<void **>(&r->root_signature));
	sig_blob->Release();
	if (FAILED(hr)) {
		U_LOG_E("Failed to create root signature: 0x%08x", hr);
		comp_d3d12_renderer_destroy(&r);
		return XRT_ERROR_D3D;
	}

	// Compile blit shaders and create PSO
	ID3DBlob *vs_blob = nullptr;
	ID3DBlob *ps_blob = nullptr;

	hr = compile_shader(blit_vs_source, "main", "vs_5_0", &vs_blob);
	if (FAILED(hr)) {
		comp_d3d12_renderer_destroy(&r);
		return XRT_ERROR_D3D;
	}

	hr = compile_shader(blit_ps_source, "main", "ps_5_0", &ps_blob);
	if (FAILED(hr)) {
		vs_blob->Release();
		comp_d3d12_renderer_destroy(&r);
		return XRT_ERROR_D3D;
	}

	D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
	pso_desc.pRootSignature = r->root_signature;
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
	pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	pso_desc.SampleDesc.Count = 1;
	pso_desc.SampleMask = UINT_MAX;

	hr = device->CreateGraphicsPipelineState(&pso_desc, __uuidof(ID3D12PipelineState),
	                                          reinterpret_cast<void **>(&r->blit_pso));
	vs_blob->Release();
	ps_blob->Release();

	if (FAILED(hr)) {
		U_LOG_E("Failed to create blit PSO: 0x%08x", hr);
		comp_d3d12_renderer_destroy(&r);
		return XRT_ERROR_D3D;
	}

	*out_renderer = r;

	U_LOG_I("Created D3D12 renderer: %ux%u per view, texture %ux%u",
	        view_width, view_height, view_width * 2, r->texture_height);

	return XRT_SUCCESS;
}


extern "C" void
comp_d3d12_renderer_destroy(struct comp_d3d12_renderer **renderer_ptr)
{
	if (renderer_ptr == nullptr || *renderer_ptr == nullptr) {
		return;
	}

	comp_d3d12_renderer *r = *renderer_ptr;

	if (r->blit_pso != nullptr) {
		r->blit_pso->Release();
	}
	if (r->root_signature != nullptr) {
		r->root_signature->Release();
	}
	if (r->stereo_texture != nullptr) {
		r->stereo_texture->Release();
	}
	if (r->srv_heap != nullptr) {
		r->srv_heap->Release();
	}
	if (r->rtv_heap != nullptr) {
		r->rtv_heap->Release();
	}

	delete r;
	*renderer_ptr = nullptr;
}


extern "C" xrt_result_t
comp_d3d12_renderer_draw(struct comp_d3d12_renderer *renderer,
                         void *cmd_list_ptr,
                         struct comp_layer_accum *layers,
                         struct xrt_vec3 *left_eye,
                         struct xrt_vec3 *right_eye,
                         uint32_t target_width,
                         uint32_t target_height,
                         bool force_mono)
{
	ID3D12GraphicsCommandList *cmd_list = static_cast<ID3D12GraphicsCommandList *>(cmd_list_ptr);

	if (cmd_list == nullptr || layers == nullptr) {
		return XRT_ERROR_IPC_FAILURE;
	}

	// Transition stereo texture to render target
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = renderer->stereo_texture;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	cmd_list->ResourceBarrier(1, &barrier);

	// Set render target to stereo texture
	D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = renderer->rtv_heap->GetCPUDescriptorHandleForHeapStart();
	cmd_list->OMSetRenderTargets(1, &rtv_handle, FALSE, nullptr);

	// Clear stereo texture
	float clear_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
	cmd_list->ClearRenderTargetView(rtv_handle, clear_color, 0, nullptr);

	// Set up descriptor heap for SRV access
	ID3D12DescriptorHeap *heaps[] = {renderer->srv_heap};
	cmd_list->SetDescriptorHeaps(1, heaps);

	// Set root signature and PSO
	cmd_list->SetGraphicsRootSignature(renderer->root_signature);
	cmd_list->SetPipelineState(renderer->blit_pso);
	cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	cmd_list->IASetVertexBuffers(0, 0, nullptr);

	// Determine view count
	uint32_t view_count = force_mono ? 1 : 2;

	// For each projection layer, blit swapchain images into left/right halves
	for (uint32_t li = 0; li < layers->layer_count; li++) {
		struct comp_layer *layer = &layers->layers[li];

		if (layer->data.type != XRT_LAYER_PROJECTION &&
		    layer->data.type != XRT_LAYER_PROJECTION_DEPTH) {
			continue;
		}

		uint32_t layer_view_count = layer->data.view_count;
		if (force_mono) {
			layer_view_count = 1;
		}

		for (uint32_t vi = 0; vi < layer_view_count && vi < view_count; vi++) {
			struct xrt_swapchain *xsc = layer->sc_array[vi];
			if (xsc == nullptr) {
				continue;
			}

			// Get the swapchain native image handle (ID3D12Resource*)
			struct xrt_swapchain_native *xscn = (struct xrt_swapchain_native *)xsc;
			uint32_t img_index = layer->data.flip_y ? 0 : 0; // Use last released
			// The image to use is determined by the sub_image index
			img_index = layer->data.proj.v[vi].sub.image_index;
			ID3D12Resource *src_resource = reinterpret_cast<ID3D12Resource *>(
			    xscn->images[img_index].handle);

			if (src_resource == nullptr) {
				continue;
			}

			// TODO: Create SRV for swapchain image and bind it.
			// For now, we record the viewport setup — actual SRV binding
			// requires per-swapchain-image descriptor management.

			// Set viewport for this eye
			D3D12_VIEWPORT viewport = {};
			if (layer_view_count == 1) {
				// Mono: fill full stereo texture width
				viewport.Width = static_cast<float>(renderer->view_width * 2);
			} else {
				// Stereo: left half or right half
				viewport.TopLeftX = static_cast<float>(vi * renderer->view_width);
				viewport.Width = static_cast<float>(renderer->view_width);
			}
			viewport.TopLeftY = 0.0f;
			viewport.Height = static_cast<float>(renderer->texture_height);
			viewport.MinDepth = 0.0f;
			viewport.MaxDepth = 1.0f;
			cmd_list->RSSetViewports(1, &viewport);

			D3D12_RECT scissor = {};
			scissor.left = static_cast<LONG>(viewport.TopLeftX);
			scissor.top = 0;
			scissor.right = static_cast<LONG>(viewport.TopLeftX + viewport.Width);
			scissor.bottom = static_cast<LONG>(viewport.Height);
			cmd_list->RSSetScissorRects(1, &scissor);

			// Set source rect root constants (full image)
			float src_rect[4] = {0.0f, 0.0f, 1.0f, 1.0f};
			cmd_list->SetGraphicsRoot32BitConstants(1, 4, src_rect, 0);

			// Draw fullscreen quad
			cmd_list->DrawInstanced(4, 1, 0, 0);
		}
	}

	// Transition stereo texture back to shader resource for display processor
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	cmd_list->ResourceBarrier(1, &barrier);

	return XRT_SUCCESS;
}


extern "C" uint64_t
comp_d3d12_renderer_get_stereo_srv_handle(struct comp_d3d12_renderer *renderer)
{
	D3D12_GPU_DESCRIPTOR_HANDLE handle = renderer->srv_heap->GetGPUDescriptorHandleForHeapStart();
	return handle.ptr;
}

extern "C" void
comp_d3d12_renderer_get_view_dimensions(struct comp_d3d12_renderer *renderer,
                                        uint32_t *out_view_width,
                                        uint32_t *out_view_height)
{
	*out_view_width = renderer->view_width;
	*out_view_height = renderer->view_height;
}

extern "C" void *
comp_d3d12_renderer_get_stereo_resource(struct comp_d3d12_renderer *renderer)
{
	return renderer->stereo_texture;
}
