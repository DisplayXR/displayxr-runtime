// Copyright 2024-2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  D3D11 output composite implementation (masked 2D-over-3D pass).
 * @author David Fattal
 * @ingroup comp_d3d11
 *
 * #918 Phase 2a. Extracted verbatim from comp_d3d11_renderer's masked composite
 * so the pass no longer reaches through the renderer (and, via the renderer's
 * hand-maintained prefix mirror, through the compositor) for its device. The
 * destination of this pass is the WEAVE TARGET, which Phase 1 already moved to
 * the scanout device under DXR_WEAVE_ON_SCANOUT — so the pass has to be able to
 * live on a device the renderer knows nothing about.
 *
 * Everything here is device-scoped and owned by the unit; the device/context
 * themselves are BORROWED (no AddRef/Release), like comp_d3d11_target's.
 */

#include "comp_d3d11_outcomp.h"
#include "comp_d3d11_composite_shaders.h"

#include "util/u_logging.h"
#include "util/u_misc.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>

#include <cstdlib>
#include <cstring>

/*!
 * Masked 2D-over-3D composite constant buffer (unified-2d-3d-compositing #439,
 * Phase 0). Matches `cbuffer CompositeParams : register(b0)` in
 * comp_d3d11_composite_shaders.h / shaders/masked_composite.hlsl. 64 bytes;
 * HLSL packs as four float4 rows with no straddle (dst_dims.xy |
 * canvas_origin.xy , canvas_size.xy | mask | mode , opaque_present | pad0 |
 * twod_uv_scale.xy , mask_uv_scale.xy | pad) — note the deliberate `pad0`,
 * which is what keeps `twod_uv_scale` from straddling a 16-byte boundary.
 */
struct CompositeParams
{
	float dst_dims[2];       // destination width,height in px
	float canvas_origin[2];  // 3D canvas sub-rect top-left (px)
	float canvas_size[2];    // 3D canvas sub-rect size (px)
	uint32_t use_rect_mask;  // 1 = Phase 0 analytic rect mask
	uint32_t composite_mode; // COMP_D3D11_COMPOSITE_MODE_*: 0 hard M-lerp, 1 #491 over, 2 zones
	uint32_t opaque_present; // #833/#116: 1 = flatten (DWM completes no blends)
	uint32_t pad0;
	//! #918 Phase 2a: region / source extent, per input. 1.0 whenever the input
	//! is exactly region-sized, which is every input on the non-split path; the
	//! bridge planes are panel-sized and need the sub-rect scale.
	float twod_uv_scale[2];
	float mask_uv_scale[2];
	uint32_t pad1[2];
};
static_assert(sizeof(CompositeParams) == 64, "CompositeParams must match the HLSL cbuffer packing");

/*!
 * #918 Phase 2a — the uv scale for one input SRV: how much of it the composite
 * region occupies. Reading the extent off the resource rather than taking it as
 * another parameter is deliberate — a caller that hands in a bigger texture gets
 * the right sub-rect automatically, instead of silently stretching it across the
 * pass. Once per composite call, so the QI cost is per frame, not per pixel.
 */
static void
outcomp_uv_scale(void *srv_ptr, uint32_t region_w, uint32_t region_h, float out_scale[2])
{
	out_scale[0] = 1.0f;
	out_scale[1] = 1.0f;
	auto *srv = static_cast<ID3D11ShaderResourceView *>(srv_ptr);
	if (srv == nullptr || region_w == 0 || region_h == 0) {
		return;
	}
	ID3D11Resource *res = nullptr;
	srv->GetResource(&res);
	if (res == nullptr) {
		return;
	}
	ID3D11Texture2D *tex = nullptr;
	if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&tex))) &&
	    tex != nullptr) {
		D3D11_TEXTURE2D_DESC td = {};
		tex->GetDesc(&td);
		if (td.Width > region_w) {
			out_scale[0] = (float)region_w / (float)td.Width;
		}
		if (td.Height > region_h) {
			out_scale[1] = (float)region_h / (float)td.Height;
		}
		tex->Release();
	}
	res->Release();
}

/*!
 * The output composite unit.
 */
struct comp_d3d11_outcomp
{
	//! BORROWED device/context — the caller outlives this unit and owns the
	//! references (no AddRef/Release here), same contract as comp_d3d11_target.
	ID3D11Device *dev;
	ID3D11DeviceContext *ctx;

