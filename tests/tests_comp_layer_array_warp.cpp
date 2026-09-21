// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  #1601 — the array quad shader actually LANDS the requested slice.
 * @ingroup tests
 *
 * tests_comp_layer_array_shaders.cpp pins what the shaders DECLARE. That is
 * necessary and it is not sufficient: a shader can declare `Texture2DArray`,
 * reflect a correctly-placed `array_params`, and still sample the wrong slice
 * — nothing in reflection reads the `float3(uv, array_params.x)` expression.
 * The bug in #1601 was half "no array shader" and half "the slice index never
 * reached the sampler", and only a rendered pixel can tell those apart.
 *
 * So this draws. It builds the same pipeline the renderer builds — the
 * whole-array `Texture2DArray` SRV over an arraySize=2 texture, `quad_vs` +
 * `quad_ps_array`, the shared `LayerConstants` cbuffer — over a 2-slice source
 * whose slices are filled with two distinct colours, and reads the result back.
 *
 * The discriminator, and why it is live:
 *
 *   array_params.x = 0  ->  the pixel must be slice 0's colour
 *   array_params.x = 1  ->  the pixel must be slice 1's colour
 *
 * Both directions are asserted. A test that only checked slice 1 could pass on
 * a shader that ignored array_params and happened to sample slice 1; a test
 * that only checked slice 0 would have PASSED ON THE UNFIXED CODE, because
 * sampling slice 0 is exactly what the bug did. It is the pair that has
 * content: the same pipeline, changing one float, must produce two different
 * colours. Proving they differ is proving the slice index steers the sample.
 *
 * Deliberately a HUE comparison, never alpha. Both fills are fully opaque.
 * #425's capture path force-stamps alpha to 255, and an alpha-based oracle
 * built over it reads the same on a fixed and a broken build — a vacuous test
 * that this project has already been bitten by once.
 *
 * WARP means no GPU is required (tests_comp_d3d12_swapchain_format.cpp sets the
 * precedent for a device-backed check in this suite). If no device can be
 * created at all the test SKIPs rather than failing — a machine without D3D11
 * should not turn the suite red.
 *
 * Fidelity note, learned the hard way: this must reproduce the renderer's
 * PIPELINE STATE, not just its shaders. `quad_vs` flips Y in model space, so
 * its strip is counter-clockwise in NDC, and D3D11's default rasterizer state
 * (CULL_BACK, front == clockwise) culls it outright. The renderer binds
 * CULL_NONE; the first CI run of this test did not, and read the magenta clear
 * for both slices. Anything else this test leaves at a D3D11 default is a
 * latent version of the same mistake.
 */

#include "d3d11/d3d11_layer_shaders.h"

#include "catch_amalgamated.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <cstdint>
#include <cstring>
#include <string>

namespace {

constexpr uint32_t kTargetDim = 8;

//! BGRA as the staging readback presents it, for R8G8B8A8_UNORM this is RGBA.
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
constexpr Rgba kSlice0 = {0, 32, 255, 255};  // blue
constexpr Rgba kSlice1 = {255, 208, 0, 255}; // amber

std::ostream &
operator<<(std::ostream &os, const Rgba &c)
{
	return os << "(" << int(c.r) << "," << int(c.g) << "," << int(c.b) << "," << int(c.a) << ")";
}

//! Everything the draw needs, released in reverse by the destructor.
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

