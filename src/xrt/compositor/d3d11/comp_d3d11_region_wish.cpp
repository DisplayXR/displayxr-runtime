// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-062 D5 (tier 4): execute a visible-region wish plan on D3D11.
 * @ingroup comp_d3d11
 */

#include "comp_d3d11_region_wish.h"

#include "util/u_logging.h"

#include <stdio.h>
#include <string.h>

static const char *
rw_kind_str(enum u_wv_wish_kind k)
{
	switch (k) {
	case U_WV_WISH_EMPTY: return "empty";
	case U_WV_WISH_CUT: return "cut";
	case U_WV_WISH_IMPLICIT: return "implicit";
	}
	return "?";
}

extern "C" void
comp_d3d11_region_wish_release(struct comp_d3d11_region_wish *rw)
{
	if (rw == nullptr) {
		return;
	}
	if (rw->srv != nullptr) {
		rw->srv->Release();
		rw->srv = nullptr;
	}
	if (rw->rtv != nullptr) {
		rw->rtv->Release();
		rw->rtv = nullptr;
	}
	if (rw->tex != nullptr) {
		rw->tex->Release();
		rw->tex = nullptr;
	}
	rw->device = nullptr;
	rw->w = 0;
	rw->h = 0;
	rw->rastered = false;
}

static bool
rw_ensure(struct comp_d3d11_region_wish *rw, ID3D11Device *device, uint32_t w, uint32_t h)
{
	if (rw->tex != nullptr && rw->device == device && rw->w == w && rw->h == h) {
		return true;
	}
	comp_d3d11_region_wish_release(rw);
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = w;
	td.Height = h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	// Published only (never bound as a render target while the DP samples it),
	// so one texture serves both.
	td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	HRESULT hr = device->CreateTexture2D(&td, nullptr, &rw->tex);
	if (SUCCEEDED(hr)) {
		hr = device->CreateRenderTargetView(rw->tex, nullptr, &rw->rtv);
	}
	if (SUCCEEDED(hr)) {
		hr = device->CreateShaderResourceView(rw->tex, nullptr, &rw->srv);
	}
	if (FAILED(hr)) {
		U_LOG_E("ADR-062 tier 4: region wish %ux%u: D3D resource creation failed: 0x%08lx", w, h,
		        (unsigned long)hr);
		comp_d3d11_region_wish_release(rw);
		return false;
	}
	rw->device = device;
	rw->w = w;
	rw->h = h;
	return true;
}

//! The source's texture when it can be copied into a WxH R8 raster as is.
static ID3D11Texture2D *
rw_copyable_source(ID3D11ShaderResourceView *source, uint32_t w, uint32_t h)
{
	if (source == nullptr) {
		return nullptr;
	}
	ID3D11Resource *res = nullptr;
	source->GetResource(&res);
	if (res == nullptr) {
		return nullptr;
	}
	ID3D11Texture2D *tex = nullptr;
	if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&tex))) || tex == nullptr) {
		res->Release();
		return nullptr;
	}
	res->Release();
	D3D11_TEXTURE2D_DESC d = {};
	tex->GetDesc(&d);
	if (d.Width != w || d.Height != h || d.ArraySize != 1 || d.MipLevels != 1 || d.SampleDesc.Count != 1 ||
	    (d.Format != DXGI_FORMAT_R8_UNORM && d.Format != DXGI_FORMAT_R8_TYPELESS)) {
		tex->Release();
		return nullptr;
	}
	return tex; // caller releases
}

static void
rw_clear_rects(ID3D11DeviceContext1 *ctx1, ID3D11RenderTargetView *rtv, const struct u_wv_rect *r, uint32_t n, float v)
{
	const float c[4] = {v, 0.0f, 0.0f, 0.0f};
	for (uint32_t i = 0; i < n; i++) {
		if (u_wv_rect_empty(&r[i])) {
			continue;
		}
		const D3D11_RECT dr = {r[i].left, r[i].top, r[i].right, r[i].bottom};
		ctx1->ClearView(rtv, c, &dr, 1);
	}
}

