// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  #1602 — the equirect2 shader pair maps rays to the sphere, and covers
 *         only the layer's angular extent.
 * @ingroup tests
 *
 * Sibling of tests_comp_layer_array_warp.cpp, same harness and the same
 * reasoning about what a rendered pixel can prove that reflection cannot. It
 * draws through the shaders the IN-PROCESS D3D11 renderer compiles — the
 * include below is `d3d11/d3d11_layer_shaders.h`, deliberately, so this file
 * also pins that the in-process path has equirect2 at all.
 *
 * The three things it discriminates, and why each is live:
 *
 *  0. DID ANYTHING DRAW. Checked FIRST and SEPARATELY, against a magenta clear
 *     nothing here can legitimately produce. A pixel-level oracle that folds
 *     this into its colour checks cannot tell "the shader sampled the wrong
 *     texel" from "the pipeline never covered the pixel", and those need
 *     opposite fixes. Not hypothetical: the first CI run of the sibling test
 *     read its own clear for every case because it left the rasterizer at
 *     D3D11's CULL_BACK default while the renderer binds CULL_NONE.
 *
 *  1. THE HUE PAIR, BOTH DIRECTIONS. The source is split by LATITUDE — top half
 *     red, bottom half green — and the assertion is that a pixel ABOVE the tile
 *     centre reads red while one BELOW reads green. One camera, one draw, two
 *     pixels that must differ. A shader that ignored the ray and sampled a
 *     constant passes neither direction; a one-sided check would pass one of
 *     them by accident. It also pins the screen-to-ray Y orientation, which is
 *     where #1580 bit the quad path, because getting it backwards swaps exactly
 *     these two colours.
 *
 *     Deliberately HUE, never alpha. Both fills are fully opaque, so #425's
 *     force-opaque capture stamping — which made an alpha-based oracle read the
 *     same on a fixed and a broken build — cannot reach this test's answer.
 *
 *  2. THE ANGULAR EXTENT, which is the sub-rect rule made visible. With a 45°
 *     `centralHorizontalAngle` the tile's centre column is inside the layer and
 *     its edge columns are outside it. Inside must draw; OUTSIDE MUST LEAVE THE
 *     CLEAR ALONE. Under the blending-off mode an unflagged layer gets, a shader
 *     that returns transparent black for out-of-extent fragments does not
 *     composite — it OVERWRITES, erasing the tile everywhere the sphere section
 *     does not reach. That is a sub-rect layer behaving as a full-tile base,
 *     which is the thing the rule forbids, so `discard` is the only correct
 *     answer and this leg is what holds it in place.
 *
 * WARP means no GPU is required, and no device at all SKIPs rather than fails.
 * Everything the renderer binds is reproduced here — CULL_NONE, depth off,
 * triangle strip, the opaque blend state — because anything left at a D3D11
 * default is a latent version of the sibling test's first-run mistake.
 */

#include "d3d11/d3d11_layer_shaders.h"

#include "catch_amalgamated.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

namespace {

//! 16 so a pixel three rows off centre is unambiguously off centre.
constexpr uint32_t kDim = 16;

constexpr float kPi = 3.14159265358979323846f;

struct Rgba
{
	uint8_t r, g, b, a;

