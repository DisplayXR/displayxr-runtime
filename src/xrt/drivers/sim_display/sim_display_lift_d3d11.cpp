// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_display's FAKE lift module (see sim_display_lift_d3d11.h).
 * @ingroup drv_sim_display
 */

#include "sim_display_lift_d3d11.h"
#include "sim_display_fake_ply.h"

#include "util/u_debug.h"
#include "util/u_logging.h"
#include "os/os_time.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <cstdlib>
#include <cstring>
#include <vector>

DEBUG_GET_ONCE_BOOL_OPTION(sim_display_fake_lift, "SIM_DISPLAY_FAKE_LIFT", false)
DEBUG_GET_ONCE_NUM_OPTION(sim_display_fake_lift_latency_ms, "SIM_DISPLAY_FAKE_LIFT_LATENCY_MS", 8)

#define SIM_FAKE_LIFT_MAX_STREAMS 8
#define SIM_FAKE_LIFT_MAX_VIEWS 8

static const char *k_vs = R"(
struct VS_OUTPUT { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VS_OUTPUT main(uint id : SV_VertexID) {
	VS_OUTPUT o;
	o.uv = float2(id & 1, id >> 1);
	o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
	return o;
}
)";

// Views side by side; view v samples the input shifted by (v - (n-1)/2) * shift.
static const char *k_ps_views = R"(
cbuffer P : register(b0) { float view_count; float shift; float2 pad; };
Texture2D src : register(t0);
SamplerState samp : register(s0);
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
	float v = min(floor(uv.x * view_count), view_count - 1.0);
	float lu = uv.x * view_count - v;
	float off = (v - (view_count - 1.0) * 0.5) * shift;
	float4 c = src.Sample(samp, float2(saturate(lu + off), uv.y));
	return float4(c.rgb, 1.0);
}
)";

// Relative depth: a plain vertical gradient, 0 (near) at the top.
static const char *k_ps_depth = R"(
float main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target { return uv.y; }
)";

struct fake_cb
{
	float view_count;
	float shift;
	float pad[2];
};

struct fake_stream
{
	bool used;
	uint64_t id;
	uint32_t mode;
	ID3D11Texture2D *out;
	ID3D11RenderTargetView *rtv;
	uint32_t out_w, out_h;
	DXGI_FORMAT out_fmt;
	std::vector<uint8_t> blob;
};

struct sim_fake_lift
{
	ID3D11Device *device;
	ID3D11VertexShader *vs;
	ID3D11PixelShader *ps_views;
	ID3D11PixelShader *ps_depth;
	ID3D11SamplerState *sampler;
	ID3D11Buffer *cb;
	uint64_t next_id;
	fake_stream streams[SIM_FAKE_LIFT_MAX_STREAMS];
};

static HRESULT
compile(const char *src, const char *target, ID3DBlob **out)
{
	ID3DBlob *err = nullptr;
	HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, "main", target, 0, 0, out, &err);
	if (FAILED(hr) && err != nullptr) {
		U_LOG_E("sim_display fake lift: shader compile error: %s", (const char *)err->GetBufferPointer());
	}
	if (err != nullptr) {
		err->Release();
	}
	return hr;
}

template <typename T>
static void
safe_release(T *&p)
{
	if (p != nullptr) {
		p->Release();
		p = nullptr;
	}
}

static void
stream_release(fake_stream &s)
{
	safe_release(s.rtv);
	safe_release(s.out);
	s.blob.clear();
	s.blob.shrink_to_fit();
	s.used = false;
}

static fake_stream *
find_stream(struct sim_fake_lift *fl, uint64_t id)
{
	for (auto &s : fl->streams) {
		if (s.used && s.id == id) {
			return &s;
		}
	}
	return nullptr;
}

static void
fake_latency(void)
{
	int64_t ms = debug_get_num_option_sim_display_fake_lift_latency_ms();
	if (ms > 0) {
		os_nanosleep(ms * 1000 * 1000);
	}
}

extern "C" bool
sim_fake_lift_enabled(void)
{
	return debug_get_bool_option_sim_display_fake_lift();
}

extern "C" struct sim_fake_lift *
sim_fake_lift_create(void *d3d11_device)
{
	if (!sim_fake_lift_enabled() || d3d11_device == nullptr) {
		return nullptr;
	}
	auto *fl = new sim_fake_lift{};
	fl->device = static_cast<ID3D11Device *>(d3d11_device);
	fl->device->AddRef();

	ID3DBlob *b = nullptr;
	bool ok =
	    SUCCEEDED(compile(k_vs, "vs_5_0", &b)) &&
	    SUCCEEDED(fl->device->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &fl->vs));
	safe_release(b);
	ok =
	    ok && SUCCEEDED(compile(k_ps_views, "ps_5_0", &b)) &&
	    SUCCEEDED(fl->device->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &fl->ps_views));
	safe_release(b);
	ok =
	    ok && SUCCEEDED(compile(k_ps_depth, "ps_5_0", &b)) &&
	    SUCCEEDED(fl->device->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &fl->ps_depth));
	safe_release(b);

	if (ok) {
		D3D11_SAMPLER_DESC sd = {};
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.MaxLOD = D3D11_FLOAT32_MAX;
		ok = SUCCEEDED(fl->device->CreateSamplerState(&sd, &fl->sampler));
	}
	if (ok) {
		D3D11_BUFFER_DESC bd = {};
		bd.ByteWidth = sizeof(fake_cb);
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		ok = SUCCEEDED(fl->device->CreateBuffer(&bd, nullptr, &fl->cb));
	}
	if (!ok) {
		U_LOG_E("sim_display fake lift: init failed — lift stays unavailable");
		sim_fake_lift_destroy(fl);
		return nullptr;
	}
	U_LOG_W(
	    "sim_display: FAKE lift module active (SIM_DISPLAY_FAKE_LIFT) — shifted SBS/N-view, gradient "
	    "depth, two-layer PLY; not a model");
	return fl;
}

