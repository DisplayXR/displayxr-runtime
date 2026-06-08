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
#include "d3d/d3d_dxgi_formats.h"
#include "math/m_api.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>
#include <cmath>

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

// Quad vertex shader for window-space layers (MVP + UV post_transform via root constants)
static const char *quad_vs_source = R"(
cbuffer LayerCB : register(b0) {
	float4x4 mvp;
	float4 post_transform;
	float4 color_scale;
	float4 color_bias;
};

struct VS_OUTPUT {
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
};

static const float2 quad_positions[4] = {
	float2(0.0, 0.0),
	float2(0.0, 1.0),
	float2(1.0, 0.0),
	float2(1.0, 1.0),
};

VS_OUTPUT main(uint vertex_id : SV_VertexID) {
	VS_OUTPUT output;
	float2 in_uv = quad_positions[vertex_id % 4];
	float2 pos = in_uv - 0.5;
	output.position = mul(mvp, float4(pos, 0.0, 1.0));
	output.uv = in_uv * post_transform.zw + post_transform.xy;
	return output;
}
)";

// Quad pixel shader with color_scale + color_bias
static const char *quad_ps_source = R"(
cbuffer LayerCB : register(b0) {
	float4x4 mvp;
	float4 post_transform;
	float4 color_scale;
	float4 color_bias;
};

Texture2D layer_tex : register(t0);
SamplerState layer_samp : register(s0);

struct VS_OUTPUT {
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
};

float4 main(VS_OUTPUT input) : SV_Target {
	float4 color = layer_tex.Sample(layer_samp, input.uv);
	color = color * color_scale + color_bias;
	return color;
}
)";

// Masked 2D-over-3D composite (#439 cross-API leg). Keep byte-aligned with
// the D3D11 reference (comp_d3d11_renderer.cpp / shaders/masked_composite.hlsl).
// The lerp path samples the authored mask (t1) against the weave snapshot (t2);
// the analytic rect path (use_rect_mask) is kept for shader parity but the
// D3D12 leg always passes a mask (the no-mask path stays on the strip copies).
static const char *masked_composite_vs_source = R"(
struct VS_OUTPUT
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

static const float2 positions[3] = {
    float2(-1.0, -1.0),
    float2(-1.0,  3.0),
    float2( 3.0, -1.0),
};
static const float2 uvs[3] = {
    float2(0.0, 1.0),
    float2(0.0, -1.0),
    float2(2.0, 1.0),
};

VS_OUTPUT VSMain(uint vertex_id : SV_VertexID)
{
    VS_OUTPUT o;
    o.position = float4(positions[vertex_id], 0.0, 1.0);
    o.uv = uvs[vertex_id];
    return o;
}
)";

static const char *masked_composite_ps_source = R"(
Texture2D twod_tex   : register(t0);
Texture2D mask_tex   : register(t1);
Texture2D weave_tex  : register(t2);
SamplerState samp    : register(s0);

cbuffer CompositeParams : register(b0)
{
    float2 dst_dims;
    float2 canvas_origin;
    float2 canvas_size;
    uint   use_rect_mask;
    uint   alpha_over;   // #491: premul "over" of the 2D atop the weave
};

struct VS_OUTPUT
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float region_mask(float2 px, float2 uv)
{
    if (use_rect_mask)
    {
        bool inside =
            px.x >= canvas_origin.x && px.x < canvas_origin.x + canvas_size.x &&
            px.y >= canvas_origin.y && px.y < canvas_origin.y + canvas_size.y;
        return inside ? 1.0 : 0.0;
    }
    return saturate(mask_tex.Sample(samp, uv).r);
}

float4 PSMain(VS_OUTPUT input) : SV_Target
{
    float2 px = input.uv * dst_dims;

    if (use_rect_mask)
    {
        float M = region_mask(px, input.uv);
        if (M >= 0.5)
            discard;
        return twod_tex.Sample(samp, input.uv);
    }

    float4 twod  = twod_tex.Sample(samp, input.uv);
    float4 weave = weave_tex.Sample(samp, input.uv);
    if (alpha_over != 0)
    {
        // #491: the 2D layer's own (premultiplied) alpha IS the blend —
        // translucent 2D reveals the 3D scene, not the desktop.
        return twod + (1.0 - twod.a) * weave;
    }
    float M = saturate(mask_tex.Sample(samp, input.uv).r);
    return M * weave + (1.0 - M) * twod;
}
)";

/*!
 * Composite shader constants — 8 DWORDs, passed as root constants (b0).
 * Layout matches the D3D11 CompositeParams CB (HLSL packing: two float4 rows).
 */
struct CompositeParams
{
	float dst_dims[2];
	float canvas_origin[2];
	float canvas_size[2];
	uint32_t use_rect_mask;
	uint32_t alpha_over; // #491
};

// #439 Phase 3 — Local2D flatten. Reuses the masked_composite fullscreen-triangle
// VS (uv 0..1 over the viewport); the PS samples the source image through
// src_rect (origin + size; size.y < 0 => flip_y). Premultiplied vs straight
// "over" is the PSO blend state, not the shader. Byte-aligned with the D3D11
// local2d_flatten reference (comp_d3d11_renderer.cpp).
static const char *local2d_flatten_ps_source = R"(
Texture2D src_tex : register(t0);
SamplerState samp : register(s0);

cbuffer FlattenParams : register(b0)
{
    float4 src_rect; // xy = origin (norm), zw = size (norm; zw.y < 0 => flip_y)
};

struct VS_OUTPUT
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 PSMain(VS_OUTPUT input) : SV_Target
{
    float2 src_uv = src_rect.xy + input.uv * src_rect.zw;
    return src_tex.Sample(samp, src_uv);
}
)";

/*!
 * D3D12 renderer structure.
 */
struct comp_d3d12_renderer
{
	//! Parent compositor.
	struct comp_d3d12_compositor *c;

	//! Side-by-side atlas texture.
	ID3D12Resource *atlas_texture;

	//! RTV descriptor heap for atlas texture.
	ID3D12DescriptorHeap *rtv_heap;

	//! SRV/CBV descriptor heap (shader visible, dynamically allocated slots).
	//! Slot 0 = atlas SRV; slots 1..N allocated per-frame for layer SRVs.
	ID3D12DescriptorHeap *srv_heap;

	//! Total number of SRV slots in the heap.
	uint32_t srv_heap_size;

	//! Next available SRV slot (reset each frame, slot 0 = atlas).
	uint32_t next_srv_slot;

	//! Root signature for blit operations.
	ID3D12RootSignature *root_signature;

	//! Blit PSO.
	ID3D12PipelineState *blit_pso;

	//! Root signature for quad (window-space layer) operations.
	ID3D12RootSignature *quad_root_signature;

	//! Quad PSO with alpha blending.
	ID3D12PipelineState *quad_pso;

	//! Quad PSO with premultiplied alpha blending.
	ID3D12PipelineState *quad_pso_premul;

	//! Root signature for the masked 2D-over-3D composite (#439).
	ID3D12RootSignature *composite_root_signature;

	//! Masked-composite PSOs (opaque, fullscreen triangle). D3D12 bakes the
	//! RTV format into the PSO (unlike D3D11's format-agnostic RTV), and the
	//! weave target's format is the app's choice — shared textures in the
	//! wild are BGRA8 (cube_texture_d3d12_win) while DXGI targets are RGBA8.
	//! The lerp shader is channel-agnostic, so the two PSOs share bytecode.
	ID3D12PipelineState *composite_pso;      // DXGI_FORMAT_R8G8B8A8_UNORM
	ID3D12PipelineState *composite_pso_bgra; // DXGI_FORMAT_B8G8R8A8_UNORM

	//! Shader-visible 3-slot SRV heap for the composite pass
	//! (t0 = 2D surround, t1 = authored mask, t2 = weave snapshot).
	//! SRVs are written fresh on every composite call.
	ID3D12DescriptorHeap *composite_srv_heap;

	//! #439 Phase 3 — Local2D flatten pipeline. Each Local2D layer is drawn
	//! into a runtime scratch with premultiplied (or straight) "over"; the
	//! masked composite then lerps that scratch under the weave. Reuses the
	//! masked_composite VS; the PS samples through src_rect.
	ID3D12RootSignature *flatten_root_signature;
	ID3D12PipelineState *flatten_pso_premul;   // SrcBlend = ONE
	ID3D12PipelineState *flatten_pso_straight; // SrcBlend = SRC_ALPHA
	//! Shader-visible SRV heap for flatten source images — one slot per layer
	//! draw (D3D12 consumes descriptors at GPU-execute time, so concurrent
	//! draws in one cmd-list each need their own slot). Written fresh / frame.
	ID3D12DescriptorHeap *flatten_srv_heap;

