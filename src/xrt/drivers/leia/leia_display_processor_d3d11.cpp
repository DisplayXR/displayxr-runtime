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
#include "leia_bg_capture_win.h"

#include "xrt/xrt_display_metrics.h"
#include "util/u_logging.h"

#include <d3d11.h>
#include <d3d11_4.h>  // ID3D11DeviceContext4 + ID3D11Fence — used by the bg-compose path.
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
 * Chroma-key pre-weave fill pixel shader.
 *
 * Replaces alpha=0 atlas pixels with the chroma key color so the SR weaver
 * (which only consumes opaque RGB) can run. Output is alpha=1 everywhere.
 *
 * Apps that pre-fill their swapchain with the key color (legacy v1.2.9
 * Unity flow) submit alpha=1 atlas pixels; this shader is a no-op for them.
 *
 * Apps that submit true alpha (RGBA(0,0,0,0) clear + content with alpha=1)
 * get their transparent regions filled with the key color here.
 *
 * Antialiased edges (0 < alpha < 1) blend toward the key color via lerp;
 * the post-weave strip pass only matches exact key-color RGB, so soft
 * edges become hard masks (a fundamental limitation of the chroma-key
 * trick on lenticular weavers).
 */
static const char *ck_fill_ps_source = R"(
Texture2D<float4> src : register(t0);
SamplerState samp : register(s0);
cbuffer Constants : register(b0) { float3 chroma_rgb; float pad; };
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
	float4 c = src.Sample(samp, uv);
	float3 rgb = lerp(chroma_rgb, c.rgb, c.a);
	return float4(rgb, 1.0);
}
)";

/*
 * Chroma-key post-weave strip pixel shader.
 *
 * Reads the woven back-buffer (opaque RGB), tests RGB exact-equality against
 * the chroma key, writes alpha=0 for matches and alpha=1 otherwise. RGB is
 * premultiplied by the recovered alpha so DWM's
 *     src.rgb + (1-alpha) * dst.rgb
 * blend doesn't add the matched chroma color to the desktop and saturate.
 *
 * Moved from the D3D11 compositor (was at comp_d3d11_compositor.cpp:1441)
 * during the chroma-key internalization for transparency support.
 */
static const char *ck_strip_ps_source = R"(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
Texture2D<float4> src : register(t0);
SamplerState samp : register(s0);
cbuffer Constants : register(b0) { float3 chroma_rgb; float pad; };
float4 main(VSOut i) : SV_Target {
	float3 c = src.Sample(samp, i.uv).rgb;
	float3 d = abs(c - chroma_rgb);
	bool match = max(max(d.r, d.g), d.b) < (1.0 / 512.0);
	float a = match ? 0.0 : 1.0;
	return float4(c * a, a);
}
)";

struct ChromaKeyConstants {
	float chroma_rgb[3];
	float pad;
};

/*
 * Default chroma key when the app didn't supply one (set_chroma_key key=0).
 * Magenta is the canonical "key color" — universally avoided in user content,
 * never produced by typical RGB+lenticular weaving from real scenes.
 * 0x00BBGGRR layout: R=0xFF, G=0x00, B=0xFF.
 */
static constexpr uint32_t kDefaultChromaKey = 0x00FF00FF;

/*
 * Compose-under-bg pre-weave fill pixel shader.
 *
 * Per-tile composes the captured desktop region behind the window UNDER the
 * app's RGBA atlas, outputting opaque RGB the SR weaver can consume. Replaces
 * the chroma-key trick for sessions that have a working WGC capture.
 *
 *   out = lerp(bg, atlas.rgb, atlas.a),  out.a = 1
 *
 * The desktop sits at z=0 (display plane) so the same captured region is
 * sampled into every tile; per-eye parallax comes from the atlas content,
 * not the background. Each tile_local UV covers [0,1] within its tile, and
 * we map it to the same window-on-monitor UV rect for all tiles.
 */
static const char *compose_under_bg_ps_source = R"(
Texture2D<float4> atlas : register(t0);
Texture2D<float4> bg    : register(t1);
SamplerState samp       : register(s0);
cbuffer Constants : register(b0) {
	float2 bg_uv_origin;  // window TL on monitor, normalized
	float2 bg_uv_extent;  // window size on monitor, normalized
	uint2  tile_count;    // (tile_columns, tile_rows)
	uint2  pad_;
};
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
	float4 a = atlas.Sample(samp, uv);
	float2 tile_local = frac(uv * float2(tile_count));
	float2 bg_uv = bg_uv_origin + tile_local * bg_uv_extent;
	float3 b = bg.SampleLevel(samp, bg_uv, 0).rgb;
	return float4(lerp(b, a.rgb, a.a), 1.0);
}
)";

struct ComposeConstants {
	float bg_uv_origin[2];
	float bg_uv_extent[2];
	uint32_t tile_count[2];
	uint32_t pad_[2];
};


/*!
 * Implementation struct wrapping leiasr_d3d11 as xrt_display_processor_d3d11.
 */
struct leia_display_processor_d3d11_impl
{
	struct xrt_display_processor_d3d11 base;
	struct leiasr_d3d11 *leiasr; //!< Owned — destroyed in leia_dp_d3d11_destroy.

	ID3D11Device *device;              //!< Cached device reference (not owned, for blit init).
	HWND hwnd;                         //!< Native window handle from factory, used by bg-capture for self-exclusion + window-on-monitor rect.

