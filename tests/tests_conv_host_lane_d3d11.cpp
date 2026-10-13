// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-053 P2 (#2034): the D3D11 lane of a satellite (d3d11_conv_host_lane)
 *         on WARP, under the D3D11 debug layer when it is installed.
 *
 * A "host" device creates an NT-handle output texture and a shared fence (as
 * displayxr-conversion-host does); a "service" device creates the ring target.
 * The lane opens both by handle, observes the host fence on the CPU and only
 * then copies. Cases:
 *  - the fence value is reached: the copy lands, bytes match;
 *  - the host "dies" before signalling (the value is never reached): the
 *    observe fails within its timeout, nothing waits on the GPU, the target
 *    keeps its previous contents — the CI shape of "kill between submit and
 *    signal: the compositor is unaffected".
 * SKIPs when WARP has no shared fences (roadmap §11.2 must-verify).
 */

#include "catch_amalgamated.hpp"

#include "d3d11_service/d3d11_conv_host_lane.h"

#include <d3d11_4.h>
#include <dxgi1_2.h>

#include <chrono>
#include <cstring>
#include <vector>

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

struct Dev
{
	ID3D11Device5 *dev = nullptr;
	ID3D11DeviceContext4 *ctx = nullptr;
	bool debug = false;
	~Dev()
	{
		rel(ctx);
		rel(dev);
	}
	bool
	create(IDXGIAdapter *warp)
	{
		ID3D11Device *d = nullptr;
		ID3D11DeviceContext *c = nullptr;
		const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
		HRESULT hr = D3D11CreateDevice(warp, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_DEBUG, &fl, 1,
		                               D3D11_SDK_VERSION, &d, nullptr, &c);
		debug = SUCCEEDED(hr);
		if (FAILED(hr)) {
			hr = D3D11CreateDevice(warp, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &d,
			                       nullptr, &c);
		}
		if (FAILED(hr)) {
			return false;
		}
		const bool ok = SUCCEEDED(d->QueryInterface(__uuidof(ID3D11Device5), (void **)&dev)) &&
		                SUCCEEDED(c->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)&ctx));
		d->Release();
		c->Release();
		return ok;
	}
};

struct Shared
{
	ID3D11Texture2D *tex = nullptr;
	HANDLE handle = nullptr;
	~Shared()
	{
		rel(tex);
		if (handle != nullptr) {
			CloseHandle(handle);
		}
	}
	bool
	create(ID3D11Device5 *dev, uint32_t w, uint32_t h, const std::vector<uint32_t> &px)
	{
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = w;
		td.Height = h;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
		D3D11_SUBRESOURCE_DATA sd = {px.data(), w * 4u, 0};
		if (FAILED(dev->CreateTexture2D(&td, &sd, &tex))) {
			return false;
		}
		IDXGIResource1 *r = nullptr;
		if (FAILED(tex->QueryInterface(__uuidof(IDXGIResource1), (void **)&r))) {
			return false;
		}
		const HRESULT hr = r->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
		                                         nullptr, &handle);
		r->Release();
		return SUCCEEDED(hr);
	}
};

uint64_t
luid_of(IDXGIAdapter1 *a)
{
	DXGI_ADAPTER_DESC1 d;
	a->GetDesc1(&d);
	return (uint64_t)(uint32_t)d.AdapterLuid.HighPart << 32 | d.AdapterLuid.LowPart;
}

IDXGIAdapter1 *
warp_adapter()
{
	IDXGIFactory1 *f = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&f))) {
		return nullptr;
	}
	IDXGIAdapter1 *a = nullptr, *found = nullptr;
	for (UINT i = 0; found == nullptr && f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
		DXGI_ADAPTER_DESC1 d;
		a->GetDesc1(&d);
		if ((d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
			found = a;
		} else {
			a->Release();
		}
	}
	f->Release();
	return found;
}

