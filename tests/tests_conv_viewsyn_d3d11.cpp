// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-053 S5 — the D3D11 reference view synthesis on WARP: a synthetic
 *         colour + depth with a known step edge, rendered into 1, 2 and 4
 *         views, checked against u_conv_viewsyn's CPU math.
 *
 * The colour image ENCODES its own coordinates: R = source u (left to right),
 * G = source v (bottom to top), B = 1 on the near square, 0 on the far ground.
 * So a single output pixel tells which source position the GPU march sampled,
 * and the B channel tells where the square's silhouette went.
 *
 * Checked per view:
 *  0. DID ANYTHING DRAW: every pixel of every view differs from the magenta
 *     clear (a pipeline that never covers the pixel reads its own clear).
 *  1. PLATEAU PARALLAX: at the square's centre and on the far ground the
 *     sampled source u equals the CPU march's (u_conv_viewsyn_march), and the
 *     shift equals the closed form (E - C).x * q / A for the plateau's q.
 *  2. THE OCCLUDING EDGE: the silhouette edge on the side the square moves
 *     toward lands where the closed form puts it (the disoccluded side is the
 *     hole-fill's, not a geometric oracle).
 *  3. A view AT the source camera (1 view) is the identity — the JS dev page's
 *     "neutral view MAE" check.
 *
 * WARP means no GPU is required; no D3D11 device at all SKIPs.
 *
 * @ingroup tests
 */

// Catch2 first, and no min/max macros from <windows.h> (pulled in by the stage header).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "catch_amalgamated.hpp"

#include "d3d11_service/d3d11_conv_viewsyn.h"

#include <d3d11.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

constexpr uint32_t kW = 512, kH = 288;  // colour = one output view
constexpr uint32_t kDW = 128, kDH = 72; // depth grid (4 px per depth texel)
// Near square in depth texels (aligned to colour pixels x4).
constexpr uint32_t kSqX0 = 48, kSqX1 = 80, kSqY0 = 24, kSqY1 = 48; // [x0, x1) x [y0, y1), row 0 = top
constexpr float kRectW = 0.32f, kRectH = 0.18f;

template <typename T>
void
rel(T *&p)
{
	if (p != nullptr) {
		p->Release();
		p = nullptr;
	}
}

struct Fixture
{
	ID3D11Device *dev = nullptr;
	ID3D11DeviceContext *ctx = nullptr;
	ID3D11Texture2D *color = nullptr, *depth = nullptr;
	ID3D11ShaderResourceView *color_srv = nullptr, *depth_srv = nullptr;
	d3d11_conv_viewsyn *vs = nullptr;

	~Fixture()
	{
		d3d11_conv_viewsyn_destroy(&vs);
		rel(depth_srv);
		rel(color_srv);
		rel(depth);
		rel(color);
		rel(ctx);
		rel(dev);
	}
};

bool
make_fixture(Fixture &f)
{
	HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &f.dev,
	                               nullptr, &f.ctx);
	if (FAILED(hr)) {
		hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
		                       &f.dev, nullptr, &f.ctx);
	}
	if (FAILED(hr)) {
		return false;
	}

	// Colour: R16G16B16A16_UNORM so R / G carry the source coordinate to 1/65535.
	std::vector<uint16_t> c(kW * kH * 4);
	for (uint32_t y = 0; y < kH; y++) {
		for (uint32_t x = 0; x < kW; x++) {
			const bool in_sq = x >= kSqX0 * 4 && x < kSqX1 * 4 && y >= kSqY0 * 4 && y < kSqY1 * 4;
			uint16_t *p = &c[(y * kW + x) * 4];
			p[0] = (uint16_t)std::lround((x + 0.5) / kW * 65535.0);
			p[1] = (uint16_t)std::lround((1.0 - (y + 0.5) / kH) * 65535.0);
			p[2] = in_sq ? 65535 : 0;
			p[3] = 65535;
		}
	}
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = kW;
	td.Height = kH;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_IMMUTABLE;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	D3D11_SUBRESOURCE_DATA sd = {c.data(), kW * 8, 0};
	if (FAILED(f.dev->CreateTexture2D(&td, &sd, &f.color)) ||
	    FAILED(f.dev->CreateShaderResourceView(f.color, nullptr, &f.color_srv))) {
		return false;
	}

	// Depth: affine disparity, 1 on the square, 0 on the ground.
	std::vector<float> d(kDW * kDH, 0.0f);
	for (uint32_t y = kSqY0; y < kSqY1; y++) {
		for (uint32_t x = kSqX0; x < kSqX1; x++) {
			d[y * kDW + x] = 1.0f;
		}
	}
	td.Width = kDW;
	td.Height = kDH;
	td.Format = DXGI_FORMAT_R32_FLOAT;
	sd = {d.data(), kDW * 4, 0};
	if (FAILED(f.dev->CreateTexture2D(&td, &sd, &f.depth)) ||
	    FAILED(f.dev->CreateShaderResourceView(f.depth, nullptr, &f.depth_srv))) {
		return false;
	}
	f.vs = d3d11_conv_viewsyn_create(f.dev);
	return f.vs != nullptr;
}