	//! @name 2D blit shader resources (passthrough stretch-blit)
	//! @{
	ID3D11VertexShader *blit_vs;
	ID3D11PixelShader *blit_ps;
	ID3D11SamplerState *blit_sampler;
	ID3D11Buffer *blit_cb; //!< 16 bytes: u_scale, v_scale, pad, pad
	//! @}

	uint32_t view_count; //!< Active mode view count (1=2D, 2=stereo).

	//! @name Chroma-key transparency support (lazy-allocated on first frame)
	//!
	//! Enabled via set_chroma_key() when the session asked for a transparent
	//! output window. When @ref ck_enabled is true, process_atlas() does:
	//!   1. Pre-weave fill: atlas SRV → ck_fill_tex (alpha=0 → chroma_rgb,
	//!      output alpha=1) so the SR weaver receives opaque RGB.
	//!   2. Pass ck_fill_srv to the weaver instead of the original atlas.
	//!   3. Post-weave strip: copy current RTV → ck_strip_tex, then run
	//!      strip shader back to RTV (chroma_rgb match → alpha=0,
	//!      else alpha=1, RGB premultiplied for DWM).
	//! @{
	bool ck_enabled;
	uint32_t ck_color; //!< 0x00BBGGRR; effective key (app override or default).
	ID3D11PixelShader *ck_fill_ps;  //!< Pre-weave fill shader.
	ID3D11PixelShader *ck_strip_ps; //!< Post-weave strip shader.
	ID3D11SamplerState *ck_sampler;
	ID3D11Buffer *ck_constants; //!< 16 bytes: chroma_rgb + pad.
	// Pre-weave: RGBA atlas → opaque-RGB filled output, sampled by weaver.
	ID3D11Texture2D *ck_fill_tex;
	ID3D11ShaderResourceView *ck_fill_srv;
	ID3D11RenderTargetView *ck_fill_rtv;
	uint32_t ck_fill_w, ck_fill_h;
	// Post-weave: copy of RTV used to sample for the strip pass.
	ID3D11Texture2D *ck_strip_tex;
	ID3D11ShaderResourceView *ck_strip_srv;
	uint32_t ck_strip_w, ck_strip_h;
	//! @}

	//! @name Compose-under-bg transparency support (lazy-allocated on first frame)
	//!
	//! Preferred path when WGC is available: replaces ck_fill/ck_strip with a
	//! single pre-weave pass that composites captured desktop pixels under the
	//! app's RGBA atlas, producing opaque RGB the weaver consumes. No post-weave
	//! pass needed (output is opaque all the way through).
	//!
	//! Reuses ck_fill_tex/ck_fill_rtv/ck_fill_srv as the intermediate target —
	//! same size, same format. Independent shader + constant buffer.
	//! @{
	struct leia_bg_capture *bg_capture; //!< Owned; NULL if WGC init failed → fall back to ck.
	bool bg_compose_enabled;            //!< True when the new path is active for this session.
	ID3D11Texture2D *bg_shared_tex;     //!< Opened from bg_capture's shared NT handle.
	ID3D11ShaderResourceView *bg_shared_srv;
	ID3D11Fence *bg_fence;              //!< Opened from bg_capture's shared fence handle.
	ID3D11PixelShader *compose_ps;
	ID3D11SamplerState *compose_sampler; //!< Linear sampler (atlas + bg both filtered).
	ID3D11Buffer *compose_constants;     //!< sizeof(ComposeConstants).
	//! @}
};

static inline struct leia_display_processor_d3d11_impl *
leia_dp_d3d11(struct xrt_display_processor_d3d11 *xdp)
{
	return (struct leia_display_processor_d3d11_impl *)xdp;
}


/*
 *
 * Chroma-key fill/strip helpers (transparency support).
 *
 * Lazy-allocated on first frame the pass runs. ck_should_run() gates the
 * whole flow — when false (the common case) none of these execute and
 * process_atlas behaves identically to the pre-transparency path.
 *
 * Reuses the existing blit_vs (fullscreen-quad VS, 4 verts, TRIANGLESTRIP)
 * for both fill and strip passes.
 *
 */

static bool
ck_should_run(struct leia_display_processor_d3d11_impl *ldp)
{
	return ldp->ck_enabled && ldp->ck_color != 0;
}