	//! Masked 2D-over-3D composite shaders (#439 Phase 0).
	ID3D11VertexShader *composite_vs;
	ID3D11PixelShader *composite_ps;
	//! Constant buffer for the composite pass (CompositeParams).
	ID3D11Buffer *composite_cb;

	//! Point sampler — the composite samples region-sized inputs 1:1, so any
	//! filtering would be a resampling error, not a smoothing choice.
	ID3D11SamplerState *sampler_point;

	//! Fixed-function state for the pass: no blend (the shader emits the
	//! finished pixel), no cull, no depth.
	ID3D11BlendState *blend_opaque;
	ID3D11RasterizerState *rasterizer_state;
	ID3D11DepthStencilState *depth_stencil_state;

	//! SRV-capable scratch snapshot of the weave target's window region, for
	//! the authored-mask lerp (the mask path reads the weave; the target is
	//! RTV-only). Lazily (re)allocated window-sized (#464).
	ID3D11Texture2D *weave_scratch;
	ID3D11ShaderResourceView *weave_scratch_srv;
};

#define SAFE_RELEASE(p)                                                                                                \
	do {                                                                                                           \
		if ((p) != nullptr) {                                                                                  \
			(p)->Release();                                                                                \
			(p) = nullptr;                                                                                 \
		}                                                                                                      \
	} while (0)

static xrt_result_t
outcomp_compile_shader(const char *source, const char *entry, const char *target, ID3DBlob **out_blob)
{
	ID3DBlob *errors = nullptr;
	HRESULT hr =
	    D3DCompile(source, strlen(source), nullptr, nullptr, nullptr, entry, target, 0, 0, out_blob, &errors);
	if (FAILED(hr)) {
		if (errors != nullptr) {
			U_LOG_E("Shader compile error: %s", (char *)errors->GetBufferPointer());
			errors->Release();
		}
		return XRT_ERROR_D3D;
	}
	if (errors != nullptr) {
		errors->Release();
	}
	return XRT_SUCCESS;
}

// (Re)allocate an SRV-capable DEFAULT-usage scratch texture + SRV to the
// given dims/format (no-op when it already matches). Returns false on
// allocation failure (with *tex/*srv released and nulled).
//
// #918: takes the device explicitly — it moved here with weave_scratch, its
// only consumer, so the snapshot is always allocated on the composite's device.
static bool
d3d11_ensure_srv_scratch(ID3D11Device *device,
                         ID3D11Texture2D **tex,
                         ID3D11ShaderResourceView **srv,
                         uint32_t w,
                         uint32_t h,
                         DXGI_FORMAT fmt,
                         const char *what)
{
	bool need_alloc = *tex == nullptr;
	if (!need_alloc) {
		D3D11_TEXTURE2D_DESC cur;
		(*tex)->GetDesc(&cur);
		need_alloc = (cur.Width != w || cur.Height != h || cur.Format != fmt);
	}
	if (!need_alloc) {
		return true;
	}
	if (*srv != nullptr) {
		(*srv)->Release();
		*srv = nullptr;
	}
	if (*tex != nullptr) {
		(*tex)->Release();
		*tex = nullptr;
	}
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = w;
	td.Height = h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = fmt;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	HRESULT hr = device->CreateTexture2D(&td, nullptr, tex);
	if (FAILED(hr) || *tex == nullptr) {
		U_LOG_W("%s: scratch alloc (%ux%u fmt=%u) failed: 0x%08x", what, w, h, fmt, hr);
		return false;
	}
	hr = device->CreateShaderResourceView(*tex, nullptr, srv);
	if (FAILED(hr) || *srv == nullptr) {
		U_LOG_W("%s: scratch SRV failed: 0x%08x", what, hr);
		(*tex)->Release();
		*tex = nullptr;
		return false;
	}
	return true;
}

