// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  #1956 -- depth swapchains for OpenGL clients over IPC
 *         (comp_gl_d3d11_depth.h, the WGL_NV_DX_interop2 path).
 *
 * Pure: which GL depth formats have an honest backing (D32F, D16 -- never the
 * stencil families), that each recipe's interop staging format is the colour
 * member of the TYPELESS family the D3D11 service allocates for that depth
 * format (cross-checked against comp_d3d11_service_formats.h, so the two sides
 * cannot drift), that the read and upload types carry the same bits, the
 * format filter the GL client applies to the service's advertised list, and
 * the transfer-buffer layout (mips x array layers) the release copy indexes.
 *
 * Device-backed (WARP): the D3D11 half of the release path, end to end. The
 * service's production depth image (TYPELESS, NT handle, keyed mutex, two
 * array slices) is opened on a second device exactly as
 * client_gl_d3d11_swapchain_create does, the staging texture is created with
 * the GL client's recipe, each slice gets a distinct value through an RTV --
 * standing in for what glTexSubImage writes through the WGL registration --
 * and the staging is CopyResource'd into the shared image under the keyed
 * mutex. The service device must read the same depth back per slice. What
 * WARP cannot do is the GL half (no WGL_NV_DX_interop2): that is the Windows
 * forced-IPC CTS recipe in the PR.
 *
 * @ingroup tests
 */

#include "catch_amalgamated.hpp"

#include "comp_d3d11_service_formats.h"

#include "ogl/ogl_api.h"
#include "ogl/ogl_helpers.h"

#include "client/comp_gl_d3d11_depth.h"

#include <d3d11_1.h>
#include <dxgi1_4.h>

#include <cmath>
#include <cstring>
#include <vector>

namespace {

struct Backable
{
	const char *name;
	GLenum gl;
	DXGI_FORMAT service_dsv; // what the service allocates for it (typed request)
	DXGI_FORMAT interop;     // expected staging format
	DXGI_FORMAT family;      // expected typeless family
	GLenum read_type;        // glGetTexImage type
	uint32_t bytes;          // texel size of both read and upload
};

const Backable kBackable[] = {
    {"GL_DEPTH_COMPONENT32F", GL_DEPTH_COMPONENT32F, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT,
     DXGI_FORMAT_R32_TYPELESS, GL_FLOAT, 4},
    {"GL_DEPTH_COMPONENT16", GL_DEPTH_COMPONENT16, DXGI_FORMAT_D16_UNORM, DXGI_FORMAT_R16_UNORM,
     DXGI_FORMAT_R16_TYPELESS, GL_UNSIGNED_SHORT, 2},
};

//! The GL format list the GL client derives from the D3D11 service's full
//! advertised list (colour list, then d3d11_service_depth_formats), exactly as
//! client_gl_compositor_init does: DXGI -> VkFormat (the wire) -> GL.
std::vector<int64_t>
gl_formats_from_service()
{
	const DXGI_FORMAT colour[] = {
	    DXGI_FORMAT_R8G8B8A8_UNORM,      DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM,
	    DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R16G16B16A16_FLOAT,
	};
	std::vector<int64_t> out;
	for (DXGI_FORMAT f : colour) {
		const uint32_t gl = ogl_vk_format_to_gl(d3d_dxgi_format_to_vk(f));
		if (gl != 0) {
			out.push_back(gl);
		}
	}
	for (size_t i = 0; i < D3D11_SERVICE_DEPTH_FORMAT_COUNT; i++) {
		const uint32_t gl = ogl_vk_format_to_gl(d3d_dxgi_format_to_vk(d3d11_service_depth_formats[i]));
		if (gl != 0) {
			out.push_back(gl);
		}
	}
	return out;
}

} // namespace


TEST_CASE("recipe_backable_depth_formats")
{
	for (const auto &b : kBackable) {
		INFO(b.name);
		const client_gl_d3d11_depth_recipe r = client_gl_d3d11_depth_recipe_for(b.gl);
		CHECK(client_gl_d3d11_is_depth_format(b.gl));
		REQUIRE(r.supported);
		CHECK(r.interop_dxgi == b.interop);
		// Read the depth exactly as stored, write the same bits as red: no
		// conversion anywhere, so the copy is bit-exact.
		CHECK(r.read_format == GL_DEPTH_COMPONENT);
		CHECK(r.read_type == b.read_type);
		CHECK(r.upload_format == GL_RED);
		CHECK(r.upload_type == r.read_type);
		CHECK(r.bytes_per_texel == b.bytes);
	}
}