static bool
ck_init_pipeline(struct leia_display_processor_d3d11_impl *ldp)
{
	if (ldp->ck_fill_ps != nullptr && ldp->ck_strip_ps != nullptr) {
		return true;
	}
	if (ldp->device == nullptr) {
		return false;
	}

	HRESULT hr;
	ID3DBlob *blob = nullptr;
	ID3DBlob *err = nullptr;

	if (ldp->ck_fill_ps == nullptr) {
		hr = D3DCompile(ck_fill_ps_source, strlen(ck_fill_ps_source), nullptr, nullptr, nullptr,
		                "main", "ps_5_0", 0, 0, &blob, &err);
		if (FAILED(hr)) {
			U_LOG_E("Leia D3D11 DP: ck fill PS compile failed: 0x%08x %s",
			        (unsigned)hr, err ? (const char *)err->GetBufferPointer() : "");
			if (err) err->Release();
			return false;
		}
		if (err) { err->Release(); err = nullptr; }
		hr = ldp->device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(),
		                                     nullptr, &ldp->ck_fill_ps);
		blob->Release(); blob = nullptr;
		if (FAILED(hr)) {
			U_LOG_E("Leia D3D11 DP: ck fill PS create failed: 0x%08x", (unsigned)hr);
			return false;
		}
	}

	if (ldp->ck_strip_ps == nullptr) {
		hr = D3DCompile(ck_strip_ps_source, strlen(ck_strip_ps_source), nullptr, nullptr, nullptr,
		                "main", "ps_5_0", 0, 0, &blob, &err);
		if (FAILED(hr)) {
			U_LOG_E("Leia D3D11 DP: ck strip PS compile failed: 0x%08x %s",
			        (unsigned)hr, err ? (const char *)err->GetBufferPointer() : "");
			if (err) err->Release();
			return false;
		}
		if (err) err->Release();
		hr = ldp->device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(),
		                                     nullptr, &ldp->ck_strip_ps);
		blob->Release();
		if (FAILED(hr)) {
			U_LOG_E("Leia D3D11 DP: ck strip PS create failed: 0x%08x", (unsigned)hr);
			return false;
		}
	}

	if (ldp->ck_sampler == nullptr) {
		// Point filter — the strip pass tests RGB exact-equality and any
		// linear interpolation would smear the key color across edges.
		D3D11_SAMPLER_DESC sd = {};
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.MaxLOD = D3D11_FLOAT32_MAX;
		hr = ldp->device->CreateSamplerState(&sd, &ldp->ck_sampler);
		if (FAILED(hr)) {
			U_LOG_E("Leia D3D11 DP: ck sampler create failed: 0x%08x", (unsigned)hr);
			return false;
		}
	}

	if (ldp->ck_constants == nullptr) {
		D3D11_BUFFER_DESC cb = {};
		cb.ByteWidth = sizeof(ChromaKeyConstants);
		cb.Usage = D3D11_USAGE_DYNAMIC;
		cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		hr = ldp->device->CreateBuffer(&cb, nullptr, &ldp->ck_constants);
		if (FAILED(hr)) {
			U_LOG_E("Leia D3D11 DP: ck constants create failed: 0x%08x", (unsigned)hr);
			return false;
		}
	}

	U_LOG_W("Leia D3D11 DP: chroma-key pipeline ready (key=0x%08X)", ldp->ck_color);
	return true;
}

static bool
ck_ensure_fill_target(struct leia_display_processor_d3d11_impl *ldp, uint32_t w, uint32_t h)
{
	if (ldp->ck_fill_tex != nullptr && ldp->ck_fill_w == w && ldp->ck_fill_h == h) {
		return true;
	}
	if (ldp->ck_fill_rtv) { ldp->ck_fill_rtv->Release(); ldp->ck_fill_rtv = nullptr; }
	if (ldp->ck_fill_srv) { ldp->ck_fill_srv->Release(); ldp->ck_fill_srv = nullptr; }
	if (ldp->ck_fill_tex) { ldp->ck_fill_tex->Release(); ldp->ck_fill_tex = nullptr; }

	D3D11_TEXTURE2D_DESC td = {};
	td.Width = w;
	td.Height = h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	HRESULT hr = ldp->device->CreateTexture2D(&td, nullptr, &ldp->ck_fill_tex);
	if (FAILED(hr)) {
		U_LOG_E("Leia D3D11 DP: ck fill tex create (%ux%u) failed: 0x%08x", w, h, (unsigned)hr);
		return false;
	}
	hr = ldp->device->CreateShaderResourceView(ldp->ck_fill_tex, nullptr, &ldp->ck_fill_srv);
	if (FAILED(hr)) {
		U_LOG_E("Leia D3D11 DP: ck fill SRV create failed: 0x%08x", (unsigned)hr);
		return false;
	}
	hr = ldp->device->CreateRenderTargetView(ldp->ck_fill_tex, nullptr, &ldp->ck_fill_rtv);
	if (FAILED(hr)) {
		U_LOG_E("Leia D3D11 DP: ck fill RTV create failed: 0x%08x", (unsigned)hr);
		return false;
	}
	ldp->ck_fill_w = w;
	ldp->ck_fill_h = h;
	return true;
}

static bool
ck_ensure_strip_source(struct leia_display_processor_d3d11_impl *ldp, uint32_t w, uint32_t h)
{
	if (ldp->ck_strip_tex != nullptr && ldp->ck_strip_w == w && ldp->ck_strip_h == h) {
		return true;
	}
	if (ldp->ck_strip_srv) { ldp->ck_strip_srv->Release(); ldp->ck_strip_srv = nullptr; }
	if (ldp->ck_strip_tex) { ldp->ck_strip_tex->Release(); ldp->ck_strip_tex = nullptr; }

	D3D11_TEXTURE2D_DESC td = {};
	td.Width = w;
	td.Height = h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	HRESULT hr = ldp->device->CreateTexture2D(&td, nullptr, &ldp->ck_strip_tex);
	if (FAILED(hr)) {
		U_LOG_E("Leia D3D11 DP: ck strip tex create (%ux%u) failed: 0x%08x", w, h, (unsigned)hr);
		return false;
	}
	hr = ldp->device->CreateShaderResourceView(ldp->ck_strip_tex, nullptr, &ldp->ck_strip_srv);
	if (FAILED(hr)) {
		U_LOG_E("Leia D3D11 DP: ck strip SRV create failed: 0x%08x", (unsigned)hr);
		return false;
	}
	ldp->ck_strip_w = w;
	ldp->ck_strip_h = h;
	return true;
}