	//! Descriptor sizes.
	uint32_t rtv_descriptor_size;
	uint32_t srv_descriptor_size;

	//! View dimensions.
	uint32_t view_width;
	uint32_t view_height;

	//! Tile layout for atlas (columns x rows of views).
	uint32_t tile_columns;
	uint32_t tile_rows;

	//! Actual atlas texture height (tile_rows * view_height).
	uint32_t texture_height;

	//! When true, view dims are fixed at legacy compromise scale and
	//! set_tile_layout must not recompute them.
	bool legacy_app_tile_scaling;
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


// Dump DRED auto-breadcrumbs + page-fault info to the log. Useful when the
// D3D12 device gets removed and we need to know which GPU command was
// executing at the time.
//
// DRED itself must be enabled BEFORE device creation; we don't own that step
// (Unity / the app creates the device). Enable it via either:
//
//   - Per-process: ID3D12DeviceRemovedExtendedDataSettings::
//                  SetAutoBreadcrumbsEnablement(FORCED_ON) before the first
//                  D3D12CreateDevice call in the process.
//   - Per-app:     HKLM\SOFTWARE\Microsoft\Direct3D\AppCompat\<exe>.exe with
//                  REG_DWORD AutoBreadcrumbsEnablement=1 and PageFaultEnablement=1.
//   - Globally:    Windows Development Build / DirectX Control Panel.
//
// When DRED isn't enabled, QueryInterface returns DXGI_ERROR_NOT_FOUND and we
// log that fact (so users know how to turn it on) but don't error.
static void
log_dred_state(ID3D12Device *device, const char *context)
{
	if (device == nullptr) return;
	ID3D12DeviceRemovedExtendedData *dred = nullptr;
	HRESULT qr = device->QueryInterface(__uuidof(ID3D12DeviceRemovedExtendedData), (void**)&dred);
	if (FAILED(qr) || dred == nullptr) {
		U_LOG_E("%s: DRED not available (qr=0x%08x). Enable via HKLM\\SOFTWARE"
		        "\\Microsoft\\Direct3D\\AppCompat\\<exe>\\AutoBreadcrumbsEnablement=1.",
		        context, (unsigned)qr);
		return;
	}

	D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT crumbs = {};
	HRESULT cb = dred->GetAutoBreadcrumbsOutput(&crumbs);
	if (SUCCEEDED(cb)) {
		const D3D12_AUTO_BREADCRUMB_NODE *node = crumbs.pHeadAutoBreadcrumbNode;
		int nidx = 0;
		while (node != nullptr && nidx < 20) {
			UINT last = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
			U_LOG_E("%s: DRED node[%d] count=%u last_completed=%u",
			        context, nidx, node->BreadcrumbCount, last);
			UINT start = (last > 16) ? last - 16 : 0;
			UINT end = (last + 4 < node->BreadcrumbCount) ? last + 4 : node->BreadcrumbCount;
			for (UINT i = start; i < end; i++) {
				const char *marker = (i == last) ? " <-- KILLED HERE" : "";
				U_LOG_E("%s:   op[%u] = %u%s",
				        context, i, (unsigned)node->pCommandHistory[i], marker);
			}
			node = node->pNext;
			nidx++;
		}
	} else {
		U_LOG_E("%s: DRED GetAutoBreadcrumbsOutput failed: 0x%08x", context, (unsigned)cb);
	}

	D3D12_DRED_PAGE_FAULT_OUTPUT pf = {};
	HRESULT pr = dred->GetPageFaultAllocationOutput(&pf);
	if (SUCCEEDED(pr) && pf.PageFaultVA != 0) {
		U_LOG_E("%s: DRED page fault VA=0x%llx", context, (unsigned long long)pf.PageFaultVA);
	}

	dred->Release();
}

static xrt_result_t
create_atlas_texture(struct comp_d3d12_renderer *r, ID3D12Device *device, uint32_t width, uint32_t height)
{
	D3D12_HEAP_PROPERTIES heap_props = {};
	heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC res_desc = {};
	res_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	res_desc.Width = r->tile_columns * width; // Atlas: tile_columns views across
	res_desc.Height = r->tile_rows * r->view_height; // Atlas: tile_rows views tall
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
	    __uuidof(ID3D12Resource), reinterpret_cast<void **>(&r->atlas_texture));
	if (FAILED(hr)) {
		U_LOG_E("Failed to create atlas texture: 0x%08x", hr);
		if (hr == DXGI_ERROR_DEVICE_REMOVED) {
			HRESULT dr_hr = device->GetDeviceRemovedReason();
			U_LOG_E("  GetDeviceRemovedReason: 0x%08x", (unsigned)dr_hr);
			log_dred_state(device, "create_atlas_texture");
		}
		return XRT_ERROR_D3D;
	}

	// Create RTV for atlas texture
	D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = r->rtv_heap->GetCPUDescriptorHandleForHeapStart();
	device->CreateRenderTargetView(r->atlas_texture, nullptr, rtv_handle);

	// Create SRV for atlas texture in slot 0
	D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
	srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv_desc.Texture2D.MipLevels = 1;
	srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

	D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu = r->srv_heap->GetCPUDescriptorHandleForHeapStart();
	device->CreateShaderResourceView(r->atlas_texture, &srv_desc, srv_cpu);

	return XRT_SUCCESS;
}


static void
get_color_scale_bias(const struct xrt_layer_data *data, float color_scale[4], float color_bias[4])
{
	bool has = (data->flags & XRT_LAYER_COMPOSITION_COLOR_BIAS_SCALE) != 0;
	if (has) {
		color_scale[0] = data->color_scale.r;
		color_scale[1] = data->color_scale.g;
		color_scale[2] = data->color_scale.b;
		color_scale[3] = data->color_scale.a;
		color_bias[0] = data->color_bias.r;
		color_bias[1] = data->color_bias.g;
		color_bias[2] = data->color_bias.b;
		color_bias[3] = data->color_bias.a;
	} else {
		color_scale[0] = 1.0f;
		color_scale[1] = 1.0f;
		color_scale[2] = 1.0f;
		color_scale[3] = 1.0f;
		color_bias[0] = 0.0f;
		color_bias[1] = 0.0f;
		color_bias[2] = 0.0f;
		color_bias[3] = 0.0f;
	}
}


/*!
 * Render a window-space layer onto the atlas for a given view.
 * Ported from D3D11's render_window_space_layer().
 */
