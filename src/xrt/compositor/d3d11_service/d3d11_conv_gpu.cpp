// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The conversion core's D3D11 GPU glue on the service device — see
 *         d3d11_conv_gpu.h.
 *
 * ## Split plan (ADR-053 P1b, #2028)
 *
 * d3d11_lift.cpp used to carry three things on one object: the pre-core lift
 * thread (activation, caps poll, lift_convert_one, lift_copy_depth, the
 * pre-core streams and their scheduler), the `dp-lift` backend the conversion
 * core's adapter drives (be_*), and the core route's D3D11 glue. P1b splits
 * them:
 *
 *  1. THIS FILE: everything on the SERVICE device — the snapshot / letterbox
 *     pipelines (snapshot_blit, letterbox_step), runtime image alloc / free
 *     (glue_alloc / glue_free), the v3 import / publish / export, the S6
 *     depth readback, the S5 view synthesis, the weave path's submit / pin
 *     (core_weave_submit_locked, core_pin_latest, core_unpin) and the bridge.
 *     The ipc_server_conv_io_ops the service installs come from here.
 *     Function names are unchanged from d3d11_lift.cpp (pure moves).
 *  2. d3d11_lift.cpp keeps only the LIFT device side: the one lift thread
 *     (a command executor for the dp-lift adapter), the vendor display
 *     processor and its activation / caps poll as backend commands, and
 *     lift_core_convert. Scheduling, readiness and the activation state
 *     machine live in the conversion core and the dp-lift adapter.
 *  3. Deleted: lift_activate's pre-core latch, lift_poll_caps' 1 Hz loop,
 *     lift_convert_one, lift_copy_depth, the pre-core autoconvergence and
 *     viewpoint policy (conv_policy has them), every pre-core stream entry
 *     point. GAUSSIANS without a SPLATS_MONO provider is UNAVAILABLE.
 *
 * Shared state lives in d3d11_lift_internal.h. Threads: producers (IPC),
 * the core's dispatcher (S5 / S6) and the render thread (pins) call in here;
 * none of them ever calls a provider slot. Lock order: svc_ctx_mutex ->
 * glue_mtx (a leaf); the lift module's mtx is only taken to queue an eviction.
 *
 * Deliberately raw COM (no WIL) so the file is portable to the MinGW
 * compile-check.
 *
 * @ingroup comp_d3d11_service
 */

#include "d3d11_lift_internal.h"
#include "d3d11_conv_gpu.h"
#include "d3d11_conv_content_key.h"

#include "util/u_lift_autoconv.h"
#include "util/u_lift_viewpoint.h"


/*
 *
 * Snapshot blit (service device): sample a source sub-rect into a slot — 1:1
 * (point) when the snapshot is uncapped, box-filtered down when the service's
 * DXR_LIFT_MAX_INPUT_EDGE cap engages (u_lift_cap_dims).
 *
 */

// src_rect = (x, y, w, h) in source texels, src_size = (tw, th).
static const char *k_snap_ps = R"(
cbuffer C : register(b0) { float4 src_rect; float4 src_size; };
Texture2D src : register(t0);
SamplerState samp : register(s0);
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
	float2 p = src_rect.xy + uv * src_rect.zw;
	float4 c = src.SampleLevel(samp, p / src_size.xy, 0);
	return float4(c.rgb, 1.0);
}
)";

/*
 * Downscaling variant (capped snapshot). One destination texel covers
 * step = src_rect.zw / dst_size.xy source texels; four bilinear taps at the
 * quarter points of that footprint are an exact box filter up to a 4x reduction
 * (7680 -> 1920) and a much better one than a single tap beyond it. Taps are
 * clamped to texel centres inside the sub-rect, so nothing outside the lifted
 * rect bleeds in at its edges.
 */
static const char *k_snap_ps_scaled = R"(
cbuffer C : register(b0) { float4 src_rect; float4 src_size; float4 dst_size; };
Texture2D src : register(t0);
SamplerState samp : register(s0);
float3 tap(float2 p, float2 lo, float2 hi) {
	return src.SampleLevel(samp, clamp(p, lo, hi) / src_size.xy, 0).rgb;
}
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
	float2 p = src_rect.xy + uv * src_rect.zw;
	float2 q = 0.25 * src_rect.zw / dst_size.xy;
	float2 lo = src_rect.xy + 0.5;
	float2 hi = src_rect.xy + src_rect.zw - 0.5;
	float3 c = tap(p + float2(-q.x, -q.y), lo, hi) + tap(p + float2(q.x, -q.y), lo, hi) +
	           tap(p + float2(-q.x, q.y), lo, hi) + tap(p + float2(q.x, q.y), lo, hi);
	return float4(c * 0.25, 1.0);
}
)";

struct snap_cb
{
	float src_rect[4];
	float src_size[4];
	float dst_size[4]; //!< read by k_snap_ps_scaled only
};

/*
 * Letterbox profile (DXR_LIFT_LETTERBOX). Target: R32_FLOAT,
 * U_LIFT_LETTERBOX_BINS_MAX x 2 — row 0 = the rect's row buckets top to bottom,
 * row 1 = its column buckets left to right. Each texel is the non-black
 * fraction of its bucket: the max over the bucket's first, middle and last
 * line, each line sampled at 96 points across the rect. Taking the max over
 * the first and LAST line means a bucket holding the picture's first row is
 * already picture, so a bar never swallows a picture row.
 * bins = (row buckets, column buckets, -, -).
 */
static const char *k_lb_ps = R"(
cbuffer C : register(b0) { float4 src_rect; float4 bins; };
Texture2D src : register(t0);
float line_frac(bool rows, float a, float span) {
	float cnt = 0.0;
	[loop] for (int k = 0; k < 96; k++) {
		float b = floor((k + 0.5) * span / 96.0);
		int2 p = rows ? int2(src_rect.x + b, src_rect.y + a) : int2(src_rect.x + a, src_rect.y + b);
		float3 c = src.Load(int3(p, 0)).rgb;
		cnt += dot(c, float3(0.2126, 0.7152, 0.0722)) > 0.07 ? 1.0 : 0.0;
	}
	return cnt / 96.0;
}
float4 main(float4 pos : SV_Position) : SV_Target {
	uint i = (uint)pos.x;
	bool rows = pos.y < 1.0;
	float n = rows ? bins.x : bins.y;
	if ((float)i >= n) {
		return 0.0;
	}
	float len = rows ? src_rect.w : src_rect.z;  // along the profile
	float span = rows ? src_rect.z : src_rect.w; // across it
	float a0 = floor(i * len / n);
	float a1 = max(a0, floor((i + 1) * len / n) - 1.0);
	float am = floor((a0 + a1) * 0.5);
	return max(line_frac(rows, a0, span), max(line_frac(rows, am, span), line_frac(rows, a1, span)));
}
)";

struct lb_cb
{
	float src_rect[4];
	float bins[4];
};

/*
 *
 * Service-device resources (created with the lift module, ADR-053 P1b split).
 *
 */

bool
conv_gpu_init(d3d11_lift *l)
{
	{
		// The adapter the runtime's images live on (xrt_conv_image::adapter_luid).
		IDXGIDevice *dd = nullptr;
		IDXGIAdapter *ad = nullptr;
		DXGI_ADAPTER_DESC desc = {};
		if (SUCCEEDED(l->svc_device->QueryInterface(__uuidof(IDXGIDevice), (void **)&dd)) &&
		    SUCCEEDED(dd->GetAdapter(&ad)) && SUCCEEDED(ad->GetDesc(&desc))) {
			l->adapter_luid =
			    ((uint64_t)(uint32_t)desc.AdapterLuid.HighPart << 32) | desc.AdapterLuid.LowPart;
		}
		rel(ad);
		rel(dd);
	}

	(void)l->svc_device->QueryInterface(__uuidof(ID3D11Device1), (void **)&l->svc_device1);
	(void)l->svc_context->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)&l->svc_context4);

	// Snapshot blit resources (service device).
	ID3DBlob *b = nullptr;
	ID3DBlob *err = nullptr;
	bool ok = SUCCEEDED(D3DCompile(k_snap_vs, strlen(k_snap_vs), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0,
	                               &b, &err)) &&
	          SUCCEEDED(l->svc_device->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr,
	                                                      &l->snap_vs));
	rel(b);
	rel(err);
	ok = ok &&
	     SUCCEEDED(D3DCompile(k_snap_ps, strlen(k_snap_ps), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &b,
	                          &err)) &&
	     SUCCEEDED(
	         l->svc_device->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &l->snap_ps));
	rel(b);
	rel(err);
	ok = ok &&
	     SUCCEEDED(D3DCompile(k_snap_ps_scaled, strlen(k_snap_ps_scaled), nullptr, nullptr, nullptr, "main",
	                          "ps_5_0", 0, 0, &b, &err)) &&
	     SUCCEEDED(l->svc_device->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr,
	                                                &l->snap_ps_scaled));
	rel(b);
	rel(err);
	if (ok) {
		D3D11_SAMPLER_DESC sd = {};
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT; // 1:1 snapshot: exact texels
		sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.MaxLOD = D3D11_FLOAT32_MAX;
		ok = SUCCEEDED(l->svc_device->CreateSamplerState(&sd, &l->snap_sampler));
		if (ok) {
			sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT; // capped snapshot: box-filter taps
			ok = SUCCEEDED(l->svc_device->CreateSamplerState(&sd, &l->snap_sampler_linear));
		}
	}

	// Weave-rect snapshot cap, read once in the service process (ADR-042:
	// the runtime owns the policy; the vendor's own input autoscaling only
	// affects inference, not the view synthesis that dominates at 8K).
	l->max_input_edge = u_lift_max_input_edge_parse(getenv("DXR_LIFT_MAX_INPUT_EDGE"));
	if (l->max_input_edge != U_LIFT_MAX_INPUT_EDGE_DEFAULT) {
		U_LOG_W("[lift] DXR_LIFT_MAX_INPUT_EDGE=%u%s", l->max_input_edge,
		        l->max_input_edge == 0 ? " (weave-rect snapshots uncapped)" : "");
	}
	if (ok) {
		D3D11_BUFFER_DESC bd = {};
		bd.ByteWidth = sizeof(snap_cb);
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		ok = SUCCEEDED(l->svc_device->CreateBuffer(&bd, nullptr, &l->snap_cb));
	}
	l->snap_ok = ok && l->svc_device1 != nullptr && l->svc_context4 != nullptr;
	if (!l->snap_ok) {
		U_LOG_E("[lift] snapshot pipeline init failed — lift will report UNAVAILABLE");
	}

	// Letterbox crop (optional: a failure here only disables the crop).
	l->letterbox = u_lift_letterbox_parse(getenv("DXR_LIFT_LETTERBOX"));
	if (l->snap_ok && l->letterbox) {
		bool lok = SUCCEEDED(D3DCompile(k_lb_ps, strlen(k_lb_ps), nullptr, nullptr, nullptr, "main", "ps_5_0",
		                                0, 0, &b, &err)) &&
		           SUCCEEDED(l->svc_device->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(),
		                                                      nullptr, &l->lb_ps));
		if (err != nullptr) {
			U_LOG_E("[lift] letterbox shader: %s", (const char *)err->GetBufferPointer());
		}
		rel(b);
		rel(err);
		if (lok) {
			D3D11_TEXTURE2D_DESC td = {};
			td.Width = U_LIFT_LETTERBOX_BINS_MAX;
			td.Height = 2;
			td.MipLevels = 1;
			td.ArraySize = 1;
			td.Format = DXGI_FORMAT_R32_FLOAT;
			td.SampleDesc.Count = 1;
			td.Usage = D3D11_USAGE_DEFAULT;
			td.BindFlags = D3D11_BIND_RENDER_TARGET;
			lok = SUCCEEDED(l->svc_device->CreateTexture2D(&td, nullptr, &l->lb_tex)) &&
			      SUCCEEDED(l->svc_device->CreateRenderTargetView(l->lb_tex, nullptr, &l->lb_rtv));
		}
		if (lok) {
			D3D11_BUFFER_DESC bd = {};
			bd.ByteWidth = sizeof(lb_cb);
			bd.Usage = D3D11_USAGE_DEFAULT;
			bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			lok = SUCCEEDED(l->svc_device->CreateBuffer(&bd, nullptr, &l->lb_cbuf));
		}
		l->lb_ok = lok;
		if (!lok) {
			U_LOG_E("[lift] letterbox profile init failed — weave-rect snapshots are not cropped");
		}
	} else if (!l->letterbox) {
		U_LOG_W("[lift] DXR_LIFT_LETTERBOX=0 (weave-rect snapshots never cropped to the active area)");
	}
	return l->snap_ok;
}