static void
ck_update_constants(struct leia_display_processor_d3d11_impl *ldp, ID3D11DeviceContext *ctx)
{
	D3D11_MAPPED_SUBRESOURCE m = {};
	HRESULT hr = ctx->Map(ldp->ck_constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
	if (FAILED(hr)) {
		return;
	}
	ChromaKeyConstants *cb = reinterpret_cast<ChromaKeyConstants *>(m.pData);
	uint32_t k = ldp->ck_color;
	cb->chroma_rgb[0] = ((k >>  0) & 0xFF) / 255.0f;
	cb->chroma_rgb[1] = ((k >>  8) & 0xFF) / 255.0f;
	cb->chroma_rgb[2] = ((k >> 16) & 0xFF) / 255.0f;
	cb->pad = 0.0f;
	ctx->Unmap(ldp->ck_constants, 0);
}

/*
 * Pre-weave fill: read RGBA atlas, write opaque RGB to ck_fill_tex with
 * alpha=0 regions filled by chroma_rgb. Returns the SRV the weaver should
 * sample (ck_fill_srv on success, original atlas_srv on fallback).
 *
 * Saves and restores RTV/DSV/viewport so the caller's swap-chain bindings
 * survive the pass.
 */
static ID3D11ShaderResourceView *
ck_run_pre_weave_fill(struct leia_display_processor_d3d11_impl *ldp,
                       ID3D11DeviceContext *ctx,
                       ID3D11ShaderResourceView *atlas_srv,
                       uint32_t atlas_w,
                       uint32_t atlas_h)
{
	if (!ck_init_pipeline(ldp) || !ck_ensure_fill_target(ldp, atlas_w, atlas_h)) {
		return atlas_srv;
	}

	ID3D11RenderTargetView *prev_rtv = nullptr;
	ID3D11DepthStencilView *prev_dsv = nullptr;
	ctx->OMGetRenderTargets(1, &prev_rtv, &prev_dsv);

	UINT prev_vp_count = 1;
	D3D11_VIEWPORT prev_vp = {};
	ctx->RSGetViewports(&prev_vp_count, &prev_vp);

	ck_update_constants(ldp, ctx);

	D3D11_VIEWPORT vp = {};
	vp.TopLeftX = 0.0f;
	vp.TopLeftY = 0.0f;
	vp.Width = static_cast<float>(atlas_w);
	vp.Height = static_cast<float>(atlas_h);
	vp.MaxDepth = 1.0f;
	ctx->RSSetViewports(1, &vp);

	ctx->OMSetRenderTargets(1, &ldp->ck_fill_rtv, nullptr);

	ctx->IASetInputLayout(nullptr);
	ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	ctx->VSSetShader(ldp->blit_vs, nullptr, 0);
	ctx->PSSetShader(ldp->ck_fill_ps, nullptr, 0);
	ctx->PSSetShaderResources(0, 1, &atlas_srv);
	ctx->PSSetSamplers(0, 1, &ldp->ck_sampler);
	ctx->PSSetConstantBuffers(0, 1, &ldp->ck_constants);
	ctx->Draw(4, 0);

	ID3D11ShaderResourceView *null_srv = nullptr;
	ctx->PSSetShaderResources(0, 1, &null_srv);

	ctx->OMSetRenderTargets(1, &prev_rtv, prev_dsv);
	ctx->RSSetViewports(prev_vp_count, &prev_vp);
	if (prev_rtv) prev_rtv->Release();
	if (prev_dsv) prev_dsv->Release();

	return ldp->ck_fill_srv;
}

/*
 * Post-weave strip: copy currently bound RTV → ck_strip_tex, then sample
 * back into the same RTV writing alpha=0 where RGB matches chroma_rgb,
 * else alpha=1 with RGB premultiplied for DWM's premultiplied alpha mode.
 */
static void
ck_run_post_weave_strip(struct leia_display_processor_d3d11_impl *ldp,
                        ID3D11DeviceContext *ctx)
{
	if (!ck_init_pipeline(ldp)) {
		return;
	}

	ID3D11RenderTargetView *rtv = nullptr;
	ID3D11DepthStencilView *dsv = nullptr;
	ctx->OMGetRenderTargets(1, &rtv, &dsv);
	if (rtv == nullptr) {
		if (dsv) dsv->Release();
		return;
	}

	ID3D11Resource *rtv_res = nullptr;
	rtv->GetResource(&rtv_res);
	ID3D11Texture2D *back_buffer = nullptr;
	if (rtv_res) {
		rtv_res->QueryInterface(__uuidof(ID3D11Texture2D),
		                         reinterpret_cast<void **>(&back_buffer));
	}
	if (back_buffer == nullptr) {
		if (rtv_res) rtv_res->Release();
		rtv->Release();
		if (dsv) dsv->Release();
		return;
	}

	D3D11_TEXTURE2D_DESC bb_desc = {};
	back_buffer->GetDesc(&bb_desc);
	if (!ck_ensure_strip_source(ldp, bb_desc.Width, bb_desc.Height)) {
		back_buffer->Release();
		rtv_res->Release();
		rtv->Release();
		if (dsv) dsv->Release();
		return;
	}

	ctx->CopyResource(ldp->ck_strip_tex, back_buffer);

	ck_update_constants(ldp, ctx);

	D3D11_VIEWPORT vp = {};
	vp.TopLeftX = 0.0f;
	vp.TopLeftY = 0.0f;
	vp.Width = static_cast<float>(bb_desc.Width);
	vp.Height = static_cast<float>(bb_desc.Height);
	vp.MaxDepth = 1.0f;
	ctx->RSSetViewports(1, &vp);

	ctx->OMSetRenderTargets(1, &rtv, nullptr);

	ctx->IASetInputLayout(nullptr);
	ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	ctx->VSSetShader(ldp->blit_vs, nullptr, 0);
	ctx->PSSetShader(ldp->ck_strip_ps, nullptr, 0);
	ctx->PSSetShaderResources(0, 1, &ldp->ck_strip_srv);
	ctx->PSSetSamplers(0, 1, &ldp->ck_sampler);
	ctx->PSSetConstantBuffers(0, 1, &ldp->ck_constants);
	ctx->Draw(4, 0);

	ID3D11ShaderResourceView *null_srv = nullptr;
	ctx->PSSetShaderResources(0, 1, &null_srv);

	back_buffer->Release();
	rtv_res->Release();
	rtv->Release();
	if (dsv) dsv->Release();
}

static void
ck_release_resources(struct leia_display_processor_d3d11_impl *ldp)
{
	if (ldp->ck_fill_rtv)   { ldp->ck_fill_rtv->Release();   ldp->ck_fill_rtv = nullptr; }
	if (ldp->ck_fill_srv)   { ldp->ck_fill_srv->Release();   ldp->ck_fill_srv = nullptr; }
	if (ldp->ck_fill_tex)   { ldp->ck_fill_tex->Release();   ldp->ck_fill_tex = nullptr; }
	if (ldp->ck_strip_srv)  { ldp->ck_strip_srv->Release();  ldp->ck_strip_srv = nullptr; }
	if (ldp->ck_strip_tex)  { ldp->ck_strip_tex->Release();  ldp->ck_strip_tex = nullptr; }
	if (ldp->ck_constants)  { ldp->ck_constants->Release();  ldp->ck_constants = nullptr; }
	if (ldp->ck_sampler)    { ldp->ck_sampler->Release();    ldp->ck_sampler = nullptr; }
	if (ldp->ck_strip_ps)   { ldp->ck_strip_ps->Release();   ldp->ck_strip_ps = nullptr; }
	if (ldp->ck_fill_ps)    { ldp->ck_fill_ps->Release();    ldp->ck_fill_ps = nullptr; }
}


/*
 *
 * Compose-under-bg pipeline (preferred path when WGC is available).
 *
 * Reuses ck_fill_tex/ck_fill_rtv/ck_fill_srv as the intermediate target
 * (created lazily via ck_ensure_fill_target — same size & format as needed).
 *
 */

static bool
compose_should_run(struct leia_display_processor_d3d11_impl *ldp)
{
	return ldp->bg_compose_enabled && ldp->bg_capture != nullptr && ldp->bg_shared_srv != nullptr;
}

static bool
compose_init_pipeline(struct leia_display_processor_d3d11_impl *ldp)
{
	if (ldp->compose_ps != nullptr && ldp->compose_sampler != nullptr && ldp->compose_constants != nullptr) {
		return true;
	}
	if (ldp->device == nullptr) {
		return false;
	}

	HRESULT hr;
	if (ldp->compose_ps == nullptr) {
		ID3DBlob *blob = nullptr;
		ID3DBlob *err = nullptr;
		hr = D3DCompile(compose_under_bg_ps_source,
		                strlen(compose_under_bg_ps_source),
		                nullptr, nullptr, nullptr,
		                "main", "ps_5_0", 0, 0, &blob, &err);
		if (FAILED(hr)) {
			U_LOG_E("Leia D3D11 DP: compose PS compile failed: 0x%08x %s",
			        (unsigned)hr, err ? (const char *)err->GetBufferPointer() : "");
			if (err) err->Release();
			return false;
		}
		if (err) err->Release();
		hr = ldp->device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(),
		                                     nullptr, &ldp->compose_ps);
		blob->Release();
		if (FAILED(hr)) {
			U_LOG_E("Leia D3D11 DP: compose PS create failed: 0x%08x", (unsigned)hr);
			return false;
		}
	}

	if (ldp->compose_sampler == nullptr) {
		D3D11_SAMPLER_DESC sd = {};
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.MaxLOD = D3D11_FLOAT32_MAX;
		hr = ldp->device->CreateSamplerState(&sd, &ldp->compose_sampler);
		if (FAILED(hr)) {
			U_LOG_E("Leia D3D11 DP: compose sampler create failed: 0x%08x", (unsigned)hr);
			return false;
		}
	}

	if (ldp->compose_constants == nullptr) {
		D3D11_BUFFER_DESC cb = {};
		cb.ByteWidth = sizeof(ComposeConstants);
		cb.Usage = D3D11_USAGE_DYNAMIC;
		cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		hr = ldp->device->CreateBuffer(&cb, nullptr, &ldp->compose_constants);
		if (FAILED(hr)) {
			U_LOG_E("Leia D3D11 DP: compose CB create failed: 0x%08x", (unsigned)hr);
			return false;
		}
	}

	U_LOG_W("Leia D3D11 DP: compose-under-bg pipeline ready");
	return true;
}

