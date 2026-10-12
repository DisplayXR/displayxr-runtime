// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  D3D11 shader implementation of the synthetic outputs (see
 *         sim_conv_synth.h). Moved from sim_display_lift_d3d11.cpp; the views
 *         shader now takes a per-view shift so explicit viewpoints are honoured.
 * @ingroup drv_sim_conversion
 */

#include "sim_conv_synth.h"

#include "util/u_logging.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <cstring>

static const char *k_vs = R"(
struct VS_OUTPUT { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VS_OUTPUT main(uint id : SV_VertexID) {
	VS_OUTPUT o;
	o.uv = float2(id & 1, id >> 1);
	o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
	return o;
}
)";

// Views side by side; view v samples the input shifted by shifts[v].
static const char *k_ps_views = R"(
cbuffer P : register(b0) { float view_count; float3 pad; float4 shifts[2]; };
Texture2D src : register(t0);
SamplerState samp : register(s0);
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
	float v = min(floor(uv.x * view_count), view_count - 1.0);
	float lu = uv.x * view_count - v;
	uint vi = (uint)v;
	float off = shifts[vi >> 2][vi & 3];
	float4 c = src.Sample(samp, float2(saturate(lu + off), uv.y));
	return float4(c.rgb, c.a); // keep the source alpha (runtime#1742: transparent gaps must stay transparent)
}
)";

// Relative depth: a plain vertical gradient, 0 (near) at the top.
static const char *k_ps_depth = R"(
float main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target { return uv.y; }
)";

struct views_cb
{
	float view_count;
	float pad[3];
	float shifts[SIM_CONV_SYNTH_MAX_VIEWS];
};
static_assert(sizeof(views_cb) == 48, "views_cb must match the HLSL cbuffer");

struct sim_conv_d3d11_synth
{
	ID3D11Device *device;
	ID3D11VertexShader *vs;
	ID3D11PixelShader *ps_views;
	ID3D11PixelShader *ps_depth;
	ID3D11SamplerState *sampler;
	ID3D11Buffer *cb;
};

template <typename T>
static void
safe_release(T *&p)
{
	if (p != nullptr) {
		p->Release();
		p = nullptr;
	}
}

static HRESULT
compile(const char *src, const char *target, ID3DBlob **out)
{
	ID3DBlob *err = nullptr;
	HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, "main", target, 0, 0, out, &err);
	if (FAILED(hr) && err != nullptr) {
		U_LOG_E("sim conversion synth: shader compile error: %s", (const char *)err->GetBufferPointer());
	}
	safe_release(err);
	return hr;
}

extern "C" struct sim_conv_d3d11_synth *
sim_conv_d3d11_synth_create(void *device)
{
	if (device == nullptr) {
		return nullptr;
	}
	auto *s = new sim_conv_d3d11_synth{};
	s->device = static_cast<ID3D11Device *>(device);
	s->device->AddRef();

	ID3DBlob *b = nullptr;
	bool ok = SUCCEEDED(compile(k_vs, "vs_5_0", &b)) &&
	          SUCCEEDED(s->device->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &s->vs));
	safe_release(b);
	ok = ok && SUCCEEDED(compile(k_ps_views, "ps_5_0", &b)) &&
	     SUCCEEDED(s->device->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &s->ps_views));
	safe_release(b);
	ok = ok && SUCCEEDED(compile(k_ps_depth, "ps_5_0", &b)) &&
	     SUCCEEDED(s->device->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &s->ps_depth));
	safe_release(b);
	if (ok) {
		D3D11_SAMPLER_DESC sd = {};
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.MaxLOD = D3D11_FLOAT32_MAX;
		ok = SUCCEEDED(s->device->CreateSamplerState(&sd, &s->sampler));
	}
	if (ok) {
		D3D11_BUFFER_DESC bd = {};
		bd.ByteWidth = sizeof(views_cb);
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		ok = SUCCEEDED(s->device->CreateBuffer(&bd, nullptr, &s->cb));
	}
	if (!ok) {
		sim_conv_d3d11_synth_destroy(s);
		return nullptr;
	}
	return s;
}

extern "C" void
sim_conv_d3d11_synth_destroy(struct sim_conv_d3d11_synth *s)
{
	if (s == nullptr) {
		return;
	}
	safe_release(s->cb);
	safe_release(s->sampler);
	safe_release(s->ps_depth);
	safe_release(s->ps_views);
	safe_release(s->vs);
	safe_release(s->device);
	delete s;
}

static void
draw_fullscreen(struct sim_conv_d3d11_synth *s,
                ID3D11DeviceContext *ctx,
                ID3D11RenderTargetView *rtv,
                ID3D11PixelShader *ps,
                ID3D11ShaderResourceView *srv,
                uint32_t w,
                uint32_t h)
{
	D3D11_VIEWPORT vp = {};
	vp.Width = (float)w;
	vp.Height = (float)h;
	vp.MaxDepth = 1.0f;
	ctx->RSSetViewports(1, &vp);
	D3D11_RECT sc = {0, 0, (LONG)w, (LONG)h};
	ctx->RSSetScissorRects(1, &sc);
	ctx->OMSetRenderTargets(1, &rtv, nullptr);
	ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
	ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	ctx->IASetInputLayout(nullptr);
	ctx->VSSetShader(s->vs, nullptr, 0);
	ctx->PSSetShader(ps, nullptr, 0);
	ctx->PSSetConstantBuffers(0, 1, &s->cb);
	ctx->PSSetSamplers(0, 1, &s->sampler);
	ctx->PSSetShaderResources(0, 1, &srv);
	ctx->Draw(4, 0);
	ID3D11ShaderResourceView *null_srv = nullptr;
	ctx->PSSetShaderResources(0, 1, &null_srv);
	ID3D11RenderTargetView *null_rtv = nullptr;
	ctx->OMSetRenderTargets(1, &null_rtv, nullptr);
}

extern "C" bool
sim_conv_d3d11_synth_views(struct sim_conv_d3d11_synth *s,
                           void *context,
                           void *src_srv,
                           void *rtv,
                           uint32_t width,
                           uint32_t height,
                           uint32_t view_count,
                           const float *shifts)
{
	if (s == nullptr || context == nullptr || src_srv == nullptr || rtv == nullptr || view_count == 0 ||
	    view_count > SIM_CONV_SYNTH_MAX_VIEWS || shifts == nullptr) {
		return false;
	}
	auto *ctx = static_cast<ID3D11DeviceContext *>(context);
	views_cb cb = {};
	cb.view_count = (float)view_count;
	for (uint32_t i = 0; i < view_count; i++) {
		cb.shifts[i] = shifts[i];
	}
	ctx->UpdateSubresource(s->cb, 0, nullptr, &cb, 0, 0);
	draw_fullscreen(s, ctx, static_cast<ID3D11RenderTargetView *>(rtv), s->ps_views,
	                static_cast<ID3D11ShaderResourceView *>(src_srv), width * view_count, height);
	return true;
}

extern "C" bool
sim_conv_d3d11_synth_depth(struct sim_conv_d3d11_synth *s, void *context, void *rtv, uint32_t width, uint32_t height)
{
	if (s == nullptr || context == nullptr || rtv == nullptr) {
		return false;
	}
	draw_fullscreen(s, static_cast<ID3D11DeviceContext *>(context), static_cast<ID3D11RenderTargetView *>(rtv),
	                s->ps_depth, nullptr, width, height);
	return true;
}