extern "C" xrt_result_t
comp_d3d11_outcomp_create(void *device, void *context, struct comp_d3d11_outcomp **out_outcomp)
{
	if (device == nullptr || context == nullptr || out_outcomp == nullptr) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	struct comp_d3d11_outcomp *oc = U_TYPED_CALLOC(struct comp_d3d11_outcomp);
	if (oc == nullptr) {
		return XRT_ERROR_ALLOCATION;
	}
	oc->dev = static_cast<ID3D11Device *>(device);
	oc->ctx = static_cast<ID3D11DeviceContext *>(context);

	ID3DBlob *blob = nullptr;

	// Masked 2D-over-3D composite vertex shader (#439 Phase 0).
	xrt_result_t xret = outcomp_compile_shader(masked_composite_vs_source, "VSMain", "vs_5_0", &blob);
	if (xret != XRT_SUCCESS) {
		U_LOG_E("Failed to compile composite vertex shader");
		comp_d3d11_outcomp_destroy(&oc);
		return xret;
	}
	HRESULT hr =
	    oc->dev->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &oc->composite_vs);
	blob->Release();
	if (FAILED(hr)) {
		U_LOG_E("Failed to create composite vertex shader: 0x%08x", hr);
		comp_d3d11_outcomp_destroy(&oc);
		return XRT_ERROR_D3D;
	}

	// Masked 2D-over-3D composite pixel shader (#439 Phase 0).
	xret = outcomp_compile_shader(masked_composite_ps_source, "PSMain", "ps_5_0", &blob);
	if (xret != XRT_SUCCESS) {
		U_LOG_E("Failed to compile composite pixel shader");
		comp_d3d11_outcomp_destroy(&oc);
		return xret;
	}
	hr = oc->dev->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &oc->composite_ps);
	blob->Release();
	if (FAILED(hr)) {
		U_LOG_E("Failed to create composite pixel shader: 0x%08x", hr);
		comp_d3d11_outcomp_destroy(&oc);
		return XRT_ERROR_D3D;
	}

	// Composite constant buffer (#439 Phase 0).
	D3D11_BUFFER_DESC compCbDesc = {};
	compCbDesc.ByteWidth = sizeof(CompositeParams);
	compCbDesc.Usage = D3D11_USAGE_DYNAMIC;
	compCbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	compCbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	hr = oc->dev->CreateBuffer(&compCbDesc, nullptr, &oc->composite_cb);
	if (FAILED(hr)) {
		U_LOG_E("Failed to create composite constant buffer: 0x%08x", hr);
		comp_d3d11_outcomp_destroy(&oc);
		return XRT_ERROR_D3D;
	}

	// Point sampler.
	D3D11_SAMPLER_DESC sampDesc = {};
	sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	hr = oc->dev->CreateSamplerState(&sampDesc, &oc->sampler_point);
	if (FAILED(hr)) {
		U_LOG_E("Failed to create point sampler: 0x%08x", hr);
		comp_d3d11_outcomp_destroy(&oc);
		return XRT_ERROR_D3D;
	}

	// Opaque blend state (BlendEnable FALSE; the rest of the desc is the
	// renderer's alpha desc with blending switched off, kept identical so the
	// write mask and alpha ops stay what they always were).
	D3D11_BLEND_DESC blendDesc = {};
	blendDesc.RenderTarget[0].BlendEnable = FALSE;
	blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
	blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
	blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
	blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
	blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	hr = oc->dev->CreateBlendState(&blendDesc, &oc->blend_opaque);
	if (FAILED(hr)) {
		U_LOG_E("Failed to create opaque blend state: 0x%08x", hr);
		comp_d3d11_outcomp_destroy(&oc);
		return XRT_ERROR_D3D;
	}

	// Rasterizer state.
	D3D11_RASTERIZER_DESC rasterDesc = {};
	rasterDesc.FillMode = D3D11_FILL_SOLID;
	rasterDesc.CullMode = D3D11_CULL_NONE;
	rasterDesc.FrontCounterClockwise = FALSE;
	rasterDesc.DepthClipEnable = TRUE;
	hr = oc->dev->CreateRasterizerState(&rasterDesc, &oc->rasterizer_state);
	if (FAILED(hr)) {
		U_LOG_E("Failed to create rasterizer state: 0x%08x", hr);
		comp_d3d11_outcomp_destroy(&oc);
		return XRT_ERROR_D3D;
	}

	// Depth stencil state.
	D3D11_DEPTH_STENCIL_DESC dsDesc = {};
	dsDesc.DepthEnable = FALSE;
	dsDesc.StencilEnable = FALSE;
	hr = oc->dev->CreateDepthStencilState(&dsDesc, &oc->depth_stencil_state);
	if (FAILED(hr)) {
		U_LOG_E("Failed to create depth stencil state: 0x%08x", hr);
		comp_d3d11_outcomp_destroy(&oc);
		return XRT_ERROR_D3D;
	}

	*out_outcomp = oc;
	return XRT_SUCCESS;
}