	bool
	operator==(const Rgba &o) const
	{
		return r == o.r && g == o.g && b == o.b && a == o.a;
	}
};

// Far apart in every channel, both fully opaque.
constexpr Rgba kTop = {255, 32, 0, 255};    // red — the texture's TOP half
constexpr Rgba kBottom = {0, 208, 64, 255}; // green — its BOTTOM half
constexpr Rgba kClear = {255, 0, 255, 255}; // magenta — nothing else makes it

std::ostream &
operator<<(std::ostream &os, const Rgba &c)
{
	return os << "(" << int(c.r) << "," << int(c.g) << "," << int(c.b) << "," << int(c.a) << ")";
}

struct Fixture
{
	ID3D11Device *dev = nullptr;
	ID3D11DeviceContext *ctx = nullptr;
	ID3D11Texture2D *src = nullptr;
	ID3D11ShaderResourceView *srv = nullptr;
	ID3D11Texture2D *rt = nullptr;
	ID3D11RenderTargetView *rtv = nullptr;
	ID3D11Texture2D *staging = nullptr;
	ID3D11VertexShader *vs = nullptr;
	ID3D11PixelShader *ps = nullptr;
	ID3D11Buffer *cb = nullptr;
	ID3D11SamplerState *samp = nullptr;
	ID3D11RasterizerState *rast = nullptr;
	ID3D11BlendState *blend_opaque = nullptr;
	ID3D11DepthStencilState *depth_off = nullptr;