void
conv_gpu_fini(d3d11_lift *l)
{
	glue_release_all(l);
	rel(l->lb_cbuf);
	rel(l->lb_rtv);
	rel(l->lb_tex);
	rel(l->lb_ps);
	rel(l->snap_cb);
	rel(l->snap_sampler_linear);
	rel(l->snap_sampler);
	rel(l->snap_ps_scaled);
	rel(l->snap_ps);
	rel(l->snap_vs);
	rel(l->svc_context4);
	rel(l->svc_device1);
}

/*!
 * Snapshot @p src region (@p x, @p y, @p w, @p h) into the top-left
 * @p dst_w x @p dst_h of input slot @p s's service texture — 1:1 when the dims
 * match, box-filtered down when the snapshot cap shrank them. Caller holds the
 * service context mutex; takes the slot's keyed mutex itself.
 */
bool
snapshot_blit(d3d11_lift *l,
              blit_dst &s,
              ID3D11ShaderResourceView *src,
              uint32_t src_tw,
              uint32_t src_th,
              uint32_t x,
              uint32_t y,
              uint32_t w,
              uint32_t h,
              uint32_t dst_w,
              uint32_t dst_h)
{
	const bool scaled = dst_w != w || dst_h != h;
	HRESULT hr = s.svc_km->AcquireSync(0, 4);
	if (FAILED(hr) || hr == (HRESULT)WAIT_TIMEOUT) {
		return false;
	}
	ID3D11DeviceContext *ctx = l->svc_context;
	snap_cb cb = {};
	cb.src_rect[0] = (float)x;
	cb.src_rect[1] = (float)y;
	cb.src_rect[2] = (float)w;
	cb.src_rect[3] = (float)h;
	cb.src_size[0] = (float)src_tw;
	cb.src_size[1] = (float)src_th;
	cb.dst_size[0] = (float)dst_w;
	cb.dst_size[1] = (float)dst_h;
	ctx->UpdateSubresource(l->snap_cb, 0, nullptr, &cb, 0, 0);

	// State this sequence completely: the context is shared (immediate_ctx_mutex
	// serializes SEQUENCES, and every sequence states its own state). The fixed-
	// function states are restored afterwards, because this runs in the MIDDLE
	// of a weave sequence whose later draws may rely on what they set earlier.
	ID3D11RasterizerState *prev_rs = nullptr;
	ID3D11BlendState *prev_bs = nullptr;
	float prev_bf[4] = {0, 0, 0, 0};
	UINT prev_mask = 0xffffffff;
	ID3D11DepthStencilState *prev_ds = nullptr;
	UINT prev_ref = 0;
	ctx->RSGetState(&prev_rs);
	ctx->OMGetBlendState(&prev_bs, prev_bf, &prev_mask);
	ctx->OMGetDepthStencilState(&prev_ds, &prev_ref);

	D3D11_VIEWPORT vp = {};
	vp.Width = (float)dst_w;
	vp.Height = (float)dst_h;
	vp.MaxDepth = 1.0f;
	ctx->RSSetViewports(1, &vp);
	D3D11_RECT sc = {0, 0, (LONG)dst_w, (LONG)dst_h};
	ctx->RSSetScissorRects(1, &sc);
	ctx->RSSetState(nullptr);
	ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
	ctx->OMSetDepthStencilState(nullptr, 0);
	ctx->OMSetRenderTargets(1, &s.svc_rtv, nullptr);
	ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	ctx->IASetInputLayout(nullptr);
	ctx->VSSetShader(l->snap_vs, nullptr, 0);
	ctx->GSSetShader(nullptr, nullptr, 0);
	ctx->PSSetShader(scaled ? l->snap_ps_scaled : l->snap_ps, nullptr, 0);
	ctx->PSSetConstantBuffers(0, 1, &l->snap_cb);
	ctx->PSSetSamplers(0, 1, scaled ? &l->snap_sampler_linear : &l->snap_sampler);
	ctx->PSSetShaderResources(0, 1, &src);
	ctx->Draw(4, 0);
	ID3D11ShaderResourceView *null_srv = nullptr;
	ctx->PSSetShaderResources(0, 1, &null_srv);
	ID3D11RenderTargetView *null_rtv = nullptr;
	ctx->OMSetRenderTargets(1, &null_rtv, nullptr);
	ctx->RSSetState(prev_rs);
	ctx->OMSetBlendState(prev_bs, prev_bf, prev_mask);
	ctx->OMSetDepthStencilState(prev_ds, prev_ref);
	rel(prev_rs);
	rel(prev_bs);
	rel(prev_ds);

	s.svc_km->ReleaseSync(0);
	return true;
}

/*!
 * Letterbox: collect finished profile readbacks (never waits) into the rect's
 * crop state, then measure this frame's rect into the next free readback. The
 * crop therefore lags the picture by a few frames, which the settle hysteresis
 * dwarfs anyway. The weave's producer thread (service context mutex held).
 */
static void
letterbox_step(
    d3d11_lift *l, core_weave_state &st, ID3D11ShaderResourceView *src, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
	ID3D11DeviceContext *ctx = l->svc_context;

	// 1. Oldest-first collection, stop at the first still in flight.
	for (uint32_t k = 0; k < LB_RING; k++) {
		lift_lb_readback &r = st.lb_rb[(st.lb_next + k) % LB_RING];
		if (!r.pending) {
			continue;
		}
		D3D11_MAPPED_SUBRESOURCE ms = {};
		HRESULT hr = ctx->Map(r.staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &ms);
		if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
			break;
		}
		r.pending = false;
		if (FAILED(hr)) {
			continue;
		}
		const float *rows = (const float *)ms.pData;
		const float *cols = (const float *)((const uint8_t *)ms.pData + ms.RowPitch);
		const bool changed = u_lift_letterbox_update(&st.lb, r.w, r.h, rows, r.nr, cols, r.nc);
		// The row profile that decided it, top to bottom in 64 bands, each the
		// band's max non-black fraction as a digit 0-9 — so a surprising crop
		// change in the field says what was in the bars (captions, player UI).
		char prof[65] = {0};
		if (changed) {
			for (uint32_t k = 0; k < 64; k++) {
				const uint32_t b0 = k * r.nr / 64,
				               b1 = (k + 1) * r.nr / 64 > b0 ? (k + 1) * r.nr / 64 : b0 + 1;
				float mx = 0.0f;
				for (uint32_t i = b0; i < b1 && i < r.nr; i++) {
					mx = rows[i] > mx ? rows[i] : mx;
				}
				const int d = (int)(mx * 10.0f);
				prof[k] = (char)('0' + (d > 9 ? 9 : (d < 0 ? 0 : d)));
			}
		}
		ctx->Unmap(r.staging, 0);
		if (changed) {
			const u_lift_crop &c = st.lb.committed;
			if (u_lift_crop_active(&c)) {
				U_LOG_W(
				    "[lift] stream %llu: letterbox crop %ux%u -> active %ux%u (bars top %u bottom %u "
				    "left %u "
				    "right %u; bars woven flat) rows[%s]",
				    (unsigned long long)st.id, r.w, r.h, r.w - c.left - c.right, r.h - c.top - c.bottom,
				    c.top, c.bottom, c.left, c.right, prof);
			} else {
				U_LOG_W("[lift] stream %llu: letterbox crop off (%ux%u lifted whole) rows[%s]",
				        (unsigned long long)st.id, r.w, r.h, prof);
			}
		}
	}

	// 2. Measure this frame into the next ring entry, if it is free.
	lift_lb_readback &r = st.lb_rb[st.lb_next];
	if (r.pending) {
		return; // GPU behind by LB_RING readbacks: skip a measurement, never stall
	}
	if (r.staging == nullptr) {
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = U_LIFT_LETTERBOX_BINS_MAX;
		td.Height = 2;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R32_FLOAT;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_STAGING;
		td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		if (FAILED(l->svc_device->CreateTexture2D(&td, nullptr, &r.staging))) {
			return;
		}
	}
	r.w = w;
	r.h = h;
	r.nr = h < U_LIFT_LETTERBOX_BINS_MAX ? h : U_LIFT_LETTERBOX_BINS_MAX;
	r.nc = w < U_LIFT_LETTERBOX_BINS_MAX ? w : U_LIFT_LETTERBOX_BINS_MAX;

	lb_cb cb = {};
	cb.src_rect[0] = (float)x;
	cb.src_rect[1] = (float)y;
	cb.src_rect[2] = (float)w;
	cb.src_rect[3] = (float)h;
	cb.bins[0] = (float)r.nr;
	cb.bins[1] = (float)r.nc;
	ctx->UpdateSubresource(l->lb_cbuf, 0, nullptr, &cb, 0, 0);

	// Same state discipline as snapshot_blit: this runs mid weave sequence.
	ID3D11RasterizerState *prev_rs = nullptr;
	ID3D11BlendState *prev_bs = nullptr;
	float prev_bf[4] = {0, 0, 0, 0};
	UINT prev_mask = 0xffffffff;
	ID3D11DepthStencilState *prev_ds = nullptr;
	UINT prev_ref = 0;
	ctx->RSGetState(&prev_rs);
	ctx->OMGetBlendState(&prev_bs, prev_bf, &prev_mask);
	ctx->OMGetDepthStencilState(&prev_ds, &prev_ref);

	D3D11_VIEWPORT vp = {};
	vp.Width = (float)U_LIFT_LETTERBOX_BINS_MAX;
	vp.Height = 2.0f;
	vp.MaxDepth = 1.0f;
	ctx->RSSetViewports(1, &vp);
	D3D11_RECT sc = {0, 0, (LONG)U_LIFT_LETTERBOX_BINS_MAX, 2};
	ctx->RSSetScissorRects(1, &sc);
	ctx->RSSetState(nullptr);
	ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
	ctx->OMSetDepthStencilState(nullptr, 0);
	ctx->OMSetRenderTargets(1, &l->lb_rtv, nullptr);
	ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	ctx->IASetInputLayout(nullptr);
	ctx->VSSetShader(l->snap_vs, nullptr, 0);
	ctx->GSSetShader(nullptr, nullptr, 0);
	ctx->PSSetShader(l->lb_ps, nullptr, 0);
	ctx->PSSetConstantBuffers(0, 1, &l->lb_cbuf);
	ctx->PSSetShaderResources(0, 1, &src);
	ctx->Draw(4, 0);
	ID3D11ShaderResourceView *null_srv = nullptr;
	ctx->PSSetShaderResources(0, 1, &null_srv);
	ID3D11RenderTargetView *null_rtv = nullptr;
	ctx->OMSetRenderTargets(1, &null_rtv, nullptr);
	ctx->RSSetState(prev_rs);
	ctx->OMSetBlendState(prev_bs, prev_bf, prev_mask);
	ctx->OMSetDepthStencilState(prev_ds, prev_ref);
	rel(prev_rs);
	rel(prev_bs);
	rel(prev_ds);

	ctx->CopyResource(r.staging, l->lb_tex);
	r.pending = true;
	st.lb_next = (st.lb_next + 1) % LB_RING;
}


bool
d3d11_lift_pin_depth(struct d3d11_lift_pin *pin)
{
	if (pin == nullptr || !pin->valid || pin->depth_km == nullptr) {
		return false;
	}
	if (pin->depth_held) {
		return true;
	}
	// The slot is pinned, so the lift thread will not write it; a busy mutex
	// means the acquire export is copying it out right now. Skip this frame.
	HRESULT hr = pin->depth_km->AcquireSync(0, 0);
	if (FAILED(hr) || hr == (HRESULT)WAIT_TIMEOUT) {
		return false;
	}
	pin->depth_held = true;
	return true;
}

/*
 *
 * Eyes (G7), producer side: the conversion core reads the panel's eyes from
 * its own per-DP snapshot; the service publishes there from every eye query.
 * A producer that finds the last publish older than 50 ms asks the service for
 * a fresh query first (it may take render_mutex — on the producer's thread,
 * never on the lift thread or the core's dispatcher).
 *
 */

void
d3d11_lift_publish_eyes(struct d3d11_lift *l, const struct xrt_eye_positions *eyes)
{
	if (l == nullptr || eyes == nullptr) {
		return;
	}
	std::lock_guard<std::mutex> g(l->eyes_mtx);
	l->eyes_ns = os_monotonic_get_ns();
}