	~Fixture()
	{
		for (IUnknown *p :
		     {static_cast<IUnknown *>(rast), static_cast<IUnknown *>(samp), static_cast<IUnknown *>(cb),
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

//! Build the device and every resource. Returns false only when no D3D11
//! device exists at all, which the caller turns into a SKIP.
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

	// The 2-slice source, one solid colour per slice.
	Rgba slice0[kTargetDim * kTargetDim];
	Rgba slice1[kTargetDim * kTargetDim];
	for (uint32_t i = 0; i < kTargetDim * kTargetDim; i++) {
		slice0[i] = kSlice0;
		slice1[i] = kSlice1;
	}
	D3D11_SUBRESOURCE_DATA init[2] = {};
	init[0].pSysMem = slice0;
	init[0].SysMemPitch = kTargetDim * sizeof(Rgba);
	init[1].pSysMem = slice1;
	init[1].SysMemPitch = kTargetDim * sizeof(Rgba);

	D3D11_TEXTURE2D_DESC sd = {};
	sd.Width = kTargetDim;
	sd.Height = kTargetDim;
	sd.MipLevels = 1;
	sd.ArraySize = 2;
	sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.SampleDesc.Count = 1;
	sd.Usage = D3D11_USAGE_DEFAULT;
	sd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	REQUIRE(SUCCEEDED(f.dev->CreateTexture2D(&sd, init, &f.src)));

	// The WHOLE-ARRAY SRV — the same shape comp_d3d11_swapchain.cpp:387
	// creates for any arraySize>1 swapchain, which is the shape that made
	// binding it to a Texture2D shader a view-dimension mismatch.
	D3D11_SHADER_RESOURCE_VIEW_DESC vd = {};
	vd.Format = sd.Format;
	vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
	vd.Texture2DArray.MipLevels = 1;
	vd.Texture2DArray.FirstArraySlice = 0;
	vd.Texture2DArray.ArraySize = 2;
	REQUIRE(SUCCEEDED(f.dev->CreateShaderResourceView(f.src, &vd, &f.srv)));

	D3D11_TEXTURE2D_DESC rd = sd;
	rd.ArraySize = 1;
	rd.BindFlags = D3D11_BIND_RENDER_TARGET;
	REQUIRE(SUCCEEDED(f.dev->CreateTexture2D(&rd, nullptr, &f.rt)));
	REQUIRE(SUCCEEDED(f.dev->CreateRenderTargetView(f.rt, nullptr, &f.rtv)));

	D3D11_TEXTURE2D_DESC std_ = rd;
	std_.BindFlags = 0;
	std_.Usage = D3D11_USAGE_STAGING;
	std_.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	REQUIRE(SUCCEEDED(f.dev->CreateTexture2D(&std_, nullptr, &f.staging)));

	ID3DBlob *vsb = compile(quad_vs_source, "VSMain", "vs_5_0");
	REQUIRE(vsb != nullptr);
	REQUIRE(SUCCEEDED(f.dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &f.vs)));
	vsb->Release();

	ID3DBlob *psb = compile(quad_ps_array_source, "PSMain", "ps_5_0");
	REQUIRE(psb != nullptr);
	REQUIRE(SUCCEEDED(f.dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &f.ps)));
	psb->Release();

	D3D11_BUFFER_DESC cbd = {};
	cbd.ByteWidth = sizeof(LayerConstants);
	cbd.Usage = D3D11_USAGE_DYNAMIC;
	cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	REQUIRE(SUCCEEDED(f.dev->CreateBuffer(&cbd, nullptr, &f.cb)));

	// POINT filtering: both slices are uniform, so filtering cannot change the
	// answer — but a point sampler removes any doubt that an edge texel or a
	// blend between slices produced the colour.
	D3D11_SAMPLER_DESC smp = {};
	smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	smp.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	smp.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	REQUIRE(SUCCEEDED(f.dev->CreateSamplerState(&smp, &f.samp)));

	// CULL_NONE, copied from comp_d3d11_renderer.cpp's create_shaders
	// (D3D11_CULL_NONE / FrontCounterClockwise FALSE / DepthClipEnable TRUE),
	// and NOT optional.
	//
	// quad_vs flips Y in model space, which makes its triangle strip
	// counter-clockwise in NDC. D3D11's DEFAULT rasterizer state is
	// CULL_BACK with FrontCounterClockwise FALSE, i.e. front == clockwise —
	// so leaving the state unset culls the quad entirely and the readback
	// returns the clear colour. That is exactly what happened on the first CI
	// run of this test: both slices read (255,0,255,255), the magenta clear.
	// The renderer never had the bug because it binds CULL_NONE; the test had
	// it because it did not reproduce that part of the pipeline.
	D3D11_RASTERIZER_DESC rs = {};
	rs.FillMode = D3D11_FILL_SOLID;
	rs.CullMode = D3D11_CULL_NONE;
	rs.FrontCounterClockwise = FALSE;
	rs.DepthClipEnable = TRUE;
	REQUIRE(SUCCEEDED(f.dev->CreateRasterizerState(&rs, &f.rast)));
	return true;
}

//! Draw the full-target quad with @p slice in array_params.x and read the
//! centre pixel back.
Rgba
draw_slice(Fixture &f, float slice)
{
	LayerConstants c = {};
	// quad_vs maps in_uv [0,1] to pos [-0.5,0.5] (with Y flipped), so an
	// mvp of diag(2,2,1,1) fills clip space exactly. Column-major as the
	// shader's mul(mvp, v) expects, and the identity in Z/W.
	c.mvp[0] = 2.0f;
	c.mvp[5] = 2.0f;
	c.mvp[10] = 1.0f;
	c.mvp[15] = 1.0f;
	// Identity UV transform: uv = in_uv.
	c.post_transform[0] = 0.0f;
	c.post_transform[1] = 0.0f;
	c.post_transform[2] = 1.0f;
	c.post_transform[3] = 1.0f;
	c.color_scale[0] = c.color_scale[1] = c.color_scale[2] = c.color_scale[3] = 1.0f;
	// The one value under test.
	c.array_params[0] = slice;

	D3D11_MAPPED_SUBRESOURCE m = {};
	REQUIRE(SUCCEEDED(f.ctx->Map(f.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)));
	memcpy(m.pData, &c, sizeof(c));
	f.ctx->Unmap(f.cb, 0);

	// Magenta clear: nothing in the test can legitimately produce it, so a
	// readback of magenta says "the draw never covered the pixel" rather than
	// being mistaken for a slice colour.
	const float clear[4] = {1.0f, 0.0f, 1.0f, 1.0f};
	f.ctx->ClearRenderTargetView(f.rtv, clear);

	D3D11_VIEWPORT vp = {};
	vp.Width = float(kTargetDim);
	vp.Height = float(kTargetDim);
	vp.MaxDepth = 1.0f;
	f.ctx->RSSetViewports(1, &vp);
	f.ctx->RSSetState(f.rast);
	f.ctx->OMSetRenderTargets(1, &f.rtv, nullptr);
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
	D3D11_MAPPED_SUBRESOURCE rm = {};
	REQUIRE(SUCCEEDED(f.ctx->Map(f.staging, 0, D3D11_MAP_READ, 0, &rm)));
	const uint8_t *row = static_cast<const uint8_t *>(rm.pData) + (kTargetDim / 2) * rm.RowPitch;
	Rgba out = {};
	memcpy(&out, row + (kTargetDim / 2) * sizeof(Rgba), sizeof(out));
	f.ctx->Unmap(f.staging, 0);
	return out;
}

} // namespace