extern "C" void
sim_fake_lift_destroy(struct sim_fake_lift *fl)
{
	if (fl == nullptr) {
		return;
	}
	for (auto &s : fl->streams) {
		stream_release(s);
	}
	safe_release(fl->cb);
	safe_release(fl->sampler);
	safe_release(fl->ps_depth);
	safe_release(fl->ps_views);
	safe_release(fl->vs);
	safe_release(fl->device);
	delete fl;
}

extern "C" bool
sim_fake_lift_get_caps(struct sim_fake_lift *fl, struct xrt_dp_lift_caps *out)
{
	if (fl == nullptr || out == nullptr || out->struct_size < sizeof(struct xrt_dp_lift_caps)) {
		return false;
	}
	out->modes =
	    XRT_DP_LIFT_MODE_DEPTH | XRT_DP_LIFT_MODE_SBS | XRT_DP_LIFT_MODE_NVIEW | XRT_DP_LIFT_MODE_GAUSSIANS;
	out->max_streams = SIM_FAKE_LIFT_MAX_STREAMS;
	out->max_views = SIM_FAKE_LIFT_MAX_VIEWS;
	out->depth_semantics = XRT_DP_LIFT_DEPTH_RELATIVE;
	out->state = XRT_DP_LIFT_STATE_READY;
	out->typical_latency_ns = (uint64_t)debug_get_num_option_sim_display_fake_lift_latency_ms() * 1000000ull;
	snprintf(out->backend, sizeof(out->backend), "sim_display-fake");
	return true;
}

extern "C" bool
sim_fake_lift_stream_create(struct sim_fake_lift *fl, const struct xrt_dp_lift_stream_info *info, uint64_t *out_id)
{
	if (fl == nullptr || info == nullptr || out_id == nullptr) {
		return false;
	}
	const uint32_t m = info->mode;
	if (m != XRT_DP_LIFT_MODE_DEPTH && m != XRT_DP_LIFT_MODE_SBS && m != XRT_DP_LIFT_MODE_NVIEW &&
	    m != XRT_DP_LIFT_MODE_GAUSSIANS) {
		return false;
	}
	for (auto &s : fl->streams) {
		if (!s.used) {
			s = fake_stream{};
			s.used = true;
			s.id = ++fl->next_id;
			s.mode = m;
			*out_id = s.id;
			return true;
		}
	}
	return false;
}

extern "C" void
sim_fake_lift_stream_destroy(struct sim_fake_lift *fl, uint64_t id)
{
	if (fl == nullptr) {
		return;
	}
	fake_stream *s = find_stream(fl, id);
	if (s != nullptr) {
		stream_release(*s);
	}
}

static bool
ensure_out(struct sim_fake_lift *fl, fake_stream &s, uint32_t w, uint32_t h, DXGI_FORMAT fmt)
{
	if (s.out != nullptr && s.out_w == w && s.out_h == h && s.out_fmt == fmt) {
		return true;
	}
	safe_release(s.rtv);
	safe_release(s.out);
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = w;
	td.Height = h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = fmt;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	if (FAILED(fl->device->CreateTexture2D(&td, nullptr, &s.out)) ||
	    FAILED(fl->device->CreateRenderTargetView(s.out, nullptr, &s.rtv))) {
		safe_release(s.rtv);
		safe_release(s.out);
		return false;
	}
	s.out_w = w;
	s.out_h = h;
	s.out_fmt = fmt;
	return true;
}

