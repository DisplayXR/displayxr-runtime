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
#include "leia_bg_capture_win.h"

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

/*
 * Chroma-key shaders — same algorithm as the D3D11 DP, retargeted to D3D12.
 * The fill pass replaces alpha=0 atlas pixels with the chroma key so the SR
 * weaver (opaque RGB only) can run. The strip pass examines the woven back
 * buffer and rewrites RGB-matching pixels to alpha=0 (with RGB premultiplied
 * for DWM's premultiplied alpha mode).
 *
 * Both shaders share a fullscreen-triangle VS (3 verts via SV_VertexID).
 * Root signature: b0 = 32-bit constants (chroma_rgb + pad), t0 = SRV
 * descriptor table, s0 = static sampler (point filter).
 */
static const char *ck_vs_source = R"(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut main(uint vid : SV_VertexID) {
    VSOut o;
    o.uv = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
)";

// Pre-weave fill: atlas RGBA → opaque RGB with alpha=0 regions filled
// by chroma_rgb. lerp(key, src.rgb, src.a) is no-op for alpha=1 (legacy
// app-pre-filled flow) and full key for alpha=0 (true-alpha flow).
static const char *ck_fill_ps_source = R"(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
Texture2D<float4> src : register(t0);
SamplerState samp : register(s0);
cbuffer Constants : register(b0) { float3 chroma_rgb; float pad; };
float4 main(VSOut i) : SV_Target {
    float4 c = src.Sample(samp, i.uv);
    float3 rgb = lerp(chroma_rgb, c.rgb, c.a);
    return float4(rgb, 1.0);
}
)";

// Post-weave strip: woven RGB → alpha=0 where RGB exact-matches chroma_rgb,
// alpha=1 elsewhere with RGB premultiplied so DWM's
//     src.rgb + (1-alpha)*dst.rgb
// blend doesn't add the matched chroma color to the desktop and saturate.
static const char *ck_strip_ps_source = R"(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
Texture2D<float4> src : register(t0);
SamplerState samp : register(s0);
cbuffer Constants : register(b0) { float3 chroma_rgb; float pad; };
float4 main(VSOut i) : SV_Target {
    float3 c = src.Sample(samp, i.uv).rgb;
    float3 d = abs(c - chroma_rgb);
    bool match = max(max(d.r, d.g), d.b) < (1.0/512.0);
    float a = match ? 0.0 : 1.0;
    return float4(c * a, a);
}
)";

/*
 * Default chroma key when the app didn't supply one (set_chroma_key key=0).
 * Magenta — matches the D3D11 DP's kDefaultChromaKey for cross-API parity.
 * 0x00BBGGRR layout: R=0xFF, G=0x00, B=0xFF.
 */
static constexpr uint32_t kDefaultChromaKey = 0x00FF00FF;

/*
 * Compose-under-bg pre-weave shader (preferred path when WGC is available).
 *
 * Reads the app's RGBA atlas + the captured desktop region behind the window,
 * composes them per-tile and outputs opaque RGB the SR weaver consumes:
 *
 *   out = lerp(bg, atlas.rgb, atlas.a),  out.a = 1
 *
 * Same VS as the ck pipeline (single fullscreen triangle).
 */
static const char *compose_under_bg_ps_source = R"(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
Texture2D<float4> atlas : register(t0);
Texture2D<float4> bg    : register(t1);
SamplerState samp       : register(s0);
cbuffer Constants : register(b0) {
    float2 bg_uv_origin;
    float2 bg_uv_extent;
    uint2  tile_count;
    uint2  pad_;
};
float4 main(VSOut i) : SV_Target {
    float4 a = atlas.Sample(samp, i.uv);
    float2 tile_local = frac(i.uv * float2(tile_count));
    float2 bg_uv = bg_uv_origin + tile_local * bg_uv_extent;
    float3 b = bg.SampleLevel(samp, bg_uv, 0).rgb;
    return float4(lerp(b, a.rgb, a.a), 1.0);
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
	HWND hwnd;                         //!< Native window handle from factory, used by bg-capture for self-exclusion + window-on-monitor rect.

	//! @name 2D blit pipeline resources (passthrough stretch-blit)
	//! @{
	ID3D12RootSignature *blit_root_sig;
	ID3D12PipelineState *blit_pso;
	ID3D12DescriptorHeap *blit_srv_heap; //!< Shader-visible, 1 SRV
	DXGI_FORMAT blit_output_format;
	//! @}

	uint32_t view_count; //!< Active mode view count (1=2D, 2=stereo).

	//! @name Chroma-key transparency support (lazy-allocated on first frame)
	//!
	//! When @ref ck_enabled and @ref ck_color != 0, process_atlas() does:
	//!   1. Pre-weave fill: atlas RGBA → ck_fill_tex (alpha=0 → chroma_rgb,
	//!      output alpha=1) so the SR weaver receives opaque RGB.
	//!   2. Pass ck_fill_tex (resource pointer) to the weaver instead of the
	//!      original atlas via leiasr_d3d12_set_input_texture.
	//!   3. Post-weave strip: copy back buffer → ck_strip_tex, then run strip
	//!      shader back to back-buffer RTV (chroma match → alpha=0, RGB
	//!      premultiplied for DWM).
	//! @{
	bool ck_enabled;
	uint32_t ck_color;                       //!< 0x00BBGGRR; effective key.
	ID3D12RootSignature *ck_root_sig;        //!< Shared by fill + strip PSOs.
	ID3D12PipelineState *ck_fill_pso;
	ID3D12PipelineState *ck_strip_pso;
	ID3D12DescriptorHeap *ck_srv_heap_fill;  //!< Shader-visible, 1 SRV (atlas).
	ID3D12DescriptorHeap *ck_srv_heap_strip; //!< Shader-visible, 1 SRV (strip_tex).
	ID3D12DescriptorHeap *ck_rtv_heap_fill;  //!< Non-shader-visible, 1 RTV (fill_tex).
	// Pre-weave fill target — RT-bindable + SRV-readable; the weaver samples
	// this resource directly via leiasr_d3d12_set_input_texture.
	ID3D12Resource *ck_fill_tex;
	uint32_t ck_fill_w, ck_fill_h;
	D3D12_RESOURCE_STATES ck_fill_state;
	// Post-weave strip source — copy of the back buffer for the strip pass
	// to sample.
	ID3D12Resource *ck_strip_tex;
	uint32_t ck_strip_w, ck_strip_h;
	D3D12_RESOURCE_STATES ck_strip_state;
	//! @}

	//! @name Compose-under-bg transparency support (preferred over chroma-key)
	//!
	//! Reuses ck_fill_tex/ck_rtv_heap_fill as the intermediate target. Own
	//! root sig + PSO + descriptor heap (2 SRV slots: atlas at 0, bg at 1).
	//! @{
	ID3D12CommandQueue *command_queue;   //!< Saved from factory; needed for fence Wait().
	struct leia_bg_capture *bg_capture;  //!< Owned; NULL → fall back to chroma-key.
	bool bg_compose_enabled;             //!< Active when the new path is in use.
	ID3D12Resource *bg_shared_tex;       //!< Opened from bg_capture's shared NT handle.
	ID3D12Fence *bg_fence;               //!< Opened from bg_capture's shared fence handle.
	ID3D12RootSignature *compose_root_sig;
	ID3D12PipelineState *compose_pso;
	ID3D12DescriptorHeap *compose_srv_heap; //!< Shader-visible, 2 entries (atlas, bg).
	UINT cbv_srv_desc_size;              //!< Cached for offset arithmetic in compose heap.
	//! @}
};