struct Px
{
	double r, g, b;
	uint16_t raw[4];
};

//! Render and read back every view (slice or column block) into views[i][y*kW + x].
bool
render(Fixture &f, const u_conv_viewsyn_params &p, uint32_t layout, std::vector<std::vector<Px>> &views)
{
	const uint32_t n = p.view_count;
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = layout == U_CONV_VIEWSYN_LAYOUT_ROW ? kW * n : kW;
	td.Height = kH;
	td.MipLevels = 1;
	td.ArraySize = layout == U_CONV_VIEWSYN_LAYOUT_ROW ? 1 : n;
	td.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_RENDER_TARGET;
	ID3D11Texture2D *rt = nullptr, *st = nullptr;
	if (FAILED(f.dev->CreateTexture2D(&td, nullptr, &rt))) {
		return false;
	}
	// Magenta clear: nothing the stage can legitimately produce.
	for (uint32_t s = 0; s < td.ArraySize; s++) {
		D3D11_RENDER_TARGET_VIEW_DESC rd = {};
		rd.Format = td.Format;
		rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
		rd.Texture2DArray.FirstArraySlice = s;
		rd.Texture2DArray.ArraySize = 1;
		ID3D11RenderTargetView *rtv = nullptr;
		f.dev->CreateRenderTargetView(rt, &rd, &rtv);
		const float magenta[4] = {1, 0, 1, 1};
		f.ctx->ClearRenderTargetView(rtv, magenta);
		rel(rtv);
	}

	d3d11_conv_viewsyn_inputs in = {f.color_srv, kW, kH, f.depth_srv, kDW, kDH, true};
	d3d11_conv_viewsyn_target t = {rt, DXGI_FORMAT_UNKNOWN, layout};
	bool ok = d3d11_conv_viewsyn_render(f.vs, f.ctx, &in, &p, &t);

	td.Usage = D3D11_USAGE_STAGING;
	td.BindFlags = 0;
	td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	ok = ok && SUCCEEDED(f.dev->CreateTexture2D(&td, nullptr, &st));
	if (ok) {
		f.ctx->CopyResource(st, rt);
		views.assign(n, std::vector<Px>(kW * kH));
		for (uint32_t s = 0; s < td.ArraySize && ok; s++) {
			D3D11_MAPPED_SUBRESOURCE m;
			ok = SUCCEEDED(f.ctx->Map(st, s, D3D11_MAP_READ, 0, &m));
			if (!ok) {
				break;
			}
			for (uint32_t y = 0; y < kH; y++) {
				const uint16_t *row = (const uint16_t *)((const uint8_t *)m.pData + y * m.RowPitch);
				for (uint32_t x = 0; x < td.Width; x++) {
					uint32_t vi = layout == U_CONV_VIEWSYN_LAYOUT_ROW ? x / kW : s;
					uint32_t vx = layout == U_CONV_VIEWSYN_LAYOUT_ROW ? x % kW : x;
					Px &px = views[vi][y * kW + vx];
					memcpy(px.raw, row + x * 4, 8);
					px.r = px.raw[0] / 65535.0;
					px.g = px.raw[1] / 65535.0;
					px.b = px.raw[2] / 65535.0;
				}
			}
			f.ctx->Unmap(st, s);
		}
	}
	rel(st);
	rel(rt);
	return ok;
}