static void
render_window_space_layer(struct comp_d3d12_renderer *r,
                          ID3D12GraphicsCommandList *cmd_list,
                          const struct comp_layer *layer,
                          uint32_t view_index,
                          uint32_t vp_w,
                          uint32_t vp_h)
{
	auto internals = get_internals(r->c);
	ID3D12Device *device = internals->device;
	const struct xrt_layer_data *data = &layer->data;
	const struct xrt_layer_window_space_data *ws = &data->window_space;

	// Get swapchain resource
	struct xrt_swapchain *xsc = layer->sc_array[0];
	if (xsc == nullptr) {
		return;
	}

	uint32_t image_index = ws->sub.image_index;
	ID3D12Resource *src_resource = static_cast<ID3D12Resource *>(
	    comp_d3d12_swapchain_get_resource(xsc, image_index));
	if (src_resource == nullptr) {
		return;
	}

	// Transition swapchain image to PIXEL_SHADER_RESOURCE for sampling
	D3D12_RESOURCE_BARRIER src_barrier = {};
	src_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	src_barrier.Transition.pResource = src_resource;
	src_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	src_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	src_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	cmd_list->ResourceBarrier(1, &src_barrier);

	// Allocate a unique SRV slot (same reason as projection blit — D3D12
	// descriptors are consumed at GPU execution time, not recording time).
	uint32_t srv_slot = r->next_srv_slot++;
	if (srv_slot >= r->srv_heap_size) {
		U_LOG_E("D3D12 renderer: SRV heap overflow in WS layer (slot %u >= %u)",
		        srv_slot, r->srv_heap_size);
		src_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		src_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
		cmd_list->ResourceBarrier(1, &src_barrier);
		return;
	}

	D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
	D3D12_RESOURCE_DESC res_desc = src_resource->GetDesc();
	// Sample app color swapchains as UNORM (not their sRGB sibling) so the GPU
	// does NOT auto-decode sRGB->linear; the DP wants display-referred bytes, so
	// pass them through. Resource is TYPELESS for 8-bit color (see swapchain),
	// which this maps to UNORM; identity for other/non-color formats.
	srv_desc.Format = d3d_dxgi_format_to_unorm_sample(res_desc.Format);
	srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv_desc.Texture2D.MipLevels = 1;
	srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

	D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu = r->srv_heap->GetCPUDescriptorHandleForHeapStart();
	srv_cpu.ptr += r->srv_descriptor_size * srv_slot;
	device->CreateShaderResourceView(src_resource, &srv_desc, srv_cpu);

	// Compute per-eye disparity offset
	float half_disp = ws->disparity / 2.0f;
	float eye_shift = (view_index == 0) ? -half_disp : half_disp;

	// Window-space fractional coords -> NDC [-1, 1]
	float frac_cx = ws->x + ws->width / 2.0f + eye_shift;
	float frac_cy = ws->y + ws->height / 2.0f;

	float ndc_cx = frac_cx * 2.0f - 1.0f;
	float ndc_cy = 1.0f - frac_cy * 2.0f;

	float ndc_sx = ws->width * 2.0f;
	float ndc_sy = -(ws->height * 2.0f);

	// Build 2D orthographic MVP (column-major for HLSL float4x4)
	// The quad VS uses a [-0.5, 0.5] unit quad
	float mvp[16];
	// clang-format off
	mvp[0]  = ndc_sx; mvp[1]  = 0.0f;   mvp[2]  = 0.0f; mvp[3]  = 0.0f;
	mvp[4]  = 0.0f;   mvp[5]  = ndc_sy;  mvp[6]  = 0.0f; mvp[7]  = 0.0f;
	mvp[8]  = 0.0f;   mvp[9]  = 0.0f;   mvp[10] = 1.0f; mvp[11] = 0.0f;
	mvp[12] = ndc_cx; mvp[13] = ndc_cy;  mvp[14] = 0.5f; mvp[15] = 1.0f;
	// clang-format on

	// UV post_transform for sub-image
	float post_transform[4];
	post_transform[0] = ws->sub.norm_rect.x;
	post_transform[1] = ws->sub.norm_rect.y;
	post_transform[2] = ws->sub.norm_rect.w;
	post_transform[3] = ws->sub.norm_rect.h;

	if (data->flip_y) {
		post_transform[1] += post_transform[3];
		post_transform[3] = -post_transform[3];
	}

	float color_scale[4];
	float color_bias[4];
	get_color_scale_bias(data, color_scale, color_bias);

	// Set quad root signature and PSO
	bool is_premultiplied = (data->flags & XRT_LAYER_COMPOSITION_BLEND_TEXTURE_SOURCE_ALPHA_BIT) == 0;
	ID3D12PipelineState *pso = is_premultiplied ? r->quad_pso_premul : r->quad_pso;

	cmd_list->SetGraphicsRootSignature(r->quad_root_signature);
	cmd_list->SetPipelineState(pso);

	// Set root constants (28 floats: mvp[16] + post_transform[4] + color_scale[4] + color_bias[4])
	cmd_list->SetGraphicsRoot32BitConstants(1, 16, mvp, 0);
	cmd_list->SetGraphicsRoot32BitConstants(1, 4, post_transform, 16);
	cmd_list->SetGraphicsRoot32BitConstants(1, 4, color_scale, 20);
	cmd_list->SetGraphicsRoot32BitConstants(1, 4, color_bias, 24);

	// Set descriptor heap and SRV table pointing to allocated slot
	ID3D12DescriptorHeap *heaps[] = {r->srv_heap};
	cmd_list->SetDescriptorHeaps(1, heaps);

	D3D12_GPU_DESCRIPTOR_HANDLE gpu_srv = r->srv_heap->GetGPUDescriptorHandleForHeapStart();
	gpu_srv.ptr += r->srv_descriptor_size * srv_slot;
	cmd_list->SetGraphicsRootDescriptorTable(0, gpu_srv);

	// Set RTV (atlas as render target) — viewport for the current view tile
	D3D12_CPU_DESCRIPTOR_HANDLE rtv = r->rtv_heap->GetCPUDescriptorHandleForHeapStart();
	cmd_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

	uint32_t tile_x = (view_index % r->tile_columns) * r->view_width;
	uint32_t tile_y = (view_index / r->tile_columns) * r->view_height;

	D3D12_VIEWPORT vp = {};
	vp.TopLeftX = (float)tile_x;
	vp.TopLeftY = (float)tile_y;
	vp.Width = (float)vp_w;
	vp.Height = (float)vp_h;
	vp.MinDepth = 0.0f;
	vp.MaxDepth = 1.0f;
	cmd_list->RSSetViewports(1, &vp);

	D3D12_RECT scissor = {};
	scissor.left = tile_x;
	scissor.top = tile_y;
	scissor.right = tile_x + vp_w;
	scissor.bottom = tile_y + vp_h;
	cmd_list->RSSetScissorRects(1, &scissor);

	// Draw quad (triangle strip, 4 vertices)
	cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	cmd_list->DrawInstanced(4, 1, 0, 0);

	// Transition swapchain image back to RENDER_TARGET
	src_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	src_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	cmd_list->ResourceBarrier(1, &src_barrier);
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

	// Initialize tile layout from the active rendering mode
	if (internals->xdev != NULL && internals->xdev->hmd != NULL) {
		uint32_t idx = internals->xdev->hmd->active_rendering_mode_index;
		if (idx < internals->xdev->rendering_mode_count) {
			r->tile_columns = internals->xdev->rendering_modes[idx].tile_columns;
			r->tile_rows = internals->xdev->rendering_modes[idx].tile_rows;
		}
	}
	// Default to 2x1 (stereo) if not set
	if (r->tile_columns == 0) {
		r->tile_columns = 2;
	}
	if (r->tile_rows == 0) {
		r->tile_rows = 1;
	}

	// Atlas height is exactly tile_rows * view_height — no target_height
	// inflation, which would corrupt weaver UV mapping in 3D mode.
	r->texture_height = r->tile_rows * view_height;

	r->rtv_descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	r->srv_descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	// Create RTV descriptor heap (1 for atlas texture)
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

	// Create SRV descriptor heap (shader visible).
	// Slot 0 = atlas; slots 1..N allocated per-frame for each layer draw.
	// 16 slots is plenty for typical usage (1 atlas + up to 15 layer SRVs).
	r->srv_heap_size = 16;
	r->next_srv_slot = 1;

	D3D12_DESCRIPTOR_HEAP_DESC srv_heap_desc = {};
	srv_heap_desc.NumDescriptors = r->srv_heap_size;
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

	// Create atlas texture
	xrt_result_t xret = create_atlas_texture(r, device, view_width, view_height);
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

	// --- Quad root signature: SRV table (param 0) + 28 root constants (param 1) + static sampler ---
	D3D12_DESCRIPTOR_RANGE quad_srv_range = {};
	quad_srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	quad_srv_range.NumDescriptors = 1;
	quad_srv_range.BaseShaderRegister = 0;
	quad_srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

	D3D12_ROOT_PARAMETER quad_root_params[2] = {};

	// Param 0: SRV descriptor table
	quad_root_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	quad_root_params[0].DescriptorTable.NumDescriptorRanges = 1;
	quad_root_params[0].DescriptorTable.pDescriptorRanges = &quad_srv_range;
	quad_root_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	// Param 1: 28 root constants (mvp[16] + post_transform[4] + color_scale[4] + color_bias[4])
	quad_root_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	quad_root_params[1].Constants.ShaderRegister = 0;
	quad_root_params[1].Constants.RegisterSpace = 0;
	quad_root_params[1].Constants.Num32BitValues = 28;
	quad_root_params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	D3D12_STATIC_SAMPLER_DESC quad_sampler = {};
	quad_sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	quad_sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	quad_sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	quad_sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	quad_sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	quad_sampler.MaxLOD = D3D12_FLOAT32_MAX;
	quad_sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_ROOT_SIGNATURE_DESC quad_rs_desc = {};
	quad_rs_desc.NumParameters = 2;
	quad_rs_desc.pParameters = quad_root_params;
	quad_rs_desc.NumStaticSamplers = 1;
	quad_rs_desc.pStaticSamplers = &quad_sampler;
	quad_rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

	sig_blob = nullptr;
	error_blob = nullptr;
	hr = D3D12SerializeRootSignature(&quad_rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &sig_blob, &error_blob);
	if (FAILED(hr)) {
		if (error_blob != nullptr) {
			U_LOG_E("Quad root signature serialize error: %s",
			        static_cast<const char *>(error_blob->GetBufferPointer()));
			error_blob->Release();
		}
		comp_d3d12_renderer_destroy(&r);
		return XRT_ERROR_D3D;
	}

	hr = device->CreateRootSignature(0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(),
	                                  __uuidof(ID3D12RootSignature),
	                                  reinterpret_cast<void **>(&r->quad_root_signature));
	sig_blob->Release();
	if (FAILED(hr)) {
		U_LOG_E("Failed to create quad root signature: 0x%08x", hr);
		comp_d3d12_renderer_destroy(&r);
		return XRT_ERROR_D3D;
	}

	// --- Compile quad shaders and create alpha-blend PSO ---
	ID3DBlob *quad_vs_blob = nullptr;
	ID3DBlob *quad_ps_blob = nullptr;

	hr = compile_shader(quad_vs_source, "main", "vs_5_0", &quad_vs_blob);
	if (FAILED(hr)) {
		comp_d3d12_renderer_destroy(&r);
		return XRT_ERROR_D3D;
	}

	hr = compile_shader(quad_ps_source, "main", "ps_5_0", &quad_ps_blob);
	if (FAILED(hr)) {
		quad_vs_blob->Release();
		comp_d3d12_renderer_destroy(&r);
		return XRT_ERROR_D3D;
	}

	// Standard alpha blend PSO.
	//
	// Alpha channel uses INV_SRC_ALPHA (proper Porter-Duff "over") so
	// dst.a is preserved through layered composition. See the matching
	// comment in comp_d3d11_renderer.cpp and issue #225.
	D3D12_GRAPHICS_PIPELINE_STATE_DESC quad_pso_desc = {};
	quad_pso_desc.pRootSignature = r->quad_root_signature;
	quad_pso_desc.VS.pShaderBytecode = quad_vs_blob->GetBufferPointer();
	quad_pso_desc.VS.BytecodeLength = quad_vs_blob->GetBufferSize();
	quad_pso_desc.PS.pShaderBytecode = quad_ps_blob->GetBufferPointer();
	quad_pso_desc.PS.BytecodeLength = quad_ps_blob->GetBufferSize();
	quad_pso_desc.BlendState.RenderTarget[0].BlendEnable = TRUE;
	quad_pso_desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
	quad_pso_desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	quad_pso_desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
	quad_pso_desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
	quad_pso_desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
	quad_pso_desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
	quad_pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	quad_pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	quad_pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	quad_pso_desc.RasterizerState.DepthClipEnable = TRUE;
	quad_pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	quad_pso_desc.NumRenderTargets = 1;
	quad_pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	quad_pso_desc.SampleDesc.Count = 1;
	quad_pso_desc.SampleMask = UINT_MAX;

	hr = device->CreateGraphicsPipelineState(&quad_pso_desc, __uuidof(ID3D12PipelineState),
	                                          reinterpret_cast<void **>(&r->quad_pso));
	if (FAILED(hr)) {
		U_LOG_E("Failed to create quad PSO: 0x%08x", hr);
		quad_vs_blob->Release();
		quad_ps_blob->Release();
		comp_d3d12_renderer_destroy(&r);
		return XRT_ERROR_D3D;
	}

	// Premultiplied alpha blend PSO
	quad_pso_desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
	quad_pso_desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;

	hr = device->CreateGraphicsPipelineState(&quad_pso_desc, __uuidof(ID3D12PipelineState),
	                                          reinterpret_cast<void **>(&r->quad_pso_premul));
	quad_vs_blob->Release();
	quad_ps_blob->Release();

	if (FAILED(hr)) {
		U_LOG_E("Failed to create quad premul PSO: 0x%08x", hr);
		comp_d3d12_renderer_destroy(&r);
		return XRT_ERROR_D3D;
	}

	// --- Masked-composite root signature (#439): SRV table of 3 (t0 2D /
	// t1 mask / t2 weave snapshot) + 8 root constants (CompositeParams, b0)
	// + static POINT sampler (byte-identity with the strip copies). ---
	D3D12_DESCRIPTOR_RANGE comp_srv_range = {};
	comp_srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	comp_srv_range.NumDescriptors = 3;
	comp_srv_range.BaseShaderRegister = 0;
	comp_srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

	D3D12_ROOT_PARAMETER comp_root_params[2] = {};

	// Param 0: SRV descriptor table (t0..t2)
	comp_root_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	comp_root_params[0].DescriptorTable.NumDescriptorRanges = 1;
	comp_root_params[0].DescriptorTable.pDescriptorRanges = &comp_srv_range;
	comp_root_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	// Param 1: 8 root constants (CompositeParams — 2x float2 + float2 + 2x uint)
	comp_root_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	comp_root_params[1].Constants.ShaderRegister = 0;
	comp_root_params[1].Constants.RegisterSpace = 0;
	comp_root_params[1].Constants.Num32BitValues = 8;
	comp_root_params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_STATIC_SAMPLER_DESC comp_sampler = {};
	comp_sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
	comp_sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	comp_sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	comp_sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	comp_sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	comp_sampler.MaxLOD = D3D12_FLOAT32_MAX;
	comp_sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_ROOT_SIGNATURE_DESC comp_rs_desc = {};
	comp_rs_desc.NumParameters = 2;
	comp_rs_desc.pParameters = comp_root_params;
	comp_rs_desc.NumStaticSamplers = 1;
	comp_rs_desc.pStaticSamplers = &comp_sampler;
	comp_rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

	sig_blob = nullptr;
	error_blob = nullptr;
	hr = D3D12SerializeRootSignature(&comp_rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &sig_blob, &error_blob);
	if (FAILED(hr)) {
		if (error_blob != nullptr) {
			U_LOG_E("Composite root signature serialize error: %s",
			        static_cast<const char *>(error_blob->GetBufferPointer()));
			error_blob->Release();
		}
		comp_d3d12_renderer_destroy(&r);
		return XRT_ERROR_D3D;
	}

	hr = device->CreateRootSignature(0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(),
	                                  __uuidof(ID3D12RootSignature),
	                                  reinterpret_cast<void **>(&r->composite_root_signature));
	sig_blob->Release();
	if (FAILED(hr)) {
		U_LOG_E("Failed to create composite root signature: 0x%08x", hr);
		comp_d3d12_renderer_destroy(&r);
		return XRT_ERROR_D3D;
	}

	// --- Compile masked-composite shaders and create the opaque PSO ---
	ID3DBlob *comp_vs_blob = nullptr;
	ID3DBlob *comp_ps_blob = nullptr;

	hr = compile_shader(masked_composite_vs_source, "VSMain", "vs_5_0", &comp_vs_blob);
	if (FAILED(hr)) {
		comp_d3d12_renderer_destroy(&r);
		return XRT_ERROR_D3D;
	}

	hr = compile_shader(masked_composite_ps_source, "PSMain", "ps_5_0", &comp_ps_blob);
	if (FAILED(hr)) {
		comp_vs_blob->Release();
		comp_d3d12_renderer_destroy(&r);
		return XRT_ERROR_D3D;
	}

	D3D12_GRAPHICS_PIPELINE_STATE_DESC comp_pso_desc = {};
	comp_pso_desc.pRootSignature = r->composite_root_signature;
	comp_pso_desc.VS.pShaderBytecode = comp_vs_blob->GetBufferPointer();
	comp_pso_desc.VS.BytecodeLength = comp_vs_blob->GetBufferSize();
	comp_pso_desc.PS.pShaderBytecode = comp_ps_blob->GetBufferPointer();
	comp_pso_desc.PS.BytecodeLength = comp_ps_blob->GetBufferSize();
	comp_pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	comp_pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	comp_pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	comp_pso_desc.RasterizerState.DepthClipEnable = TRUE;
	comp_pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	comp_pso_desc.NumRenderTargets = 1;
	comp_pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	comp_pso_desc.SampleDesc.Count = 1;
	comp_pso_desc.SampleMask = UINT_MAX;

	hr = device->CreateGraphicsPipelineState(&comp_pso_desc, __uuidof(ID3D12PipelineState),
	                                          reinterpret_cast<void **>(&r->composite_pso));

	if (SUCCEEDED(hr)) {
		// BGRA8 variant — same shader/state, only the baked RTV format differs.
		comp_pso_desc.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
		hr = device->CreateGraphicsPipelineState(&comp_pso_desc, __uuidof(ID3D12PipelineState),
		                                          reinterpret_cast<void **>(&r->composite_pso_bgra));
	}
	comp_vs_blob->Release();
	comp_ps_blob->Release();

	if (FAILED(hr)) {
		U_LOG_E("Failed to create composite PSO: 0x%08x", hr);
		comp_d3d12_renderer_destroy(&r);
		return XRT_ERROR_D3D;
	}

	// Dedicated shader-visible heap for the composite's 3 SRVs — written
	// fresh per composite call so a stale slot can never be sampled.
	D3D12_DESCRIPTOR_HEAP_DESC comp_heap_desc = {};
	comp_heap_desc.NumDescriptors = 3;
	comp_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	comp_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

	hr = device->CreateDescriptorHeap(
	    &comp_heap_desc, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void **>(&r->composite_srv_heap));
	if (FAILED(hr)) {
		U_LOG_E("Failed to create composite SRV heap: 0x%08x", hr);
		comp_d3d12_renderer_destroy(&r);
		return XRT_ERROR_D3D;
	}

	// --- #439 Phase 3: Local2D flatten root signature (SRV table of 1 [t0
	// source] + 4 root constants [src_rect, b0] + static LINEAR sampler —
	// linear matches the D3D11 flatten's bilinear sub-rect sampling). ---
	D3D12_DESCRIPTOR_RANGE flat_srv_range = {};
	flat_srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	flat_srv_range.NumDescriptors = 1;
	flat_srv_range.BaseShaderRegister = 0;
	flat_srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

	D3D12_ROOT_PARAMETER flat_root_params[2] = {};
	flat_root_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	flat_root_params[0].DescriptorTable.NumDescriptorRanges = 1;
	flat_root_params[0].DescriptorTable.pDescriptorRanges = &flat_srv_range;
	flat_root_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	flat_root_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	flat_root_params[1].Constants.ShaderRegister = 0;
	flat_root_params[1].Constants.RegisterSpace = 0;
	flat_root_params[1].Constants.Num32BitValues = 4; // src_rect float4
	flat_root_params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_STATIC_SAMPLER_DESC flat_sampler = {};
	flat_sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	flat_sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	flat_sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	flat_sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	flat_sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	flat_sampler.MaxLOD = D3D12_FLOAT32_MAX;
	flat_sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_ROOT_SIGNATURE_DESC flat_rs_desc = {};
	flat_rs_desc.NumParameters = 2;
	flat_rs_desc.pParameters = flat_root_params;
	flat_rs_desc.NumStaticSamplers = 1;
	flat_rs_desc.pStaticSamplers = &flat_sampler;
	flat_rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

	sig_blob = nullptr;
	error_blob = nullptr;
	hr = D3D12SerializeRootSignature(&flat_rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &sig_blob, &error_blob);
	if (FAILED(hr)) {
		if (error_blob != nullptr) {
			U_LOG_E("Flatten root signature serialize error: %s",
			        static_cast<const char *>(error_blob->GetBufferPointer()));
			error_blob->Release();
		}
		comp_d3d12_renderer_destroy(&r);
		return XRT_ERROR_D3D;
	}
	hr = device->CreateRootSignature(0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(),
	                                  __uuidof(ID3D12RootSignature),
	                                  reinterpret_cast<void **>(&r->flatten_root_signature));
	sig_blob->Release();
	if (FAILED(hr)) {
		U_LOG_E("Failed to create flatten root signature: 0x%08x", hr);
		comp_d3d12_renderer_destroy(&r);
		return XRT_ERROR_D3D;
	}

	// Flatten PSOs: shared masked_composite VS + local2d_flatten PS. The
	// scratch is always R8G8B8A8_UNORM (the masked composite is channel-
	// agnostic), so one RTV format covers both blend variants.
	ID3DBlob *flat_vs_blob = nullptr;
	ID3DBlob *flat_ps_blob = nullptr;
	hr = compile_shader(masked_composite_vs_source, "VSMain", "vs_5_0", &flat_vs_blob);
	if (FAILED(hr)) {
		comp_d3d12_renderer_destroy(&r);
		return XRT_ERROR_D3D;
	}
	hr = compile_shader(local2d_flatten_ps_source, "PSMain", "ps_5_0", &flat_ps_blob);
	if (FAILED(hr)) {
		flat_vs_blob->Release();
		comp_d3d12_renderer_destroy(&r);
		return XRT_ERROR_D3D;
	}

	D3D12_GRAPHICS_PIPELINE_STATE_DESC flat_pso_desc = {};
	flat_pso_desc.pRootSignature = r->flatten_root_signature;
	flat_pso_desc.VS.pShaderBytecode = flat_vs_blob->GetBufferPointer();
	flat_pso_desc.VS.BytecodeLength = flat_vs_blob->GetBufferSize();
	flat_pso_desc.PS.pShaderBytecode = flat_ps_blob->GetBufferPointer();
	flat_pso_desc.PS.BytecodeLength = flat_ps_blob->GetBufferSize();
	// Straight-alpha "over": SrcBlend = SRC_ALPHA. Alpha channel composes
	// Porter-Duff over (SrcAlpha = ONE, DestAlpha = INV_SRC_ALPHA) so stacked
	// layers accumulate coverage.
	flat_pso_desc.BlendState.RenderTarget[0].BlendEnable = TRUE;
	flat_pso_desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
	flat_pso_desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	flat_pso_desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
	flat_pso_desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
	flat_pso_desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
	flat_pso_desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
	flat_pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	flat_pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	flat_pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	flat_pso_desc.RasterizerState.DepthClipEnable = TRUE;
	flat_pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	flat_pso_desc.NumRenderTargets = 1;
	flat_pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	flat_pso_desc.SampleDesc.Count = 1;
	flat_pso_desc.SampleMask = UINT_MAX;

	hr = device->CreateGraphicsPipelineState(&flat_pso_desc, __uuidof(ID3D12PipelineState),
	                                          reinterpret_cast<void **>(&r->flatten_pso_straight));
	if (SUCCEEDED(hr)) {
		// Premultiplied "over": SrcBlend = ONE (the default for Local2D).
		flat_pso_desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
		hr = device->CreateGraphicsPipelineState(&flat_pso_desc, __uuidof(ID3D12PipelineState),
		                                          reinterpret_cast<void **>(&r->flatten_pso_premul));
	}
	flat_vs_blob->Release();
	flat_ps_blob->Release();
	if (FAILED(hr)) {
		U_LOG_E("Failed to create flatten PSO: 0x%08x", hr);
		comp_d3d12_renderer_destroy(&r);
		return XRT_ERROR_D3D;
	}

	// Shader-visible heap for flatten source SRVs — one slot per Local2D layer.
	D3D12_DESCRIPTOR_HEAP_DESC flat_heap_desc = {};
	flat_heap_desc.NumDescriptors = XRT_MAX_LAYERS;
	flat_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	flat_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	hr = device->CreateDescriptorHeap(
	    &flat_heap_desc, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void **>(&r->flatten_srv_heap));
	if (FAILED(hr)) {
		U_LOG_E("Failed to create flatten SRV heap: 0x%08x", hr);
		comp_d3d12_renderer_destroy(&r);
		return XRT_ERROR_D3D;
	}

	*out_renderer = r;

	U_LOG_I("Created D3D12 renderer: %ux%u per view, atlas %ux%u (%u cols x %u rows), texture_h=%u",
	        view_width, view_height,
	        r->tile_columns * view_width, r->tile_rows * r->texture_height,
	        r->tile_columns, r->tile_rows, r->texture_height);

	return XRT_SUCCESS;
}