std::vector<uint32_t>
read_back(Dev &d, ID3D11Texture2D *tex, uint32_t w, uint32_t h)
{
	D3D11_TEXTURE2D_DESC td;
	tex->GetDesc(&td);
	td.Usage = D3D11_USAGE_STAGING;
	td.BindFlags = 0;
	td.MiscFlags = 0;
	td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	ID3D11Texture2D *st = nullptr;
	std::vector<uint32_t> out(w * h, 0);
	if (FAILED(d.dev->CreateTexture2D(&td, nullptr, &st))) {
		return out;
	}
	d.ctx->CopyResource(st, tex);
	D3D11_MAPPED_SUBRESOURCE m;
	if (SUCCEEDED(d.ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
		for (uint32_t y = 0; y < h; y++) {
			std::memcpy(&out[y * w], (const uint8_t *)m.pData + y * m.RowPitch, w * 4u);
		}
		d.ctx->Unmap(st, 0);
	}
	st->Release();
	return out;
}

} // namespace

TEST_CASE("d3d11 lane: a host output is consumed only after its fence is observed on the CPU", "[conv_host_lane]")
{
	IDXGIAdapter1 *warp = warp_adapter();
	if (warp == nullptr) {
		SKIP("no WARP adapter");
	}
	const uint64_t luid = luid_of(warp);
	Dev host, service;
	const bool devs = host.create(warp) && service.create(warp);
	warp->Release();
	if (!devs) {
		SKIP("no D3D11.4 device on WARP");
	}
	INFO("debug layer: " << (host.debug ? "on" : "not installed"));

	constexpr uint32_t W = 16, H = 8;
	const std::vector<uint32_t> result(W * H, 0xFF4080C0u), previous(W * H, 0xFF000000u);
	Shared out, target;
	REQUIRE(out.create(host.dev, W, H, result));
	REQUIRE(target.create(service.dev, W, H, previous));

	ID3D11Fence *fence = nullptr;
	HANDLE fence_h = nullptr;
	if (FAILED(host.dev->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), (void **)&fence)) ||
	    FAILED(fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fence_h))) {
		rel(fence);
		SKIP("WARP has no shared fences (roadmap §11.2: those cases run on hardware only)");
	}

	conv_host_proxy_gpu_ops ops{};
	REQUIRE(d3d11_conv_host_lane_create(luid, &ops));

	// The lane takes ownership of the handles it opens: hand it duplicates.
	HANDLE out_dup = nullptr, fence_dup = nullptr;
	REQUIRE(DuplicateHandle(GetCurrentProcess(), out.handle, GetCurrentProcess(), &out_dup, 0, FALSE,
	                        DUPLICATE_SAME_ACCESS));
	REQUIRE(DuplicateHandle(GetCurrentProcess(), fence_h, GetCurrentProcess(), &fence_dup, 0, FALSE,
	                        DUPLICATE_SAME_ACCESS));
	xrt_conv_image desc{};
	desc.memory_kind = XRT_CONV_MEM_WIN32_NT_HANDLE;
	desc.width = W;
	desc.height = H;
	desc.format = XRT_CONV_FMT_RGBA8_UNORM;
	void *octx = nullptr, *lfence = nullptr;
	REQUIRE(ops.open_output(ops.ud, (uint64_t)(uintptr_t)out_dup, &desc, &octx));
	REQUIRE(ops.open_fence(ops.ud, (uint64_t)(uintptr_t)fence_dup, &lfence));

	xrt_conv_image dst{};
	dst.memory_kind = XRT_CONV_MEM_WIN32_NT_HANDLE;
	dst.handle = (uint64_t)(uintptr_t)target.handle;
	dst.import_key = 77;
	dst.width = W;
	dst.height = H;

	SECTION("the host died before signalling: the observe times out, nothing is copied, nothing waits on the GPU")
	{
		const auto t0 = std::chrono::steady_clock::now();
		CHECK_FALSE(ops.observe_fence(ops.ud, lfence, 1, 200));
		const auto ms =
		    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
		CHECK(ms >= 150);
		CHECK(ms < 2000);
		// The service device is untouched and still responsive.
		CHECK(read_back(service, target.tex, W, H) == previous);
	}
	SECTION("the host died after its completion record: the fence reads UINT64_MAX, the observe fails")
	{
		// What Windows does to a shared fence whose signaller died
		// (tests_conv_d3d11_xproc): every holder sees UINT64_MAX and every
		// SetEventOnCompletion fires. That is a lost host, not a done frame.
		host.ctx->Signal(fence, UINT64_MAX);
		host.ctx->Flush();
		const auto t0 = std::chrono::steady_clock::now();
		CHECK_FALSE(ops.observe_fence(ops.ud, lfence, 1, 2000));
		const auto ms =
		    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
		CHECK(ms < 1500); // returns at once (or when the event fires), never at the timeout
		CHECK(read_back(service, target.tex, W, H) == previous);
	}
	SECTION("the fence reaches the value: copy, bytes match")
	{
		host.ctx->Signal(fence, 1);
		host.ctx->Flush();
		REQUIRE(ops.observe_fence(ops.ud, lfence, 1, 2000));
		REQUIRE(ops.copy_output(ops.ud, octx, &dst));
		CHECK(read_back(service, target.tex, W, H) == result);
	}

	ops.close_output(ops.ud, octx);
	ops.close_fence(ops.ud, lfence);
	ops.destroy(ops.ud);
	CloseHandle(fence_h);
	rel(fence);
}