TEST_CASE("recipe_interop_is_the_colour_member_of_the_service_image_family")
{
	for (const auto &b : kBackable) {
		INFO(b.name);
		const client_gl_d3d11_depth_recipe r = client_gl_d3d11_depth_recipe_for(b.gl);
		// The GL client sees this GL format for the service's depth format...
		CHECK(ogl_vk_format_to_gl(d3d_dxgi_format_to_vk(b.service_dsv)) == b.gl);
		// ...whose shared image is created TYPELESS in this family...
		const d3d11_service_image_formats svc = d3d11_service_image_formats_for(b.service_dsv);
		CHECK(svc.depth);
		CHECK(svc.texture == b.family);
		// ...and the staging is that family's SRV/RT-legal colour member, which
		// is the only thing that makes CopyResource(shared, staging) legal.
		CHECK(r.interop_dxgi == d3d_dxgi_typeless_to_typed_dxgi(svc.texture));
		CHECK_FALSE(d3d11_service_is_depth_format(r.interop_dxgi));
	}
}

TEST_CASE("recipe_refuses_stencil_and_colour")
{
	// No RENDER_TARGET member in R24G8 / R32G8X24, and no 24-bit-only service
	// format: never offered.
	const GLenum refused_depth[] = {GL_DEPTH24_STENCIL8, GL_DEPTH32F_STENCIL8, GL_DEPTH_COMPONENT24};
	for (GLenum f : refused_depth) {
		INFO("GL 0x" << std::hex << f);
		CHECK(client_gl_d3d11_is_depth_format(f));
		CHECK_FALSE(client_gl_d3d11_depth_recipe_for(f).supported);
	}
	const GLenum colour[] = {GL_SRGB8_ALPHA8, GL_RGBA8, GL_RGBA16F, GL_R32F};
	for (GLenum f : colour) {
		INFO("GL 0x" << std::hex << f);
		CHECK_FALSE(client_gl_d3d11_is_depth_format(f));
		CHECK_FALSE(client_gl_d3d11_depth_recipe_for(f).supported);
	}
}

TEST_CASE("filter_keeps_colour_and_backable_depth_in_order")
{
	std::vector<int64_t> formats = gl_formats_from_service();
	// The service's three depth formats all reach GL (D24S8, D32F, D16).
	REQUIRE(formats.size() >= 3);
	const size_t colour_count = formats.size() - 3;
	const std::vector<int64_t> colour(formats.begin(), formats.begin() + (long)colour_count);
	REQUIRE(colour_count > 0);

	const uint32_t kept = client_gl_d3d11_filter_formats(formats.data(), (uint32_t)formats.size());
	REQUIRE(kept == colour_count + 2);
	for (size_t i = 0; i < colour_count; i++) {
		CHECK(formats[i] == colour[i]);
	}
	CHECK(formats[colour_count + 0] == GL_DEPTH_COMPONENT32F);
	CHECK(formats[colour_count + 1] == GL_DEPTH_COMPONENT16);

	// Probe-narrowed service (D24S8 refused, e.g. WARP): nothing else changes.
	int64_t narrowed[] = {GL_SRGB8_ALPHA8, GL_DEPTH_COMPONENT32F, GL_DEPTH24_STENCIL8,
	                      GL_RGBA16F,      GL_DEPTH_COMPONENT16,  GL_DEPTH32F_STENCIL8};
	REQUIRE(client_gl_d3d11_filter_formats(narrowed, 6) == 4);
	CHECK(narrowed[0] == GL_SRGB8_ALPHA8);
	CHECK(narrowed[1] == GL_DEPTH_COMPONENT32F);
	CHECK(narrowed[2] == GL_RGBA16F);
	CHECK(narrowed[3] == GL_DEPTH_COMPONENT16);
}

TEST_CASE("transfer_buffer_layout")
{
	const client_gl_d3d11_depth_recipe d32 = client_gl_d3d11_depth_recipe_for(GL_DEPTH_COMPONENT32F);
	const client_gl_d3d11_depth_recipe d16 = client_gl_d3d11_depth_recipe_for(GL_DEPTH_COMPONENT16);

	CHECK(client_gl_d3d11_mip_extent(10, 0) == 10);
	CHECK(client_gl_d3d11_mip_extent(10, 1) == 5);
	CHECK(client_gl_d3d11_mip_extent(10, 3) == 1);
	CHECK(client_gl_d3d11_mip_extent(1, 5) == 1);
	CHECK(client_gl_d3d11_mip_extent(7, 40) == 1);

	// 10x6, two layers (arraySize=2 stereo), three mips, 4 bytes.
	CHECK(client_gl_d3d11_depth_level_offset(&d32, 10, 6, 2, 0) == 0);
	CHECK(client_gl_d3d11_depth_level_offset(&d32, 10, 6, 2, 1) == 10 * 6 * 2 * 4);
	CHECK(client_gl_d3d11_depth_level_offset(&d32, 10, 6, 2, 2) == 480 + 5 * 3 * 2 * 4);
	CHECK(client_gl_d3d11_depth_level_offset(&d32, 10, 6, 2, 3) == 600 + 2 * 1 * 2 * 4);
	// Same shape at 2 bytes is exactly half.
	CHECK(client_gl_d3d11_depth_level_offset(&d16, 10, 6, 2, 3) == 308);
	// arraySize 0 means one layer.
	CHECK(client_gl_d3d11_depth_level_offset(&d32, 4, 4, 0, 1) == 64);
	CHECK(client_gl_d3d11_depth_level_offset(&d32, 4, 4, 1, 1) == 64);
	// A full stereo panel fits easily in 32 bits worth of buffer.
	CHECK(client_gl_d3d11_depth_level_offset(&d32, 3840, 2160, 2, 1) == 3840ull * 2160 * 2 * 4);
}