/*
 * Pre-weave compose-under-bg: poll WGC, composite captured desktop under
 * the RGBA atlas tiles. Returns the SRV the weaver should sample (the
 * ck_fill_srv, repurposed as the intermediate target). On any failure
 * (no captured frame yet, monitor-cross, init failure) returns the
 * original atlas_srv — weaver still runs, just without the desktop
 * background composited in.
 */
static ID3D11ShaderResourceView *
compose_run_pre_weave(struct leia_display_processor_d3d11_impl *ldp,
                      ID3D11DeviceContext *ctx,
                      ID3D11ShaderResourceView *atlas_srv,
                      uint32_t atlas_w,
                      uint32_t atlas_h,
                      uint32_t tile_columns,
                      uint32_t tile_rows)
{
	if (!compose_init_pipeline(ldp) || !ck_ensure_fill_target(ldp, atlas_w, atlas_h)) {
		return atlas_srv;
	}

	float bg_origin[2] = {0.0f, 0.0f};
	float bg_extent[2] = {0.0f, 0.0f};
	uint64_t fence_wait_value = 0;
	bool have_bg = leia_bg_capture_poll(ldp->bg_capture, bg_origin, bg_extent, &fence_wait_value);
	if (!have_bg) {
		// No captured frame yet (or window crossed monitors). Pass atlas
		// straight to the weaver; transparent regions stay alpha=0 RGB=0
		// — visually black but won't crash. The DP will recover next frame.
		return atlas_srv;
	}

	// Update constants.
	D3D11_MAPPED_SUBRESOURCE m = {};
	if (FAILED(ctx->Map(ldp->compose_constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
		return atlas_srv;
	}
	ComposeConstants *cb = reinterpret_cast<ComposeConstants *>(m.pData);
	cb->bg_uv_origin[0] = bg_origin[0];
	cb->bg_uv_origin[1] = bg_origin[1];
	cb->bg_uv_extent[0] = bg_extent[0];
	cb->bg_uv_extent[1] = bg_extent[1];
	cb->tile_count[0] = tile_columns;
	cb->tile_count[1] = tile_rows;
	cb->pad_[0] = 0;
	cb->pad_[1] = 0;
	ctx->Unmap(ldp->compose_constants, 0);

	// Save state.
	ID3D11RenderTargetView *prev_rtv = nullptr;
	ID3D11DepthStencilView *prev_dsv = nullptr;
	ctx->OMGetRenderTargets(1, &prev_rtv, &prev_dsv);
	UINT prev_vp_count = 1;
	D3D11_VIEWPORT prev_vp = {};
	ctx->RSGetViewports(&prev_vp_count, &prev_vp);

	D3D11_VIEWPORT vp = {};
	vp.Width = (float)atlas_w;
	vp.Height = (float)atlas_h;
	vp.MaxDepth = 1.0f;
	ctx->RSSetViewports(1, &vp);
	ctx->OMSetRenderTargets(1, &ldp->ck_fill_rtv, nullptr);

	// Wait on the producer's shared fence so our sample sees the latest
	// captured frame. Requires ID3D11DeviceContext4 (Win10 1809+).
	if (ldp->bg_fence != nullptr) {
		ID3D11DeviceContext4 *ctx4 = nullptr;
		if (SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)&ctx4))) {
			ctx4->Wait(ldp->bg_fence, fence_wait_value);
			ctx4->Release();
		}
	}

	ctx->IASetInputLayout(nullptr);
	ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	ctx->VSSetShader(ldp->blit_vs, nullptr, 0);
	ctx->PSSetShader(ldp->compose_ps, nullptr, 0);
	ID3D11ShaderResourceView *srvs[2] = {atlas_srv, ldp->bg_shared_srv};
	ctx->PSSetShaderResources(0, 2, srvs);
	ctx->PSSetSamplers(0, 1, &ldp->compose_sampler);
	ctx->PSSetConstantBuffers(0, 1, &ldp->compose_constants);
	ctx->Draw(4, 0);

	ID3D11ShaderResourceView *null_srvs[2] = {nullptr, nullptr};
	ctx->PSSetShaderResources(0, 2, null_srvs);

	ctx->OMSetRenderTargets(1, &prev_rtv, prev_dsv);
	ctx->RSSetViewports(prev_vp_count, &prev_vp);
	if (prev_rtv) prev_rtv->Release();
	if (prev_dsv) prev_dsv->Release();

	return ldp->ck_fill_srv;
}