static inline struct leia_display_processor_d3d12_impl *
leia_dp_d3d12(struct xrt_display_processor_d3d12 *xdp)
{
	return (struct leia_display_processor_d3d12_impl *)xdp;
}


/*
 *
 * Chroma-key fill/strip helpers (transparency support).
 *
 * Lazy-allocated on first frame the pass runs. ck_should_run() gates the
 * whole flow — when false (the common case) none of these execute and
 * process_atlas behaves identically to the pre-transparency path.
 *
 */

static bool
ck_should_run(struct leia_display_processor_d3d12_impl *ldp)
{
	return ldp->ck_enabled && ldp->ck_color != 0;
}

// Compile a single shader source via D3DCompile. Returns owning blob (caller releases).
static ID3DBlob *
ck_compile_shader(const char *src, const char *entry, const char *target)
{
	ID3DBlob *blob = nullptr;
	ID3DBlob *err = nullptr;
	HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr,
	                        entry, target, 0, 0, &blob, &err);
	if (FAILED(hr)) {
		U_LOG_E("Leia D3D12 DP: ck shader compile (%s) failed: 0x%08x %s",
		        target, (unsigned)hr,
		        err ? (const char *)err->GetBufferPointer() : "");
		if (err) err->Release();
		return nullptr;
	}
	if (err) err->Release();
	return blob;
}

// Build the shared root signature: 32-bit constants for chroma_rgb (b0) +
// SRV descriptor table (t0) + static point sampler (s0).
static bool
ck_build_root_sig(struct leia_display_processor_d3d12_impl *ldp)
{
	D3D12_DESCRIPTOR_RANGE srv_range = {};
	srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	srv_range.NumDescriptors = 1;
	srv_range.BaseShaderRegister = 0;
	srv_range.OffsetInDescriptorsFromTableStart = 0;

	D3D12_ROOT_PARAMETER root_params[2] = {};
	root_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	root_params[0].Constants.ShaderRegister = 0;
	root_params[0].Constants.RegisterSpace = 0;
	root_params[0].Constants.Num32BitValues = 4;
	root_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	root_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	root_params[1].DescriptorTable.NumDescriptorRanges = 1;
	root_params[1].DescriptorTable.pDescriptorRanges = &srv_range;
	root_params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_STATIC_SAMPLER_DESC sampler = {};
	// Point filter — strip's RGB exact-equality test would smear with linear.
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
	sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.MaxLOD = D3D12_FLOAT32_MAX;
	sampler.ShaderRegister = 0;
	sampler.RegisterSpace = 0;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_ROOT_SIGNATURE_DESC rs_desc = {};
	rs_desc.NumParameters = 2;
	rs_desc.pParameters = root_params;
	rs_desc.NumStaticSamplers = 1;
	rs_desc.pStaticSamplers = &sampler;
	rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS;

	ID3DBlob *rs_blob = nullptr;
	ID3DBlob *err = nullptr;
	HRESULT hr = D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1,
	                                          &rs_blob, &err);
	if (FAILED(hr)) {
		U_LOG_E("Leia D3D12 DP: ck root sig serialize failed: 0x%08x %s",
		        (unsigned)hr, err ? (const char *)err->GetBufferPointer() : "");
		if (err) err->Release();
		return false;
	}
	if (err) err->Release();

	hr = ldp->device->CreateRootSignature(0, rs_blob->GetBufferPointer(),
	                                       rs_blob->GetBufferSize(),
	                                       __uuidof(ID3D12RootSignature),
	                                       reinterpret_cast<void **>(&ldp->ck_root_sig));
	rs_blob->Release();
	if (FAILED(hr)) {
		U_LOG_E("Leia D3D12 DP: ck root sig create failed: 0x%08x", (unsigned)hr);
		return false;
	}
	return true;
}

