// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  #1956 -- depth swapchains over IPC: the D3D11 service's format policy.
 *
 * Two halves.
 *
 * Pure (no device): the wire format is a VkFormat and must map to the DXGI
 * format the client asked for -- the old converter sent VkFormat 130
 * (D32_SFLOAT_S8_UINT) to D32_FLOAT and 124/126 (D16/D32) to RGBA8; a depth
 * image is TYPELESS storage with an SRV-legal typed view; depth never gets
 * RENDER_TARGET; and the advertised depth list is the in-process parity set.
 *
 * Device-backed (WARP, so no GPU): the production description is created, an
 * NT handle is made for it, a SECOND device opens that handle the way the D3D11
 * IPC client does (OpenSharedResource1), builds its DSV, clears depth under the
 * keyed mutex, and the FIRST device reads the value back. That is the whole
 * cross-process contract minus the process boundary (an NT handle is the same
 * object in either case -- DuplicateHandle only changes which process holds
 * it). With D3D12 available the handle is also opened by a D3D12 device, as
 * comp_d3d12_client does. Self-skips when no D3D11 device exists.
 *
 * @ingroup tests
 */

#include "catch_amalgamated.hpp"

#include "comp_d3d11_service_formats.h"

#include <d3d11_1.h>
#include <dxgi1_4.h>
#ifdef DXR_TEST_HAVE_D3D12
#include <d3d12.h>
#endif

#include <cmath>
#include <cstring>

namespace {

struct DepthCase
{
	const char *name;
	DXGI_FORMAT typed;
	int64_t vk;
	DXGI_FORMAT typeless;
	DXGI_FORMAT srv;
};

// The four DXGI depth formats -- the three advertised plus D32S8X24, which a
// client can still name on the wire (VkFormat 130) even though it is not
// enumerated, and which used to be mis-mapped.
const DepthCase kDepth[] = {
    {"D24_UNORM_S8_UINT", DXGI_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_R24G8_TYPELESS,
     DXGI_FORMAT_R24_UNORM_X8_TYPELESS},
    {"D32_FLOAT", DXGI_FORMAT_D32_FLOAT, VK_FORMAT_D32_SFLOAT, DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT},
    {"D16_UNORM", DXGI_FORMAT_D16_UNORM, VK_FORMAT_D16_UNORM, DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UNORM},
    {"D32_FLOAT_S8X24_UINT", DXGI_FORMAT_D32_FLOAT_S8X24_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT,
     DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS},
};

// The service's colour list (comp_d3d11_service.cpp, system_create_native_compositor).
const DXGI_FORMAT kColour[] = {
    DXGI_FORMAT_R8G8B8A8_UNORM,      DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM,
    DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R16G16B16A16_FLOAT,
};

} // namespace


TEST_CASE("wire_format_vk_to_dxgi_depth")
{
	for (const auto &d : kDepth) {
		INFO(d.name);
		CHECK(d3d11_service_wire_format_to_dxgi(d.vk) == d.typed);
	}
	// The specific pre-#1956 mis-mappings, named so a regression reads plainly.
	CHECK(d3d11_service_wire_format_to_dxgi(VK_FORMAT_D32_SFLOAT_S8_UINT) != DXGI_FORMAT_D32_FLOAT);
	CHECK(d3d11_service_wire_format_to_dxgi(VK_FORMAT_D32_SFLOAT) != DXGI_FORMAT_R8G8B8A8_UNORM);
	CHECK(d3d11_service_wire_format_to_dxgi(VK_FORMAT_D16_UNORM) != DXGI_FORMAT_R8G8B8A8_UNORM);
}

TEST_CASE("wire_format_round_trips_every_advertised_format")
{
	// What the service advertises (DXGI -> VkFormat) must come back as the same
	// DXGI format when a client sends it to create_swapchain.
	for (DXGI_FORMAT f : kColour) {
		INFO("colour DXGI " << (int)f);
		CHECK(d3d11_service_wire_format_to_dxgi(d3d_dxgi_format_to_vk(f)) == f);
	}
	for (size_t i = 0; i < D3D11_SERVICE_DEPTH_FORMAT_COUNT; i++) {
		DXGI_FORMAT f = d3d11_service_depth_formats[i];
		INFO("depth DXGI " << (int)f);
		CHECK(d3d11_service_wire_format_to_dxgi(d3d_dxgi_format_to_vk(f)) == f);
	}
}