void
d3d11_lift_set_eyes_refresh(struct d3d11_lift *l, void (*fn)(void *ud), void *ud)
{
	if (l == nullptr) {
		return;
	}
	std::lock_guard<std::mutex> g(l->eyes_mtx);
	l->eyes_refresh = fn;
	l->eyes_refresh_ud = ud;
}

//! Producer side: pull fresh eyes when nothing published them lately.
static void
eyes_refresh_if_stale(d3d11_lift *l)
{
	void (*fn)(void *) = nullptr;
	void *ud = nullptr;
	{
		std::lock_guard<std::mutex> g(l->eyes_mtx);
		const uint64_t now = os_monotonic_get_ns();
		if (l->eyes_ns != 0 && now - l->eyes_ns < 50ull * 1000ull * 1000ull) {
			return;
		}
		fn = l->eyes_refresh;
		ud = l->eyes_refresh_ud;
	}
	if (fn != nullptr) {
		fn(ud); // publishes through d3d11_lift_publish_eyes
	}
}

void
d3d11_lift_set_core_bridge(struct d3d11_lift *l, const struct d3d11_lift_core_bridge *bridge)
{
	if (l == nullptr) {
		return;
	}
	std::lock_guard<std::mutex> g(l->bridge_mtx);
	if (bridge != nullptr) {
		l->bridge = *bridge;
	} else {
		l->bridge = d3d11_lift_core_bridge{};
	}
}

bool
bridge_resolve(
    d3d11_lift *l, uint64_t owner, uint64_t wire_id, conv_core **out_core, uint64_t *out_core_id, uint32_t *out_mode)
{
	d3d11_lift_core_bridge b;
	{
		std::lock_guard<std::mutex> g(l->bridge_mtx);
		b = l->bridge;
	}
	if (b.core == nullptr || b.resolve == nullptr || !b.resolve(b.ud, owner, wire_id, out_core_id, out_mode)) {
		return false;
	}
	if (out_core != nullptr) {
		*out_core = b.core;
	}
	return true;
}


/*
 *
 * The conversion core's D3D11 glue on the service device (#2003).
 *
 * Every entry point is gated (glue_enter / glue_leave): d3d11_lift_destroy
 * closes the gate and waits for in-flight calls before anything goes away.
 * glue_mtx guards the tables only and is never held across a GPU wait or the
 * service context mutex (order: svc_ctx_mutex -> glue_mtx).
 *
 */

static bool
glue_enter(d3d11_lift *l)
{
	std::lock_guard<std::mutex> g(l->glue_mtx);
	if (l->glue_closed) {
		return false;
	}
	l->glue_calls++;
	return true;
}

static void
glue_leave(d3d11_lift *l)
{
	std::lock_guard<std::mutex> g(l->glue_mtx);
	l->glue_calls--;
	l->glue_cv.notify_all();
}

void
glue_close(d3d11_lift *l)
{
	std::unique_lock<std::mutex> lk(l->glue_mtx);
	l->glue_closed = true;
	l->glue_cv.wait(lk, [l] { return l->glue_calls == 0; });
}

static void
glue_img_release(glue_img &gi)
{
	rel(gi.rtv);
	rel(gi.srv);
	rel(gi.km);
	rel(gi.tex);
	close_handle(gi.share);
}

static void
glue_stream_release(glue_stream &gs)
{
	rel(gs.imp_km);
	rel(gs.imp_srv);
	rel(gs.imp_tex);
	if (!gs.imp_dxgi) {
		close_handle(gs.imp_handle);
	}
	gs.imp_handle = nullptr;
	rel(gs.exp_fence);
	close_handle(gs.exp_fence_handle);
	rel(gs.exp_tex);
	close_handle(gs.exp_handle);
	rel(gs.exp_depth_tex);
	close_handle(gs.exp_depth_handle);
	rel(gs.imp_fence);
	close_handle(gs.imp_fence_handle);
	rel(gs.cpu_srv);
	rel(gs.cpu_tex);
	if (gs.cpu_view != nullptr) {
		UnmapViewOfFile(gs.cpu_view);
		gs.cpu_view = nullptr;
	}
	close_handle(gs.cpu_map);
	rel(gs.cpu_stage);
}

void
glue_release_all(d3d11_lift *l)
{
	std::lock_guard<std::mutex> g(l->glue_mtx);
	for (auto &kv : l->glue_imgs) {
		glue_img_release(kv.second);
	}
	l->glue_imgs.clear();
	for (auto &kv : l->glue_streams) {
		glue_stream_release(kv.second);
	}
	l->glue_streams.clear();
	for (auto &kv : l->glue_rbs) {
		rel(kv.second.staging);
	}
	l->glue_rbs.clear();
	for (auto &kv : l->core_weave) {
		for (auto &r : kv.second.lb_rb) {
			rel(r.staging);
		}
	}
	l->core_weave.clear();
	d3d11_conv_viewsyn_destroy(&l->viewsyn);
	rel(l->viewsyn_deferred);
	d3d11_conv_content_check_destroy(&l->content_check);
}

//! A glue image by its import key (copied; its COM pointers stay valid while
//! the caller owns the slot / pin it belongs to). False = not ours (CPU).
static bool
glue_find(d3d11_lift *l, uint64_t key, glue_img *out)
{
	std::lock_guard<std::mutex> g(l->glue_mtx);
	auto it = l->glue_imgs.find(key);
	if (it == l->glue_imgs.end()) {
		return false;
	}
	*out = it->second;
	return true;
}

static bool
format_renderable(DXGI_FORMAT f)
{
	return f != DXGI_FORMAT_UNKNOWN;
}

static bool
glue_alloc(void *ud, const struct conv_image_request *req, struct xrt_conv_image *out)
{
	d3d11_lift *l = (d3d11_lift *)ud;
	// The bound capability must take a shared NT handle with a keyed mutex
	// (or say nothing); otherwise the service's CPU allocator serves it.
	if (req->import_memory_kinds != 0 && (req->import_memory_kinds & XRT_CONV_MEM_WIN32_NT_HANDLE) == 0) {
		return false;
	}
	if (req->import_sync_kinds != 0 && (req->import_sync_kinds & (1u << XRT_CONV_SYNC_KEYED_MUTEX)) == 0) {
		return false;
	}
	const DXGI_FORMAT f = conv_fmt_to_dxgi(req->format);
	if (f == DXGI_FORMAT_UNKNOWN || req->width == 0 || req->height == 0 || !glue_enter(l)) {
		return false;
	}
	glue_img gi;
	gi.w = req->width;
	gi.h = req->height;
	gi.layers = req->array_layers > 0 ? req->array_layers : 1;
	gi.dxgi = f;
	gi.stream_id = req->stream_id;
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = gi.w;
	td.Height = gi.h;
	td.MipLevels = 1;
	td.ArraySize = gi.layers;
	td.Format = f;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (format_renderable(f) ? D3D11_BIND_RENDER_TARGET : 0u);
	td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
	HRESULT hr = l->svc_device->CreateTexture2D(&td, nullptr, &gi.tex);
	if (SUCCEEDED(hr)) {
		hr = gi.tex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&gi.km);
	}
	IDXGIResource1 *r1 = nullptr;
	if (SUCCEEDED(hr)) {
		hr = gi.tex->QueryInterface(__uuidof(IDXGIResource1), (void **)&r1);
	}
	if (SUCCEEDED(hr)) {
		hr = r1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
		                            &gi.share);
	}
	rel(r1);
	if (SUCCEEDED(hr)) {
		D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
		sd.Format = typed_srv_format(f);
		if (gi.layers > 1) {
			sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
			sd.Texture2DArray.MipLevels = 1;
			sd.Texture2DArray.ArraySize = gi.layers;
		} else {
			sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			sd.Texture2D.MipLevels = 1;
		}
		hr = l->svc_device->CreateShaderResourceView(gi.tex, &sd, &gi.srv);
	}
	if (SUCCEEDED(hr) && format_renderable(f)) {
		D3D11_RENDER_TARGET_VIEW_DESC rd = {};
		rd.Format = typed_srv_format(f);
		rd.ViewDimension =
		    D3D11_RTV_DIMENSION_TEXTURE2D; // slice 0 (ROW layouts; ARRAY views render per slice in S5)
		if (gi.layers > 1) {
			rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
			rd.Texture2DArray.ArraySize = 1;
		}
		hr = l->svc_device->CreateRenderTargetView(gi.tex, &rd, &gi.rtv);
	}
	if (FAILED(hr)) {
		U_LOG_E("[conv] D3D11 glue: image %ux%u fmt=%u create/share failed: 0x%08lx", gi.w, gi.h, req->format,
		        (unsigned long)hr);
		glue_img_release(gi);
		glue_leave(l);
		return false;
	}
	uint64_t key;
	{
		std::lock_guard<std::mutex> g(l->glue_mtx);
		key = ++l->glue_next_key; // never collides with the service CPU allocator's (bit 63 set)
		l->glue_imgs[key] = gi;
	}
	memset(out, 0, sizeof(*out));
	out->memory_kind = XRT_CONV_MEM_WIN32_NT_HANDLE;
	out->handle = (uint64_t)(uintptr_t)gi.share;
	out->import_key = key;
	out->width = gi.w;
	out->height = gi.h;
	out->format = req->format;
	out->color_space = (req->role == 0 || req->role == XRT_CONV_OUT_VIEWS) ? XRT_CONV_CS_SRGB : XRT_CONV_CS_DATA;
	out->adapter_luid = l->adapter_luid;
	out->sync.kind = XRT_CONV_SYNC_KEYED_MUTEX;
	out->sync.import_key = key;
	out->sync.acquire_value = 0;
	out->sync.release_value = 0;
	glue_leave(l);
	return true;
}

static void
glue_free(void *ud, struct xrt_conv_image *img)
{
	d3d11_lift *l = (d3d11_lift *)ud;
	if (img == nullptr || img->memory_kind != XRT_CONV_MEM_WIN32_NT_HANDLE || !glue_enter(l)) {
		return;
	}
	{
		std::lock_guard<std::mutex> g(l->glue_mtx);
		auto it = l->glue_imgs.find(img->import_key);
		if (it != l->glue_imgs.end()) {
			d3d11_conv_content_check_forget_image(l->content_check, img->import_key);
			const uint64_t sid = it->second.stream_id;
			glue_img_release(it->second);
			l->glue_imgs.erase(it);
			bool any = false;
			for (auto &kv : l->glue_imgs) {
				any = any || kv.second.stream_id == sid;
			}
			if (!any) {
				auto rb = l->glue_rbs.find(sid);
				if (rb != l->glue_rbs.end()) {
					rel(rb->second.staging);
					l->glue_rbs.erase(rb);
				}
			}
		}
	}
	{
		// The lift thread drops its view of it on its next command.
		std::lock_guard<std::mutex> g(l->mtx);
		l->lift_evict.push_back(img->import_key);
	}
	glue_leave(l);
}

static uint32_t
conv_fmt_bytes(uint32_t fmt)
{
	switch (fmt) {
	case XRT_CONV_FMT_R8_UNORM: return 1;
	case XRT_CONV_FMT_R16_FLOAT:
	case XRT_CONV_FMT_R16_UNORM: return 2;
	case XRT_CONV_FMT_RGBA16_FLOAT: return 8;
	default: return 4;
	}
}

/*!
 * #2030: a CPU-memory provider result (the sim provider, a CPU-only provider)
 * into its GPU mirror, on the conversion dispatcher thread, so the weave path
 * pins views instead of weaving the rect flat. One UpdateSubresource per
 * layer (nothing bound: no shared-context state is touched) inside the
 * mirror's keyed mutex. CPU ARRAY views hold their layers back to back.
 */