// Build a chroma-key PSO for the given pixel-shader source. RTV format is
// R8G8B8A8_UNORM in both passes (fill_tex format and back-buffer format).
static bool
ck_build_pso(struct leia_display_processor_d3d12_impl *ldp,
             const char *ps_source,
             ID3D12PipelineState **out_pso)
{
	ID3DBlob *vs_blob = ck_compile_shader(ck_vs_source, "main", "vs_5_0");
	if (vs_blob == nullptr) return false;
	ID3DBlob *ps_blob = ck_compile_shader(ps_source, "main", "ps_5_0");
	if (ps_blob == nullptr) {
		vs_blob->Release();
		return false;
	}

	D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
	pso_desc.pRootSignature = ldp->ck_root_sig;
	pso_desc.VS = {vs_blob->GetBufferPointer(), vs_blob->GetBufferSize()};
	pso_desc.PS = {ps_blob->GetBufferPointer(), ps_blob->GetBufferSize()};
	pso_desc.BlendState.RenderTarget[0].BlendEnable = FALSE;
	pso_desc.BlendState.RenderTarget[0].LogicOpEnable = FALSE;
	pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	pso_desc.SampleMask = UINT_MAX;
	pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	pso_desc.RasterizerState.DepthClipEnable = TRUE;
	pso_desc.DepthStencilState.DepthEnable = FALSE;
	pso_desc.DepthStencilState.StencilEnable = FALSE;
	pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pso_desc.NumRenderTargets = 1;
	pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	pso_desc.SampleDesc.Count = 1;

	HRESULT hr = ldp->device->CreateGraphicsPipelineState(
	    &pso_desc, __uuidof(ID3D12PipelineState),
	    reinterpret_cast<void **>(out_pso));
	vs_blob->Release();
	ps_blob->Release();
	if (FAILED(hr)) {
		U_LOG_E("Leia D3D12 DP: ck PSO create failed: 0x%08x", (unsigned)hr);
		return false;
	}
	return true;
}

// Lazy init root sig + 2 PSOs + 2 shader-visible SRV heaps + 1 RTV heap.
static bool
ck_init_pipeline(struct leia_display_processor_d3d12_impl *ldp)
{
	if (ldp->ck_fill_pso != nullptr && ldp->ck_strip_pso != nullptr) {
		return true;
	}
	if (ldp->device == nullptr) {
		return false;
	}

	if (ldp->ck_root_sig == nullptr && !ck_build_root_sig(ldp)) {
		return false;
	}
	if (ldp->ck_fill_pso == nullptr && !ck_build_pso(ldp, ck_fill_ps_source, &ldp->ck_fill_pso)) {
		return false;
	}
	if (ldp->ck_strip_pso == nullptr && !ck_build_pso(ldp, ck_strip_ps_source, &ldp->ck_strip_pso)) {
		return false;
	}

	if (ldp->ck_srv_heap_fill == nullptr) {
		D3D12_DESCRIPTOR_HEAP_DESC hd = {};
		hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		hd.NumDescriptors = 1;
		hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		HRESULT hr = ldp->device->CreateDescriptorHeap(
		    &hd, __uuidof(ID3D12DescriptorHeap),
		    reinterpret_cast<void **>(&ldp->ck_srv_heap_fill));
		if (FAILED(hr)) {
			U_LOG_E("Leia D3D12 DP: ck fill SRV heap create failed: 0x%08x", (unsigned)hr);
			return false;
		}
	}
	if (ldp->ck_srv_heap_strip == nullptr) {
		D3D12_DESCRIPTOR_HEAP_DESC hd = {};
		hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		hd.NumDescriptors = 1;
		hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		HRESULT hr = ldp->device->CreateDescriptorHeap(
		    &hd, __uuidof(ID3D12DescriptorHeap),
		    reinterpret_cast<void **>(&ldp->ck_srv_heap_strip));
		if (FAILED(hr)) {
			U_LOG_E("Leia D3D12 DP: ck strip SRV heap create failed: 0x%08x", (unsigned)hr);
			return false;
		}
	}
	if (ldp->ck_rtv_heap_fill == nullptr) {
		D3D12_DESCRIPTOR_HEAP_DESC hd = {};
		hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		hd.NumDescriptors = 1;
		hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		HRESULT hr = ldp->device->CreateDescriptorHeap(
		    &hd, __uuidof(ID3D12DescriptorHeap),
		    reinterpret_cast<void **>(&ldp->ck_rtv_heap_fill));
		if (FAILED(hr)) {
			U_LOG_E("Leia D3D12 DP: ck fill RTV heap create failed: 0x%08x", (unsigned)hr);
			return false;
		}
	}

	U_LOG_W("Leia D3D12 DP: chroma-key pipeline ready (key=0x%08X)", ldp->ck_color);
	return true;
}

// Allocate ck_fill_tex sized to the atlas. State at exit: PIXEL_SHADER_RESOURCE
// (so the weaver, which samples it next, sees it ready). Recreates RTV + SRV
// descriptors on every (re)alloc.
static bool
ck_ensure_fill_target(struct leia_display_processor_d3d12_impl *ldp,
                       uint32_t w, uint32_t h)
{
	if (ldp->ck_fill_tex != nullptr && ldp->ck_fill_w == w && ldp->ck_fill_h == h) {
		return true;
	}
	if (ldp->ck_fill_tex != nullptr) {
		ldp->ck_fill_tex->Release();
		ldp->ck_fill_tex = nullptr;
	}

	D3D12_HEAP_PROPERTIES heap_props = {};
	heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_DESC td = {};
	td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	td.Width = w;
	td.Height = h;
	td.DepthOrArraySize = 1;
	td.MipLevels = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

	HRESULT hr = ldp->device->CreateCommittedResource(
	    &heap_props, D3D12_HEAP_FLAG_NONE, &td,
	    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
	    __uuidof(ID3D12Resource),
	    reinterpret_cast<void **>(&ldp->ck_fill_tex));
	if (FAILED(hr)) {
		U_LOG_E("Leia D3D12 DP: ck fill tex create (%ux%u) failed: 0x%08x",
		        w, h, (unsigned)hr);
		return false;
	}
	ldp->ck_fill_w = w;
	ldp->ck_fill_h = h;
	ldp->ck_fill_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

	D3D12_RENDER_TARGET_VIEW_DESC rtv_desc = {};
	rtv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
	ldp->device->CreateRenderTargetView(
	    ldp->ck_fill_tex, &rtv_desc,
	    ldp->ck_rtv_heap_fill->GetCPUDescriptorHandleForHeapStart());
	return true;
}