TEST_CASE("depth_shape_gate")
{
	CHECK(client_gl_d3d11_depth_shape_supported(1, 1));
	CHECK(client_gl_d3d11_depth_shape_supported(0, 0));
	CHECK_FALSE(client_gl_d3d11_depth_shape_supported(4, 1)); // MSAA: glGetTexImage cannot read it
	CHECK_FALSE(client_gl_d3d11_depth_shape_supported(1, 6)); // cube
}


/*
 *
 * Device-backed: the D3D11 half of a GL depth release, on WARP.
 *
 */

namespace {

template <typename T>
void
safe_release(T *&p)
{
	if (p != nullptr) {
		p->Release();
		p = nullptr;
	}
}

IDXGIAdapter *
warp_adapter()
{
	IDXGIFactory4 *factory = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void **)&factory)) || factory == nullptr) {
		return nullptr;
	}
	IDXGIAdapter *adapter = nullptr;
	if (FAILED(factory->EnumWarpAdapter(__uuidof(IDXGIAdapter), (void **)&adapter))) {
		adapter = nullptr;
	}
	factory->Release();
	return adapter;
}

bool
make_device(IDXGIAdapter *adapter, ID3D11Device **out_dev, ID3D11DeviceContext **out_ctx)
{
	const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
	HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, out_dev,
	                               nullptr, out_ctx);
	return SUCCEEDED(hr) && *out_dev != nullptr;
}

//! Depth value of texel (0,0) of array slice @p slice of a TYPELESS depth image.
double
read_depth_slice(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *tex, DXGI_FORMAT typed, UINT slice)
{
	D3D11_TEXTURE2D_DESC sd = {};
	tex->GetDesc(&sd);
	sd.BindFlags = 0;
	sd.MiscFlags = 0;
	sd.Usage = D3D11_USAGE_STAGING;
	sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	ID3D11Texture2D *staging = nullptr;
	REQUIRE(SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &staging)));
	ctx->CopyResource(staging, tex);
	const UINT sub = D3D11CalcSubresource(0, slice, sd.MipLevels);
	D3D11_MAPPED_SUBRESOURCE m = {};
	REQUIRE(SUCCEEDED(ctx->Map(staging, sub, D3D11_MAP_READ, 0, &m)));
	double v = -1.0;
	if (typed == DXGI_FORMAT_D16_UNORM) {
		v = *(const uint16_t *)m.pData / 65535.0;
	} else {
		float f = 0.0f;
		std::memcpy(&f, m.pData, sizeof(f));
		v = f;
	}
	ctx->Unmap(staging, sub);
	staging->Release();
	return v;
}

} // namespace