TEST_CASE("wire_format_unknown_is_rejected_not_rgba8")
{
	CHECK(d3d11_service_wire_format_to_dxgi(0) == DXGI_FORMAT_UNKNOWN);
	CHECK(d3d11_service_wire_format_to_dxgi(VK_FORMAT_R32_SFLOAT) == DXGI_FORMAT_UNKNOWN);
	CHECK(d3d11_service_wire_format_to_dxgi(VK_FORMAT_S8_UINT) == DXGI_FORMAT_UNKNOWN);
	CHECK(d3d11_service_wire_format_to_dxgi(123456) == DXGI_FORMAT_UNKNOWN);
	// Legacy colour pass-through and VkFormat 64 survive.
	CHECK(d3d11_service_wire_format_to_dxgi(DXGI_FORMAT_R16G16B16A16_UNORM) == DXGI_FORMAT_R16G16B16A16_UNORM);
	CHECK(d3d11_service_wire_format_to_dxgi(VK_FORMAT_A2B10G10R10_UNORM_PACK32) == DXGI_FORMAT_R10G10B10A2_UNORM);
}

TEST_CASE("advertised_depth_formats_are_the_in_process_parity_set")
{
	// comp_d3d11_compositor.cpp / comp_d3d12_compositor.cpp enumerate exactly
	// these, in this order, after their colour formats.
	REQUIRE(D3D11_SERVICE_DEPTH_FORMAT_COUNT == 3);
	CHECK(d3d11_service_depth_formats[0] == DXGI_FORMAT_D24_UNORM_S8_UINT);
	CHECK(d3d11_service_depth_formats[1] == DXGI_FORMAT_D32_FLOAT);
	CHECK(d3d11_service_depth_formats[2] == DXGI_FORMAT_D16_UNORM);

	for (size_t i = 0; i < D3D11_SERVICE_DEPTH_FORMAT_COUNT; i++) {
		DXGI_FORMAT f = d3d11_service_depth_formats[i];
		INFO("depth DXGI " << (int)f);
		CHECK(d3d11_service_is_depth_format(f));
		// comp_d3d11_client / comp_d3d12_client keep a service format only if it
		// converts VkFormat -> DXGI -> VkFormat and has a typeless sibling.
		const int64_t vk = d3d_dxgi_format_to_vk(f);
		CHECK(vk != 0);
		CHECK(d3d_vk_format_to_dxgi(vk) == f);
		CHECK(d3d_dxgi_format_to_typeless_dxgi(f) != f);
	}
}

TEST_CASE("depth_image_is_typeless_with_typed_views")
{
	for (const auto &d : kDepth) {
		INFO(d.name);
		const d3d11_service_image_formats f = d3d11_service_image_formats_for(d.typed);
		CHECK(f.depth);
		CHECK(f.texture == d.typeless);
		CHECK(f.srv == d.srv);
		CHECK(f.dsv == d.typed);
	}
}

TEST_CASE("colour_image_formats_unchanged")
{
	for (DXGI_FORMAT c : kColour) {
		INFO("colour DXGI " << (int)c);
		const d3d11_service_image_formats f = d3d11_service_image_formats_for(c);
		CHECK_FALSE(f.depth);
		CHECK(f.texture == c);
		CHECK(f.srv == c);
		CHECK(f.dsv == DXGI_FORMAT_UNKNOWN);
		CHECK_FALSE(d3d11_service_is_depth_format(c));
	}
}

TEST_CASE("bind_flags")
{
	const auto all = (enum xrt_swapchain_usage_bits)(XRT_SWAPCHAIN_USAGE_COLOR | XRT_SWAPCHAIN_USAGE_SAMPLED |
	                                                 XRT_SWAPCHAIN_USAGE_UNORDERED_ACCESS);
	for (const auto &d : kDepth) {
		INFO(d.name);
		// Even an app declaring colour usage on a depth format gets a legal
		// depth image: never RENDER_TARGET / UNORDERED_ACCESS on a depth family.
		CHECK(d3d11_service_bind_flags(d.typed, all) ==
		      (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL));
		CHECK(d3d11_service_bind_flags(d.typed, XRT_SWAPCHAIN_USAGE_SAMPLED) ==
		      (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL));
	}
	CHECK(d3d11_service_bind_flags(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, XRT_SWAPCHAIN_USAGE_COLOR) ==
	      (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET));
	CHECK(d3d11_service_bind_flags(DXGI_FORMAT_R8G8B8A8_UNORM, all) ==
	      (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS));
	CHECK(d3d11_service_bind_flags(DXGI_FORMAT_R8G8B8A8_UNORM, XRT_SWAPCHAIN_USAGE_SAMPLED) ==
	      D3D11_BIND_SHADER_RESOURCE);
}