// Allocate ck_strip_tex sized to the back buffer. State at exit:
// PIXEL_SHADER_RESOURCE.
static bool
ck_ensure_strip_source(struct leia_display_processor_d3d12_impl *ldp,
                        uint32_t w, uint32_t h)
{
	if (ldp->ck_strip_tex != nullptr && ldp->ck_strip_w == w && ldp->ck_strip_h == h) {
		return true;
	}
	if (ldp->ck_strip_tex != nullptr) {
		ldp->ck_strip_tex->Release();
		ldp->ck_strip_tex = nullptr;
	}

	D3D12_HEAP_PROPERTIES heap_props = {};
	heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_DESC td = {};
	td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	td.Width = w;
	td.Height = h;
	td.DepthOrArraySize = 1;
	td.MipLevels = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	td.Flags = D3D12_RESOURCE_FLAG_NONE;

	HRESULT hr = ldp->device->CreateCommittedResource(
	    &heap_props, D3D12_HEAP_FLAG_NONE, &td,
	    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
	    __uuidof(ID3D12Resource),
	    reinterpret_cast<void **>(&ldp->ck_strip_tex));
	if (FAILED(hr)) {
		U_LOG_E("Leia D3D12 DP: ck strip tex create (%ux%u) failed: 0x%08x",
		        w, h, (unsigned)hr);
		return false;
	}
	ldp->ck_strip_w = w;
	ldp->ck_strip_h = h;
	ldp->ck_strip_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

	D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
	srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv_desc.Texture2D.MipLevels = 1;
	ldp->device->CreateShaderResourceView(
	    ldp->ck_strip_tex, &srv_desc,
	    ldp->ck_srv_heap_strip->GetCPUDescriptorHandleForHeapStart());
	return true;
}

// Pack ck_color (0x00BBGGRR) → 4 root constants (R, G, B, pad).
static void
ck_root_constants(struct leia_display_processor_d3d12_impl *ldp, float out[4])
{
	uint32_t k = ldp->ck_color;
	out[0] = ((k >>  0) & 0xFF) / 255.0f; // R
	out[1] = ((k >>  8) & 0xFF) / 255.0f; // G
	out[2] = ((k >> 16) & 0xFF) / 255.0f; // B
	out[3] = 0.0f;
}

/*
 * Pre-weave fill: read RGBA atlas, write opaque RGB to ck_fill_tex with
 * alpha=0 regions filled by chroma_rgb. Returns the resource the weaver
 * should sample (ck_fill_tex on success, original atlas on fallback).
 *
 * The caller (process_atlas) has already set viewport+scissor and bound
 * the back-buffer RTV. This pass switches the RT binding to ck_fill_tex,
 * draws, then restores the caller's RTV via prev_rtv so the subsequent
 * weaver call writes to the right back buffer (D3D11's analog uses
 * OMGetRenderTargets to save+restore; D3D12 has no Getter on command
 * lists so prev_rtv is passed in explicitly).
 */
static ID3D12Resource *
ck_run_pre_weave_fill(struct leia_display_processor_d3d12_impl *ldp,
                       ID3D12GraphicsCommandList *cmd,
                       ID3D12Resource *atlas_resource,
                       uint32_t atlas_w, uint32_t atlas_h,
                       DXGI_FORMAT atlas_format,
                       D3D12_CPU_DESCRIPTOR_HANDLE prev_rtv)
{
	if (!ck_init_pipeline(ldp) || !ck_ensure_fill_target(ldp, atlas_w, atlas_h)) {
		return atlas_resource;
	}

	// Create SRV on the atlas resource in the fill heap (slot 0).
	D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
	srv_desc.Format = atlas_format;
	srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv_desc.Texture2D.MipLevels = 1;
	ldp->device->CreateShaderResourceView(
	    atlas_resource, &srv_desc,
	    ldp->ck_srv_heap_fill->GetCPUDescriptorHandleForHeapStart());

	// fill_tex PIXEL_SHADER_RESOURCE → RENDER_TARGET
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = ldp->ck_fill_tex;
	barrier.Transition.StateBefore = ldp->ck_fill_state;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	cmd->ResourceBarrier(1, &barrier);
	ldp->ck_fill_state = D3D12_RESOURCE_STATE_RENDER_TARGET;

	// Bind PSO + root sig + descriptor heap + chroma constants.
	cmd->SetPipelineState(ldp->ck_fill_pso);
	cmd->SetGraphicsRootSignature(ldp->ck_root_sig);
	ID3D12DescriptorHeap *heaps[] = {ldp->ck_srv_heap_fill};
	cmd->SetDescriptorHeaps(1, heaps);
	float root_consts[4];
	ck_root_constants(ldp, root_consts);
	cmd->SetGraphicsRoot32BitConstants(0, 4, root_consts, 0);
	cmd->SetGraphicsRootDescriptorTable(
	    1, ldp->ck_srv_heap_fill->GetGPUDescriptorHandleForHeapStart());

	D3D12_VIEWPORT vp = {0.0f, 0.0f, (float)atlas_w, (float)atlas_h, 0.0f, 1.0f};
	D3D12_RECT scissor = {0, 0, (LONG)atlas_w, (LONG)atlas_h};
	cmd->RSSetViewports(1, &vp);
	cmd->RSSetScissorRects(1, &scissor);

	D3D12_CPU_DESCRIPTOR_HANDLE rtv =
	    ldp->ck_rtv_heap_fill->GetCPUDescriptorHandleForHeapStart();
	cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
	cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	cmd->IASetVertexBuffers(0, 0, nullptr);
	cmd->IASetIndexBuffer(nullptr);
	cmd->DrawInstanced(3, 1, 0, 0);

	// fill_tex RENDER_TARGET → PIXEL_SHADER_RESOURCE (so weaver can sample).
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	cmd->ResourceBarrier(1, &barrier);
	ldp->ck_fill_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

	// Restore the back-buffer RTV so the subsequent weave writes to the
	// right target. D3D11's analog uses OMGetRenderTargets to save/restore;
	// D3D12 has no Get on the command list so the caller passes prev_rtv in.
	cmd->OMSetRenderTargets(1, &prev_rtv, FALSE, nullptr);

	return ldp->ck_fill_tex;
}