extern "C" void
comp_d3d11_outcomp_destroy(struct comp_d3d11_outcomp **outcomp_ptr)
{
	if (outcomp_ptr == nullptr || *outcomp_ptr == nullptr) {
		return;
	}
	struct comp_d3d11_outcomp *oc = *outcomp_ptr;

	SAFE_RELEASE(oc->weave_scratch_srv);
	SAFE_RELEASE(oc->weave_scratch);
	SAFE_RELEASE(oc->depth_stencil_state);
	SAFE_RELEASE(oc->rasterizer_state);
	SAFE_RELEASE(oc->blend_opaque);
	SAFE_RELEASE(oc->sampler_point);
	SAFE_RELEASE(oc->composite_cb);
	SAFE_RELEASE(oc->composite_ps);
	SAFE_RELEASE(oc->composite_vs);
	// dev/ctx are borrowed — never released here.

	free(oc);
	*outcomp_ptr = nullptr;
}

extern "C" bool
comp_d3d11_outcomp_ensure_weave_scratch(struct comp_d3d11_outcomp *outcomp,
                                        uint32_t w,
                                        uint32_t h,
                                        uint32_t dxgi_format)
{
	if (outcomp == nullptr) {
		return false;
	}
	return d3d11_ensure_srv_scratch(outcomp->dev, &outcomp->weave_scratch, &outcomp->weave_scratch_srv, w, h,
	                                static_cast<DXGI_FORMAT>(dxgi_format), "local2d weave");
}

extern "C" void *
comp_d3d11_outcomp_snapshot_weave(struct comp_d3d11_outcomp *outcomp,
                                  void *dst_texture,
                                  uint32_t region_w,
                                  uint32_t region_h)
{
	if (outcomp == nullptr || outcomp->weave_scratch == nullptr || dst_texture == nullptr) {
		return nullptr;
	}

	// Snapshot the window region of the weave (the DP wrote dst; RT≠SRV, so
	// the lerp reads this copy — impl doc §3 step 2).
	D3D11_BOX sbox = {0, 0, 0, region_w, region_h, 1};
	outcomp->ctx->CopySubresourceRegion(outcomp->weave_scratch, 0, 0, 0, 0,
	                                    static_cast<ID3D11Texture2D *>(dst_texture), 0, &sbox);
	return outcomp->weave_scratch_srv;
}