extern "C" void
comp_d3d12_renderer_destroy(struct comp_d3d12_renderer **renderer_ptr)
{
	if (renderer_ptr == nullptr || *renderer_ptr == nullptr) {
		return;
	}

	comp_d3d12_renderer *r = *renderer_ptr;

	if (r->flatten_srv_heap != nullptr) {
		r->flatten_srv_heap->Release();
	}
	if (r->flatten_pso_premul != nullptr) {
		r->flatten_pso_premul->Release();
	}
	if (r->flatten_pso_straight != nullptr) {
		r->flatten_pso_straight->Release();
	}
	if (r->flatten_root_signature != nullptr) {
		r->flatten_root_signature->Release();
	}
	if (r->composite_srv_heap != nullptr) {
		r->composite_srv_heap->Release();
	}
	if (r->composite_pso_bgra != nullptr) {
		r->composite_pso_bgra->Release();
	}
	if (r->composite_pso != nullptr) {
		r->composite_pso->Release();
	}
	if (r->composite_root_signature != nullptr) {
		r->composite_root_signature->Release();
	}
	if (r->quad_pso_premul != nullptr) {
		r->quad_pso_premul->Release();
	}
	if (r->quad_pso != nullptr) {
		r->quad_pso->Release();
	}
	if (r->quad_root_signature != nullptr) {
		r->quad_root_signature->Release();
	}
	if (r->blit_pso != nullptr) {
		r->blit_pso->Release();
	}
	if (r->root_signature != nullptr) {
		r->root_signature->Release();
	}
	if (r->atlas_texture != nullptr) {
		r->atlas_texture->Release();
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
comp_d3d12_renderer_draw_projection_pass(struct comp_d3d12_renderer *renderer,
                                          void *cmd_list_ptr,
                                          struct comp_layer_accum *layers,
                                          struct xrt_vec3 *left_eye,
                                          struct xrt_vec3 *right_eye,
                                          uint32_t target_width,
                                          uint32_t target_height,
                                          bool hardware_display_3d)
{
	ID3D12GraphicsCommandList *cmd_list = static_cast<ID3D12GraphicsCommandList *>(cmd_list_ptr);

	if (cmd_list == nullptr || layers == nullptr) {
		return XRT_ERROR_IPC_FAILURE;
	}

	// Diagnostic: log draw info (first 5 frames + every 300)
	static uint32_t draw_counter = 0;
	bool draw_log = (draw_counter < 5 || draw_counter % 300 == 0);
	draw_counter++;
	if (draw_log) {
		U_LOG_W("D3D12 renderer draw: layers=%u, 3d=%d, view=%ux%u, target=%ux%u",
		        layers->layer_count, hardware_display_3d,
		        renderer->view_width, renderer->view_height,
		        target_width, target_height);
	}

	auto internals = get_internals(renderer->c);
	ID3D12Device *device = internals->device;

	// Reset per-frame SRV slot allocator (slot 0 = atlas, slots 1+ for layers)
	renderer->next_srv_slot = 1;

	// Transition atlas to RENDER_TARGET for shader-based stretch-blit
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = renderer->atlas_texture;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	cmd_list->ResourceBarrier(1, &barrier);

	// Clear atlas to black
	D3D12_CPU_DESCRIPTOR_HANDLE rtv = renderer->rtv_heap->GetCPUDescriptorHandleForHeapStart();
	float clear_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
	cmd_list->ClearRenderTargetView(rtv, clear_color, 0, nullptr);

	// Set blit pipeline (shared across all projection views)
	cmd_list->SetGraphicsRootSignature(renderer->root_signature);
	cmd_list->SetPipelineState(renderer->blit_pso);
	ID3D12DescriptorHeap *heaps[] = {renderer->srv_heap};
	cmd_list->SetDescriptorHeaps(1, heaps);
	cmd_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
	cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

	// Determine view count
	uint32_t view_count = hardware_display_3d ? 2 : 1;

	// For each projection layer, stretch-blit swapchain images into atlas tiles
	for (uint32_t li = 0; li < layers->layer_count; li++) {
		struct comp_layer *layer = &layers->layers[li];

		if (layer->data.type != XRT_LAYER_PROJECTION &&
		    layer->data.type != XRT_LAYER_PROJECTION_DEPTH) {
			continue;
		}

		uint32_t layer_view_count = layer->data.view_count;
		if (!hardware_display_3d) {
			layer_view_count = 1;
		}

		for (uint32_t vi = 0; vi < layer_view_count && vi < view_count; vi++) {
			struct xrt_swapchain *xsc = layer->sc_array[vi];
			if (xsc == nullptr) {
				continue;
			}

			// Get the swapchain native image handle (ID3D12Resource*)
			struct xrt_swapchain_native *xscn = (struct xrt_swapchain_native *)xsc;
			uint32_t img_index = layer->data.proj.v[vi].sub.image_index;
			ID3D12Resource *src_resource = reinterpret_cast<ID3D12Resource *>(
			    xscn->images[img_index].handle);

			if (src_resource == nullptr) {
				continue;
			}

			if (draw_log) {
				U_LOG_W("D3D12 blit: compositor src_resource=%p, img_index=%u, xscn=%p",
				        (void *)src_resource, img_index, (void *)xscn);
			}

			// Transition swapchain image: COMMON → PIXEL_SHADER_RESOURCE
			// The app leaves the resource in COMMON after rendering.
			D3D12_RESOURCE_BARRIER src_barrier = {};
			src_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			src_barrier.Transition.pResource = src_resource;
			src_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
			src_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			src_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			cmd_list->ResourceBarrier(1, &src_barrier);

			// Get swapchain image dimensions
			D3D12_RESOURCE_DESC src_desc = src_resource->GetDesc();
			float sw = static_cast<float>(src_desc.Width);
			float sh = static_cast<float>(src_desc.Height);

			// Use image_rect from layer data to handle single-swapchain
			// packed views (e.g., 7680x4320 with left at x=0, right at x=3840)
			struct xrt_rect sub_rect = layer->data.proj.v[vi].sub.rect;
			float src_x = 0.0f;
			float src_y = 0.0f;
			float src_w = sw;
			float src_h = sh;

			if (sub_rect.extent.w > 0 && sub_rect.extent.h > 0) {
				src_x = static_cast<float>(sub_rect.offset.w);
				src_y = static_cast<float>(sub_rect.offset.h);
				src_w = static_cast<float>(sub_rect.extent.w);
				src_h = static_cast<float>(sub_rect.extent.h);
			}

			// Compute normalized src_rect for the blit shader UV transform
			float blit_src_rect[4] = {
			    src_x / sw,
			    src_y / sh,
			    src_w / sw,
			    src_h / sh,
			};

			// Allocate a unique SRV slot for this draw (D3D12 descriptors
			// are read at GPU execution time, so reusing a slot across
			// multiple draws causes all draws to see the last SRV written).
			uint32_t srv_slot = renderer->next_srv_slot++;
			if (srv_slot >= renderer->srv_heap_size) {
				U_LOG_E("D3D12 renderer: SRV heap overflow (slot %u >= %u)",
				        srv_slot, renderer->srv_heap_size);
				continue;
			}

			D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
			// UNORM sample (see the other SRV site): no sRGB auto-decode; the
			// app's display-referred bytes pass through to the DP unchanged.
			srv_desc.Format = d3d_dxgi_format_to_unorm_sample(src_desc.Format);
			srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srv_desc.Texture2D.MipLevels = 1;
			srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

			D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu = renderer->srv_heap->GetCPUDescriptorHandleForHeapStart();
			srv_cpu.ptr += renderer->srv_descriptor_size * srv_slot;
			device->CreateShaderResourceView(src_resource, &srv_desc, srv_cpu);

			D3D12_GPU_DESCRIPTOR_HANDLE gpu_srv = renderer->srv_heap->GetGPUDescriptorHandleForHeapStart();
			gpu_srv.ptr += renderer->srv_descriptor_size * srv_slot;

			// Tile position in atlas grid
			uint32_t tile_idx = (layer_view_count == 1) ? 0 : vi;
			uint32_t tile_x = (tile_idx % renderer->tile_columns) * renderer->view_width;
			uint32_t tile_y = (tile_idx / renderer->tile_columns) * renderer->view_height;

			if (draw_log) {
				U_LOG_W("D3D12 renderer: blit layer=%u view=%u, src=%p (%llux%u), "
				        "sub_rect=(%.0f,%.0f %.0fx%.0f), tile=(%u,%u), view=%ux%u",
				        li, vi, (void *)src_resource,
				        (unsigned long long)src_desc.Width, (unsigned)src_desc.Height,
				        src_x, src_y, src_w, src_h,
				        tile_x, tile_y, renderer->view_width, renderer->view_height);
			}

			// In mono/2D mode, clamp viewport to window dims (matching D3D11)
			uint32_t vp_w = renderer->view_width;
			uint32_t vp_h = renderer->view_height;
			if (!hardware_display_3d) {
				uint32_t atlas_w = renderer->tile_columns * renderer->view_width;
				if (target_width < atlas_w) vp_w = target_width;
				if (target_height < renderer->texture_height) vp_h = target_height;
			}

			// Set viewport and scissor to the tile rectangle
			D3D12_VIEWPORT vp = {};
			vp.TopLeftX = static_cast<float>(tile_x);
			vp.TopLeftY = static_cast<float>(tile_y);
			vp.Width = static_cast<float>(vp_w);
			vp.Height = static_cast<float>(vp_h);
			vp.MinDepth = 0.0f;
			vp.MaxDepth = 1.0f;
			cmd_list->RSSetViewports(1, &vp);

			D3D12_RECT scissor = {};
			scissor.left = tile_x;
			scissor.top = tile_y;
			scissor.right = tile_x + vp_w;
			scissor.bottom = tile_y + vp_h;
			cmd_list->RSSetScissorRects(1, &scissor);

			// Bind SRV and root constants, draw fullscreen quad
			cmd_list->SetGraphicsRootDescriptorTable(0, gpu_srv);
			cmd_list->SetGraphicsRoot32BitConstants(1, 4, blit_src_rect, 0);
			cmd_list->DrawInstanced(4, 1, 0, 0);

			// For mono: draw same quad again at second tile position
			if (layer_view_count == 1 && view_count == 2) {
				uint32_t dup_x = (1 % renderer->tile_columns) * renderer->view_width;
				uint32_t dup_y = (1 / renderer->tile_columns) * renderer->view_height;

				vp.TopLeftX = static_cast<float>(dup_x);
				vp.TopLeftY = static_cast<float>(dup_y);
				vp.Width = static_cast<float>(vp_w);
				vp.Height = static_cast<float>(vp_h);
				cmd_list->RSSetViewports(1, &vp);

				scissor.left = dup_x;
				scissor.top = dup_y;
				scissor.right = dup_x + vp_w;
				scissor.bottom = dup_y + vp_h;
				cmd_list->RSSetScissorRects(1, &scissor);

				cmd_list->DrawInstanced(4, 1, 0, 0);
			}

			// Transition swapchain image back: PIXEL_SHADER_RESOURCE → COMMON
			src_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			src_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
			cmd_list->ResourceBarrier(1, &src_barrier);
		}
	}

	// Atlas left in RENDER_TARGET; window-space pass continues into the
	// same cmd_list (or after a capture round-trip through PSR).
	return XRT_SUCCESS;
}

extern "C" xrt_result_t
comp_d3d12_renderer_draw_window_space_pass(struct comp_d3d12_renderer *renderer,
                                            void *cmd_list_ptr,
                                            struct comp_layer_accum *layers,
                                            uint32_t target_width,
                                            uint32_t target_height,
                                            bool hardware_display_3d)
{
	ID3D12GraphicsCommandList *cmd_list = static_cast<ID3D12GraphicsCommandList *>(cmd_list_ptr);
	if (cmd_list == nullptr || layers == nullptr) {
		return XRT_ERROR_IPC_FAILURE;
	}

	uint32_t view_count = hardware_display_3d ? 2 : 1;

	// Compute effective viewport for window-space layers (same clamp as projection)
	uint32_t ws_vp_w = renderer->view_width;
	uint32_t ws_vp_h = renderer->view_height;
	if (!hardware_display_3d) {
		uint32_t atlas_w = renderer->tile_columns * renderer->view_width;
		if (target_width < atlas_w) ws_vp_w = target_width;
		if (target_height < renderer->texture_height) ws_vp_h = target_height;
	}

	// Draw window-space layers (atlas is in RENDER_TARGET state)
	for (uint32_t vi = 0; vi < view_count; vi++) {
		for (uint32_t li = 0; li < layers->layer_count; li++) {
			struct comp_layer *layer = &layers->layers[li];
			if (layer->data.type != XRT_LAYER_WINDOW_SPACE) {
				continue;
			}
			render_window_space_layer(renderer, cmd_list, layer, vi, ws_vp_w, ws_vp_h);
		}
	}

	// Final transition: RENDER_TARGET → PIXEL_SHADER_RESOURCE for DP
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = renderer->atlas_texture;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	cmd_list->ResourceBarrier(1, &barrier);

	return XRT_SUCCESS;
}

extern "C" xrt_result_t
comp_d3d12_renderer_draw(struct comp_d3d12_renderer *renderer,
                         void *cmd_list_ptr,
                         struct comp_layer_accum *layers,
                         struct xrt_vec3 *left_eye,
                         struct xrt_vec3 *right_eye,
                         uint32_t target_width,
                         uint32_t target_height,
                         bool hardware_display_3d)
{
	xrt_result_t xret = comp_d3d12_renderer_draw_projection_pass(
	    renderer, cmd_list_ptr, layers, left_eye, right_eye,
	    target_width, target_height, hardware_display_3d);
	if (xret != XRT_SUCCESS) {
		return xret;
	}
	return comp_d3d12_renderer_draw_window_space_pass(
	    renderer, cmd_list_ptr, layers,
	    target_width, target_height, hardware_display_3d);
}


extern "C" uint64_t
comp_d3d12_renderer_get_atlas_srv_handle(struct comp_d3d12_renderer *renderer)
{
	D3D12_GPU_DESCRIPTOR_HANDLE handle = renderer->srv_heap->GetGPUDescriptorHandleForHeapStart();
	return handle.ptr;
}

extern "C" uint64_t
comp_d3d12_renderer_get_atlas_srv_cpu_handle(struct comp_d3d12_renderer *renderer)
{
	D3D12_CPU_DESCRIPTOR_HANDLE handle = renderer->srv_heap->GetCPUDescriptorHandleForHeapStart();
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

extern "C" void
comp_d3d12_renderer_get_tile_layout(struct comp_d3d12_renderer *renderer,
                                    uint32_t *out_tile_columns,
                                    uint32_t *out_tile_rows)
{
	*out_tile_columns = renderer->tile_columns;
	*out_tile_rows = renderer->tile_rows;
}

extern "C" void
comp_d3d12_renderer_set_tile_layout(struct comp_d3d12_renderer *renderer,
                                    uint32_t tile_columns,
                                    uint32_t tile_rows)
{
	if (!renderer->legacy_app_tile_scaling) {
		// Extension app: recompute view dimensions so the atlas logical size stays constant.
		// E.g. stereo 2×1 (vw=1920) → 2D 1×1 (vw=3840) keeps atlas_w=3840.
		if (tile_columns > 0 && renderer->tile_columns > 0) {
			uint32_t atlas_w = renderer->tile_columns * renderer->view_width;
			renderer->view_width = atlas_w / tile_columns;
		}
		if (tile_rows > 0 && renderer->tile_rows > 0) {
			uint32_t atlas_h = renderer->tile_rows * renderer->view_height;
			renderer->view_height = atlas_h / tile_rows;
		}
	}
	// Legacy app: view dims stay fixed at compromise scale.
	// Only update tile layout — the app always renders the same atlas.
	renderer->tile_columns = tile_columns;
	renderer->tile_rows = tile_rows;
}

extern "C" void
comp_d3d12_renderer_set_legacy_app_tile_scaling(struct comp_d3d12_renderer *renderer,
                                                 bool legacy)
{
	if (renderer != nullptr) {
		renderer->legacy_app_tile_scaling = legacy;
	}
}

extern "C" void *
comp_d3d12_renderer_get_atlas_resource(struct comp_d3d12_renderer *renderer)
{
	return renderer->atlas_texture;
}

extern "C" xrt_result_t
comp_d3d12_renderer_resize(struct comp_d3d12_renderer *renderer,
                           uint32_t new_view_width,
                           uint32_t new_view_height,
                           uint32_t new_target_height)
{
	if (renderer == nullptr) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	if (new_view_width < 64) {
		new_view_width = 64;
	}
	if (new_view_height < 64) {
		new_view_height = 64;
	}

	uint32_t new_texture_height = renderer->tile_rows * new_view_height;

	if (new_view_width == renderer->view_width &&
	    new_view_height == renderer->view_height &&
	    new_texture_height == renderer->texture_height) {
		return XRT_SUCCESS;
	}

	U_LOG_W("D3D12 renderer resize: view %ux%u -> %ux%u, tex_h %u -> %u",
	        renderer->view_width, renderer->view_height,
	        new_view_width, new_view_height,
	        renderer->texture_height, new_texture_height);

	// Release existing atlas texture
	if (renderer->atlas_texture != nullptr) {
		renderer->atlas_texture->Release();
		renderer->atlas_texture = nullptr;
	}

	renderer->view_width = new_view_width;
	renderer->view_height = new_view_height;
	renderer->texture_height = new_texture_height;

	// Recreate atlas texture with new dimensions
	auto internals = get_internals(renderer->c);
	return create_atlas_texture(renderer, internals->device, new_view_width, new_view_height);
}

extern "C" xrt_result_t
comp_d3d12_renderer_composite_2d_masked(struct comp_d3d12_renderer *renderer,
                                        void *cmd_list_ptr,
                                        uint64_t dst_rtv_handle,
                                        uint32_t dst_format,
                                        void *twod_resource,
                                        void *mask_resource,
                                        void *weave_resource,
                                        uint32_t region_w,
                                        uint32_t region_h,
                                        int32_t cx,
                                        int32_t cy,
                                        uint32_t cw,
                                        uint32_t ch,
                                        bool alpha_over)
{
	if (renderer == nullptr || cmd_list_ptr == nullptr || dst_rtv_handle == 0 || twod_resource == nullptr ||
	    mask_resource == nullptr || weave_resource == nullptr) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	// PSO by target format — D3D12 bakes the RTV format into the PSO.
	ID3D12PipelineState *pso = nullptr;
	switch (static_cast<DXGI_FORMAT>(dst_format)) {
	case DXGI_FORMAT_R8G8B8A8_UNORM: pso = renderer->composite_pso; break;
	case DXGI_FORMAT_B8G8R8A8_UNORM: pso = renderer->composite_pso_bgra; break;
	default: break;
	}
	if (pso == nullptr) {
		return XRT_ERROR_D3D;
	}

	auto internals = get_internals(renderer->c);
	ID3D12Device *device = internals->device;
	auto *cmd_list = static_cast<ID3D12GraphicsCommandList *>(cmd_list_ptr);

	// Write the 3 SRVs fresh into the dedicated shader-visible heap
	// (t0 = 2D surround scratch, t1 = authored mask staged copy, t2 = weave
	// snapshot scratch). Formats come from each resource — the scratches are
	// runtime-created with concrete formats, the mask is R8_UNORM.
	ID3D12Resource *srcs[3] = {
	    static_cast<ID3D12Resource *>(twod_resource),
	    static_cast<ID3D12Resource *>(mask_resource),
	    static_cast<ID3D12Resource *>(weave_resource),
	};
	D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu = renderer->composite_srv_heap->GetCPUDescriptorHandleForHeapStart();
	for (int i = 0; i < 3; i++) {
		D3D12_RESOURCE_DESC rd = srcs[i]->GetDesc();
		D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
		srv_desc.Format = rd.Format;
		srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv_desc.Texture2D.MipLevels = 1;
		device->CreateShaderResourceView(srcs[i], &srv_desc, srv_cpu);
		srv_cpu.ptr += renderer->srv_descriptor_size;
	}

	// #464: the composite region is window-sized at the top-left anchor of
	// the (worst-case-allocated) surface; pixels beyond it are never written.
	// The full-screen triangle's uv [0,1] spans the viewport, so region-sized
	// SRVs sample 1:1.
	D3D12_VIEWPORT vp = {};
	vp.Width = static_cast<float>(region_w);
	vp.Height = static_cast<float>(region_h);
	vp.MaxDepth = 1.0f;
	D3D12_RECT scissor = {};
	scissor.right = static_cast<LONG>(region_w);
	scissor.bottom = static_cast<LONG>(region_h);

	CompositeParams params = {};
	params.dst_dims[0] = static_cast<float>(region_w);
	params.dst_dims[1] = static_cast<float>(region_h);
	params.canvas_origin[0] = static_cast<float>(cx);
	params.canvas_origin[1] = static_cast<float>(cy);
	params.canvas_size[0] = static_cast<float>(cw);
	params.canvas_size[1] = static_cast<float>(ch);
	// The D3D12 leg always composites an authored mask; the analytic rect
	// path (use_rect_mask=1) exists for shader parity with D3D11 only.
	params.use_rect_mask = 0;
	params.alpha_over = alpha_over ? 1u : 0u; // #491: implicit Local2D = premul over

	D3D12_CPU_DESCRIPTOR_HANDLE rtv = {};
	rtv.ptr = static_cast<SIZE_T>(dst_rtv_handle);

	// NOTE: this leaves the composite heap / root signature / PSO bound —
	// downstream recording (strips fallback never runs after a successful
	// composite; HUD + capture are copy ops) re-binds what it needs.
	cmd_list->SetDescriptorHeaps(1, &renderer->composite_srv_heap);
	cmd_list->SetGraphicsRootSignature(renderer->composite_root_signature);
	cmd_list->SetPipelineState(pso);
	cmd_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
	cmd_list->RSSetViewports(1, &vp);
	cmd_list->RSSetScissorRects(1, &scissor);
	cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	cmd_list->SetGraphicsRootDescriptorTable(0, renderer->composite_srv_heap->GetGPUDescriptorHandleForHeapStart());
	cmd_list->SetGraphicsRoot32BitConstants(1, 8, &params, 0);
	cmd_list->DrawInstanced(3, 1, 0, 0);

	return XRT_SUCCESS;
}

// #439 Phase 3 — flatten one Local2D layer into the scratch RTV (the `twod`
// source the masked composite reads). Records into the OPEN cmd-list mid-frame;
// the caller has already bound nothing (we set our own RTV) and transitioned
// the scratch to RENDER_TARGET + cleared it. Ported from the D3D11
// comp_d3d11_renderer_flatten_local_2d: a fullscreen triangle whose viewport
// is the clipped dest sub-rect, sampling the source through src_rect (with
// flip_y via a negative src_h). Premultiplied vs straight "over" is the PSO.
//
// slot_index gives this draw its own flatten_srv_heap slot — D3D12 consumes
// descriptors at GPU-execute time, so concurrent draws can't share one slot.
// The source image is transitioned RENDER_TARGET<->PIXEL_SHADER_RESOURCE around
// the draw, the same steady-state convention render_window_space_layer uses for
// app color swapchains.
extern "C" xrt_result_t
comp_d3d12_renderer_flatten_local_2d(struct comp_d3d12_renderer *renderer,
                                     void *cmd_list_ptr,
                                     uint64_t scratch_rtv_handle,
                                     void *src_resource,
                                     uint32_t slot_index,
                                     int32_t dst_x,
                                     int32_t dst_y,
                                     uint32_t dst_w,
                                     uint32_t dst_h,
                                     float src_x,
                                     float src_y,
                                     float src_w,
                                     float src_h,
                                     bool unpremultiplied)
{
	if (renderer == nullptr || cmd_list_ptr == nullptr || scratch_rtv_handle == 0 || src_resource == nullptr) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}
	if (slot_index >= XRT_MAX_LAYERS) {
		U_LOG_E("D3D12 flatten: SRV slot %u >= %u", slot_index, (uint32_t)XRT_MAX_LAYERS);
		return XRT_ERROR_D3D;
	}

	auto internals = get_internals(renderer->c);
	ID3D12Device *device = internals->device;
	auto *cmd_list = static_cast<ID3D12GraphicsCommandList *>(cmd_list_ptr);
	auto *src = static_cast<ID3D12Resource *>(src_resource);

	// Source image → sampleable. Sample app color swapchains as their UNORM
	// sibling (no implicit sRGB decode — the DP wants display-referred bytes).
	D3D12_RESOURCE_BARRIER src_barrier = {};
	src_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	src_barrier.Transition.pResource = src;
	src_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	src_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	src_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	cmd_list->ResourceBarrier(1, &src_barrier);

	D3D12_RESOURCE_DESC rd = src->GetDesc();
	D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
	srv_desc.Format = d3d_dxgi_format_to_unorm_sample(rd.Format);
	srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv_desc.Texture2D.MipLevels = 1;
	srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu = renderer->flatten_srv_heap->GetCPUDescriptorHandleForHeapStart();
	srv_cpu.ptr += renderer->srv_descriptor_size * slot_index;
	device->CreateShaderResourceView(src, &srv_desc, srv_cpu);

	cmd_list->SetDescriptorHeaps(1, &renderer->flatten_srv_heap);
	cmd_list->SetGraphicsRootSignature(renderer->flatten_root_signature);
	cmd_list->SetPipelineState(unpremultiplied ? renderer->flatten_pso_straight : renderer->flatten_pso_premul);

	D3D12_CPU_DESCRIPTOR_HANDLE rtv = {};
	rtv.ptr = static_cast<SIZE_T>(scratch_rtv_handle);
	cmd_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

	// Viewport = clipped dest sub-rect; the fullscreen triangle's uv [0,1]
	// spans it, mapping through src_rect into the source image.
	D3D12_VIEWPORT vp = {};
	vp.TopLeftX = static_cast<float>(dst_x);
	vp.TopLeftY = static_cast<float>(dst_y);
	vp.Width = static_cast<float>(dst_w);
	vp.Height = static_cast<float>(dst_h);
	vp.MaxDepth = 1.0f;
	cmd_list->RSSetViewports(1, &vp);
	D3D12_RECT scissor = {dst_x, dst_y, dst_x + (int32_t)dst_w, dst_y + (int32_t)dst_h};
	cmd_list->RSSetScissorRects(1, &scissor);

	float src_rect[4] = {src_x, src_y, src_w, src_h};
	cmd_list->SetGraphicsRoot32BitConstants(1, 4, src_rect, 0);

	D3D12_GPU_DESCRIPTOR_HANDLE gpu_srv = renderer->flatten_srv_heap->GetGPUDescriptorHandleForHeapStart();
	gpu_srv.ptr += renderer->srv_descriptor_size * slot_index;
	cmd_list->SetGraphicsRootDescriptorTable(0, gpu_srv);

	cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	cmd_list->DrawInstanced(3, 1, 0, 0);

	// Source back to its steady RENDER_TARGET state.
	src_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	src_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	cmd_list->ResourceBarrier(1, &src_barrier);

	return XRT_SUCCESS;
}