/*
 * Post-weave strip: copy back buffer → ck_strip_tex, then sample strip_tex
 * back to back-buffer RTV with alpha=0 where RGB matches chroma_rgb. Caller
 * passes back_buffer in RENDER_TARGET state; we transition through
 * COPY_SOURCE and back to RENDER_TARGET.
 */
static void
ck_run_post_weave_strip(struct leia_display_processor_d3d12_impl *ldp,
                         ID3D12GraphicsCommandList *cmd,
                         ID3D12Resource *back_buffer,
                         D3D12_CPU_DESCRIPTOR_HANDLE back_buffer_rtv,
                         uint32_t bb_w, uint32_t bb_h)
{
	if (!ck_init_pipeline(ldp) || !ck_ensure_strip_source(ldp, bb_w, bb_h)) {
		return;
	}

	// back_buffer RENDER_TARGET → COPY_SOURCE; strip_tex
	// PIXEL_SHADER_RESOURCE → COPY_DEST.
	D3D12_RESOURCE_BARRIER barriers[2] = {};
	barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barriers[0].Transition.pResource = back_buffer;
	barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barriers[1].Transition.pResource = ldp->ck_strip_tex;
	barriers[1].Transition.StateBefore = ldp->ck_strip_state;
	barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
	barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	cmd->ResourceBarrier(2, barriers);
	ldp->ck_strip_state = D3D12_RESOURCE_STATE_COPY_DEST;

	cmd->CopyResource(ldp->ck_strip_tex, back_buffer);

	// back_buffer COPY_SOURCE → RENDER_TARGET; strip_tex COPY_DEST →
	// PIXEL_SHADER_RESOURCE.
	barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
	barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	cmd->ResourceBarrier(2, barriers);
	ldp->ck_strip_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

	// Bind strip PSO + descriptors + chroma constants.
	cmd->SetPipelineState(ldp->ck_strip_pso);
	cmd->SetGraphicsRootSignature(ldp->ck_root_sig);
	ID3D12DescriptorHeap *heaps[] = {ldp->ck_srv_heap_strip};
	cmd->SetDescriptorHeaps(1, heaps);
	float root_consts[4];
	ck_root_constants(ldp, root_consts);
	cmd->SetGraphicsRoot32BitConstants(0, 4, root_consts, 0);
	cmd->SetGraphicsRootDescriptorTable(
	    1, ldp->ck_srv_heap_strip->GetGPUDescriptorHandleForHeapStart());

	D3D12_VIEWPORT vp = {0.0f, 0.0f, (float)bb_w, (float)bb_h, 0.0f, 1.0f};
	D3D12_RECT scissor = {0, 0, (LONG)bb_w, (LONG)bb_h};
	cmd->RSSetViewports(1, &vp);
	cmd->RSSetScissorRects(1, &scissor);

	cmd->OMSetRenderTargets(1, &back_buffer_rtv, FALSE, nullptr);
	cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	cmd->IASetVertexBuffers(0, 0, nullptr);
	cmd->IASetIndexBuffer(nullptr);
	cmd->DrawInstanced(3, 1, 0, 0);
}

static void
ck_release_resources(struct leia_display_processor_d3d12_impl *ldp)
{
	if (ldp->ck_fill_tex)        { ldp->ck_fill_tex->Release();        ldp->ck_fill_tex = nullptr; }
	if (ldp->ck_strip_tex)       { ldp->ck_strip_tex->Release();       ldp->ck_strip_tex = nullptr; }
	if (ldp->ck_rtv_heap_fill)   { ldp->ck_rtv_heap_fill->Release();   ldp->ck_rtv_heap_fill = nullptr; }
	if (ldp->ck_srv_heap_strip)  { ldp->ck_srv_heap_strip->Release();  ldp->ck_srv_heap_strip = nullptr; }
	if (ldp->ck_srv_heap_fill)   { ldp->ck_srv_heap_fill->Release();   ldp->ck_srv_heap_fill = nullptr; }
	if (ldp->ck_strip_pso)       { ldp->ck_strip_pso->Release();       ldp->ck_strip_pso = nullptr; }
	if (ldp->ck_fill_pso)        { ldp->ck_fill_pso->Release();        ldp->ck_fill_pso = nullptr; }
	if (ldp->ck_root_sig)        { ldp->ck_root_sig->Release();        ldp->ck_root_sig = nullptr; }
}


/*
 *
 * Compose-under-bg pipeline (preferred path when WGC is available).
 * Reuses ck_fill_tex / ck_rtv_heap_fill as the intermediate render target.
 *
 */

static bool
compose_should_run(struct leia_display_processor_d3d12_impl *ldp)
{
	return ldp->bg_compose_enabled && ldp->bg_capture != nullptr && ldp->bg_shared_tex != nullptr;
}

// Root signature: 8 32-bit constants (bg_uv_origin xy + bg_uv_extent xy +
// tile_count xy + pad xy = 32 bytes) at b0 + 2-SRV descriptor table (t0,t1)
// + static linear sampler at s0.
static bool
compose_build_root_sig(struct leia_display_processor_d3d12_impl *ldp)
{
	D3D12_DESCRIPTOR_RANGE srv_range = {};
	srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	srv_range.NumDescriptors = 2;
	srv_range.BaseShaderRegister = 0;
	srv_range.OffsetInDescriptorsFromTableStart = 0;

	D3D12_ROOT_PARAMETER params[2] = {};
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	params[0].Constants.ShaderRegister = 0;
	params[0].Constants.Num32BitValues = 8;
	params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[1].DescriptorTable.NumDescriptorRanges = 1;
	params[1].DescriptorTable.pDescriptorRanges = &srv_range;
	params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_STATIC_SAMPLER_DESC sampler = {};
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.MaxLOD = D3D12_FLOAT32_MAX;
	sampler.ShaderRegister = 0;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_ROOT_SIGNATURE_DESC desc = {};
	desc.NumParameters = 2;
	desc.pParameters = params;
	desc.NumStaticSamplers = 1;
	desc.pStaticSamplers = &sampler;
	desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS;

	ID3DBlob *blob = nullptr;
	ID3DBlob *err = nullptr;
	HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
	if (FAILED(hr)) {
		U_LOG_E("Leia D3D12 DP: compose root sig serialize failed: 0x%08x %s",
		        (unsigned)hr, err ? (const char *)err->GetBufferPointer() : "");
		if (err) err->Release();
		return false;
	}
	if (err) err->Release();
	hr = ldp->device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
	                                       __uuidof(ID3D12RootSignature),
	                                       reinterpret_cast<void **>(&ldp->compose_root_sig));
	blob->Release();
	if (FAILED(hr)) {
		U_LOG_E("Leia D3D12 DP: compose root sig create failed: 0x%08x", (unsigned)hr);
		return false;
	}
	return true;
}