TEST_CASE("gl_depth_release_path_reaches_service_depth_image_warp")
{
	IDXGIAdapter *adapter = warp_adapter();
	ID3D11Device *svc = nullptr;
	ID3D11DeviceContext *svc_ctx = nullptr;
	ID3D11Device *cli = nullptr;
	ID3D11DeviceContext *cli_ctx = nullptr;
	if (adapter == nullptr || !make_device(adapter, &svc, &svc_ctx) || !make_device(adapter, &cli, &cli_ctx)) {
		safe_release(svc_ctx);
		safe_release(svc);
		safe_release(adapter);
		SKIP("no WARP D3D11.1 device available");
	}
	ID3D11Device1 *cli1 = nullptr;
	REQUIRE(SUCCEEDED(cli->QueryInterface(__uuidof(ID3D11Device1), (void **)&cli1)));

	const float slice_value[2] = {0.25f, 0.75f};

	for (const auto &b : kBackable) {
		INFO(b.name);
		// The service only advertises what its probe shares; WARP shares both.
		REQUIRE(d3d11_service_depth_format_shareable(svc, b.service_dsv));
		const client_gl_d3d11_depth_recipe r = client_gl_d3d11_depth_recipe_for(b.gl);
		REQUIRE(r.supported);

		// The interop staging must be renderable (the colour staging recipe and
		// what WGL registers as a colour texture).
		UINT support = 0;
		REQUIRE(SUCCEEDED(cli->CheckFormatSupport(r.interop_dxgi, &support)));
		CHECK((support & D3D11_FORMAT_SUPPORT_RENDER_TARGET) != 0);
		CHECK((support & D3D11_FORMAT_SUPPORT_TEXTURE2D) != 0);

		// --- service: the production depth image, stereo array. ---
		const D3D11_TEXTURE2D_DESC desc = d3d11_service_shared_texture_desc(32, 16, 1, 2, 1, b.service_dsv,
		                                                                    XRT_SWAPCHAIN_USAGE_DEPTH_STENCIL);
		ID3D11Texture2D *shared = nullptr;
		REQUIRE(SUCCEEDED(svc->CreateTexture2D(&desc, nullptr, &shared)));
		IDXGIResource1 *res1 = nullptr;
		REQUIRE(SUCCEEDED(shared->QueryInterface(__uuidof(IDXGIResource1), (void **)&res1)));
		HANDLE h = nullptr;
		REQUIRE(SUCCEEDED(res1->CreateSharedHandle(
		    nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &h)));
		res1->Release();

		// --- GL client's D3D11 device: open + staging, as in
		//     client_gl_d3d11_swapchain_create. ---
		ID3D11Texture2D *opened = nullptr;
		REQUIRE(SUCCEEDED(cli1->OpenSharedResource1(h, __uuidof(ID3D11Texture2D), (void **)&opened)));
		D3D11_TEXTURE2D_DESC staging_desc = {};
		opened->GetDesc(&staging_desc);
		CHECK(staging_desc.Format == b.family);
		staging_desc.MiscFlags = 0;
		staging_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		staging_desc.Format = r.interop_dxgi;
		ID3D11Texture2D *staging = nullptr;
		REQUIRE(SUCCEEDED(cli->CreateTexture2D(&staging_desc, nullptr, &staging)));

		// What glTexSubImage writes through the WGL registration: one distinct
		// depth value per array slice.
		for (UINT s = 0; s < 2; s++) {
			D3D11_RENDER_TARGET_VIEW_DESC rtvd = {};
			rtvd.Format = r.interop_dxgi;
			rtvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
			rtvd.Texture2DArray.FirstArraySlice = s;
			rtvd.Texture2DArray.ArraySize = 1;
			ID3D11RenderTargetView *rtv = nullptr;
			REQUIRE(SUCCEEDED(cli->CreateRenderTargetView(staging, &rtvd, &rtv)));
			const float c[4] = {slice_value[s], 0.0f, 0.0f, 0.0f};
			cli_ctx->ClearRenderTargetView(rtv, c);
			rtv->Release();
		}

		// client_gl_d3d11_swapchain_release_image: staging -> shared under the
		// keyed mutex.
		IDXGIKeyedMutex *cli_km = nullptr;
		REQUIRE(SUCCEEDED(opened->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&cli_km)));
		REQUIRE(cli_km->AcquireSync(0, 1000) == S_OK);
		cli_ctx->CopyResource(opened, staging);
		cli_ctx->Flush();
		REQUIRE(SUCCEEDED(cli_km->ReleaseSync(0)));

		// --- service: reads depth, per slice. ---
		IDXGIKeyedMutex *svc_km = nullptr;
		REQUIRE(SUCCEEDED(shared->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&svc_km)));
		REQUIRE(svc_km->AcquireSync(0, 1000) == S_OK);
		const double d0 = read_depth_slice(svc, svc_ctx, shared, b.service_dsv, 0);
		const double d1 = read_depth_slice(svc, svc_ctx, shared, b.service_dsv, 1);
		REQUIRE(SUCCEEDED(svc_km->ReleaseSync(0)));
		CHECK(std::fabs(d0 - 0.25) < 1e-3);
		CHECK(std::fabs(d1 - 0.75) < 1e-3);

		// And the service's sampling view over it still builds.
		D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
		srvd.Format = d3d11_service_image_formats_for(b.service_dsv).srv;
		srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
		srvd.Texture2DArray.MipLevels = 1;
		srvd.Texture2DArray.ArraySize = 2;
		ID3D11ShaderResourceView *srv = nullptr;
		CHECK(SUCCEEDED(svc->CreateShaderResourceView(shared, &srvd, &srv)));
		safe_release(srv);

		safe_release(svc_km);
		safe_release(cli_km);
		safe_release(staging);
		safe_release(opened);
		CloseHandle(h);
		safe_release(shared);
	}

	safe_release(cli1);
	safe_release(cli_ctx);
	safe_release(cli);
	safe_release(svc_ctx);
	safe_release(svc);
	safe_release(adapter);
}