TEST_CASE("shared_texture_desc")
{
	const D3D11_TEXTURE2D_DESC desc = d3d11_service_shared_texture_desc(
	    640, 480, 0, 2, 0, DXGI_FORMAT_D24_UNORM_S8_UINT, XRT_SWAPCHAIN_USAGE_DEPTH_STENCIL);
	CHECK(desc.Width == 640);
	CHECK(desc.Height == 480);
	CHECK(desc.MipLevels == 1);
	CHECK(desc.ArraySize == 2);
	CHECK(desc.SampleDesc.Count == 1);
	CHECK(desc.Format == DXGI_FORMAT_R24G8_TYPELESS);
	CHECK(desc.BindFlags == (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL));
	CHECK(desc.MiscFlags == (D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE));
	CHECK(desc.Usage == D3D11_USAGE_DEFAULT);
}


/*
 *
 * Device-backed: share a depth image between two devices.
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

//! The WARP adapter, so every device below sits on the same LUID (an NT handle
//! opens only on the adapter that created it).
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

//! Read texel (0,0) of a typeless depth image as a [0,1] depth value.
double
read_depth(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *tex, DXGI_FORMAT typed)
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
	D3D11_MAPPED_SUBRESOURCE m = {};
	REQUIRE(SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m)));
	double v = -1.0;
	switch (typed) {
	case DXGI_FORMAT_D16_UNORM: v = *(const uint16_t *)m.pData / 65535.0; break;
	case DXGI_FORMAT_D24_UNORM_S8_UINT: v = (*(const uint32_t *)m.pData & 0xFFFFFFu) / 16777215.0; break;
	case DXGI_FORMAT_D32_FLOAT:
	case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: {
		float f = 0.0f;
		std::memcpy(&f, m.pData, sizeof(f));
		v = f;
		break;
	}
	default: break;
	}
	ctx->Unmap(staging, 0);
	staging->Release();
	return v;
}

} // namespace

TEST_CASE("depth_image_shares_across_devices_warp")
{
	IDXGIAdapter *adapter = warp_adapter();
	ID3D11Device *svc = nullptr;
	ID3D11DeviceContext *svc_ctx = nullptr;
	ID3D11Device *app = nullptr;
	ID3D11DeviceContext *app_ctx = nullptr;
	if (adapter == nullptr || !make_device(adapter, &svc, &svc_ctx) || !make_device(adapter, &app, &app_ctx)) {
		safe_release(svc_ctx);
		safe_release(svc);
		safe_release(adapter);
		SKIP("no WARP D3D11.1 device available");
	}
	ID3D11Device1 *app1 = nullptr;
	REQUIRE(SUCCEEDED(app->QueryInterface(__uuidof(ID3D11Device1), (void **)&app1)));

#ifdef DXR_TEST_HAVE_D3D12
	ID3D12Device *d12 = nullptr;
	if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void **)&d12))) {
		d12 = nullptr;
		WARN("no WARP D3D12 device -- the D3D12-client half is not exercised");
	}
#endif

	// Every DXGI depth format: the three advertised must pass; D32S8X24 is
	// reported, not required (it is not advertised).
	for (const auto &d : kDepth) {
		bool advertised = false;
		for (size_t i = 0; i < D3D11_SERVICE_DEPTH_FORMAT_COUNT; i++) {
			advertised = advertised || d3d11_service_depth_formats[i] == d.typed;
		}
		INFO(d.name << (advertised ? " (advertised)" : " (not advertised)"));

		const bool shareable = d3d11_service_depth_format_shareable(svc, d.typed);
		UNSCOPED_INFO("probe " << d.name << " shareable=" << shareable);
		if (advertised) {
			CHECK(shareable);
		}
		if (!shareable) {
			continue;
		}

		// --- service side: the production description, two array slices. ---
		const D3D11_TEXTURE2D_DESC desc =
		    d3d11_service_shared_texture_desc(32, 32, 1, 2, 1, d.typed, XRT_SWAPCHAIN_USAGE_DEPTH_STENCIL);
		ID3D11Texture2D *tex = nullptr;
		REQUIRE(SUCCEEDED(svc->CreateTexture2D(&desc, nullptr, &tex)));

		// The service's own SRV over it (compositor_create_swapchain).
		D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
		srvd.Format = d3d11_service_image_formats_for(d.typed).srv;
		srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
		srvd.Texture2DArray.MipLevels = 1;
		srvd.Texture2DArray.ArraySize = 2;
		ID3D11ShaderResourceView *srv = nullptr;
		CHECK(SUCCEEDED(svc->CreateShaderResourceView(tex, &srvd, &srv)));
		safe_release(srv);

		IDXGIResource1 *res1 = nullptr;
		REQUIRE(SUCCEEDED(tex->QueryInterface(__uuidof(IDXGIResource1), (void **)&res1)));
		HANDLE h = nullptr;
		REQUIRE(SUCCEEDED(res1->CreateSharedHandle(
		    nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &h)));
		res1->Release();

		// --- client side (comp_d3d11_client): OpenSharedResource1. ---
		ID3D11Texture2D *opened = nullptr;
		REQUIRE(SUCCEEDED(app1->OpenSharedResource1(h, __uuidof(ID3D11Texture2D), (void **)&opened)));
		D3D11_TEXTURE2D_DESC od = {};
		opened->GetDesc(&od);
		// The app receives the TYPELESS resource (OpenXR D3D11 binding + CTS).
		CHECK(od.Format == d.typeless);
		CHECK((od.BindFlags & D3D11_BIND_DEPTH_STENCIL) != 0);

		// The app builds its own typed DSV over both slices and clears under
		// the keyed mutex, exactly as it would render.
		D3D11_DEPTH_STENCIL_VIEW_DESC dsvd = {};
		dsvd.Format = d.typed;
		dsvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
		dsvd.Texture2DArray.FirstArraySlice = 0;
		dsvd.Texture2DArray.ArraySize = 2;
		ID3D11DepthStencilView *dsv = nullptr;
		REQUIRE(SUCCEEDED(app->CreateDepthStencilView(opened, &dsvd, &dsv)));

		IDXGIKeyedMutex *app_km = nullptr;
		REQUIRE(SUCCEEDED(opened->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&app_km)));
		REQUIRE(app_km->AcquireSync(0, 1000) == S_OK);
		app_ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 0.25f, 0);
		app_ctx->Flush();
		REQUIRE(SUCCEEDED(app_km->ReleaseSync(0)));

		// --- service side again: the depth the app wrote is visible. ---
		IDXGIKeyedMutex *svc_km = nullptr;
		REQUIRE(SUCCEEDED(tex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&svc_km)));
		REQUIRE(svc_km->AcquireSync(0, 1000) == S_OK);
		const double depth = read_depth(svc, svc_ctx, tex, d.typed);
		REQUIRE(SUCCEEDED(svc_km->ReleaseSync(0)));
		CHECK(std::fabs(depth - 0.25) < 1e-3);

#ifdef DXR_TEST_HAVE_D3D12
		// --- client side (comp_d3d12_client): OpenSharedHandle. ---
		if (d12 != nullptr) {
			ID3D12Resource *r12 = nullptr;
			HRESULT hr12 = d12->OpenSharedHandle(h, __uuidof(ID3D12Resource), (void **)&r12);
			CHECK(SUCCEEDED(hr12));
			if (SUCCEEDED(hr12) && r12 != nullptr) {
				const D3D12_RESOURCE_DESC rd = r12->GetDesc();
				CHECK(rd.Format == d.typeless);
				CHECK((rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0);
				CHECK(rd.DepthOrArraySize == 2);
			}
			safe_release(r12);
		}
#endif

		safe_release(svc_km);
		safe_release(app_km);
		safe_release(dsv);
		safe_release(opened);
		CloseHandle(h);
		safe_release(tex);
	}

#ifdef DXR_TEST_HAVE_D3D12
	safe_release(d12);
#endif
	safe_release(app1);
	safe_release(app_ctx);
	safe_release(app);
	safe_release(svc_ctx);
	safe_release(svc);
	safe_release(adapter);
}