	~Fixture()
	{
		for (IUnknown *p :
		     {static_cast<IUnknown *>(depth_off), static_cast<IUnknown *>(blend_opaque),
		      static_cast<IUnknown *>(rast), static_cast<IUnknown *>(samp), static_cast<IUnknown *>(cb),
		      static_cast<IUnknown *>(ps), static_cast<IUnknown *>(vs), static_cast<IUnknown *>(staging),
		      static_cast<IUnknown *>(rtv), static_cast<IUnknown *>(rt), static_cast<IUnknown *>(srv),
		      static_cast<IUnknown *>(src), static_cast<IUnknown *>(ctx), static_cast<IUnknown *>(dev)}) {
			if (p != nullptr) {
				p->Release();
			}
		}
	}
};

ID3DBlob *
compile(const char *source, const char *entry, const char *target)
{
	ID3DBlob *blob = nullptr;
	ID3DBlob *errors = nullptr;
	HRESULT hr = D3DCompile(source, strlen(source), nullptr, nullptr, nullptr, entry, target, 0, 0, &blob, &errors);
	if (FAILED(hr)) {
		std::string msg =
		    errors != nullptr ? static_cast<const char *>(errors->GetBufferPointer()) : "no error blob";
		if (errors != nullptr) {
			errors->Release();
		}
		FAIL(entry << " failed to compile: " << msg);
		return nullptr;
	}
	if (errors != nullptr) {
		errors->Release();
	}
	return blob;
}

//! @return false only when no D3D11 device exists at all → the caller SKIPs.
bool
setup(Fixture &f)
{
	// WARP first: deterministic, present on any Windows box, and it makes the
	// result independent of whatever driver the machine happens to have.
	HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &f.dev,
	                               nullptr, &f.ctx);
	if (FAILED(hr)) {
		hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
		                       &f.dev, nullptr, &f.ctx);
	}
	if (FAILED(hr) || f.dev == nullptr) {
		return false;
	}

	// The equirect source: top half one colour, bottom half the other. v == 0
	// is the TOP row under D3D sampling, and the shader's `lat` runs 0 at the
	// zenith, so this is a latitude split and nothing else.
	Rgba texels[kDim * kDim];
	for (uint32_t y = 0; y < kDim; y++) {
		for (uint32_t x = 0; x < kDim; x++) {
			texels[y * kDim + x] = (y < kDim / 2) ? kTop : kBottom;
		}
	}
	D3D11_SUBRESOURCE_DATA init = {};
	init.pSysMem = texels;
	init.SysMemPitch = kDim * sizeof(Rgba);

	D3D11_TEXTURE2D_DESC sd = {};
	sd.Width = kDim;
	sd.Height = kDim;
	sd.MipLevels = 1;
	sd.ArraySize = 1;
	sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.SampleDesc.Count = 1;
	sd.Usage = D3D11_USAGE_DEFAULT;
	sd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	REQUIRE(SUCCEEDED(f.dev->CreateTexture2D(&sd, &init, &f.src)));
	REQUIRE(SUCCEEDED(f.dev->CreateShaderResourceView(f.src, nullptr, &f.srv)));

	D3D11_TEXTURE2D_DESC rd = sd;
	rd.BindFlags = D3D11_BIND_RENDER_TARGET;
	REQUIRE(SUCCEEDED(f.dev->CreateTexture2D(&rd, nullptr, &f.rt)));
	REQUIRE(SUCCEEDED(f.dev->CreateRenderTargetView(f.rt, nullptr, &f.rtv)));

	D3D11_TEXTURE2D_DESC stg = rd;
	stg.BindFlags = 0;
	stg.Usage = D3D11_USAGE_STAGING;
	stg.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	REQUIRE(SUCCEEDED(f.dev->CreateTexture2D(&stg, nullptr, &f.staging)));

	ID3DBlob *vsb = compile(equirect2_vs_hlsl, "VSMain", "vs_5_0");
	REQUIRE(vsb != nullptr);
	REQUIRE(SUCCEEDED(f.dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &f.vs)));
	vsb->Release();

	ID3DBlob *psb = compile(equirect2_ps_hlsl, "PSMain", "ps_5_0");
	REQUIRE(psb != nullptr);
	REQUIRE(SUCCEEDED(f.dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &f.ps)));
	psb->Release();

	D3D11_BUFFER_DESC cbd = {};
	cbd.ByteWidth = static_cast<UINT>((sizeof(Equirect2LayerConstants) + 15) & ~static_cast<size_t>(15));
	cbd.Usage = D3D11_USAGE_DYNAMIC;
	cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	REQUIRE(SUCCEEDED(f.dev->CreateBuffer(&cbd, nullptr, &f.cb)));

	// POINT filtering. Both halves are uniform and every sample this test takes
	// sits well away from the boundary row, so filtering cannot change the
	// answer — but point sampling removes any doubt that a blended edge texel
	// produced the colour.
	D3D11_SAMPLER_DESC smp = {};
	smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	smp.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	smp.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	REQUIRE(SUCCEEDED(f.dev->CreateSamplerState(&smp, &f.samp)));

	// CULL_NONE, copied from comp_d3d11_renderer.cpp's create_resources. See
	// the sibling test: leaving this at D3D11's CULL_BACK default is how a
	// pixel oracle silently reads its own clear colour and reports total
	// failure. The equirect2 strip is wound the same way the quad's is.
	D3D11_RASTERIZER_DESC rs = {};
	rs.FillMode = D3D11_FILL_SOLID;
	rs.CullMode = D3D11_CULL_NONE;
	rs.FrontCounterClockwise = FALSE;
	rs.DepthClipEnable = TRUE;
	REQUIRE(SUCCEEDED(f.dev->CreateRasterizerState(&rs, &f.rast)));

	// The renderer's `blend_opaque`: BlendEnable FALSE, write mask ALL. This is
	// the state BOTH the base blit and OPAQUE_COVER bind, and binding it is the
	// whole point of the extent leg — with blending off, a returned
	// float4(0,0,0,0) overwrites the destination instead of compositing.
	D3D11_BLEND_DESC bd = {};
	bd.RenderTarget[0].BlendEnable = FALSE;
	bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	REQUIRE(SUCCEEDED(f.dev->CreateBlendState(&bd, &f.blend_opaque)));

	// Depth disabled, as the renderer's depth-stencil state is.
	D3D11_DEPTH_STENCIL_DESC dsd = {};
	dsd.DepthEnable = FALSE;
	dsd.StencilEnable = FALSE;
	REQUIRE(SUCCEEDED(f.dev->CreateDepthStencilState(&dsd, &f.depth_off)));
	return true;
}