static bool
glue_upload(void *ud, const struct xrt_conv_image *src, const struct xrt_conv_image *dst)
{
	d3d11_lift *l = (d3d11_lift *)ud;
	if (src == nullptr || dst == nullptr || src->memory_kind != XRT_CONV_MEM_CPU || src->cpu_ptr == nullptr ||
	    dst->memory_kind != XRT_CONV_MEM_WIN32_NT_HANDLE || !glue_enter(l)) {
		return false;
	}
	glue_img g;
	const uint32_t pitch = src->row_stride != 0 ? src->row_stride : src->width * conv_fmt_bytes(src->format);
	const uint64_t layer_bytes = (uint64_t)pitch * src->height;
	bool ok = glue_find(l, dst->import_key, &g) && g.w == src->width && g.h == src->height &&
	          g.dxgi == conv_fmt_to_dxgi(src->format) && src->byte_size >= layer_bytes * g.layers;
	const bool held = ok && img_acquire(g.km, dst, 4);
	if (held) {
		std::lock_guard<std::mutex> ctx_lock(*l->svc_ctx_mutex);
		for (uint32_t i = 0; i < g.layers; i++) {
			l->svc_context->UpdateSubresource(g.tex, D3D11CalcSubresource(0, i, 1), nullptr,
			                                  (const uint8_t *)src->cpu_ptr + layer_bytes * i, pitch, 0);
		}
		l->svc_context->Flush();
	}
	if (held) {
		img_release(g.km, dst);
	}
	glue_leave(l);
	return held;
}

void
d3d11_lift_glue_alloc_ops(struct d3d11_lift *l, struct conv_image_alloc_ops *out)
{
	memset(out, 0, sizeof(*out));
	if (l == nullptr) {
		return;
	}
	out->alloc = glue_alloc;
	out->free = glue_free;
	out->upload = glue_upload;
	out->ud = l;
}

//! IEEE half -> float (the depth may be R16_FLOAT).
static float
half_to_float(uint16_t h)
{
	const uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
	uint32_t exp = (h >> 10) & 0x1fu;
	uint32_t man = h & 0x3ffu;
	uint32_t bits;
	if (exp == 0) {
		if (man == 0) {
			bits = sign;
		} else {
			// Subnormal: normalise.
			exp = 127 - 15 + 1;
			while ((man & 0x400u) == 0) {
				man <<= 1;
				exp--;
			}
			man &= 0x3ffu;
			bits = sign | (exp << 23) | (man << 13);
		}
	} else if (exp == 0x1f) {
		bits = sign | 0x7f800000u | (man << 13); // inf / NaN
	} else {
		bits = sign | ((exp + 127 - 15) << 23) | (man << 13);
	}
	float f;
	memcpy(&f, &bits, sizeof(f));
	return f;
}
/*
 * S6 / S7: depth readback. Never stalls: one staging copy in flight per
 * stream, issued at most every U_LIFT_AUTOCONV_SAMPLE_PERIOD_NS (the lift
 * thread's old cadence) and mapped DO_NOT_WAIT on a later conversion. The
 * samples returned are therefore of an earlier conversion of the same stream.
 */
static bool
glue_rb_read(void *ud,
             const struct xrt_conv_image *depth,
             const float **out_samples,
             uint32_t *out_w,
             uint32_t *out_h,
             uint32_t *out_stride_floats)
{
	d3d11_lift *l = (d3d11_lift *)ud;
	if (depth == nullptr || depth->memory_kind != XRT_CONV_MEM_WIN32_NT_HANDLE || !glue_enter(l)) {
		return false;
	}
	glue_img gi;
	glue_rb *rb = nullptr;
	{
		std::lock_guard<std::mutex> g(l->glue_mtx);
		auto it = l->glue_imgs.find(depth->import_key);
		if (it != l->glue_imgs.end()) {
			gi = it->second;
			rb = &l->glue_rbs[gi.stream_id]; // std::map: stable; one dispatcher per stream
		}
	}
	if (rb == nullptr || (gi.dxgi != DXGI_FORMAT_R32_FLOAT && gi.dxgi != DXGI_FORMAT_R16_FLOAT)) {
		glue_leave(l);
		return false;
	}
	bool fresh = false;
	const uint64_t now = os_monotonic_get_ns();
	if (rb->pending && rb->staging != nullptr) {
		std::lock_guard<std::mutex> ctx_lock(*l->svc_ctx_mutex);
		D3D11_MAPPED_SUBRESOURCE m = {};
		HRESULT hr = l->svc_context->Map(rb->staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
		if (hr != DXGI_ERROR_WAS_STILL_DRAWING) {
			rb->pending = false;
			if (SUCCEEDED(hr)) {
				rb->samples.resize((size_t)rb->w * rb->h);
				for (uint32_t y = 0; y < rb->h; y++) {
					const uint8_t *row = (const uint8_t *)m.pData + (size_t)y * m.RowPitch;
					float *dst = &rb->samples[(size_t)y * rb->w];
					if (rb->fmt == DXGI_FORMAT_R32_FLOAT) {
						memcpy(dst, row, (size_t)rb->w * sizeof(float));
					} else {
						for (uint32_t x = 0; x < rb->w; x++) {
							dst[x] = half_to_float(((const uint16_t *)row)[x]);
						}
					}
				}
				l->svc_context->Unmap(rb->staging, 0);
				fresh = true;
			}
		}
	}
	if (!rb->pending && (rb->issued_ns == 0 || now - rb->issued_ns >= U_LIFT_AUTOCONV_SAMPLE_PERIOD_NS)) {
		if (rb->staging == nullptr || rb->w != gi.w || rb->h != gi.h || rb->fmt != gi.dxgi) {
			rel(rb->staging);
			D3D11_TEXTURE2D_DESC td = {};
			td.Width = gi.w;
			td.Height = gi.h;
			td.MipLevels = 1;
			td.ArraySize = 1;
			td.Format = gi.dxgi;
			td.SampleDesc.Count = 1;
			td.Usage = D3D11_USAGE_STAGING;
			td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			if (FAILED(l->svc_device->CreateTexture2D(&td, nullptr, &rb->staging))) {
				rb->staging = nullptr;
			}
			rb->w = gi.w;
			rb->h = gi.h;
			rb->fmt = gi.dxgi;
		}
		if (rb->staging != nullptr && img_acquire(gi.km, depth, 4)) {
			{
				std::lock_guard<std::mutex> ctx_lock(*l->svc_ctx_mutex);
				l->svc_context->CopyResource(rb->staging, gi.tex);
				l->svc_context->Flush();
			}
			img_release(gi.km, depth);
			rb->pending = true;
			rb->issued_ns = now;
		}
	}
	if (fresh) {
		*out_samples = rb->samples.data();
		*out_w = rb->w;
		*out_h = rb->h;
		*out_stride_floats = rb->w;
	}
	glue_leave(l);
	return fresh;
}

static void
glue_rb_release(void *ud, const float *samples)
{
	(void)ud;
	(void)samples; // owned by the stream's readback state until its next read
}

/*!
 * #2030: run one S5 synthesis off the shared immediate context. The draws are
 * recorded on the glue's own deferred context (no service lock held), then
 * replayed with ExecuteCommandList(list, TRUE): the list starts from the
 * default state and the immediate context's state is saved and restored
 * around it, so no binding leaks either way between the synthesis and the
 * other threads drawing on that context (shared immediate-context state must
 * be per draw). The images' keyed mutexes are held by the caller across both.
 * Caller holds viewsyn_mtx; the stage exists.
 */
static bool
glue_viewsyn_draw(d3d11_lift *l,
                  const d3d11_conv_viewsyn_inputs *in,
                  const u_conv_viewsyn_params *p,
                  const d3d11_conv_viewsyn_target *tg)
{
	if (l->viewsyn_deferred == nullptr) {
		static bool logged = false;
		const HRESULT hr = l->svc_device->CreateDeferredContext(0, &l->viewsyn_deferred);
		if (FAILED(hr) && !logged) {
			logged = true;
			l->viewsyn_deferred = nullptr;
			U_LOG_W("[conv] S5: no deferred context (0x%08lx): view synthesis draws on the service context",
			        (unsigned long)hr);
		}
	}
	if (l->viewsyn_deferred == nullptr) {
		std::lock_guard<std::mutex> ctx_lock(*l->svc_ctx_mutex);
		const bool ok = d3d11_conv_viewsyn_render(l->viewsyn, l->svc_context, in, p, tg);
		l->svc_context->Flush();
		return ok;
	}
	ID3D11CommandList *list = nullptr;
	if (!d3d11_conv_viewsyn_record(l->viewsyn, l->viewsyn_deferred, in, p, tg, &list)) {
		return false;
	}
	{
		std::lock_guard<std::mutex> ctx_lock(*l->svc_ctx_mutex);
		l->svc_context->ExecuteCommandList(list, TRUE);
		l->svc_context->Flush();
	}
	rel(list);
	return true;
}

//! S5: the reference view synthesis on the service device.
static bool
glue_viewsyn_render(void *ud, const struct conv_viewsyn_request *req)
{
	d3d11_lift *l = (d3d11_lift *)ud;
	if (req == nullptr || req->color == nullptr || req->depth == nullptr || req->target == nullptr ||
	    !glue_enter(l)) {
		return false;
	}
	glue_img c, d, t;
	bool ok = glue_find(l, req->color->import_key, &c) && glue_find(l, req->depth->import_key, &d) &&
	          glue_find(l, req->target->import_key, &t);
	std::lock_guard<std::mutex> vg(l->viewsyn_mtx);
	if (ok && l->viewsyn == nullptr && !l->viewsyn_tried) {
		l->viewsyn_tried = true;
		l->viewsyn = d3d11_conv_viewsyn_create(l->svc_device);
	}
	ok = ok && l->viewsyn != nullptr;
	bool held_c = false, held_d = false, held_t = false;
	ok = ok && (held_c = img_acquire(c.km, req->color, 4));
	ok = ok && (held_d = img_acquire(d.km, req->depth, 4));
	ok = ok && (held_t = img_acquire(t.km, req->target, 4));
	if (ok) {
		u_conv_viewsyn_params p;
		u_conv_viewsyn_params_defaults(&p);
		p.view_count = req->view_count;
		for (uint32_t i = 0; i < req->view_count && i < U_CONV_VIEWSYN_MAX_VIEWS; i++) {
			p.viewpoints_m[i][0] = req->viewpoints_m[i][0];
			p.viewpoints_m[i][1] = req->viewpoints_m[i][1];
			p.viewpoints_m[i][2] = req->viewpoints_m[i][2];
		}
		p.rect_width_m = req->rect_width_m;
		p.rect_height_m = req->rect_height_m;
		if (req->depth_info != nullptr) {
			p.depth = *req->depth_info;
		}
		p.range_near = req->range_near;
		p.range_far = req->range_far;
		p.convergence = req->convergence;
		p.budget = req->budget;
		p.rear_cap_m = req->rear_cap_m;
		d3d11_conv_viewsyn_inputs in = {};
		in.color = c.srv;
		in.color_width = c.w;
		in.color_height = c.h;
		in.depth = d.srv;
		in.depth_width = d.w;
		in.depth_height = d.h;
		in.new_content = req->new_content;
		d3d11_conv_viewsyn_target tg = {};
		tg.texture = t.tex;
		tg.rtv_format = DXGI_FORMAT_UNKNOWN;
		tg.layout = req->layout;
		ok = glue_viewsyn_draw(l, &in, &p, &tg);
	}
	if (held_t) {
		img_release(t.km, req->target);
	}
	if (held_d) {
		img_release(d.km, req->depth);
	}
	if (held_c) {
		img_release(c.km, req->color);
	}
	glue_leave(l);
	return ok;
}

void
d3d11_lift_glue_gpu_ops(struct d3d11_lift *l,
                        struct conv_viewsyn_ops *viewsyn,
                        struct conv_depth_readback_ops *readback)
{
	if (viewsyn != nullptr) {
		memset(viewsyn, 0, sizeof(*viewsyn));
		if (l != nullptr) {
			viewsyn->render = glue_viewsyn_render;
			viewsyn->ud = l;
		}
	}
	if (readback != nullptr) {
		memset(readback, 0, sizeof(*readback));
		if (l != nullptr) {
			readback->read = glue_rb_read;
			readback->release = glue_rb_release;
			readback->ud = l;
		}
	}
}

/*!
 * Open / reuse the client's shared input for v3 stream @p wire (producer
 * thread). Takes ownership of @p handle. Returns the cached import.
 */
static bool
glue_import_client(d3d11_lift *l, uint64_t wire, HANDLE handle, bool is_dxgi, glue_stream **out_gs)
{
	glue_stream *gs;
	{
		std::lock_guard<std::mutex> g(l->glue_mtx);
		gs = &l->glue_streams[wire]; // std::map: stable; one producer thread per stream
	}
	*out_gs = gs;
	bool reuse = false;
	if (gs->imp_tex != nullptr && gs->imp_dxgi == is_dxgi) {
		reuse = is_dxgi ? (gs->imp_handle == handle) : same_kernel_object(gs->imp_handle, handle);
	}
	if (reuse) {
		if (!is_dxgi) {
			CloseHandle(handle);
		}
		return true;
	}
	rel(gs->imp_km);
	rel(gs->imp_srv);
	rel(gs->imp_tex);
	if (!gs->imp_dxgi) {
		close_handle(gs->imp_handle);
	}
	gs->imp_handle = nullptr;
	HRESULT hr;
	if (is_dxgi) {
		hr = l->svc_device->OpenSharedResource(handle, __uuidof(ID3D11Texture2D), (void **)&gs->imp_tex);
	} else {
		hr = l->svc_device1->OpenSharedResource1(handle, __uuidof(ID3D11Texture2D), (void **)&gs->imp_tex);
	}
	D3D11_TEXTURE2D_DESC d = {};
	if (SUCCEEDED(hr)) {
		gs->imp_tex->GetDesc(&d);
		D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
		sd.Format = typed_srv_format(d.Format);
		sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		sd.Texture2D.MipLevels = 1;
		hr = l->svc_device->CreateShaderResourceView(gs->imp_tex, &sd, &gs->imp_srv);
	}
	if (FAILED(hr)) {
		U_LOG_E("[conv] v3 input OpenSharedResource(%s) failed: 0x%08lx", is_dxgi ? "DXGI" : "NT",
		        (unsigned long)hr);
		rel(gs->imp_srv);
		rel(gs->imp_tex);
		if (!is_dxgi) {
			CloseHandle(handle);
		}
		return false;
	}
	(void)gs->imp_tex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&gs->imp_km);
	gs->imp_handle = handle; // kept open (NT) for the identity compare
	gs->imp_dxgi = is_dxgi;
	gs->imp_w = d.Width;
	gs->imp_h = d.Height;
	U_LOG_I("[conv] v3 stream 0x%llx input import cached (%s, %ux%u)", (unsigned long long)wire,
	        is_dxgi ? "DXGI" : "NT", d.Width, d.Height);
	return true;
}