extern "C" bool
sim_fake_lift_convert(struct sim_fake_lift *fl,
                      uint64_t id,
                      void *d3d11_context,
                      void *input_resource,
                      uint32_t w,
                      uint32_t h,
                      const struct xrt_dp_lift_params *p,
                      void **out_resource,
                      uint32_t *out_w,
                      uint32_t *out_h,
                      uint32_t *out_format)
{
	if (fl == nullptr || d3d11_context == nullptr || input_resource == nullptr || w == 0 || h == 0 ||
	    out_resource == nullptr || out_w == nullptr || out_h == nullptr || out_format == nullptr) {
		return false;
	}
	fake_stream *s = find_stream(fl, id);
	if (s == nullptr || s->mode == XRT_DP_LIFT_MODE_GAUSSIANS) {
		return false;
	}
	auto *ctx = static_cast<ID3D11DeviceContext *>(d3d11_context);

	uint32_t views = 1;
	if (s->mode == XRT_DP_LIFT_MODE_SBS) {
		views = 2;
	} else if (s->mode == XRT_DP_LIFT_MODE_NVIEW) {
		views = (p != nullptr && p->view_count >= 1) ? p->view_count : 4;
		views = views > SIM_FAKE_LIFT_MAX_VIEWS ? SIM_FAKE_LIFT_MAX_VIEWS : views;
	}
	const bool depth = s->mode == XRT_DP_LIFT_MODE_DEPTH;
	const DXGI_FORMAT fmt = depth ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
	const uint32_t ow = w * views;
	if (!ensure_out(fl, *s, ow, h, fmt)) {
		return false;
	}

	ID3D11ShaderResourceView *srv = nullptr;
	if (!depth) {
		D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
		sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		sd.Texture2D.MipLevels = 1;
		if (FAILED(fl->device->CreateShaderResourceView(static_cast<ID3D11Resource *>(input_resource), &sd,
		                                                &srv))) {
			return false;
		}
	}

	// ~2% of the view width per view step at strength 1: visible, obviously fake.
	fake_cb cb = {};
	cb.view_count = (float)views;
	cb.shift = 0.02f * (p != nullptr && p->strength > 0.0f ? p->strength : 1.0f);
	ctx->UpdateSubresource(fl->cb, 0, nullptr, &cb, 0, 0);

	D3D11_VIEWPORT vp = {};
	vp.Width = (float)ow;
	vp.Height = (float)h;
	vp.MaxDepth = 1.0f;
	ctx->RSSetViewports(1, &vp);
	D3D11_RECT sc = {0, 0, (LONG)ow, (LONG)h};
	ctx->RSSetScissorRects(1, &sc);
	ctx->OMSetRenderTargets(1, &s->rtv, nullptr);
	ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
	ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	ctx->IASetInputLayout(nullptr);
	ctx->VSSetShader(fl->vs, nullptr, 0);
	ctx->PSSetShader(depth ? fl->ps_depth : fl->ps_views, nullptr, 0);
	ctx->PSSetConstantBuffers(0, 1, &fl->cb);
	ctx->PSSetSamplers(0, 1, &fl->sampler);
	ctx->PSSetShaderResources(0, 1, &srv);
	ctx->Draw(4, 0);
	ID3D11ShaderResourceView *null_srv = nullptr;
	ctx->PSSetShaderResources(0, 1, &null_srv);
	ID3D11RenderTargetView *null_rtv = nullptr;
	ctx->OMSetRenderTargets(1, &null_rtv, nullptr);
	safe_release(srv);

	fake_latency();

	*out_resource = s->out;
	*out_w = ow;
	*out_h = h;
	*out_format = (uint32_t)fmt;
	return true;
}

extern "C" bool
sim_fake_lift_convert_blob(struct sim_fake_lift *fl,
                           uint64_t id,
                           void *d3d11_context,
                           void *input_resource,
                           uint32_t w,
                           uint32_t h,
                           uint32_t *out_format,
                           const void **out_bytes,
                           size_t *out_size)
{
	if (fl == nullptr || d3d11_context == nullptr || input_resource == nullptr || out_format == nullptr ||
	    out_bytes == nullptr || out_size == nullptr) {
		return false;
	}
	fake_stream *s = find_stream(fl, id);
	if (s == nullptr || s->mode != XRT_DP_LIFT_MODE_GAUSSIANS) {
		return false;
	}
	auto *ctx = static_cast<ID3D11DeviceContext *>(d3d11_context);

	// Read the photo back (the fake colours its front layer from it).
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = w;
	td.Height = h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_STAGING;
	td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	ID3D11Texture2D *staging = nullptr;
	const uint8_t *rgba = nullptr;
	uint32_t pitch = 0;
	D3D11_MAPPED_SUBRESOURCE map = {};
	bool mapped = false;
	if (SUCCEEDED(fl->device->CreateTexture2D(&td, nullptr, &staging))) {
		D3D11_BOX box = {0, 0, 0, w, h, 1};
		ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, static_cast<ID3D11Resource *>(input_resource), 0, &box);
		if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map))) {
			mapped = true;
			rgba = static_cast<const uint8_t *>(map.pData);
			pitch = map.RowPitch;
		}
	}

	size_t need = sim_fake_ply_write(nullptr, 0, nullptr, w, h, 0);
	s->blob.resize(need);
	sim_fake_ply_write(s->blob.data(), s->blob.size(), rgba, w, h, pitch);

	if (mapped) {
		ctx->Unmap(staging, 0);
	}
	safe_release(staging);

	// Photo → splats is seconds on a real module; the fake is quick but not free.
	fake_latency();

	*out_format = XRT_DP_LIFT_BLOB_PLY_3DGS;
	*out_bytes = s->blob.data();
	*out_size = s->blob.size();
	return true;
}