extern "C" xrt_result_t
comp_d3d11_outcomp_composite_2d_masked(struct comp_d3d11_outcomp *outcomp,
                                       void *dst_texture,
                                       void *twod_srv,
                                       void *mask_srv,
                                       void *weave_srv,
                                       uint32_t region_w,
                                       uint32_t region_h,
                                       int32_t cx,
                                       int32_t cy,
                                       uint32_t cw,
                                       uint32_t ch,
                                       uint32_t composite_mode,
                                       bool opaque_present)
{
	if (outcomp == nullptr || dst_texture == nullptr || twod_srv == nullptr) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}
	// The authored-mask path lerps against the weave, so it must be readable.
	if (mask_srv != nullptr && weave_srv == nullptr) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	ID3D11Texture2D *dst = static_cast<ID3D11Texture2D *>(dst_texture);

	// Temporary RTV on the weave target (which already holds the weave).
	ID3D11RenderTargetView *rtv = nullptr;
	HRESULT hr = outcomp->dev->CreateRenderTargetView(dst, nullptr, &rtv);
	if (FAILED(hr)) {
		U_LOG_E("composite_2d_masked: failed to create RTV: 0x%08x", hr);
		return XRT_ERROR_D3D;
	}

	// Bind weave target as RTV (no depth — the rect path discards inside the
	// canvas so those weaved pixels stay untouched; the mask path lerps
	// against the weave snapshot in t2).
	outcomp->ctx->OMSetRenderTargets(1, &rtv, nullptr);

	// #464: the composite region is window-sized at the top-left anchor of
	// the (worst-case-allocated) surface; pixels beyond it are never written.
	// The full-screen triangle's uv [0,1] spans the viewport, so region-sized
	// SRVs sample 1:1. Phase 0 passes region == dst dims (full surface).
	D3D11_VIEWPORT vp = {};
	vp.Width = static_cast<float>(region_w);
	vp.Height = static_cast<float>(region_h);
	vp.MaxDepth = 1.0f;
	outcomp->ctx->RSSetViewports(1, &vp);

	// Full-screen triangle (3 verts pulled from SV_VertexID), opaque output
	// + point sampling → byte-identical to the strip CopySubresourceRegion.
	outcomp->ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	outcomp->ctx->IASetInputLayout(nullptr);
	outcomp->ctx->VSSetShader(outcomp->composite_vs, nullptr, 0);
	outcomp->ctx->PSSetShader(outcomp->composite_ps, nullptr, 0);
	outcomp->ctx->RSSetState(outcomp->rasterizer_state);
	outcomp->ctx->OMSetDepthStencilState(outcomp->depth_stencil_state, 0);
	outcomp->ctx->OMSetBlendState(outcomp->blend_opaque, nullptr, 0xFFFFFFFF);
	outcomp->ctx->PSSetSamplers(0, 1, &outcomp->sampler_point);

	// t0 = 2D layer, t1 = authored mask (Phase 1), t2 = weave snapshot
	// (Phase 1). t1/t2 stay NULL on the Phase 0 rect path — the shader never
	// samples them when use_rect_mask is set.
	ID3D11ShaderResourceView *srvs[3] = {
	    static_cast<ID3D11ShaderResourceView *>(twod_srv),
	    static_cast<ID3D11ShaderResourceView *>(mask_srv),
	    static_cast<ID3D11ShaderResourceView *>(weave_srv),
	};
	outcomp->ctx->PSSetShaderResources(0, 3, srvs);

	CompositeParams params = {};
	params.dst_dims[0] = static_cast<float>(region_w);
	params.dst_dims[1] = static_cast<float>(region_h);
	params.canvas_origin[0] = static_cast<float>(cx);
	params.canvas_origin[1] = static_cast<float>(cy);
	params.canvas_size[0] = static_cast<float>(cw);
	params.canvas_size[1] = static_cast<float>(ch);
	// Phase 0: hard rect mask derived from the canvas. Phase 1: sample the
	// authored mask and run the lerp.
	params.use_rect_mask = (mask_srv == nullptr) ? 1 : 0;
	params.composite_mode = composite_mode; // LERP / ALPHA_OVER (#491) / ZONES (ADR-027)
	// #833/#116 — the flatten needs the weave, so it stays off on the rect path.
	params.opaque_present = (mask_srv != nullptr && opaque_present) ? 1 : 0;
	// #918 Phase 2a: the bridge planes are panel-sized, so scale their uv into
	// the region's sub-rect. The weave scratch is allocated at exactly the region
	// (comp_d3d11_outcomp_ensure_weave_scratch), so it needs none.
	outcomp_uv_scale(twod_srv, region_w, region_h, params.twod_uv_scale);
	outcomp_uv_scale(mask_srv, region_w, region_h, params.mask_uv_scale);

	D3D11_MAPPED_SUBRESOURCE mapped;
	hr = outcomp->ctx->Map(outcomp->composite_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
	if (FAILED(hr)) {
		// A stale CB could write the 2D layer over the canvas — bail and let
		// the caller fall back to the strip copy.
		U_LOG_E("composite_2d_masked: failed to map constant buffer: 0x%08x", hr);
		rtv->Release();
		return XRT_ERROR_D3D;
	}
	memcpy(mapped.pData, &params, sizeof(params));
	outcomp->ctx->Unmap(outcomp->composite_cb, 0);
	outcomp->ctx->PSSetConstantBuffers(0, 1, &outcomp->composite_cb);

	outcomp->ctx->Draw(3, 0);

	// Unbind SRVs + RTV to avoid read/write hazard warnings — the dst is the
	// DP's weave target and gets copied/sampled downstream (shared-texture
	// readback, capture) while still in flight.
	ID3D11ShaderResourceView *null_srvs[3] = {nullptr, nullptr, nullptr};
	outcomp->ctx->PSSetShaderResources(0, 3, null_srvs);
	ID3D11RenderTargetView *null_rtv = nullptr;
	outcomp->ctx->OMSetRenderTargets(1, &null_rtv, nullptr);

	rtv->Release();
	return XRT_SUCCESS;
}