//! One equirect2 draw over the whole target. @p central_h is the layer's
//! `centralHorizontalAngle`; vertical extent is always the full hemisphere pair.
void
draw(Fixture &f, float radius, float central_h)
{
	Equirect2LayerConstants c = {};

	// Identity inverse model-view: the layer pose and the view pose are both
	// identity, so the camera sits at the sphere's centre looking down -Z. The
	// test asserts the SHADER's ray mapping, so it deliberately does not route
	// through the math helpers the draw site uses to build this.
	c.mv_inverse[0] = 1.0f;
	c.mv_inverse[5] = 1.0f;
	c.mv_inverse[10] = 1.0f;
	c.mv_inverse[15] = 1.0f;

	// A symmetric 90° frustum: tan(±45°) = ±1, so uv [0,1] maps to tangent
	// [-1,+1] on both axes.
	c.to_tangent[0] = -1.0f; // tan(angle_left)
	c.to_tangent[1] = -1.0f; // tan(angle_down)
	c.to_tangent[2] = 2.0f;  // tan(right) - tan(left)
	c.to_tangent[3] = 2.0f;  // tan(up) - tan(down)

	// Identity sub-image transform.
	c.post_transform[2] = 1.0f;
	c.post_transform[3] = 1.0f;

	c.color_scale[0] = c.color_scale[1] = c.color_scale[2] = 1.0f;
	// OPAQUE_COVER's alpha-of-one, folded exactly as
	// comp_layer_blend_fold_opaque_cover() folds it for an unflagged layer.
	// Out-of-extent fragments never reach the line that applies it, which is
	// the property the extent leg depends on.
	c.color_scale[3] = 0.0f;
	c.color_bias[3] = 1.0f;

	c.radius = radius;
	c.central_horizontal_angle = central_h;
	c.upper_vertical_angle = kPi / 2.0f;
	c.lower_vertical_angle = -kPi / 2.0f;

	D3D11_MAPPED_SUBRESOURCE m = {};
	REQUIRE(SUCCEEDED(f.ctx->Map(f.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)));
	memcpy(m.pData, &c, sizeof(c));
	f.ctx->Unmap(f.cb, 0);

	const float clear[4] = {1.0f, 0.0f, 1.0f, 1.0f};
	f.ctx->ClearRenderTargetView(f.rtv, clear);

	D3D11_VIEWPORT vp = {};
	vp.Width = float(kDim);
	vp.Height = float(kDim);
	vp.MaxDepth = 1.0f;
	f.ctx->RSSetViewports(1, &vp);
	f.ctx->RSSetState(f.rast);
	f.ctx->OMSetRenderTargets(1, &f.rtv, nullptr);
	f.ctx->OMSetBlendState(f.blend_opaque, nullptr, 0xFFFFFFFF);
	f.ctx->OMSetDepthStencilState(f.depth_off, 0);
	f.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	f.ctx->IASetInputLayout(nullptr);
	f.ctx->VSSetShader(f.vs, nullptr, 0);
	f.ctx->PSSetShader(f.ps, nullptr, 0);
	f.ctx->VSSetConstantBuffers(0, 1, &f.cb);
	f.ctx->PSSetConstantBuffers(0, 1, &f.cb);
	f.ctx->PSSetShaderResources(0, 1, &f.srv);
	f.ctx->PSSetSamplers(0, 1, &f.samp);
	f.ctx->Draw(4, 0);

	f.ctx->CopyResource(f.staging, f.rt);
}

//! Read one pixel of the last @ref draw. Column/row are pixel indices.
Rgba
pixel(Fixture &f, uint32_t col, uint32_t row)
{
	D3D11_MAPPED_SUBRESOURCE rm = {};
	REQUIRE(SUCCEEDED(f.ctx->Map(f.staging, 0, D3D11_MAP_READ, 0, &rm)));
	const uint8_t *r = static_cast<const uint8_t *>(rm.pData) + row * rm.RowPitch;
	Rgba out = {};
	memcpy(&out, r + col * sizeof(Rgba), sizeof(out));
	f.ctx->Unmap(f.staging, 0);
	return out;
}

} // namespace