u_conv_viewsyn_params
make_params(const std::vector<double> &eye_x_m)
{
	u_conv_viewsyn_params p;
	u_conv_viewsyn_params_defaults(&p);
	p.rect_width_m = kRectW;
	p.rect_height_m = kRectH;
	p.view_count = (uint32_t)eye_x_m.size();
	for (uint32_t i = 0; i < p.view_count; i++) {
		p.viewpoints_m[i][0] = (float)eye_x_m[i];
		p.viewpoints_m[i][1] = 0.0f;
		p.viewpoints_m[i][2] = 0.6f;
	}
	p.depth.kind = U_CONV_VIEWSYN_DEPTH_AFFINE_DISPARITY;
	p.range_near = 1.0f;
	p.range_far = 0.0f;
	p.convergence = 0.5f; // decoded: half-way, so the square pops out and the ground sinks
	p.budget = 0.1f;      // large enough that one eye's shift is ~13 px at 512 wide
	return p;
}

//! The step scene for the CPU march (source uv, v up).
double
step_sample(void *, double u, double v)
{
	const double x0 = (double)kSqX0 / kDW, x1 = (double)kSqX1 / kDW;
	const double v1 = 1.0 - (double)kSqY0 / kDH, v0 = 1.0 - (double)kSqY1 / kDH;
	return (u >= x0 && u < x1 && v > v0 && v <= v1) ? 1.0 : 0.0;
}

/*!
 * Column (fractional, px) where B crosses 0.5 on row @p y, scanning from
 * @p from toward @p to; NaN if none.
 */
double
edge_col(const std::vector<Px> &img, uint32_t y, int from, int to)
{
	int step = to > from ? 1 : -1;
	for (int x = from; x != to; x += step) {
		double b0 = img[y * kW + x].b, b1 = img[y * kW + x + step].b;
		if ((b0 < 0.5) != (b1 < 0.5)) {
			double t = (0.5 - b0) / (b1 - b0);
			return x + 0.5 + step * t; // pixel centres at x + 0.5
		}
	}
	return NAN;
}