xrt_result_t
d3d11_lift_glue_import_input(struct d3d11_lift *l,
                             uint64_t wire,
                             HANDLE handle,
                             bool is_dxgi,
                             uint32_t w,
                             uint32_t h,
                             const struct conv_submit_slot *slot)
{
	if (l == nullptr || !l->snap_ok || slot == nullptr || slot->image_count < 1 || !glue_enter(l)) {
		if (handle != nullptr && !is_dxgi) {
			CloseHandle(handle);
		}
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	eyes_refresh_if_stale(l);
	glue_stream *gs = nullptr;
	if (!glue_import_client(l, wire, handle, is_dxgi, &gs)) {
		glue_leave(l);
		return XRT_ERROR_WEAVE_REFUSED;
	}
	const xrt_conv_image *img = &slot->images[0];
	w = w > gs->imp_w ? gs->imp_w : w;
	h = h > gs->imp_h ? gs->imp_h : h;
	glue_img gi;
	const bool gpu = glue_find(l, img->import_key, &gi);
	if (!gpu &&
	    (img->memory_kind != XRT_CONV_MEM_CPU || img->cpu_ptr == nullptr || img->width != w || img->height != h)) {
		glue_leave(l);
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	}
	// The caller's keyed mutex: 4 ms, the budget every weave-side acquire uses.
	bool acquired = false;
	if (gs->imp_km != nullptr) {
		HRESULT hr = gs->imp_km->AcquireSync(0, 4);
		if (FAILED(hr) || hr == (HRESULT)WAIT_TIMEOUT) {
			glue_leave(l);
			return XRT_ERROR_WEAVE_REFUSED;
		}
		acquired = true;
	}
	bool ok;
	if (gpu) {
		std::lock_guard<std::mutex> ctx_lock(*l->svc_ctx_mutex);
		// The snapshot into the core's slot: 1:1 (point) at the
		// submitted size, box-filtered when inputScale shrank the slot.
		blit_dst dst = {gi.km, gi.rtv};
		ok = gi.rtv != nullptr &&
		     snapshot_blit(l, dst, gs->imp_srv, gs->imp_w, gs->imp_h, 0, 0, w, h, gi.w, gi.h);
		l->svc_context->Flush();
	} else {
		// A CPU provider's slot: read the region back (slow path, sim providers only).
		D3D11_TEXTURE2D_DESC td = {};
		gs->imp_tex->GetDesc(&td);
		td.Width = w;
		td.Height = h;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Usage = D3D11_USAGE_STAGING;
		td.BindFlags = 0;
		td.MiscFlags = 0;
		td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		ID3D11Texture2D *st = nullptr;
		ok = (td.Format == DXGI_FORMAT_R8G8B8A8_UNORM || td.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
		      td.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) &&
		     SUCCEEDED(l->svc_device->CreateTexture2D(&td, nullptr, &st));
		if (ok) {
			std::lock_guard<std::mutex> ctx_lock(*l->svc_ctx_mutex);
			D3D11_BOX box = {0, 0, 0, w, h, 1};
			l->svc_context->CopySubresourceRegion(st, 0, 0, 0, 0, gs->imp_tex, 0, &box);
			D3D11_MAPPED_SUBRESOURCE m = {};
			ok = SUCCEEDED(l->svc_context->Map(st, 0, D3D11_MAP_READ, 0, &m));
			if (ok) {
				const uint32_t stride = img->row_stride != 0 ? img->row_stride : w * 4u;
				for (uint32_t y = 0; y < h; y++) {
					memcpy((uint8_t *)img->cpu_ptr + (size_t)y * stride,
					       (const uint8_t *)m.pData + (size_t)y * m.RowPitch, (size_t)w * 4u);
				}
				l->svc_context->Unmap(st, 0);
			}
		}
		rel(st);
	}
	if (acquired) {
		gs->imp_km->ReleaseSync(0);
	}
	glue_leave(l);
	return ok ? XRT_SUCCESS : XRT_ERROR_WEAVE_REFUSED;
}

uint64_t
d3d11_lift_glue_input_content_key(struct d3d11_lift *l, uint64_t wire, const struct conv_submit_slot *slot)
{
	if (l == nullptr || slot == nullptr || slot->image_count < 1 || !glue_enter(l)) {
		return 0;
	}
	glue_img gi;
	if (!glue_find(l, slot->images[0].import_key, &gi) || gi.share == nullptr) {
		glue_leave(l); // a CPU slot: the core hashes the pixels itself
		return 0;
	}
	d3d11_conv_content_check *chk;
	{
		std::lock_guard<std::mutex> g(l->glue_mtx);
		if (l->content_check == nullptr && !l->content_check_tried) {
			l->content_check_tried = true;
			IDXGIDevice *dd = nullptr;
			IDXGIAdapter *ad = nullptr;
			if (SUCCEEDED(l->svc_device->QueryInterface(__uuidof(IDXGIDevice), (void **)&dd))) {
				(void)dd->GetAdapter(&ad);
			}
			l->content_check = d3d11_conv_content_check_create(ad);
			rel(ad);
			rel(dd);
		}
		chk = l->content_check;
	}
	const uint64_t key = d3d11_conv_content_check_sample(chk, wire, gi.share, slot->images[0].import_key);
	glue_leave(l);
	return key;
}

xrt_result_t
d3d11_lift_glue_import_input_cpu(
    struct d3d11_lift *l, uint64_t wire, HANDLE handle, bool is_dxgi, uint32_t w, uint32_t h, uint8_t **out_pixels)
{
	*out_pixels = nullptr;
	if (l == nullptr || !l->snap_ok || !glue_enter(l)) {
		if (handle != nullptr && !is_dxgi) {
			CloseHandle(handle);
		}
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	glue_stream *gs = nullptr;
	if (!glue_import_client(l, wire, handle, is_dxgi, &gs)) {
		glue_leave(l);
		return XRT_ERROR_WEAVE_REFUSED;
	}
	w = w > gs->imp_w ? gs->imp_w : w;
	h = h > gs->imp_h ? gs->imp_h : h;
	// Snapshot into a plain RGBA8 target (any client format), then read it back.
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = w;
	td.Height = h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_RENDER_TARGET;
	td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
	ID3D11Texture2D *rt = nullptr, *st = nullptr;
	ID3D11RenderTargetView *rtv = nullptr;
	IDXGIKeyedMutex *rkm = nullptr;
	bool ok = w > 0 && h > 0 && SUCCEEDED(l->svc_device->CreateTexture2D(&td, nullptr, &rt)) &&
	          SUCCEEDED(l->svc_device->CreateRenderTargetView(rt, nullptr, &rtv)) &&
	          SUCCEEDED(rt->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&rkm));
	td.Usage = D3D11_USAGE_STAGING;
	td.BindFlags = 0;
	td.MiscFlags = 0;
	td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	ok = ok && SUCCEEDED(l->svc_device->CreateTexture2D(&td, nullptr, &st));
	bool acquired = false;
	if (ok && gs->imp_km != nullptr) {
		HRESULT hr = gs->imp_km->AcquireSync(0, 4);
		ok = SUCCEEDED(hr) && hr != (HRESULT)WAIT_TIMEOUT;
		acquired = ok;
	}
	uint8_t *px = nullptr;
	if (ok) {
		std::lock_guard<std::mutex> ctx_lock(*l->svc_ctx_mutex);
		blit_dst dst = {rkm, rtv};
		ok = snapshot_blit(l, dst, gs->imp_srv, gs->imp_w, gs->imp_h, 0, 0, w, h, w, h);
		if (ok) {
			l->svc_context->CopyResource(st, rt);
			D3D11_MAPPED_SUBRESOURCE m = {};
			ok = SUCCEEDED(l->svc_context->Map(st, 0, D3D11_MAP_READ, 0, &m));
			if (ok) {
				px = (uint8_t *)malloc((size_t)w * h * 4u);
				ok = px != nullptr;
				for (uint32_t y = 0; ok && y < h; y++) {
					memcpy(px + (size_t)y * w * 4u,
					       (const uint8_t *)m.pData + (size_t)y * m.RowPitch, (size_t)w * 4u);
				}
				l->svc_context->Unmap(st, 0);
			}
		}
	}
	if (acquired) {
		gs->imp_km->ReleaseSync(0);
	}
	rel(st);
	rel(rkm);
	rel(rtv);
	rel(rt);
	glue_leave(l);
	if (!ok) {
		free(px);
		return XRT_ERROR_WEAVE_REFUSED;
	}
	*out_pixels = px;
	return XRT_SUCCESS;
}

/*
 *
 * Spec-4 transports (#2054): D3D12 NT textures + a shared D3D fence, CPU
 * bytes in a file mapping, a sub-rect region; CPU export.
 *
 */

//! The stream's cached shared fence for @p handle (takes ownership of it).
static ID3D11Fence *
glue_import_fence(d3d11_lift *l, glue_stream *gs, HANDLE handle)
{
	if (gs->imp_fence != nullptr && same_kernel_object(gs->imp_fence_handle, handle)) {
		CloseHandle(handle);
		return gs->imp_fence;
	}
	rel(gs->imp_fence);
	close_handle(gs->imp_fence_handle);
	ID3D11Device5 *d5 = nullptr;
	HRESULT hr = l->svc_device->QueryInterface(__uuidof(ID3D11Device5), (void **)&d5);
	if (SUCCEEDED(hr)) {
		hr = d5->OpenSharedFence(handle, __uuidof(ID3D11Fence), (void **)&gs->imp_fence);
	}
	rel(d5);
	if (FAILED(hr)) {
		U_LOG_E("[conv] spec-4 input fence OpenSharedFence failed: 0x%08lx", (unsigned long)hr);
		rel(gs->imp_fence);
		CloseHandle(handle);
		return nullptr;
	}
	gs->imp_fence_handle = handle; // kept open for the identity compare
	return gs->imp_fence;
}

//! The stream's upload texture for CPU input (@p w x @p h, RGBA8 or BGRA8).
static bool
glue_cpu_upload_ensure(d3d11_lift *l, glue_stream *gs, uint32_t w, uint32_t h, DXGI_FORMAT fmt)
{
	if (gs->cpu_tex != nullptr && gs->cpu_w == w && gs->cpu_h == h && gs->cpu_fmt == fmt) {
		return true;
	}
	rel(gs->cpu_srv);
	rel(gs->cpu_tex);
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = w;
	td.Height = h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = fmt;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	HRESULT hr = l->svc_device->CreateTexture2D(&td, nullptr, &gs->cpu_tex);
	if (SUCCEEDED(hr)) {
		hr = l->svc_device->CreateShaderResourceView(gs->cpu_tex, nullptr, &gs->cpu_srv);
	}
	if (FAILED(hr)) {
		rel(gs->cpu_srv);
		rel(gs->cpu_tex);
		return false;
	}
	gs->cpu_w = w;
	gs->cpu_h = h;
	gs->cpu_fmt = fmt;
	return true;
}

//! CPU input: a read-only view of the caller's file mapping, bounds-checked.
static xrt_result_t
glue_import_cpu_bytes(d3d11_lift *l, glue_stream *gs, const struct conv_input_import *in, const conv_submit_slot *slot)
{
	const xrt_conv_image *img = &slot->images[0];
	const uint32_t w = in->w, h = in->h, pitch = in->cpu_row_pitch;
	void *view = MapViewOfFile((HANDLE)in->handle, FILE_MAP_READ, 0, 0, 0);
	CloseHandle((HANDLE)in->handle);
	if (view == nullptr) {
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	}
	MEMORY_BASIC_INFORMATION mbi = {};
	const size_t mapped = VirtualQuery(view, &mbi, sizeof(mbi)) != 0 ? mbi.RegionSize : 0;
	const uint64_t need = (uint64_t)pitch * (in->y + h - 1) + (uint64_t)(in->x + w) * 4u;
	if (w == 0 || h == 0 || pitch < (in->x + w) * 4u || need > (uint64_t)mapped) {
		UnmapViewOfFile(view);
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE; // the wire is not trusted
	}
	const uint8_t *base = (const uint8_t *)view + (size_t)in->y * pitch + (size_t)in->x * 4u;
	const bool bgra = in->cpu_bgra;
	glue_img gi;
	const bool gpu = glue_find(l, img->import_key, &gi);
	bool ok;
	if (gpu) {
		ok = gi.rtv != nullptr &&
		     glue_cpu_upload_ensure(l, gs, w, h, bgra ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM);
		if (ok) {
			std::lock_guard<std::mutex> ctx_lock(*l->svc_ctx_mutex);
			l->svc_context->UpdateSubresource(gs->cpu_tex, 0, nullptr, base, pitch, 0);
			blit_dst dst = {gi.km, gi.rtv};
			ok = snapshot_blit(l, dst, gs->cpu_srv, w, h, 0, 0, w, h, gi.w, gi.h);
			l->svc_context->Flush();
		}
	} else {
		ok = img->memory_kind == XRT_CONV_MEM_CPU && img->cpu_ptr != nullptr && img->width == w && img->height == h;
		const uint32_t stride = img->row_stride != 0 ? img->row_stride : w * 4u;
		for (uint32_t yy = 0; ok && yy < h; yy++) {
			uint8_t *dst = (uint8_t *)img->cpu_ptr + (size_t)yy * stride;
			const uint8_t *src = base + (size_t)yy * pitch;
			memcpy(dst, src, (size_t)w * 4u);
			if (bgra) {
				for (uint32_t xx = 0; xx < w; xx++) {
					const uint8_t b = dst[4 * xx + 0];
					dst[4 * xx + 0] = dst[4 * xx + 2];
					dst[4 * xx + 2] = b;
				}
			}
		}
	}
	UnmapViewOfFile(view);
	return ok ? XRT_SUCCESS : XRT_ERROR_WEAVE_REFUSED;
}

xrt_result_t
d3d11_lift_glue_import_input_ex(struct d3d11_lift *l,
                                uint64_t wire,
                                const struct conv_input_import *in,
                                const struct conv_submit_slot *slot)
{
	const bool is_dxgi = in->kind == D3D11_LIFT_INPUT_DXGI;
	if (l == nullptr || !l->snap_ok || slot == nullptr || slot->image_count < 1 || !glue_enter(l)) {
		if (in->handle != nullptr && !is_dxgi) {
			CloseHandle((HANDLE)in->handle);
		}
		if (in->sync_handle != nullptr) {
			CloseHandle((HANDLE)in->sync_handle);
		}
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	eyes_refresh_if_stale(l);
	glue_stream *gs;
	{
		std::lock_guard<std::mutex> g(l->glue_mtx);
		gs = &l->glue_streams[wire]; // std::map: stable; one producer thread per stream
	}
	if (in->kind == D3D11_LIFT_INPUT_CPU) {
		if (in->sync_handle != nullptr) {
			CloseHandle((HANDLE)in->sync_handle);
		}
		const xrt_result_t r = glue_import_cpu_bytes(l, gs, in, slot);
		glue_leave(l);
		return r;
	}
	ID3D11Fence *fence = nullptr;
	if (in->sync_kind == XRT_CONV_SYNC_FENCE_NT) {
		fence = in->sync_handle != nullptr ? glue_import_fence(l, gs, (HANDLE)in->sync_handle) : nullptr;
		if (fence == nullptr) {
			if (in->handle != nullptr && !is_dxgi) {
				CloseHandle((HANDLE)in->handle);
			}
			glue_leave(l);
			return XRT_ERROR_WEAVE_REFUSED;
		}
	} else if (in->sync_handle != nullptr) {
		CloseHandle((HANDLE)in->sync_handle);
	}
	if (!glue_import_client(l, wire, (HANDLE)in->handle, is_dxgi, &gs)) {
		glue_leave(l);
		return XRT_ERROR_WEAVE_REFUSED;
	}
	const xrt_conv_image *img = &slot->images[0];
	const uint32_t x = in->x, y = in->y;
	if (x >= gs->imp_w || y >= gs->imp_h) {
		glue_leave(l);
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	}
	const uint32_t w = in->w > gs->imp_w - x ? gs->imp_w - x : in->w;
	const uint32_t h = in->h > gs->imp_h - y ? gs->imp_h - y : in->h;
	glue_img gi;
	const bool gpu = glue_find(l, img->import_key, &gi);
	if (!gpu &&
	    (img->memory_kind != XRT_CONV_MEM_CPU || img->cpu_ptr == nullptr || img->width != w || img->height != h)) {
		glue_leave(l);
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	}
	// The caller's keyed mutex (when its texture has one): 4 ms, the budget
	// every weave-side acquire uses. A D3D12 texture has none: its writes are
	// ordered by the fence wait below (or were complete before the call).
	bool acquired = false;
	if (gs->imp_km != nullptr) {
		HRESULT hr = gs->imp_km->AcquireSync(0, 4);
		if (FAILED(hr) || hr == (HRESULT)WAIT_TIMEOUT) {
			glue_leave(l);
			return XRT_ERROR_WEAVE_REFUSED;
		}
		acquired = true;
	}
	bool ok;
	if (gpu) {
		std::lock_guard<std::mutex> ctx_lock(*l->svc_ctx_mutex);
		if (fence != nullptr) {
			l->svc_context4->Wait(fence, in->sync_value); // GPU-side: the caller's writes land first
		}
		// The pre-core snapshot, into the core's slot: 1:1 (point) at the
		// submitted size, box-filtered when inputScale shrank the slot.
		blit_dst dst = {gi.km, gi.rtv};
		ok = gi.rtv != nullptr && snapshot_blit(l, dst, gs->imp_srv, gs->imp_w, gs->imp_h, x, y, w, h, gi.w, gi.h);
		l->svc_context->Flush();
	} else {
		// A CPU provider's slot: read the region back (slow path, sim providers only).
		D3D11_TEXTURE2D_DESC td = {};
		gs->imp_tex->GetDesc(&td);
		td.Width = w;
		td.Height = h;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Usage = D3D11_USAGE_STAGING;
		td.BindFlags = 0;
		td.MiscFlags = 0;
		td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		ID3D11Texture2D *st = nullptr;
		ok = (td.Format == DXGI_FORMAT_R8G8B8A8_UNORM || td.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
		      td.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) &&
		     SUCCEEDED(l->svc_device->CreateTexture2D(&td, nullptr, &st));
		if (ok) {
			std::lock_guard<std::mutex> ctx_lock(*l->svc_ctx_mutex);
			if (fence != nullptr) {
				l->svc_context4->Wait(fence, in->sync_value);
			}
			D3D11_BOX box = {x, y, 0, x + w, y + h, 1};
			l->svc_context->CopySubresourceRegion(st, 0, 0, 0, 0, gs->imp_tex, 0, &box);
			D3D11_MAPPED_SUBRESOURCE m = {};
			ok = SUCCEEDED(l->svc_context->Map(st, 0, D3D11_MAP_READ, 0, &m));
			if (ok) {
				const uint32_t stride = img->row_stride != 0 ? img->row_stride : w * 4u;
				for (uint32_t yy = 0; yy < h; yy++) {
					memcpy((uint8_t *)img->cpu_ptr + (size_t)yy * stride,
					       (const uint8_t *)m.pData + (size_t)yy * m.RowPitch, (size_t)w * 4u);
				}
				l->svc_context->Unmap(st, 0);
			}
		}
		rel(st);
	}
	if (acquired) {
		gs->imp_km->ReleaseSync(0);
	}
	glue_leave(l);
	return ok ? XRT_SUCCESS : XRT_ERROR_WEAVE_REFUSED;
}

bool
d3d11_lift_glue_export_cpu(struct d3d11_lift *l,
                           uint64_t wire,
                           bool copy,
                           HANDLE *out_mapping,
                           uint32_t *out_w,
                           uint32_t *out_h,
                           uint32_t *out_format,
                           uint64_t *out_bytes,
                           bool *out_realloc)
{
	*out_mapping = nullptr;
	*out_realloc = false;
	if (l == nullptr || !glue_enter(l)) {
		return false;
	}
	glue_stream *gs = nullptr;
	{
		std::lock_guard<std::mutex> g(l->glue_mtx);
		auto it = l->glue_streams.find(wire);
		gs = it != l->glue_streams.end() ? &it->second : nullptr;
	}
	if (gs == nullptr || gs->exp_tex == nullptr) {
		glue_leave(l);
		return false;
	}
	if (!copy) {
		// The current mapping, as the last copy left it (the export call).
		const bool have = gs->cpu_map != nullptr;
		if (have) {
			*out_mapping = gs->cpu_map;
			*out_w = gs->cpu_stage_w;
			*out_h = gs->cpu_stage_h;
			*out_format = (uint32_t)gs->cpu_stage_fmt;
			*out_bytes = gs->cpu_map_bytes;
		}
		glue_leave(l);
		return have;
	}
	const uint32_t w = gs->exp_w, h = gs->exp_h;
	const DXGI_FORMAT fmt = (DXGI_FORMAT)gs->exp_format;
	const uint32_t bpp = fmt == DXGI_FORMAT_R16_FLOAT ? 2u : 4u;
	const uint64_t bytes = (uint64_t)w * h * bpp; // tightly packed rows
	if (gs->cpu_map == nullptr || gs->cpu_map_bytes != bytes || gs->cpu_stage_w != w || gs->cpu_stage_h != h ||
	    gs->cpu_stage_fmt != fmt) {
		if (gs->cpu_view != nullptr) {
			UnmapViewOfFile(gs->cpu_view);
			gs->cpu_view = nullptr;
		}
		close_handle(gs->cpu_map);
		rel(gs->cpu_stage);
		D3D11_TEXTURE2D_DESC td = {};
		gs->exp_tex->GetDesc(&td);
		td.Usage = D3D11_USAGE_STAGING;
		td.BindFlags = 0;
		td.MiscFlags = 0;
		td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		bool ok = SUCCEEDED(l->svc_device->CreateTexture2D(&td, nullptr, &gs->cpu_stage));
		if (ok) {
			gs->cpu_map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, (DWORD)(bytes >> 32),
			                                 (DWORD)(bytes & 0xffffffffu), nullptr);
			gs->cpu_view = gs->cpu_map != nullptr ? MapViewOfFile(gs->cpu_map, FILE_MAP_WRITE, 0, 0, 0) : nullptr;
			ok = gs->cpu_view != nullptr;
		}
		if (!ok) {
			if (gs->cpu_view != nullptr) {
				UnmapViewOfFile(gs->cpu_view);
				gs->cpu_view = nullptr;
			}
			close_handle(gs->cpu_map);
			rel(gs->cpu_stage);
			glue_leave(l);
			return false;
		}
		gs->cpu_map_bytes = bytes;
		gs->cpu_stage_w = w;
		gs->cpu_stage_h = h;
		gs->cpu_stage_fmt = fmt;
		*out_realloc = true;
	}
	bool ok;
	{
		std::lock_guard<std::mutex> ctx_lock(*l->svc_ctx_mutex);
		l->svc_context->CopyResource(gs->cpu_stage, gs->exp_tex);
		D3D11_MAPPED_SUBRESOURCE m = {};
		ok = SUCCEEDED(l->svc_context->Map(gs->cpu_stage, 0, D3D11_MAP_READ, 0, &m)); // waits for the copy
		if (ok) {
			for (uint32_t y = 0; y < h; y++) {
				memcpy((uint8_t *)gs->cpu_view + (size_t)y * w * bpp, (const uint8_t *)m.pData + (size_t)y * m.RowPitch,
				       (size_t)w * bpp);
			}
			l->svc_context->Unmap(gs->cpu_stage, 0);
		}
	}
	if (ok) {
		*out_mapping = gs->cpu_map; // service-owned; the transport duplicates it
		*out_w = w;
		*out_h = h;
		*out_format = gs->exp_format;
		*out_bytes = bytes;
	}
	glue_leave(l);
	return ok;
}

void
d3d11_lift_glue_release_input(HANDLE handle, bool is_dxgi)
{
	if (handle != nullptr && !is_dxgi) {
		CloseHandle(handle);
	}
}

//! (Re)create a shared export texture @p w x @p h @p fmt (+ its NT handle).
static bool
glue_export_ensure(d3d11_lift *l, ID3D11Texture2D **tex, HANDLE *handle, uint32_t w, uint32_t h, DXGI_FORMAT fmt)
{
	rel(*tex);
	close_handle(*handle);
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = w;
	td.Height = h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = fmt;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
	HRESULT hr = l->svc_device->CreateTexture2D(&td, nullptr, tex);
	IDXGIResource1 *r1 = nullptr;
	if (SUCCEEDED(hr)) {
		hr = (*tex)->QueryInterface(__uuidof(IDXGIResource1), (void **)&r1);
	}
	if (SUCCEEDED(hr)) {
		hr = r1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
		                            handle);
	}
	rel(r1);
	if (FAILED(hr)) {
		U_LOG_E("[conv] export texture %ux%u fmt=%u create failed: 0x%08lx", w, h, (unsigned)fmt,
		        (unsigned long)hr);
		rel(*tex);
		close_handle(*handle);
		return false;
	}
	return true;
}