static void
compose_release_resources(struct leia_display_processor_d3d11_impl *ldp)
{
	if (ldp->bg_fence)          { ldp->bg_fence->Release();          ldp->bg_fence = nullptr; }
	if (ldp->bg_shared_srv)     { ldp->bg_shared_srv->Release();     ldp->bg_shared_srv = nullptr; }
	if (ldp->bg_shared_tex)     { ldp->bg_shared_tex->Release();     ldp->bg_shared_tex = nullptr; }
	if (ldp->compose_constants) { ldp->compose_constants->Release(); ldp->compose_constants = nullptr; }
	if (ldp->compose_sampler)   { ldp->compose_sampler->Release();   ldp->compose_sampler = nullptr; }
	if (ldp->compose_ps)        { ldp->compose_ps->Release();        ldp->compose_ps = nullptr; }
	if (ldp->bg_capture)        { leia_bg_capture_destroy(ldp->bg_capture); ldp->bg_capture = nullptr; }
	ldp->bg_compose_enabled = false;
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
	struct leia_display_processor_d3d11_impl *ldp = leia_dp_d3d11(xdp);
	ID3D11DeviceContext *ctx = static_cast<ID3D11DeviceContext *>(d3d11_context);

	// Compute effective viewport: canvas sub-rect when set, else full target.
	// The SR SDK weaver reads the D3D11 viewport via RSGetViewports() and
	// incorporates vpX/vpY into the phase calculation automatically:
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
		if (ldp->blit_vs == NULL || ldp->blit_ps == NULL) {
			return;
		}

		ID3D11ShaderResourceView *srv = static_cast<ID3D11ShaderResourceView *>(atlas_srv);

		// Atlas is guaranteed content-sized by compositor crop-blit.
		// In 2D mode, content occupies min(viewport, atlas) of the atlas.
		uint32_t atlas_w = tile_columns * view_width;
		uint32_t atlas_h = tile_rows * view_height;

		// Compose-under-bg: pre-composite captured desktop under the atlas,
		// then stretch-blit the opaque result. Replaces the post-weave strip
		// path for 2D mode when WGC capture is active.
		if (compose_should_run(ldp)) {
			ID3D11ShaderResourceView *composed =
			    compose_run_pre_weave(ldp, ctx, srv, atlas_w, atlas_h, tile_columns, tile_rows);
			if (composed != nullptr) {
				srv = composed;
			}
		}
		uint32_t content_w = (vp_w < atlas_w) ? vp_w : atlas_w;
		uint32_t content_h = (vp_h < atlas_h) ? vp_h : atlas_h;
		struct { float u_scale; float v_scale; float pad0; float pad1; } cb_data;
		cb_data.u_scale = (atlas_w > 0) ? (float)content_w / (float)atlas_w : 1.0f;
		cb_data.v_scale = (atlas_h > 0) ? (float)content_h / (float)atlas_h : 1.0f;
		cb_data.pad0 = 0.0f;
		cb_data.pad1 = 0.0f;
		ctx->UpdateSubresource(ldp->blit_cb, 0, NULL, &cb_data, 0, 0);

		// Set viewport to canvas sub-rect (or full target if no canvas)
		D3D11_VIEWPORT viewport = {};
		viewport.TopLeftX = static_cast<float>(vp_x);
		viewport.TopLeftY = static_cast<float>(vp_y);
		viewport.Width = static_cast<float>(vp_w);
		viewport.Height = static_cast<float>(vp_h);
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

		// 2D mode has no weaver, but if chroma-key is enabled the legacy
		// flow (app pre-fills with key RGB on opaque alpha=1 surface) still
		// needs the strip pass to recover alpha=0 for DWM. The new flow
		// (true alpha through blit) works without it; running strip in that
		// case is a no-op for non-key pixels. The compose-under-bg path
		// already produced opaque RGB, so the strip pass is unnecessary.
		if (!compose_should_run(ldp) && ck_should_run(ldp)) {
			ck_run_post_weave_strip(ldp, ctx);
		}
		return;
	}

	// Atlas is guaranteed content-sized SBS (2*view_width x view_height)
	// by compositor crop-blit.
	//
	// Two transparency paths feed the SR weaver opaque RGB:
	//
	//   1. Compose-under-bg (preferred): captures the desktop region behind
	//      the window via WGC and composites it under each per-view tile in
	//      the atlas. Output is genuinely opaque pixels — the user sees
	//      desktop through transparent app regions, with no chroma-key
	//      artifacts on antialiased edges.
	//
	//   2. Chroma-key (fallback): replace alpha=0 atlas pixels with the key
	//      color so the opaque-only weaver runs, then post-weave strip
	//      reconstructs alpha=0 holes for DWM. AA edges collapse to hard
	//      masks. Used when WGC is unavailable (Win<2004, DRM, env-disabled).
	//
	// Legacy apps that pre-filled their swapchain with the key color on an
	// alpha=1 surface stay backward-compatible under either path: chroma-key
	// is a no-op (lerp(key, src.rgb, 1.0) == src.rgb); compose-under-bg
	// overwrites their fill with the actual captured desktop (better!).
	void *weaver_srv = atlas_srv;
	uint32_t atlas_w = tile_columns * view_width;
	uint32_t atlas_h = tile_rows * view_height;
	if (compose_should_run(ldp)) {
		weaver_srv = compose_run_pre_weave(
		    ldp, ctx,
		    static_cast<ID3D11ShaderResourceView *>(atlas_srv),
		    atlas_w, atlas_h, tile_columns, tile_rows);
	} else if (ck_should_run(ldp)) {
		weaver_srv = ck_run_pre_weave_fill(
		    ldp, ctx,
		    static_cast<ID3D11ShaderResourceView *>(atlas_srv),
		    atlas_w, atlas_h);
	}
	leiasr_d3d11_set_input_texture(ldp->leiasr, weaver_srv, view_width, view_height, format);

	// Set viewport to canvas sub-rect — the SR SDK weaver reads this
	// and uses vpX/vpY for correct interlacing phase alignment.
	D3D11_VIEWPORT viewport = {};
	viewport.TopLeftX = static_cast<float>(vp_x);
	viewport.TopLeftY = static_cast<float>(vp_y);
	viewport.Width = static_cast<float>(vp_w);
	viewport.Height = static_cast<float>(vp_h);
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	ctx->RSSetViewports(1, &viewport);

	// Diagnostic: log weave params (throttled to every 60 frames + on change)
	{
		static uint32_t frame_ctr = 0;
		static uint32_t last_tgt_w = 0, last_tgt_h = 0;
		static int32_t last_vp_x = -1, last_vp_y = -1;
		static uint32_t last_vp_w = 0, last_vp_h = 0;
		static uint32_t last_view_w = 0, last_view_h = 0;
		bool changed = (target_width != last_tgt_w || target_height != last_tgt_h ||
		                vp_x != last_vp_x || vp_y != last_vp_y ||
		                vp_w != last_vp_w || vp_h != last_vp_h ||
		                view_width != last_view_w || view_height != last_view_h);
		if (changed || (frame_ctr % 300 == 0)) {
			U_LOG_W("weave: target=%ux%u vp=(%d,%d %ux%u) view=%ux%u canvas=(%d,%d %ux%u)%s",
			        target_width, target_height, vp_x, vp_y, vp_w, vp_h,
			        view_width, view_height,
			        canvas_offset_x, canvas_offset_y, canvas_width, canvas_height,
			        changed ? " [CHANGED]" : "");
			last_tgt_w = target_width; last_tgt_h = target_height;
			last_vp_x = vp_x; last_vp_y = vp_y;
			last_vp_w = vp_w; last_vp_h = vp_h;
			last_view_w = view_width; last_view_h = view_height;
		}
		frame_ctr++;
	}

	leiasr_d3d11_weave(ldp->leiasr);

	// Post-weave chroma-key strip: convert key-color RGB pixels in the
	// woven back-buffer to alpha=0 (with RGB premultiplied for DWM's
	// premultiplied alpha mode). No-op when chroma-key is disabled, and
	// suppressed entirely when compose-under-bg ran (the weaver already
	// consumed opaque pre-composited input; the back buffer is opaque RGB).
	if (!compose_should_run(ldp) && ck_should_run(ldp)) {
		ck_run_post_weave_strip(ldp, ctx);
	}
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
leia_dp_d3d11_get_hardware_3d_state(struct xrt_display_processor_d3d11 *xdp, bool *out_is_3d)
{
	struct leia_display_processor_d3d11_impl *ldp = leia_dp_d3d11(xdp);
	return leiasr_d3d11_get_hardware_3d_state(ldp->leiasr, out_is_3d);
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

static bool
leia_dp_d3d11_is_alpha_native(struct xrt_display_processor_d3d11 *xdp)
{
	(void)xdp;
	// SR SDK weaver interlaces into opaque RGB — alpha is destroyed.
	// Transparency is recovered via the chroma-key trick (see set_chroma_key).
	return false;
}

static void
leia_dp_d3d11_set_chroma_key(struct xrt_display_processor_d3d11 *xdp,
                              uint32_t key_color,
                              bool transparent_bg_enabled)
{
	struct leia_display_processor_d3d11_impl *ldp = leia_dp_d3d11(xdp);

	// Preserve the ck_color/ck_enabled values regardless of path — they
	// are the fallback if WGC fails or gets disabled mid-session.
	ldp->ck_color = (key_color != 0) ? key_color : kDefaultChromaKey;
	ldp->ck_enabled = transparent_bg_enabled;

	// Preferred path: try to initialize WGC-based desktop capture so we can
	// pre-composite the desktop UNDER the atlas tiles instead of using the
	// chroma-key trick. On any failure (older Windows, capture blocked,
	// env-disabled), fall through to chroma-key.
	if (transparent_bg_enabled && !ldp->bg_compose_enabled && ldp->hwnd != nullptr) {
		ldp->bg_capture = leia_bg_capture_create(ldp->hwnd);
		if (ldp->bg_capture != nullptr && ldp->device != nullptr) {
			HRESULT hr = leia_bg_capture_open_d3d11(
			    ldp->bg_capture, ldp->device, &ldp->bg_shared_tex, &ldp->bg_shared_srv);
			if (SUCCEEDED(hr)) {
				hr = leia_bg_capture_open_fence_d3d11(
				    ldp->bg_capture, ldp->device, &ldp->bg_fence);
			}
			if (SUCCEEDED(hr)) {
				ldp->bg_compose_enabled = true;
				// Disable the chroma-key path so we don't run both.
				ldp->ck_enabled = false;
				U_LOG_W("Leia D3D11 DP: transparency = compose-under-bg (WGC)");
			} else {
				U_LOG_W("Leia D3D11 DP: WGC import failed (0x%08x) — falling back to chroma-key",
				        (unsigned)hr);
				compose_release_resources(ldp);
			}
		}
	}
	if (!ldp->bg_compose_enabled) {
		U_LOG_W("Leia D3D11 DP: transparency = chroma-key %s (key=0x%08X%s)",
		        ldp->ck_enabled ? "ENABLED" : "disabled",
		        ldp->ck_color,
		        (key_color == 0) ? " — DP default" : " — app override");
	}
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

	compose_release_resources(ldp);
	ck_release_resources(ldp);

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
	ldp->base.get_hardware_3d_state = leia_dp_d3d11_get_hardware_3d_state;
	ldp->base.get_display_dimensions = leia_dp_d3d11_get_display_dimensions;
	ldp->base.get_display_pixel_info = leia_dp_d3d11_get_display_pixel_info;
	ldp->base.is_alpha_native = leia_dp_d3d11_is_alpha_native;
	ldp->base.set_chroma_key = leia_dp_d3d11_set_chroma_key;
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
	ldp->hwnd = static_cast<HWND>(window_handle);
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