void
check_views(const u_conv_viewsyn_params &p, const std::vector<std::vector<Px>> &views)
{
	u_conv_viewsyn_frame fr;
	REQUIRE(u_conv_viewsyn_resolve(&p, &fr));
	const double q_near = fr.views[0].q_scale * (1.0 - fr.conv_n);
	const double q_far = fr.views[0].q_scale * (0.0 - fr.conv_n);
	INFO("qs " << fr.views[0].q_scale << " cam.z " << fr.cam.z << " conv_n " << fr.conv_n << " rear_k " << fr.rear_k
	           << " A " << fr.aspect << " lo " << fr.near_lo << " hi " << fr.near_hi);
	REQUIRE(std::isfinite(q_near));
	REQUIRE(std::isfinite(q_far));
	const uint32_t yc = kH / 2;
	const double src_left = kSqX0 * 4.0, src_right = kSqX1 * 4.0; // px

	double prev_shift = INFINITY;
	for (uint32_t i = 0; i < p.view_count; i++) {
		const std::vector<Px> &img = views[i];
		INFO("view " << i << " eye.x " << fr.views[i].eye.x << " cam.x " << fr.cam.x);

		// 0. coverage: no magenta survives.
		uint32_t clear = 0;
		for (const Px &px : img) {
			clear += (px.raw[0] == 65535 && px.raw[1] == 0 && px.raw[2] == 65535) ? 1 : 0;
		}
		REQUIRE(clear == 0);

		// Closed form: content shift in px for parallax q (E.z == C.z here).
		const double dx_eye = fr.views[i].eye.x - fr.cam.x;
		auto shift_px = [&](double q) { return -dx_eye * q / fr.aspect * kW; };

		// 1. plateau centre and far ground vs the CPU march.
		// Ground probes stay clear of the border taper band for every eye here
		// (shift <= 26 px + taper 0.03 * 512 = 15 px): inside that band the
		// taper's own gradient trips the disocclusion gather, as it does in JS.
		for (uint32_t x : {kW / 2, 90u, kW - 90}) {
			const double uv[2] = {(x + 0.5) / kW, 1.0 - (yc + 0.5) / kH};
			double s[2];
			double q = u_conv_viewsyn_march(&fr, &fr.views[i], step_sample, nullptr, uv, s);
			const Px &px = img[yc * kW + x];
			INFO("x " << x << " gpu u " << px.r << " cpu u " << s[0] << " q " << q);
			CHECK(std::fabs(px.r - s[0]) * kW <= 1.0); // within one output px
			CHECK(std::fabs(px.g - s[1]) * kH <= 0.5); // no vertical parallax
			const bool on_square = x == kW / 2;
			CHECK(std::fabs(q - (on_square ? q_near : q_far)) <= std::fabs(q_near - q_far) / fr.steps);
			CHECK(std::fabs((uv[0] - px.r) * kW - shift_px(on_square ? q_near : q_far)) <= 1.0);
		}

		// 2. the occluding edge: the side the square moves toward.
		const double sh = shift_px(q_near);
		if (std::fabs(dx_eye) > 1e-9) {
			const bool moves_left = sh < 0;
			double got = moves_left ? edge_col(img, yc, kW / 2, 0) : edge_col(img, yc, kW / 2, kW - 1);
			double want = (moves_left ? src_left : src_right) + sh;
			INFO("edge got " << got << " want " << want << " shift " << sh);
			CHECK(std::fabs(got - want) <= 1.5);
			// Every view's near content shifts monotonically with its eye x.
			CHECK(sh < prev_shift);
			prev_shift = sh;
		}
	}
}

} // namespace

TEST_CASE("viewsyn d3d11: 2 views, SBS row, step edge parallax", "[conv_viewsyn][d3d11]")
{
	Fixture f;
	if (!make_fixture(f)) {
		SKIP("no D3D11 device (WARP or hardware) available, or the stage failed to init");
	}
	u_conv_viewsyn_params p = make_params({-0.0315, 0.0315});
	std::vector<std::vector<Px>> views;
	REQUIRE(render(f, p, U_CONV_VIEWSYN_LAYOUT_ROW, views));
	check_views(p, views);
	CHECK(d3d11_conv_viewsyn_prepass_count(f.vs) == 1);
}

TEST_CASE("viewsyn d3d11: 4 views, array slices, step edge parallax", "[conv_viewsyn][d3d11]")
{
	Fixture f;
	if (!make_fixture(f)) {
		SKIP("no D3D11 device (WARP or hardware) available, or the stage failed to init");
	}
	u_conv_viewsyn_params p = make_params({-0.063, -0.021, 0.021, 0.063});
	std::vector<std::vector<Px>> views;
	REQUIRE(render(f, p, U_CONV_VIEWSYN_LAYOUT_ARRAY, views));
	check_views(p, views);
}

TEST_CASE("viewsyn d3d11: a view at the source camera is the identity", "[conv_viewsyn][d3d11]")
{
	Fixture f;
	if (!make_fixture(f)) {
		SKIP("no D3D11 device (WARP or hardware) available, or the stage failed to init");
	}
	u_conv_viewsyn_params p = make_params({0.01});
	std::vector<std::vector<Px>> views;
	REQUIRE(render(f, p, U_CONV_VIEWSYN_LAYOUT_ROW, views));
	double max_err = 0.0;
	for (uint32_t y = 0; y < kH; y++) {
		for (uint32_t x = 0; x < kW; x++) {
			const Px &px = views[0][y * kW + x];
			max_err = std::fmax(max_err, std::fabs(px.r - (x + 0.5) / kW));
			max_err = std::fmax(max_err, std::fabs(px.g - (1.0 - (y + 0.5) / kH)));
		}
	}
	INFO("max |err| " << max_err * 255.0 << " /255");
	CHECK(max_err * 255.0 < 1.0);
}