//! Copy runtime image @p img into export @p dst (caller holds the context mutex).
static bool
glue_copy_out(d3d11_lift *l, const xrt_conv_image *img, const glue_img *gi, ID3D11Texture2D *dst)
{
	if (gi != nullptr) {
		l->svc_context->CopySubresourceRegion(dst, 0, 0, 0, 0, gi->tex, 0, nullptr);
		return true;
	}
	if (img->memory_kind == XRT_CONV_MEM_CPU && img->cpu_ptr != nullptr) {
		const uint32_t bpp = img->format == XRT_CONV_FMT_R16_FLOAT ? 2u : 4u;
		l->svc_context->UpdateSubresource(dst, 0, nullptr, img->cpu_ptr,
		                                  img->row_stride != 0 ? img->row_stride : img->width * bpp, 0);
		return true;
	}
	return false;
}

xrt_result_t
d3d11_lift_glue_publish(struct d3d11_lift *l,
                        uint64_t wire,
                        const struct conv_result *res,
                        struct d3d11_lift_published *out)
{
	memset(out, 0, sizeof(*out));
	if (l == nullptr || res == nullptr || l->svc_context4 == nullptr || !glue_enter(l)) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	// The main export: the views, or a DEPTH-mode stream's depth.
	const bool views = (res->written & XRT_CONV_OUT_VIEWS) != 0;
	const xrt_conv_image *main =
	    views ? &res->views : ((res->written & XRT_CONV_OUT_DEPTH) != 0 ? &res->depth : nullptr);
	const xrt_conv_image *aux = views && (res->written & XRT_CONV_OUT_DEPTH) != 0 ? &res->depth : nullptr;
	if (main == nullptr) {
		glue_leave(l);
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	}
	glue_img mg, ag;
	const bool m_gpu = glue_find(l, main->import_key, &mg);
	const bool a_gpu = aux != nullptr && glue_find(l, aux->import_key, &ag);
	const DXGI_FORMAT mf = m_gpu ? mg.dxgi : conv_fmt_to_dxgi(main->format);
	const DXGI_FORMAT af = aux == nullptr ? DXGI_FORMAT_UNKNOWN : (a_gpu ? ag.dxgi : conv_fmt_to_dxgi(aux->format));
	glue_stream *gs;
	{
		std::lock_guard<std::mutex> g(l->glue_mtx);
		gs = &l->glue_streams[wire];
	}
	// Export textures: sized to the result, re-created on size / format change.
	if (gs->exp_tex == nullptr || gs->exp_w != main->width || gs->exp_h != main->height ||
	    gs->exp_format != (uint32_t)mf) {
		if (!glue_export_ensure(l, &gs->exp_tex, &gs->exp_handle, main->width, main->height, mf)) {
			glue_leave(l);
			return XRT_ERROR_WEAVE_REFUSED;
		}
		gs->exp_w = main->width;
		gs->exp_h = main->height;
		gs->exp_format = (uint32_t)mf;
		out->realloc = true;
		U_LOG_W("[conv] v3 stream 0x%llx export texture %ux%u fmt=%u ready", (unsigned long long)wire,
		        main->width, main->height, (unsigned)mf);
	}
	if (gs->exp_fence == nullptr) {
		ID3D11Device5 *d5 = nullptr;
		HRESULT hr = l->svc_device->QueryInterface(__uuidof(ID3D11Device5), (void **)&d5);
		if (SUCCEEDED(hr)) {
			hr =
			    d5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), (void **)&gs->exp_fence);
		}
		rel(d5);
		if (SUCCEEDED(hr)) {
			hr = gs->exp_fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &gs->exp_fence_handle);
		}
		if (FAILED(hr)) {
			rel(gs->exp_fence);
			glue_leave(l);
			return XRT_ERROR_WEAVE_REFUSED;
		}
	}
	bool depth_ok = aux != nullptr && af != DXGI_FORMAT_UNKNOWN;
	if (depth_ok && (gs->exp_depth_tex == nullptr || gs->exp_depth_w != aux->width ||
	                 gs->exp_depth_h != aux->height || gs->exp_depth_format != (uint32_t)af)) {
		depth_ok =
		    glue_export_ensure(l, &gs->exp_depth_tex, &gs->exp_depth_handle, aux->width, aux->height, af);
		gs->exp_depth_w = depth_ok ? aux->width : 0;
		gs->exp_depth_h = depth_ok ? aux->height : 0;
		gs->exp_depth_format = depth_ok ? (uint32_t)af : 0;
		out->depth_realloc = depth_ok;
	}

	const bool held_m = m_gpu ? img_acquire(mg.km, main, 4) : true;
	if (!held_m) {
		glue_leave(l);
		return XRT_ERROR_WEAVE_REFUSED;
	}
	const bool held_a = depth_ok && a_gpu ? img_acquire(ag.km, aux, 4) : depth_ok;
	depth_ok = depth_ok && held_a;
	bool ok;
	{
		std::lock_guard<std::mutex> ctx_lock(*l->svc_ctx_mutex);
		ok = glue_copy_out(l, main, m_gpu ? &mg : nullptr, gs->exp_tex);
		if (depth_ok) {
			depth_ok = glue_copy_out(l, aux, a_gpu ? &ag : nullptr, gs->exp_depth_tex);
		}
		// One fence covers the views and the depth (XrLiftDepthResultDXR).
		gs->exp_fence_value++;
		l->svc_context4->Signal(gs->exp_fence, gs->exp_fence_value);
		l->svc_context->Flush();
	}
	if (m_gpu) {
		img_release(mg.km, main);
	}
	if (held_a && a_gpu) {
		img_release(ag.km, aux);
	}
	out->fence_value = gs->exp_fence_value;
	out->width = gs->exp_w;
	out->height = gs->exp_h;
	out->format = gs->exp_format;
	if (depth_ok) {
		out->depth_width = gs->exp_depth_w;
		out->depth_height = gs->exp_depth_h;
		out->depth_format = gs->exp_depth_format;
	} else {
		out->depth_realloc = false;
	}
	glue_leave(l);
	return ok ? XRT_SUCCESS : XRT_ERROR_WEAVE_REFUSED;
}