TEST_CASE("#1601 the array quad shader samples the slice array_params names")
{
	Fixture f;
	if (!setup(f)) {
		SKIP("no D3D11 device (WARP or hardware) available");
	}

	const Rgba got0 = draw_slice(f, 0.0f);
	const Rgba got1 = draw_slice(f, 1.0f);

	INFO("slice 0 -> " << got0 << " (expected " << kSlice0 << ")");
	INFO("slice 1 -> " << got1 << " (expected " << kSlice1 << ")");

	// Separate "the draw never happened" from "the draw sampled the wrong
	// slice" BEFORE the colour checks, because they need opposite responses
	// and the colour checks alone cannot tell them apart -- an uncovered
	// pixel fails all three below and looks like a total slice failure.
	// Magenta is the clear and nothing in this test can legitimately produce
	// it. (This is not hypothetical: the first CI run of this test read
	// magenta for both slices because the rasterizer state was left at
	// D3D11's CULL_BACK default and the quad was culled.)
	const Rgba kClear = {255, 0, 255, 255};
	INFO("a read of " << kClear
	                  << " means the draw never covered the pixel -- a pipeline-state problem, "
	                     "NOT a wrong-slice problem");
	REQUIRE_FALSE(got0 == kClear);
	REQUIRE_FALSE(got1 == kClear);

	// The load-bearing assertion. Before the fix there was no array shader at
	// all, and the behaviour it replaced was "always slice 0" — so this is the
	// line that distinguishes a working slice index from the bug.
	CHECK(got1 == kSlice1);

	// Its partner: the same pipeline with the one float changed must give the
	// OTHER colour. Without this, a shader that ignored array_params and
	// hardcoded slice 1 would pass.
	CHECK(got0 == kSlice0);

	// Said directly, so a future reader cannot mistake a one-sided pass for
	// coverage: if these two are ever equal, array_params is not reaching the
	// sampler, whatever the individual values are.
	CHECK_FALSE(got0 == got1);
}
