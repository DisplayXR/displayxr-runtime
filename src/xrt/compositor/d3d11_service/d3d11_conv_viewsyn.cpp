// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Reference view synthesis (ADR-053 S5) on D3D11 — see the header.
 * @ingroup comp_d3d11_service
 */

#include "d3d11_conv_viewsyn.h"
#include "d3d11_conv_viewsyn_shaders.h"

#include "util/u_logging.h"

#include <d3dcompiler.h>

#include <cmath>
#include <cstring>

namespace {

template <typename T>
void
rel(T *&p)
{
	if (p != nullptr) {
		p->Release();
		p = nullptr;
	}
}

//! A render-target + shader-resource texture.
struct rt_tex
{
	ID3D11Texture2D *tex = nullptr;
	ID3D11ShaderResourceView *srv = nullptr;
	ID3D11RenderTargetView *rtv = nullptr;
	uint32_t w = 0, h = 0;
	DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;

	void
	reset()
	{
		rel(rtv);
		rel(srv);
		rel(tex);
		w = h = 0;
		fmt = DXGI_FORMAT_UNKNOWN;
	}

	bool
	ensure(ID3D11Device *dev, uint32_t nw, uint32_t nh, DXGI_FORMAT nf)
	{
		if (tex != nullptr && w == nw && h == nh && fmt == nf) {
			return true;
		}
		reset();
		D3D11_TEXTURE2D_DESC d = {};
		d.Width = nw;
		d.Height = nh;
		d.MipLevels = 1;
		d.ArraySize = 1;
		d.Format = nf;
		d.SampleDesc.Count = 1;
		d.Usage = D3D11_USAGE_DEFAULT;
		d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		if (FAILED(dev->CreateTexture2D(&d, nullptr, &tex)) ||
		    FAILED(dev->CreateShaderResourceView(tex, nullptr, &srv)) ||
		    FAILED(dev->CreateRenderTargetView(tex, nullptr, &rtv))) {
			reset();
			return false;
		}
		w = nw;
		h = nh;
		fmt = nf;
		return true;
	}
};

// Constant buffers (16-byte aligned, layouts match the HLSL cbuffers).
struct cb_prep
{
	float value_scale, value_shift, near_lo, inv_span;
	uint32_t kind, has_invalid;
	float invalid_sample, pad0;
};
struct cb_filter
{
	int32_t dir[2];
	int32_t rad_r, rad_c;
	int32_t seed, pad0[3];
};
struct cb_guide
{
	float out_res[2], pad0[2];
};
struct cb_up
{
	float out_res[2];
	float sigma_s, sigma_r;
	int32_t snap, snap_r;
	float snap_tau, pad0;
};
struct cb_main
{
	float vp_origin[2], vp_size[2];
	float disp_res[2];
	float conv, qscale;
	float taper, aspect, rear_k;
	int32_t steps;
	float eye[3];
	int32_t debug;
	float cam[3];
	float pad0;
};
static_assert(sizeof(cb_prep) == 32, "cb_prep");
static_assert(sizeof(cb_filter) == 32, "cb_filter");
static_assert(sizeof(cb_guide) == 16, "cb_guide");
static_assert(sizeof(cb_up) == 32, "cb_up");
static_assert(sizeof(cb_main) == 80, "cb_main");

//! Everything passes 1-6 read; a change re-runs them (live-dibr.js upDirty).
struct prepass_key
{
	ID3D11ShaderResourceView *color, *depth;
	uint32_t color_w, color_h, depth_w, depth_h;
	uint32_t kind;
	float value_scale, value_shift, invalid_sample;
	double near_lo, near_hi;
	uint32_t dilate, dilate_hi, upsample_max, core, radius;
	float sigma_s, sigma_r, snap_tau;
	bool upsample, snap;
};

bool
key_equal(const prepass_key &a, const prepass_key &b)
{
	// Field-wise: NaN invalid_sample must compare equal to itself.
	auto feq = [](float x, float y) { return x == y || (std::isnan(x) && std::isnan(y)); };
	return a.color == b.color && a.depth == b.depth && a.color_w == b.color_w && a.color_h == b.color_h &&
	       a.depth_w == b.depth_w && a.depth_h == b.depth_h && a.kind == b.kind &&
	       feq(a.value_scale, b.value_scale) && feq(a.value_shift, b.value_shift) &&
	       feq(a.invalid_sample, b.invalid_sample) && a.near_lo == b.near_lo && a.near_hi == b.near_hi &&
	       a.dilate == b.dilate && a.dilate_hi == b.dilate_hi && a.upsample_max == b.upsample_max &&
	       a.core == b.core && a.radius == b.radius && a.sigma_s == b.sigma_s && a.sigma_r == b.sigma_r &&
	       a.snap_tau == b.snap_tau && a.upsample == b.upsample && a.snap == b.snap;
}

} // namespace