bool
d3d11_lift_glue_export_output(struct d3d11_lift *l,
                              uint64_t wire,
                              bool depth,
                              HANDLE *out_handle,
                              uint32_t *out_w,
                              uint32_t *out_h,
                              uint32_t *out_format)
{
	if (l == nullptr) {
		return false;
	}
	std::lock_guard<std::mutex> g(l->glue_mtx);
	auto it = l->glue_streams.find(wire);
	if (it == l->glue_streams.end()) {
		return false;
	}
	const glue_stream &gs = it->second;
	HANDLE h = depth ? gs.exp_depth_handle : gs.exp_handle;
	if (h == nullptr) {
		return false;
	}
	*out_handle = h;
	*out_w = depth ? gs.exp_depth_w : gs.exp_w;
	*out_h = depth ? gs.exp_depth_h : gs.exp_h;
	*out_format = depth ? gs.exp_depth_format : gs.exp_format;
	return true;
}

bool
d3d11_lift_glue_export_fence(struct d3d11_lift *l, uint64_t wire, HANDLE *out_handle)
{
	if (l == nullptr) {
		return false;
	}
	std::lock_guard<std::mutex> g(l->glue_mtx);
	auto it = l->glue_streams.find(wire);
	if (it == l->glue_streams.end() || it->second.exp_fence_handle == nullptr) {
		return false;
	}
	*out_handle = it->second.exp_fence_handle;
	return true;
}

void
d3d11_lift_glue_stream_gone(struct d3d11_lift *l, uint64_t wire)
{
	if (l == nullptr || !glue_enter(l)) {
		return;
	}
	{
		// The weave producer uses core_weave under the context mutex.
		std::lock_guard<std::mutex> ctx_lock(*l->svc_ctx_mutex);
		std::lock_guard<std::mutex> g(l->glue_mtx);
		auto it = l->glue_streams.find(wire);
		if (it != l->glue_streams.end()) {
			glue_stream_release(it->second);
			l->glue_streams.erase(it);
		}
		d3d11_conv_content_check_forget_stream(l->content_check, wire);
		auto cw = l->core_weave.find(wire);
		if (cw != l->core_weave.end()) {
			for (auto &r : cw->second.lb_rb) {
				rel(r.staging);
			}
			l->core_weave.erase(cw);
		}
	}
	glue_leave(l);
}


/*
 *
 * The weave path on core-routed streams (#2003): the same snapshot policy
 * (letterbox crop, DXR_LIFT_MAX_INPUT_EDGE cap) into a core slot, and pins of
 * the core's latest result as d3d11_lift_pin.
 *
 */