static bool
compose_init_pipeline(struct leia_display_processor_d3d12_impl *ldp)
{
	if (ldp->compose_pso != nullptr && ldp->compose_srv_heap != nullptr) {
		return true;
	}
	if (ldp->device == nullptr) {
		return false;
	}
	if (ldp->compose_root_sig == nullptr && !compose_build_root_sig(ldp)) {
		return false;
	}
	if (ldp->compose_pso == nullptr) {
		ID3DBlob *vs_blob = ck_compile_shader(ck_vs_source, "main", "vs_5_0");
		if (vs_blob == nullptr) return false;
		ID3DBlob *ps_blob = ck_compile_shader(compose_under_bg_ps_source, "main", "ps_5_0");
		if (ps_blob == nullptr) { vs_blob->Release(); return false; }

		D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
		pd.pRootSignature = ldp->compose_root_sig;
		pd.VS = {vs_blob->GetBufferPointer(), vs_blob->GetBufferSize()};
		pd.PS = {ps_blob->GetBufferPointer(), ps_blob->GetBufferSize()};
		pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		pd.SampleMask = UINT_MAX;
		pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		pd.RasterizerState.DepthClipEnable = TRUE;
		pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		pd.NumRenderTargets = 1;
		pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
		pd.SampleDesc.Count = 1;
		HRESULT hr = ldp->device->CreateGraphicsPipelineState(
		    &pd, __uuidof(ID3D12PipelineState),
		    reinterpret_cast<void **>(&ldp->compose_pso));
		vs_blob->Release();
		ps_blob->Release();
		if (FAILED(hr)) {
			U_LOG_E("Leia D3D12 DP: compose PSO create failed: 0x%08x", (unsigned)hr);
			return false;
		}
	}
	if (ldp->compose_srv_heap == nullptr) {
		D3D12_DESCRIPTOR_HEAP_DESC hd = {};
		hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		hd.NumDescriptors = 2;
		hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		HRESULT hr = ldp->device->CreateDescriptorHeap(
		    &hd, __uuidof(ID3D12DescriptorHeap),
		    reinterpret_cast<void **>(&ldp->compose_srv_heap));
		if (FAILED(hr)) {
			U_LOG_E("Leia D3D12 DP: compose SRV heap create failed: 0x%08x", (unsigned)hr);
			return false;
		}
		ldp->cbv_srv_desc_size = ldp->device->GetDescriptorHandleIncrementSize(
		    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

		// Slot 1: bg SRV (stable across frames — bg_shared_tex doesn't change).
		D3D12_SHADER_RESOURCE_VIEW_DESC bg_srv = {};
		bg_srv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		bg_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		bg_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		bg_srv.Texture2D.MipLevels = 1;
		D3D12_CPU_DESCRIPTOR_HANDLE bg_cpu =
		    ldp->compose_srv_heap->GetCPUDescriptorHandleForHeapStart();
		bg_cpu.ptr += ldp->cbv_srv_desc_size;
		ldp->device->CreateShaderResourceView(ldp->bg_shared_tex, &bg_srv, bg_cpu);
	}
	U_LOG_W("Leia D3D12 DP: compose-under-bg pipeline ready");
	return true;
}

/*
 * Pre-weave compose-under-bg. Captures the latest desktop frame, composes
 * it under the atlas per tile, writes opaque RGB into ck_fill_tex. Returns
 * ck_fill_tex on success (weaver samples it), original atlas on fallback.
 *
 * Caller passes prev_rtv (current back-buffer RTV) to restore after we
 * change the binding to ck_fill_tex.
 */
static ID3D12Resource *
compose_run_pre_weave(struct leia_display_processor_d3d12_impl *ldp,
                      ID3D12GraphicsCommandList *cmd,
                      ID3D12Resource *atlas_resource,
                      uint32_t atlas_w, uint32_t atlas_h,
                      DXGI_FORMAT atlas_format,
                      uint32_t tile_columns, uint32_t tile_rows,
                      D3D12_CPU_DESCRIPTOR_HANDLE prev_rtv)
{
	if (!compose_init_pipeline(ldp) || !ck_ensure_fill_target(ldp, atlas_w, atlas_h)) {
		return atlas_resource;
	}

	float bg_origin[2] = {0.0f, 0.0f};
	float bg_extent[2] = {0.0f, 0.0f};
	uint64_t fence_value = 0;
	bool have_bg = leia_bg_capture_poll(ldp->bg_capture, bg_origin, bg_extent, &fence_value);
	if (!have_bg) {
		return atlas_resource;
	}

	// Order the consumer's GPU work after the producer's signal. Queue Wait
	// gates all subsequently-executed cmd lists on this queue.
	if (ldp->bg_fence != nullptr && ldp->command_queue != nullptr && fence_value > 0) {
		ldp->command_queue->Wait(ldp->bg_fence, fence_value);
	}

	// Slot 0: atlas SRV (refreshed each frame — atlas resource may change).
	D3D12_SHADER_RESOURCE_VIEW_DESC atlas_srv = {};
	atlas_srv.Format = atlas_format;
	atlas_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	atlas_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	atlas_srv.Texture2D.MipLevels = 1;
	ldp->device->CreateShaderResourceView(
	    atlas_resource, &atlas_srv,
	    ldp->compose_srv_heap->GetCPUDescriptorHandleForHeapStart());

	// ck_fill_tex PIXEL_SHADER_RESOURCE → RENDER_TARGET
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = ldp->ck_fill_tex;
	barrier.Transition.StateBefore = ldp->ck_fill_state;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	cmd->ResourceBarrier(1, &barrier);
	ldp->ck_fill_state = D3D12_RESOURCE_STATE_RENDER_TARGET;

	cmd->SetPipelineState(ldp->compose_pso);
	cmd->SetGraphicsRootSignature(ldp->compose_root_sig);
	ID3D12DescriptorHeap *heaps[] = {ldp->compose_srv_heap};
	cmd->SetDescriptorHeaps(1, heaps);
	uint32_t consts[8];
	memcpy(&consts[0], &bg_origin[0], sizeof(float));
	memcpy(&consts[1], &bg_origin[1], sizeof(float));
	memcpy(&consts[2], &bg_extent[0], sizeof(float));
	memcpy(&consts[3], &bg_extent[1], sizeof(float));
	consts[4] = tile_columns;
	consts[5] = tile_rows;
	consts[6] = 0;
	consts[7] = 0;
	cmd->SetGraphicsRoot32BitConstants(0, 8, consts, 0);
	cmd->SetGraphicsRootDescriptorTable(
	    1, ldp->compose_srv_heap->GetGPUDescriptorHandleForHeapStart());

	D3D12_VIEWPORT vp = {0.0f, 0.0f, (float)atlas_w, (float)atlas_h, 0.0f, 1.0f};
	D3D12_RECT scissor = {0, 0, (LONG)atlas_w, (LONG)atlas_h};
	cmd->RSSetViewports(1, &vp);
	cmd->RSSetScissorRects(1, &scissor);

	D3D12_CPU_DESCRIPTOR_HANDLE rtv =
	    ldp->ck_rtv_heap_fill->GetCPUDescriptorHandleForHeapStart();
	cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
	cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	cmd->IASetVertexBuffers(0, 0, nullptr);
	cmd->IASetIndexBuffer(nullptr);
	cmd->DrawInstanced(3, 1, 0, 0);

	// ck_fill_tex RENDER_TARGET → PIXEL_SHADER_RESOURCE (weaver will sample).
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	cmd->ResourceBarrier(1, &barrier);
	ldp->ck_fill_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

	cmd->OMSetRenderTargets(1, &prev_rtv, FALSE, nullptr);
	return ldp->ck_fill_tex;
}

static void
compose_release_resources(struct leia_display_processor_d3d12_impl *ldp)
{
	if (ldp->compose_srv_heap) { ldp->compose_srv_heap->Release(); ldp->compose_srv_heap = nullptr; }
	if (ldp->compose_pso)      { ldp->compose_pso->Release();      ldp->compose_pso = nullptr; }
	if (ldp->compose_root_sig) { ldp->compose_root_sig->Release(); ldp->compose_root_sig = nullptr; }
	if (ldp->bg_fence)         { ldp->bg_fence->Release();         ldp->bg_fence = nullptr; }
	if (ldp->bg_shared_tex)    { ldp->bg_shared_tex->Release();    ldp->bg_shared_tex = nullptr; }
	if (ldp->bg_capture)       { leia_bg_capture_destroy(ldp->bg_capture); ldp->bg_capture = nullptr; }
	ldp->bg_compose_enabled = false;
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

		// Compose-under-bg: pre-composite captured desktop under the atlas,
		// then stretch-blit the opaque result. Replaces the post-weave strip
		// path for 2D mode when WGC capture is active.
		if (compose_should_run(ldp)) {
			uint32_t atlas_w = tile_columns * view_width;
			uint32_t atlas_h = tile_rows * view_height;
			D3D12_CPU_DESCRIPTOR_HANDLE bb_rtv;
			bb_rtv.ptr = static_cast<SIZE_T>(target_rtv_cpu_handle);
			ID3D12Resource *composed = compose_run_pre_weave(
			    ldp, cmd, atlas_res, atlas_w, atlas_h,
			    static_cast<DXGI_FORMAT>(format), tile_columns, tile_rows, bb_rtv);
			if (composed != nullptr) {
				atlas_res = composed;
			}
		}

		// Create SRV for the (possibly pre-composed) atlas resource in our
		// shader-visible heap. The compose path produces an opaque RGBA8 result
		// in ck_fill_tex; the original-atlas format may differ (e.g. SRGB).
		D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
		srv_desc.Format = compose_should_run(ldp) ? DXGI_FORMAT_R8G8B8A8_UNORM
		                                          : static_cast<DXGI_FORMAT>(format);
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

		// 2D mode has no weaver, but if chroma-key is enabled the legacy
		// flow (app pre-fills with key RGB on alpha=1 surface) still needs
		// the strip pass to recover alpha=0 for DWM. The new flow (true
		// alpha through the blit) works without it; running strip in that
		// case is a no-op for non-key pixels (RGB=0 doesn't match magenta).
		// Compose-under-bg path is fully opaque already — strip is unnecessary.
		if (!compose_should_run(ldp) && ck_should_run(ldp) && target_resource != NULL) {
			D3D12_CPU_DESCRIPTOR_HANDLE bb_rtv;
			bb_rtv.ptr = static_cast<SIZE_T>(target_rtv_cpu_handle);
			ck_run_post_weave_strip(
			    ldp, cmd,
			    static_cast<ID3D12Resource *>(target_resource),
			    bb_rtv, target_width, target_height);
		}
		return;
	}

	(void)atlas_srv_gpu_handle;

	ID3D12GraphicsCommandList *cmd_3d =
	    static_cast<ID3D12GraphicsCommandList *>(d3d12_command_list);

	// Atlas is guaranteed content-sized SBS (2*view_width x view_height)
	// by compositor crop-blit.
	//
	// Two transparency paths feed the SR weaver opaque RGB:
	//   1. Compose-under-bg (preferred): pre-composite captured desktop under
	//      each per-view tile so the weaver consumes opaque RGB with the
	//      desktop already integrated. No post-weave pass. Quality-correct
	//      on AA edges and semi-transparent pixels.
	//   2. Chroma-key (fallback): replace alpha=0 with key color before
	//      weaver, strip back to alpha=0 after. Hard edges, used when WGC
	//      is unavailable.
	ID3D12Resource *weaver_input = static_cast<ID3D12Resource *>(atlas_texture_resource);
	uint32_t atlas_w = tile_columns * view_width;
	uint32_t atlas_h = tile_rows * view_height;
	D3D12_CPU_DESCRIPTOR_HANDLE bb_rtv;
	bb_rtv.ptr = static_cast<SIZE_T>(target_rtv_cpu_handle);
	if (compose_should_run(ldp) && weaver_input != NULL) {
		weaver_input = compose_run_pre_weave(
		    ldp, cmd_3d, weaver_input, atlas_w, atlas_h,
		    static_cast<DXGI_FORMAT>(format), tile_columns, tile_rows, bb_rtv);
	} else if (ck_should_run(ldp) && weaver_input != NULL) {
		weaver_input = ck_run_pre_weave_fill(
		    ldp, cmd_3d, weaver_input, atlas_w, atlas_h,
		    static_cast<DXGI_FORMAT>(format), bb_rtv);
	}

	if (weaver_input != NULL) {
		leiasr_d3d12_set_input_texture(ldp->leiasr, weaver_input,
		                               view_width, view_height, format);
	}

	// vp_x/vp_y/vp_w/vp_h carry the canvas sub-rect. leiasr_d3d12_weave
	// applies them via RSSetViewports/RSSetScissorRects on the cmd list —
	// the weaver's setViewport/setScissorRect alone do NOT scope the draw.
	// See gotcha at leiasr_d3d12_weave().
	leiasr_d3d12_weave(ldp->leiasr, d3d12_command_list, vp_x, vp_y, vp_w, vp_h);

	// Post-weave chroma-key strip: convert key-color RGB pixels in the woven
	// back buffer to alpha=0 with RGB premultiplied for DWM. No-op when
	// disabled, suppressed entirely when compose-under-bg ran (the back
	// buffer is already opaque from end-to-end compose pipeline).
	if (!compose_should_run(ldp) && ck_should_run(ldp) && target_resource != NULL) {
		D3D12_CPU_DESCRIPTOR_HANDLE bb_rtv_strip;
		bb_rtv_strip.ptr = static_cast<SIZE_T>(target_rtv_cpu_handle);
		ck_run_post_weave_strip(
		    ldp, cmd_3d,
		    static_cast<ID3D12Resource *>(target_resource),
		    bb_rtv_strip, target_width, target_height);
	}
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

static bool
leia_dp_d3d12_is_alpha_native(struct xrt_display_processor_d3d12 *xdp)
{
	(void)xdp;
	// SR SDK D3D12 weaver interlaces into opaque RGB — alpha is destroyed.
	// Transparency is recovered via the chroma-key trick (see set_chroma_key).
	return false;
}

static void
leia_dp_d3d12_set_chroma_key(struct xrt_display_processor_d3d12 *xdp,
                              uint32_t key_color,
                              bool transparent_bg_enabled)
{
	struct leia_display_processor_d3d12_impl *ldp = leia_dp_d3d12(xdp);

	// Preserve ck_color/ck_enabled regardless of path — chroma-key is the
	// fallback if WGC init fails.
	ldp->ck_color = (key_color != 0) ? key_color : kDefaultChromaKey;
	ldp->ck_enabled = transparent_bg_enabled;

	// Preferred path: WGC desktop capture + per-tile compose-under-bg.
	// On any failure (older Windows, capture blocked, env-disabled), fall
	// back to chroma-key.
	if (transparent_bg_enabled && !ldp->bg_compose_enabled && ldp->hwnd != nullptr) {
		ldp->bg_capture = leia_bg_capture_create(ldp->hwnd);
		if (ldp->bg_capture != nullptr && ldp->device != nullptr) {
			HRESULT hr = leia_bg_capture_open_d3d12(
			    ldp->bg_capture, ldp->device, &ldp->bg_shared_tex);
			if (SUCCEEDED(hr)) {
				hr = leia_bg_capture_open_fence_d3d12(
				    ldp->bg_capture, ldp->device, &ldp->bg_fence);
			}
			if (SUCCEEDED(hr)) {
				ldp->bg_compose_enabled = true;
				ldp->ck_enabled = false;
				U_LOG_W("Leia D3D12 DP: transparency = compose-under-bg (WGC)");
			} else {
				U_LOG_W("Leia D3D12 DP: WGC import failed (0x%08x) — falling back to chroma-key",
				        (unsigned)hr);
				compose_release_resources(ldp);
			}
		}
	}
	if (!ldp->bg_compose_enabled) {
		U_LOG_W("Leia D3D12 DP: transparency = chroma-key %s (key=0x%08X%s)",
		        ldp->ck_enabled ? "ENABLED" : "disabled",
		        ldp->ck_color,
		        (key_color == 0) ? " — DP default" : " — app override");
	}
}

static void
leia_dp_d3d12_destroy(struct xrt_display_processor_d3d12 *xdp)
{
	struct leia_display_processor_d3d12_impl *ldp = leia_dp_d3d12(xdp);

	compose_release_resources(ldp);
	ck_release_resources(ldp);

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
	ldp->base.is_alpha_native = leia_dp_d3d12_is_alpha_native;
	ldp->base.set_chroma_key = leia_dp_d3d12_set_chroma_key;
	ldp->base.destroy = leia_dp_d3d12_destroy;
	ldp->leiasr = weaver;
	ldp->device = static_cast<ID3D12Device *>(d3d12_device);
	ldp->hwnd = static_cast<HWND>(window_handle);
	ldp->command_queue = static_cast<ID3D12CommandQueue *>(d3d12_command_queue);
	ldp->view_count = 2;

	// Init blit root signature and SRV heap for 2D passthrough mode
	if (!leia_dp_d3d12_init_blit(ldp)) {
		U_LOG_W("Leia D3D12 DP: blit init failed — 2D mode will be unavailable");
	}

	*out_xdp = &ldp->base;

	U_LOG_W("Created Leia SR D3D12 display processor (factory, owns weaver)");

	return XRT_SUCCESS;
}