struct d3d11_conv_viewsyn
{
	ID3D11Device *dev = nullptr;
	ID3D11VertexShader *vs = nullptr;
	ID3D11PixelShader *ps_prep = nullptr, *ps_filter = nullptr, *ps_guide = nullptr, *ps_up = nullptr,
	                  *ps_main = nullptr;
	ID3D11Buffer *cb_prep = nullptr, *cb_filter = nullptr, *cb_guide = nullptr, *cb_up = nullptr,
	             *cb_main = nullptr;
	ID3D11SamplerState *samp = nullptr;
	ID3D11RasterizerState *rast = nullptr;
	ID3D11BlendState *blend = nullptr;
	ID3D11DepthStencilState *depth_off = nullptr;

	rt_tex dlo_a, dlo_b; //!< depth res RGBA16F ping-pong: r = u, g = min core, b = max core
	rt_tex guide;        //!< depth res RGBA8
	rt_tex up_a, up_b;   //!< colour res (capped) R16F ping-pong
	ID3D11ShaderResourceView *disp_srv = nullptr; //!< borrowed: what the main pass samples
	bool have_key = false;
	prepass_key key = {};

	// Target RTV cache.
	ID3D11Texture2D *tgt_tex = nullptr;
	DXGI_FORMAT tgt_fmt = DXGI_FORMAT_UNKNOWN;
	uint32_t tgt_layout = 0;
	ID3D11RenderTargetView *tgt_rtv[U_CONV_VIEWSYN_MAX_VIEWS] = {};
	uint32_t tgt_rtv_count = 0;

	uint64_t prepasses = 0;
};

static void
release_target(d3d11_conv_viewsyn *v)
{
	for (auto &r : v->tgt_rtv) {
		rel(r);
	}
	v->tgt_rtv_count = 0;
	rel(v->tgt_tex);
}

static bool
compile(ID3D11Device *dev, const char *src, const char *profile, ID3D11VertexShader **vs, ID3D11PixelShader **ps)
{
	ID3DBlob *b = nullptr;
	ID3DBlob *err = nullptr;
	HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, "main", profile,
	                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &b, &err);
	if (FAILED(hr)) {
		U_LOG_E("[viewsyn] %s compile failed: %s", profile,
		        err != nullptr ? (const char *)err->GetBufferPointer() : "?");
		rel(err);
		return false;
	}
	rel(err);
	if (vs != nullptr) {
		hr = dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, vs);
	} else {
		hr = dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, ps);
	}
	rel(b);
	return SUCCEEDED(hr);
}

static bool
make_cb(ID3D11Device *dev, UINT size, ID3D11Buffer **out)
{
	D3D11_BUFFER_DESC d = {};
	d.ByteWidth = size;
	d.Usage = D3D11_USAGE_DYNAMIC;
	d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	return SUCCEEDED(dev->CreateBuffer(&d, nullptr, out));
}