TEST_CASE("d3d11 lane: a mismatched adapter is refused loudly", "[conv_host_lane]")
{
	// No adapter has this LUID: no lane (the satellite stays on the CPU lane).
	conv_host_proxy_gpu_ops ops{};
	CHECK_FALSE(d3d11_conv_host_lane_create(0x7FFFFFFF00000001ull, &ops));

	// A lane on WARP cannot open a texture that lives on a hardware adapter.
	IDXGIAdapter1 *warp = warp_adapter();
	if (warp == nullptr) {
		SKIP("no WARP adapter");
	}
	const uint64_t warp_luid = luid_of(warp);
	warp->Release();
	IDXGIFactory1 *f = nullptr;
	REQUIRE(SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&f)));
	IDXGIAdapter1 *hw = nullptr, *a = nullptr;
	for (UINT i = 0; hw == nullptr && f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
		DXGI_ADAPTER_DESC1 d;
		a->GetDesc1(&d);
		// A REAL hardware adapter only: not flagged software, not WARP itself,
		// and not the Microsoft Basic Render Driver (vendor 0x1414), which a
		// CI runner exposes without the software flag and which shares with WARP.
		const bool real = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 && luid_of(a) != warp_luid &&
		                  d.VendorId != 0x1414;
		if (real) {
			hw = a;
		} else {
			a->Release();
		}
	}
	f->Release();
	if (hw == nullptr) {
		SKIP("no real hardware adapter to mismatch against (software / basic render only)");
	}
	Dev other;
	const bool ok = other.create(hw);
	hw->Release();
	if (!ok) {
		SKIP("no D3D11.4 device on the hardware adapter");
	}
	const std::vector<uint32_t> px(4 * 4, 0xFFFFFFFFu);
	Shared tex;
	REQUIRE(tex.create(other.dev, 4, 4, px));
	REQUIRE(d3d11_conv_host_lane_create(warp_luid, &ops));
	HANDLE dup = nullptr;
	REQUIRE(DuplicateHandle(GetCurrentProcess(), tex.handle, GetCurrentProcess(), &dup, 0, FALSE,
	                        DUPLICATE_SAME_ACCESS));
	xrt_conv_image desc{};
	desc.memory_kind = XRT_CONV_MEM_WIN32_NT_HANDLE;
	void *ctx = nullptr;
	CHECK_FALSE(ops.open_output(ops.ud, (uint64_t)(uintptr_t)dup, &desc, &ctx)); // WARN: wrong adapter
	ops.destroy(ops.ud);
}