extern "C" ID3D11ShaderResourceView *
comp_d3d11_region_wish_resolve(struct comp_d3d11_region_wish *rw,
                               ID3D11Device *device,
                               ID3D11DeviceContext *context,
                               const struct u_wv_wish_input *in,
                               ID3D11ShaderResourceView *source,
                               const char *who,
                               uint64_t *out_seq)
{
	if (rw == nullptr || device == nullptr || context == nullptr || in == nullptr || in->mask_w == 0 ||
	    in->mask_h == 0) {
		return nullptr;
	}

	struct u_wv_wish_plan plan;
	u_wv_wish_plan_build(in, &plan);

	// A CUT with nothing to cut is the source itself: no copy at all.
	ID3D11Texture2D *src_tex = nullptr;
	bool verbatim = false;
	if (plan.kind == U_WV_WISH_CUT) {
		if (source == nullptr) {
			return nullptr;
		}
		if (plan.off_count == 0) {
			verbatim = true;
		} else {
			src_tex = rw_copyable_source(source, plan.mask_w, plan.mask_h);
			verbatim = src_tex == nullptr; // cannot copy it: publish it uncut (the 3D side)
		}
	}

	bool changed = false;
	const uint64_t seq =
	    u_wv_wish_seq_step(&rw->seq, plan.signature ^ (verbatim ? 0x9e3779b97f4a7c15ull : 0), &changed);
	if (out_seq != nullptr) {
		*out_seq = seq;
	}

	char line[64];
	(void)snprintf(line, sizeof(line), "%s%s", rw_kind_str(plan.kind), verbatim ? " (uncut)" : "");
	if (changed) {
		if (strcmp(line, rw->last_log) != 0) {
			U_LOG_W("ADR-062 tier 4 %s: zone wish gen %llu is %s - mask %ux%u, %u on / %u off rect(s)%s",
			        who != nullptr ? who : "?", (unsigned long long)seq, line, plan.mask_w, plan.mask_h,
			        plan.on_count, plan.off_count,
			        plan.cut_skipped ? ", the covered part too fragmented" : "");
			(void)snprintf(rw->last_log, sizeof(rw->last_log), "%s", line);
		} else {
			U_LOG_I("ADR-062 tier 4 %s: zone wish gen %llu (%s, %u on / %u off rect(s))",
			        who != nullptr ? who : "?", (unsigned long long)seq, line, plan.on_count,
			        plan.off_count);
		}
	}

	if (verbatim) {
		return source;
	}

	if (!rw_ensure(rw, device, plan.mask_w, plan.mask_h)) {
		if (src_tex != nullptr) {
			src_tex->Release();
		}
		return nullptr;
	}

	// EMPTY / IMPLICIT are pure rects: raster once per content change. A CUT
	// re-copies its source every frame (one small R8 copy): its pixels are the
	// app's and only their generation is known here.
	if (plan.kind != U_WV_WISH_CUT && rw->rastered && !changed) {
		return rw->srv;
	}

	ID3D11DeviceContext1 *ctx1 = nullptr;
	if (FAILED(context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void **>(&ctx1))) ||
	    ctx1 == nullptr) {
		if (src_tex != nullptr) {
			src_tex->Release();
		}
		return nullptr;
	}
	if (src_tex != nullptr) {
		context->CopyResource(rw->tex, src_tex);
		src_tex->Release();
	} else {
		const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
		context->ClearRenderTargetView(rw->rtv, zero);
	}
	rw_clear_rects(ctx1, rw->rtv, plan.on, plan.on_count, 1.0f);
	rw_clear_rects(ctx1, rw->rtv, plan.off, plan.off_count, 0.0f);
	ctx1->Release();
	rw->rastered = true;
	return rw->srv;
}