template <typename T>
static void
upload(ID3D11DeviceContext *ctx, ID3D11Buffer *cb, const T &data)
{
	D3D11_MAPPED_SUBRESOURCE m;
	if (SUCCEEDED(ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
		memcpy(m.pData, &data, sizeof(T));
		ctx->Unmap(cb, 0);
	}
}

struct d3d11_conv_viewsyn *
d3d11_conv_viewsyn_create(ID3D11Device *device)
{
	if (device == nullptr) {
		return nullptr;
	}
	auto *v = new d3d11_conv_viewsyn();
	v->dev = device;
	v->dev->AddRef();

	bool ok = compile(device, k_viewsyn_vs, "vs_5_0", &v->vs, nullptr) &&
	          compile(device, k_viewsyn_prep_ps, "ps_5_0", nullptr, &v->ps_prep) &&
	          compile(device, k_viewsyn_filter_ps, "ps_5_0", nullptr, &v->ps_filter) &&
	          compile(device, k_viewsyn_guide_ps, "ps_5_0", nullptr, &v->ps_guide) &&
	          compile(device, k_viewsyn_upsample_ps, "ps_5_0", nullptr, &v->ps_up) &&
	          compile(device, k_viewsyn_main_ps, "ps_5_0", nullptr, &v->ps_main);
	ok = ok && make_cb(device, sizeof(cb_prep), &v->cb_prep) && make_cb(device, sizeof(cb_filter), &v->cb_filter) &&
	     make_cb(device, sizeof(cb_guide), &v->cb_guide) && make_cb(device, sizeof(cb_up), &v->cb_up) &&
	     make_cb(device, sizeof(cb_main), &v->cb_main);

	// live-dibr.js: LINEAR min/mag, CLAMP_TO_EDGE, no mips.
	D3D11_SAMPLER_DESC sd = {};
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.MaxLOD = D3D11_FLOAT32_MAX;
	ok = ok && SUCCEEDED(device->CreateSamplerState(&sd, &v->samp));

	D3D11_RASTERIZER_DESC rd = {};
	rd.FillMode = D3D11_FILL_SOLID;
	rd.CullMode = D3D11_CULL_NONE;
	rd.DepthClipEnable = TRUE;
	ok = ok && SUCCEEDED(device->CreateRasterizerState(&rd, &v->rast));

	D3D11_BLEND_DESC bd = {};
	bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	ok = ok && SUCCEEDED(device->CreateBlendState(&bd, &v->blend));

	D3D11_DEPTH_STENCIL_DESC dd = {};
	dd.DepthEnable = FALSE;
	dd.StencilEnable = FALSE;
	ok = ok && SUCCEEDED(device->CreateDepthStencilState(&dd, &v->depth_off));

	if (!ok) {
		U_LOG_E("[viewsyn] D3D11 view-synthesis stage init failed");
		d3d11_conv_viewsyn_destroy(&v);
		return nullptr;
	}
	return v;
}

void
d3d11_conv_viewsyn_destroy(struct d3d11_conv_viewsyn **vs_ptr)
{
	if (vs_ptr == nullptr || *vs_ptr == nullptr) {
		return;
	}
	d3d11_conv_viewsyn *v = *vs_ptr;
	release_target(v);
	v->dlo_a.reset();
	v->dlo_b.reset();
	v->guide.reset();
	v->up_a.reset();
	v->up_b.reset();
	rel(v->depth_off);
	rel(v->blend);
	rel(v->rast);
	rel(v->samp);
	rel(v->cb_main);
	rel(v->cb_up);
	rel(v->cb_guide);
	rel(v->cb_filter);
	rel(v->cb_prep);
	rel(v->ps_main);
	rel(v->ps_up);
	rel(v->ps_guide);
	rel(v->ps_filter);
	rel(v->ps_prep);
	rel(v->vs);
	rel(v->dev);
	delete v;
	*vs_ptr = nullptr;
}

uint64_t
d3d11_conv_viewsyn_prepass_count(const struct d3d11_conv_viewsyn *vs)
{
	return vs != nullptr ? vs->prepasses : 0;
}

static void
unbind(ID3D11DeviceContext *ctx)
{
	ID3D11ShaderResourceView *null_srv[3] = {};
	ctx->PSSetShaderResources(0, 3, null_srv);
	ctx->OMSetRenderTargets(0, nullptr, nullptr);
}

static void
set_viewport(ID3D11DeviceContext *ctx, float x, float y, float w, float h)
{
	D3D11_VIEWPORT vp = {x, y, w, h, 0.0f, 1.0f};
	ctx->RSSetViewports(1, &vp);
}

//! One full-raster pass of @p ps from @p srcs into @p dst.
static void
draw_pass(d3d11_conv_viewsyn *v,
          ID3D11DeviceContext *ctx,
          ID3D11PixelShader *ps,
          ID3D11Buffer *cb,
          ID3D11ShaderResourceView *const *srcs,
          UINT src_count,
          rt_tex &dst)
{
	unbind(ctx);
	ctx->OMSetRenderTargets(1, &dst.rtv, nullptr);
	set_viewport(ctx, 0.0f, 0.0f, (float)dst.w, (float)dst.h);
	ctx->PSSetShader(ps, nullptr, 0);
	ctx->PSSetConstantBuffers(0, 1, &cb);
	ctx->PSSetShaderResources(0, src_count, srcs);
	ctx->Draw(3, 0);
}

//! Separable filter, both directions: src -> tmp (H) -> src (V). Result in @p src.
static void
filter2(d3d11_conv_viewsyn *v, ID3D11DeviceContext *ctx, rt_tex &src, rt_tex &tmp, int rad_r, int rad_c, bool seed)
{
	cb_filter c = {};
	c.rad_r = rad_r;
	c.rad_c = rad_c;
	c.dir[0] = 1;
	c.dir[1] = 0;
	c.seed = seed ? 1 : 0;
	upload(ctx, v->cb_filter, c);
	draw_pass(v, ctx, v->ps_filter, v->cb_filter, &src.srv, 1, tmp);
	c.dir[0] = 0;
	c.dir[1] = 1;
	c.seed = 0; // the H pass already seeded g / b
	upload(ctx, v->cb_filter, c);
	draw_pass(v, ctx, v->ps_filter, v->cb_filter, &tmp.srv, 1, src);
}

static bool
ensure_target(
    d3d11_conv_viewsyn *v, const d3d11_conv_viewsyn_target *t, uint32_t views, uint32_t *out_vw, uint32_t *out_vh)
{
	D3D11_TEXTURE2D_DESC td;
	t->texture->GetDesc(&td);
	DXGI_FORMAT fmt = t->rtv_format != DXGI_FORMAT_UNKNOWN ? t->rtv_format : td.Format;
	if (t->layout == XRT_CONV_VIEWS_ROW) {
		if (td.Width < views) {
			return false;
		}
		*out_vw = td.Width / views;
		*out_vh = td.Height;
	} else if (t->layout == XRT_CONV_VIEWS_ARRAY) {
		if (td.ArraySize < views) {
			return false;
		}
		*out_vw = td.Width;
		*out_vh = td.Height;
	} else {
		return false;
	}
	uint32_t need = t->layout == XRT_CONV_VIEWS_ROW ? 1u : views;
	if (v->tgt_tex == t->texture && v->tgt_fmt == fmt && v->tgt_layout == t->layout && v->tgt_rtv_count >= need) {
		return true;
	}
	release_target(v);
	for (uint32_t i = 0; i < need; i++) {
		D3D11_RENDER_TARGET_VIEW_DESC rd = {};
		rd.Format = fmt;
		if (td.ArraySize > 1) {
			rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
			rd.Texture2DArray.MipSlice = 0;
			rd.Texture2DArray.FirstArraySlice = i;
			rd.Texture2DArray.ArraySize = 1;
		} else {
			rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
		}
		if (FAILED(v->dev->CreateRenderTargetView(t->texture, &rd, &v->tgt_rtv[i]))) {
			release_target(v);
			return false;
		}
	}
	v->tgt_rtv_count = need;
	v->tgt_tex = t->texture;
	v->tgt_tex->AddRef();
	v->tgt_fmt = fmt;
	v->tgt_layout = t->layout;
	return true;
}

static bool
run_prepasses(d3d11_conv_viewsyn *v,
              ID3D11DeviceContext *ctx,
              const d3d11_conv_viewsyn_inputs *in,
              const u_conv_viewsyn_params *p,
              const u_conv_viewsyn_frame *f,
              const prepass_key &k)
{
	const uint32_t dw = in->depth_width, dh = in->depth_height;
	if (!v->dlo_a.ensure(v->dev, dw, dh, DXGI_FORMAT_R16G16B16A16_FLOAT) ||
	    !v->dlo_b.ensure(v->dev, dw, dh, DXGI_FORMAT_R16G16B16A16_FLOAT)) {
		return false;
	}

	// 1. prep: raw -> normalised nearness (r = g = b = u).
	cb_prep cp = {};
	cp.value_scale = p->depth.value_scale == 0.0f ? 1.0f : p->depth.value_scale;
	cp.value_shift = p->depth.value_shift;
	cp.near_lo = (float)f->near_lo;
	cp.inv_span = (float)(1.0 / (f->near_hi - f->near_lo));
	cp.kind = p->depth.kind;
	cp.has_invalid = std::isnan(p->depth.invalid_sample) ? 0u : 1u;
	cp.invalid_sample = cp.has_invalid ? p->depth.invalid_sample : 0.0f;
	upload(ctx, v->cb_prep, cp);
	draw_pass(v, ctx, v->ps_prep, v->cb_prep, &in->depth, 1, v->dlo_a);

	// 2. foreground dilation at depth res (lift() ships 0).
	if (k.dilate > 0) {
		filter2(v, ctx, v->dlo_a, v->dlo_b, (int)k.dilate, 0, false);
	}
	// 3. the snap's cores (live-dibr.js snapPack: core 0 when the snap is off).
	if (k.core > 0) {
		filter2(v, ctx, v->dlo_a, v->dlo_b, 0, (int)k.core, true);
	} else if (k.dilate > 0) {
		// g / b must follow the dilated r: re-seed with a zero-radius pass.
		filter2(v, ctx, v->dlo_a, v->dlo_b, 0, 0, true);
	}
	v->disp_srv = v->dlo_a.srv;

	if (k.upsample) {
		// 4. guide: the colour at depth res.
		if (!v->guide.ensure(v->dev, dw, dh, DXGI_FORMAT_R8G8B8A8_UNORM)) {
			return false;
		}
		cb_guide cg = {{(float)dw, (float)dh}, {0, 0}};
		upload(ctx, v->cb_guide, cg);
		draw_pass(v, ctx, v->ps_guide, v->cb_guide, &in->color, 1, v->guide);

		// 5. JBU + edge snap to (capped) colour res.
		uint32_t uw, uh;
		u_conv_viewsyn_upsample_size(in->color_width, in->color_height, k.upsample_max, &uw, &uh);
		if (!v->up_a.ensure(v->dev, uw, uh, DXGI_FORMAT_R16_FLOAT) ||
		    !v->up_b.ensure(v->dev, uw, uh, DXGI_FORMAT_R16_FLOAT)) {
			return false;
		}
		cb_up cu = {};
		cu.out_res[0] = (float)uw;
		cu.out_res[1] = (float)uh;
		cu.sigma_s = std::fmax(0.1f, k.sigma_s > 0.0f ? k.sigma_s : 1.0f);
		cu.sigma_r = std::fmax(0.005f, k.sigma_r > 0.0f ? k.sigma_r : 0.1f);
		cu.snap = k.snap ? 1 : 0;
		cu.snap_r = (int32_t)k.radius;
		cu.snap_tau = std::fmax(1e-6f, k.snap_tau); // normalised: (hi - lo) == 1
		upload(ctx, v->cb_up, cu);
		ID3D11ShaderResourceView *srcs[3] = {v->dlo_a.srv, v->guide.srv, in->color};
		ctx->PSSetSamplers(0, 1, &v->samp);
		draw_pass(v, ctx, v->ps_up, v->cb_up, srcs, 3, v->up_a);

		// 6. optional max filter after the upsample.
		if (k.dilate_hi > 0) {
			filter2(v, ctx, v->up_a, v->up_b, (int)k.dilate_hi, 0, false);
		}
		v->disp_srv = v->up_a.srv;
	}
	unbind(ctx);
	v->prepasses++;
	return true;
}

bool
d3d11_conv_viewsyn_render(struct d3d11_conv_viewsyn *v,
                          ID3D11DeviceContext *ctx,
                          const struct d3d11_conv_viewsyn_inputs *in,
                          const struct u_conv_viewsyn_params *p,
                          const struct d3d11_conv_viewsyn_target *target)
{
	if (v == nullptr || ctx == nullptr || in == nullptr || p == nullptr || target == nullptr ||
	    in->color == nullptr || in->depth == nullptr || target->texture == nullptr || in->depth_width == 0 ||
	    in->depth_height == 0 || in->color_width == 0 || in->color_height == 0) {
		return false;
	}
	u_conv_viewsyn_frame f;
	if (!u_conv_viewsyn_resolve(p, &f)) {
		return false;
	}
	uint32_t vw = 0, vh = 0;
	if (!ensure_target(v, target, f.view_count, &vw, &vh)) {
		return false;
	}

	// Fixed pipeline state (everything a D3D11 default would get wrong).
	ctx->IASetInputLayout(nullptr);
	ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ctx->VSSetShader(v->vs, nullptr, 0);
	ctx->GSSetShader(nullptr, nullptr, 0);
	ctx->HSSetShader(nullptr, nullptr, 0);
	ctx->DSSetShader(nullptr, nullptr, 0);
	ctx->RSSetState(v->rast);
	ctx->OMSetBlendState(v->blend, nullptr, 0xffffffff);
	ctx->OMSetDepthStencilState(v->depth_off, 0);
	ctx->PSSetSamplers(0, 1, &v->samp);

	prepass_key k = {};
	k.color = in->color;
	k.depth = in->depth;
	k.color_w = in->color_width;
	k.color_h = in->color_height;
	k.depth_w = in->depth_width;
	k.depth_h = in->depth_height;
	k.kind = p->depth.kind;
	k.value_scale = p->depth.value_scale;
	k.value_shift = p->depth.value_shift;
	k.invalid_sample = p->depth.invalid_sample;
	k.near_lo = f.near_lo;
	k.near_hi = f.near_hi;
	k.dilate = p->dilate > 16 ? 16 : p->dilate;
	k.upsample = p->upsample;
	k.snap = p->upsample && p->snap;
	k.dilate_hi = p->upsample ? (p->dilate_hi > 8 ? 8 : p->dilate_hi) : 0;
	k.upsample_max = p->upsample_max > 0 ? p->upsample_max : 1;
	k.sigma_s = p->upsample_sigma_s;
	k.sigma_r = p->upsample_sigma_r;
	k.snap_tau = p->snap_tau;
	if (k.snap) {
		u_conv_viewsyn_snap_radii(p->snap_core, p->snap_radius, in->depth_width, &k.core, &k.radius);
		if (k.core > 16) {
			k.core = 16;
		}
	}

	if (in->new_content || !v->have_key || !key_equal(k, v->key)) {
		if (!run_prepasses(v, ctx, in, p, &f, k)) {
			v->have_key = false;
			unbind(ctx);
			return false;
		}
		v->key = k;
		v->have_key = true;
	}

	// 7. one draw per view.
	unbind(ctx);
	ID3D11ShaderResourceView *srcs[2] = {in->color, v->disp_srv};
	ctx->PSSetShader(v->ps_main, nullptr, 0);
	ctx->PSSetConstantBuffers(0, 1, &v->cb_main);
	cb_main c = {};
	c.disp_res[0] = (float)in->depth_width;
	c.disp_res[1] = (float)in->depth_height;
	c.conv = (float)f.conv_n;
	c.taper = (float)f.taper;
	c.aspect = (float)f.aspect;
	c.rear_k = (float)f.rear_k;
	c.steps = (int32_t)f.steps;
	c.debug = p->debug == 1 ? 1 : 0;
	c.cam[0] = (float)f.cam.x;
	c.cam[1] = (float)f.cam.y;
	c.cam[2] = (float)f.cam.z;
	for (uint32_t i = 0; i < f.view_count; i++) {
		const bool row = target->layout == XRT_CONV_VIEWS_ROW;
		ID3D11RenderTargetView *rtv = v->tgt_rtv[row ? 0 : i];
		float x0 = row ? (float)(i * vw) : 0.0f;
		c.vp_origin[0] = x0;
		c.vp_origin[1] = 0.0f;
		c.vp_size[0] = (float)vw;
		c.vp_size[1] = (float)vh;
		c.qscale = (float)f.views[i].q_scale;
		c.eye[0] = (float)f.views[i].eye.x;
		c.eye[1] = (float)f.views[i].eye.y;
		c.eye[2] = (float)f.views[i].eye.z;
		upload(ctx, v->cb_main, c);
		ctx->OMSetRenderTargets(1, &rtv, nullptr);
		ctx->PSSetShaderResources(0, 2, srcs);
		set_viewport(ctx, x0, 0.0f, (float)vw, (float)vh);
		ctx->Draw(3, 0);
	}
	unbind(ctx);
	return true;
}

bool
d3d11_conv_viewsyn_record(struct d3d11_conv_viewsyn *v,
                          ID3D11DeviceContext *deferred,
                          const struct d3d11_conv_viewsyn_inputs *in,
                          const struct u_conv_viewsyn_params *p,
                          const struct d3d11_conv_viewsyn_target *target,
                          ID3D11CommandList **out_list)
{
	if (out_list == nullptr) {
		return false;
	}
	*out_list = nullptr;
	if (deferred == nullptr || deferred->GetType() != D3D11_DEVICE_CONTEXT_DEFERRED) {
		return false;
	}
	const bool ok = d3d11_conv_viewsyn_render(v, deferred, in, p, target);
	// Always finish: the next recording must start from the default state.
	ID3D11CommandList *list = nullptr;
	const HRESULT hr = deferred->FinishCommandList(FALSE, &list);
	if (!ok || FAILED(hr) || list == nullptr) {
		rel(list);
		return false;
	}
	*out_list = list;
	return true;
}