xrt_result_t
core_weave_submit_locked(d3d11_lift *l,
                         uint64_t owner,
                         uint64_t id,
                         ID3D11ShaderResourceView *src,
                         uint32_t src_tw,
                         uint32_t src_th,
                         uint32_t x,
                         uint32_t y,
                         uint32_t w,
                         uint32_t h,
                         int64_t source_time,
                         const xrt_dp_lift_params *params,
                         const float *viewpoints,
                         uint32_t viewpoint_floats,
                         const xrt_lift_view_control *view_control,
                         const xrt_lift_rig *rig,
                         const xrt_lift_rect_frame *frame,
                         uint64_t *out_frame_id)
{
	*out_frame_id = 0;
	conv_core *core = nullptr;
	uint64_t core_id = 0;
	uint32_t mode = 0;
	if (w == 0 || h == 0 || x + w > src_tw || y + h > src_th ||
	    !bridge_resolve(l, owner, id, &core, &core_id, &mode) || !glue_enter(l)) {
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE; // unknown stream / bad extent: non-fatal
	}
	core_weave_state *ws;
	{
		std::lock_guard<std::mutex> g(l->glue_mtx);
		ws = &l->core_weave[id];
		ws->id = id;
	}
	const bool letterbox = l->letterbox && l->lb_ok;
	uint32_t cx = x, cy = y, cw = w, ch = h;
	if (letterbox && ws->lb.w == w && ws->lb.h == h && u_lift_crop_active(&ws->lb.committed)) {
		const u_lift_crop &c = ws->lb.committed;
		cx = x + c.left;
		cy = y + c.top;
		cw = w - c.left - c.right;
		ch = h - c.top - c.bottom;
	}
	uint32_t dw = cw, dh = ch;
	const bool capped = u_lift_cap_dims(cw, ch, l->max_input_edge, &dw, &dh);

	conv_submit_layout lay = {};
	lay.image_count = 1;
	lay.width[0] = dw;
	lay.height[0] = dh;
	lay.format[0] = XRT_CONV_FMT_RGBA8_UNORM;
	lay.color_space[0] = XRT_CONV_CS_SRGB;
	lay.role[0] = XRT_CONV_IMG_COLOR;
	conv_submit_slot slot = {};
	if (conv_core_stream_begin_submit(core, core_id, &lay, &slot) != XRT_SUCCESS) {
		glue_leave(l);
		return XRT_ERROR_WEAVE_REFUSED;
	}
	glue_img gi;
	bool ok = glue_find(l, slot.images[0].import_key, &gi) && gi.rtv != nullptr;
	if (ok) {
		blit_dst dst = {gi.km, gi.rtv};
		ok = snapshot_blit(l, dst, src, src_tw, src_th, cx, cy, cw, ch, dw, dh);
	}
	if (ok && letterbox) {
		letterbox_step(l, *ws, src, x, y, w, h); // measures the WHOLE rect, bars included
	}
	if (!ok) {
		conv_core_stream_abort_submit(core, core_id, slot.slot);
		glue_leave(l);
		return XRT_ERROR_WEAVE_REFUSED;
	}
	if (capped && (ws->cap_logged_w != dw || ws->cap_logged_h != dh)) {
		ws->cap_logged_w = dw;
		ws->cap_logged_h = dh;
		U_LOG_W("[conv] v3 stream 0x%llx: snapshot %ux%u capped to %ux%u (DXR_LIFT_MAX_INPUT_EDGE=%u)",
		        (unsigned long long)id, cw, ch, dw, dh, l->max_input_edge);
	}

	// The rect's options are sticky (the last ones any submit carried).
	if (params != nullptr) {
		ws->last_params = *params;
		ws->has_last_params = true;
	}
	xrt_dp_lift_params p = {};
	p.convergence = -1.0f;
	p.strength = 1.0f;
	p.inpaint = 1;
	if (ws->has_last_params) {
		p = ws->last_params;
	}
	conv_frame_submit fr = {};
	fr.source_time = source_time;
	fr.frame_flags = p.inpaint != 0 ? XRT_CONV_FRAME_INPAINT : 0u;
	uint32_t n = viewpoint_floats / 3;
	n = n > XRT_CONV_MAX_VIEWS ? XRT_CONV_MAX_VIEWS : n;
	if (viewpoints != nullptr && n > 0) {
		fr.explicit_count = n;
		memcpy(fr.explicit_xyz, viewpoints, sizeof(float) * 3 * n);
	}
	if (view_control != nullptr) {
		fr.has_view_control = true;
		u_lift_view_control_default(&fr.vc);
		fr.vc.ipd_factor = view_control->ipd_factor;
		fr.vc.parallax_factor = view_control->parallax_factor;
		fr.vc.axis_mode = view_control->axis_mode;
		fr.vc.max_offset_m = view_control->max_offset_m;
		fr.vc.recenter_mode = view_control->recenter_mode;
		if (view_control->hold_s >= 0.0f) {
			fr.vc.hold_s = view_control->hold_s;
		}
		if (view_control->tau_s > 0.0f) {
			fr.vc.tau_s = view_control->tau_s;
		}
	}
	if (rig != nullptr && rig->type != XRT_LIFT_RIG_NONE) {
		fr.rig.type = rig->type;
		fr.rig.ipd_factor = rig->ipd_factor;
		fr.rig.parallax_factor = rig->parallax_factor;
		fr.rig.perspective_factor = rig->perspective_factor;
		fr.rig.inv_convergence_distance = rig->inv_convergence_distance;
		fr.rig.half_tan_vfov = rig->half_tan_vfov;
		fr.rig.m2v = rig->m2v;
		u_lift_rig_sanitize(&fr.rig);
	}
	if (frame != nullptr && frame->valid) {
		fr.rect_valid = true;
		memcpy(fr.rect_center, frame->center, sizeof(fr.rect_center));
		fr.rect_w_m = frame->width_m;
		fr.rect_h_m = frame->height_m;
	}
	if (frame != nullptr && frame->eyes_dp_key != 0) {
		fr.eyes_dp_set = true; // the DP that owns the woven rect (roadmap §5.2)
		fr.eyes_dp = frame->eyes_dp_key;
	}
	fr.active[0] = (float)(cx - x) / (float)w;
	fr.active[1] = (float)(cy - y) / (float)h;
	fr.active[2] = (float)(cx - x + cw) / (float)w;
	fr.active[3] = (float)(cy - y + ch) / (float)h;
	if (p.convergence >= 0.0f) {
		fr.convergence_set = true;
		fr.convergence_rel01 = p.convergence > 1.0f ? 1.0f : p.convergence;
	}
	fr.strength = p.strength;
	fr.focal_px = p.focal_px;
	if (capped && fr.focal_px > 0.0f) {
		fr.focal_px *= (float)dw / (float)cw; // focal is in INPUT pixels (a crop keeps the scale)
	}
	const xrt_result_t r = conv_core_stream_commit_submit(core, core_id, slot.slot, &fr, out_frame_id);
	glue_leave(l);
	return r;
}

bool
core_pin_latest(d3d11_lift *l, uint64_t owner, uint64_t id, d3d11_lift_pin *out)
{
	conv_core *core = nullptr;
	uint64_t core_id = 0;
	uint32_t mode = 0;
	if (!bridge_resolve(l, owner, id, &core, &core_id, &mode) || !glue_enter(l)) {
		return false;
	}
	conv_result r = {};
	if (conv_core_stream_acquire(core, core_id, false, &r) != XRT_SUCCESS) {
		glue_leave(l);
		return false;
	}
	glue_img vg, dg;
	const bool have_views = (r.written & XRT_CONV_OUT_VIEWS) != 0 && glue_find(l, r.views.import_key, &vg) &&
	                        vg.srv != nullptr && vg.km != nullptr;
	if (!have_views || !img_acquire(vg.km, &r.views, 4)) {
		// No GPU views (a busy slot, or a CPU result the glue could not
		// mirror, #2030): weave it flat.
		conv_core_stream_release(core, core_id, r.ring_slot);
		glue_leave(l);
		return false;
	}
	out->lift = l;
	out->stream_id = id;
	out->slot = -1;
	out->km = vg.km;
	out->srv = vg.srv;
	out->width = vg.w;
	out->height = vg.h;
	out->view_count = r.views_info.view_count != 0 ? r.views_info.view_count : 1;
	const bool whole = r.active[0] == 0.0f && r.active[1] == 0.0f && r.active[2] == 0.0f && r.active[3] == 0.0f;
	out->active[0] = whole ? 0.0f : r.active[0];
	out->active[1] = whole ? 0.0f : r.active[1];
	out->active[2] = whole ? 1.0f : r.active[2];
	out->active[3] = whole ? 1.0f : r.active[3];
	out->vp_count = r.views_info.view_count <= XRT_LIFT_MAX_VIEWS ? r.views_info.view_count : XRT_LIFT_MAX_VIEWS;
	for (uint32_t i = 0; i < out->vp_count; i++) {
		out->vps[3 * i + 0] = r.views_info.eye_xyz[i][0];
		out->vps[3 * i + 1] = r.views_info.eye_xyz[i][1];
		out->vps[3 * i + 2] = r.views_info.eye_xyz[i][2];
	}
	memcpy(out->rect_center, r.rect_center, sizeof(out->rect_center));
	memcpy(out->rect_size, r.rect_size, sizeof(out->rect_size));
	// The pin is the service's own consumer (the DP weave + the lifted-rect
	// depth cursor, ADR-046 A1): like the pre-core path, it sees the module's
	// depth whenever one was produced, including the observer-only depth
	// (#2003) the app never asked for. The app-facing export
	// (d3d11_lift_glue_publish) still follows written.
	if ((r.provider_written & XRT_CONV_OUT_DEPTH) != 0 && glue_find(l, r.depth.import_key, &dg) &&
	    dg.km != nullptr) {
		const uint32_t k = r.depth_info.kind;
		out->depth_tex = dg.tex;
		out->depth_km = dg.km;
		out->depth_w = dg.w;
		out->depth_h = dg.h;
		out->depth_format = (uint32_t)dg.dxgi;
		out->depth_encoding = (k == XRT_CONV_DEPTH_METRIC_INVERSE || k == XRT_CONV_DEPTH_AFFINE_INVERSE ||
		                       k == XRT_CONV_DEPTH_AFFINE_DISPARITY || k == XRT_CONV_DEPTH_STEREO_DISPARITY_PX)
		                          ? XRT_DP_LIFT_DEPTH_ENCODING_INVERSE
		                          : XRT_DP_LIFT_DEPTH_ENCODING_LINEAR;
		out->depth_value_scale = r.depth_info.value_scale != 0.0f ? r.depth_info.value_scale : 1.0f;
		out->depth_value_offset = r.depth_info.value_shift;
		out->relief_valid = r.depth_info.relief_valid != 0;
		out->relief_scale = r.depth_info.relief_scale;
		out->relief_offset = r.depth_info.relief_offset;
	}
	out->core = true;
	out->core_id = core_id;
	out->core_ring_slot = r.ring_slot;
	out->valid = true;
	glue_leave(l);
	return true;
}

void
core_unpin(d3d11_lift *l, d3d11_lift_pin *pin)
{
	d3d11_lift_core_bridge b;
	{
		std::lock_guard<std::mutex> g(l->bridge_mtx);
		b = l->bridge;
	}
	if (b.core != nullptr) {
		conv_core_stream_release(b.core, pin->core_id, pin->core_ring_slot);
	}
}


/*
 *
 * The weave path's entry points (comp_d3d11_service's lift-rect weave): every
 * lifted rect is a core-routed v3 stream (D3D11_LIFT_CORE_STREAM_TAG).
 *
 */

uint32_t
d3d11_lift_stream_mode(struct d3d11_lift *l, uint64_t owner, uint64_t id)
{
	if (l == nullptr || (id & D3D11_LIFT_CORE_STREAM_TAG) == 0) {
		return 0;
	}
	uint64_t core_id = 0;
	uint32_t mode = 0;
	return bridge_resolve(l, owner, id, nullptr, &core_id, &mode) ? mode : 0;
}

xrt_result_t
d3d11_lift_submit_srv_locked(struct d3d11_lift *l,
                             uint64_t owner,
                             uint64_t id,
                             ID3D11ShaderResourceView *src,
                             uint32_t src_tw,
                             uint32_t src_th,
                             uint32_t x,
                             uint32_t y,
                             uint32_t w,
                             uint32_t h,
                             int64_t source_time,
                             const struct xrt_dp_lift_params *params,
                             const float *viewpoints,
                             uint32_t viewpoint_floats,
                             const struct xrt_lift_view_control *view_control,
                             const struct xrt_lift_rig *rig,
                             const struct xrt_lift_rect_frame *frame,
                             uint64_t *out_frame_id)
{
	if (l == nullptr || !l->snap_ok || src == nullptr || out_frame_id == nullptr) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	if ((id & D3D11_LIFT_CORE_STREAM_TAG) == 0) {
		*out_frame_id = 0;
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE; // not a stream of this route: non-fatal
	}
	return core_weave_submit_locked(l, owner, id, src, src_tw, src_th, x, y, w, h, source_time, params, viewpoints,
	                                viewpoint_floats, view_control, rig, frame, out_frame_id);
}

bool
d3d11_lift_pin_latest(struct d3d11_lift *l, uint64_t owner, uint64_t id, struct d3d11_lift_pin *out)
{
	*out = d3d11_lift_pin{};
	if (l == nullptr || (id & D3D11_LIFT_CORE_STREAM_TAG) == 0) {
		return false;
	}
	return core_pin_latest(l, owner, id, out);
}

void
d3d11_lift_unpin(struct d3d11_lift_pin *pin)
{
	if (pin == nullptr || !pin->valid || pin->lift == nullptr) {
		return;
	}
	if (pin->depth_held) {
		pin->depth_km->ReleaseSync(0);
	}
	pin->km->ReleaseSync(0);
	core_unpin(pin->lift, pin);
	*pin = d3d11_lift_pin{};
}