TEST_CASE("#1602 the equirect2 shaders map screen rays onto the sphere")
{
	Fixture f;
	if (!setup(f)) {
		SKIP("no D3D11 device (WARP or hardware) available");
	}

	// radius 0 is how the CPU side spells the spec's +INFINITY: the shader
	// skips the intersection and uses the ray direction itself.
	draw(f, 0.0f, 2.0f * kPi);

	const Rgba above = pixel(f, kDim / 2, 3);
	const Rgba below = pixel(f, kDim / 2, kDim - 4);

	INFO("above centre -> " << above << " (expected " << kTop << ")");
	INFO("below centre -> " << below << " (expected " << kBottom << ")");

	// GATE 0, first and on its own. A read of the clear means the draw never
	// covered the pixel — a pipeline-state problem, NOT a wrong-texel problem —
	// and the colour checks below cannot tell the two apart.
	INFO("a read of " << kClear << " means the draw never covered the pixel");
	REQUIRE_FALSE(above == kClear);
	REQUIRE_FALSE(below == kClear);

	// The hue pair, both directions. Either one alone would pass on a shader
	// that ignored the ray and always sampled that half.
	CHECK(above == kTop);
	CHECK(below == kBottom);

	// Said directly: if these two are ever equal, the screen position is not
	// steering the sample, whatever the individual values are.
	CHECK_FALSE(above == below);

	// A FINITE radius with the camera at the sphere's centre must agree: the
	// near root is behind the viewer, the far root is on the sphere, and the
	// direction to it is the ray direction. Different code path in the shader,
	// same answer — so this separates "the intersection branch is broken" from
	// "nothing works".
	draw(f, 1.0f, 2.0f * kPi);
	const Rgba above_r = pixel(f, kDim / 2, 3);
	const Rgba below_r = pixel(f, kDim / 2, kDim - 4);
	INFO("finite radius: above -> " << above_r << ", below -> " << below_r);
	REQUIRE_FALSE(above_r == kClear);
	CHECK(above_r == kTop);
	CHECK(below_r == kBottom);
}

TEST_CASE("#1602 an equirect2 layer covers only its angular extent, leaving the rest of the tile alone")
{
	Fixture f;
	if (!setup(f)) {
		SKIP("no D3D11 device (WARP or hardware) available");
	}

	// 45° horizontal extent on a 90° frustum. With the camera looking down -Z
	// the tile's centre column is well inside the layer (longitude ≈ 0.510 of a
	// turn, against a gate of 0.4375 … 0.5625) and its outermost columns are
	// well outside it (≈ 0.380 and ≈ 0.620). Blending is OFF, which is what an
	// unflagged layer gets.
	draw(f, 0.0f, kPi / 4.0f);

	const uint32_t mid_row = kDim / 2;
	const Rgba inside = pixel(f, kDim / 2, mid_row);
	const Rgba left_edge = pixel(f, 0, mid_row);
	const Rgba right_edge = pixel(f, kDim - 1, mid_row);

	INFO("inside the extent -> " << inside);
	INFO("left  edge (outside) -> " << left_edge);
	INFO("right edge (outside) -> " << right_edge);

	// GATE 0 again, and note it is the OPPOSITE expectation from the two below:
	// inside the extent the layer must have drawn.
	REQUIRE_FALSE(inside == kClear);

	// The load-bearing assertions. A sub-rect layer only MARKS the tile — it
	// never establishes the tile's base — so outside its angular extent the
	// destination must be untouched. With blending disabled that requires the
	// shader to `discard`; returning float4(0,0,0,0) there WRITES transparent
	// black over the clear, which is a full-tile cover wearing a sub-rect's
	// clothes. On a shader that returns instead of discarding these read
	// (0,0,0,0) and this test goes red, which is exactly its job.
	INFO("outside the extent the tile must still hold the clear colour " << kClear);
	CHECK(left_edge == kClear);
	CHECK(right_edge == kClear);
}
