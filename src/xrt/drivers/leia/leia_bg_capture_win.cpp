// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Implementation of @ref leia_bg_capture_win.h
 * @ingroup drv_leia
 */

#include "leia_bg_capture_win.h"

#include "util/u_logging.h"

#include <atomic>

#include <windows.h>
#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_2.h>

#include <inspectable.h>
#include <roapi.h>
#include <windows.foundation.h>
#include <windows.graphics.h>
#include <windows.graphics.directx.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <windows.graphics.capture.h>
#include <windows.graphics.capture.interop.h>

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HStringReference;

namespace WGC = ABI::Windows::Graphics::Capture;
namespace WGDX = ABI::Windows::Graphics::DirectX;
namespace WGD3D = ABI::Windows::Graphics::DirectX::Direct3D11;
namespace WG = ABI::Windows::Graphics;
namespace WF = ABI::Windows::Foundation;

// IDirect3DDxgiInterfaceAccess is a Win32 COM interface (not ABI), declared in
// the global Windows::Graphics::DirectX::Direct3D11 namespace by the SDK
// interop header.
using IDxgiAccess = Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess;

struct leia_bg_capture
{
	HWND hwnd;
	HMONITOR monitor;
	UINT monitor_w;
	UINT monitor_h;
	RECT monitor_rect; // virtual-screen coords

	// Producer-side D3D11 (internal — separate from any DP).
	ComPtr<ID3D11Device> d3d11_device;
	ComPtr<ID3D11DeviceContext> d3d11_context;
	ComPtr<ID3D11DeviceContext4> d3d11_context4; //!< For Signal().

	// WGC plumbing.
	ComPtr<WGD3D::IDirect3DDevice> wg_device;
	ComPtr<WGC::IGraphicsCaptureItem> capture_item;
	ComPtr<WGC::IDirect3D11CaptureFramePool> frame_pool;
	ComPtr<WGC::IGraphicsCaptureSession> capture_session;

	// Shared staging texture (monitor-sized, BGRA8, SHARED_NTHANDLE).
	ComPtr<ID3D11Texture2D> staging_tex;
	HANDLE staging_shared_handle;

	// Cross-API GPU sync — D3D11 shared fence. Producer signals after each
	// CopyResource; consumers (D3D11 or D3D12) Wait before sampling.
	ComPtr<ID3D11Fence> shared_fence;
	HANDLE shared_fence_handle;
	std::atomic<UINT64> signaled_value;

	std::atomic<bool> has_frame;
};

// ---------- helpers --------------------------------------------------------

static bool
env_disable()
{
	char buf[8];
	DWORD n = GetEnvironmentVariableA("LEIA_DP_DISABLE_BG_CAPTURE", buf, sizeof(buf));
	return n > 0 && n < sizeof(buf) && (buf[0] == '1' || buf[0] == 't' || buf[0] == 'T');
}

static bool
os_supports_wda_exclude_from_capture()
{
	// WDA_EXCLUDEFROMCAPTURE requires Windows 10 2004 (build 19041).
	HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	if (ntdll == nullptr) {
		return false;
	}
	typedef LONG(WINAPI * RtlGetVersion_t)(OSVERSIONINFOEXW *);
	auto fn = (RtlGetVersion_t)GetProcAddress(ntdll, "RtlGetVersion");
	if (fn == nullptr) {
		return false;
	}
	OSVERSIONINFOEXW vi = {};
	vi.dwOSVersionInfoSize = sizeof(vi);
	if (fn(&vi) != 0) {
		return false;
	}
	return vi.dwBuildNumber >= 19041;
}

static HRESULT
create_internal_d3d11(ID3D11Device **out_dev, ID3D11DeviceContext **out_ctx)
{
	UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
	D3D_FEATURE_LEVEL fl;
	const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
	return D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2,
	                         D3D11_SDK_VERSION, out_dev, &fl, out_ctx);
}

static HRESULT
wrap_d3d11_as_winrt(ID3D11Device *d3d11, WGD3D::IDirect3DDevice **out)
{
	ComPtr<IDXGIDevice> dxgi_device;
	HRESULT hr = d3d11->QueryInterface(IID_PPV_ARGS(&dxgi_device));
	if (FAILED(hr)) {
		return hr;
	}
	ComPtr<IInspectable> inspectable;
	hr = CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.Get(), &inspectable);
	if (FAILED(hr)) {
		return hr;
	}
	return inspectable->QueryInterface(__uuidof(WGD3D::IDirect3DDevice), reinterpret_cast<void **>(out));
}

static HRESULT
create_staging_texture(ID3D11Device *dev, UINT w, UINT h, ID3D11Texture2D **out_tex, HANDLE *out_handle)
{
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = w;
	desc.Height = h;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	// SHARED_NTHANDLE for cross-process / cross-API import; SHARED is implied.
	// Sync is via a separate shared fence (no keyed mutex — keyed mutex is
	// awkward to access from D3D12).
	desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
	HRESULT hr = dev->CreateTexture2D(&desc, nullptr, out_tex);
	if (FAILED(hr)) {
		return hr;
	}
	ComPtr<IDXGIResource1> resource1;
	hr = (*out_tex)->QueryInterface(IID_PPV_ARGS(&resource1));
	if (FAILED(hr)) {
		return hr;
	}
	return resource1->CreateSharedHandle(nullptr,
	                                     DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
	                                     nullptr,
	                                     out_handle);
}

static HRESULT
create_shared_fence(ID3D11Device *dev, ID3D11Fence **out_fence, HANDLE *out_handle)
{
	ComPtr<ID3D11Device5> dev5;
	HRESULT hr = dev->QueryInterface(IID_PPV_ARGS(&dev5));
	if (FAILED(hr)) {
		return hr;
	}
	hr = dev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(out_fence));
	if (FAILED(hr)) {
		return hr;
	}
	return (*out_fence)->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, out_handle);
}

// ---------- public API -----------------------------------------------------

extern "C" struct leia_bg_capture *
leia_bg_capture_create(HWND hwnd)
{
	if (env_disable()) {
		U_LOG_W("leia_bg_capture: disabled via LEIA_DP_DISABLE_BG_CAPTURE — falling back to chroma-key");
		return nullptr;
	}
	if (hwnd == nullptr) {
		U_LOG_W("leia_bg_capture: NULL hwnd — falling back to chroma-key");
		return nullptr;
	}
	if (!os_supports_wda_exclude_from_capture()) {
		U_LOG_W("leia_bg_capture: OS < Win10 2004 lacks WDA_EXCLUDEFROMCAPTURE — falling back to chroma-key");
		return nullptr;
	}
	if (!SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)) {
		U_LOG_W("leia_bg_capture: SetWindowDisplayAffinity failed (err=%lu) — falling back to chroma-key",
		        GetLastError());
		return nullptr;
	}

	HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
	MONITORINFO mi = {};
	mi.cbSize = sizeof(mi);
	if (monitor == nullptr || !GetMonitorInfoW(monitor, &mi)) {
		U_LOG_W("leia_bg_capture: monitor info lookup failed");
		SetWindowDisplayAffinity(hwnd, WDA_NONE);
		return nullptr;
	}
	UINT monitor_w = mi.rcMonitor.right - mi.rcMonitor.left;
	UINT monitor_h = mi.rcMonitor.bottom - mi.rcMonitor.top;

	HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
	if (FAILED(hr) && hr != RPC_E_CHANGED_MODE && hr != S_FALSE) {
		U_LOG_W("leia_bg_capture: RoInitialize failed: 0x%08x", (unsigned)hr);
		SetWindowDisplayAffinity(hwnd, WDA_NONE);
		return nullptr;
	}

	ComPtr<ID3D11Device> d3d11_device;
	ComPtr<ID3D11DeviceContext> d3d11_context;
	hr = create_internal_d3d11(d3d11_device.GetAddressOf(), d3d11_context.GetAddressOf());
	if (FAILED(hr)) {
		U_LOG_W("leia_bg_capture: D3D11CreateDevice failed: 0x%08x", (unsigned)hr);
		SetWindowDisplayAffinity(hwnd, WDA_NONE);
		return nullptr;
	}

	ComPtr<WGD3D::IDirect3DDevice> wg_device;
	hr = wrap_d3d11_as_winrt(d3d11_device.Get(), wg_device.GetAddressOf());
	if (FAILED(hr)) {
		U_LOG_W("leia_bg_capture: WinRT D3D11 device wrap failed: 0x%08x", (unsigned)hr);
		SetWindowDisplayAffinity(hwnd, WDA_NONE);
		return nullptr;
	}

	ComPtr<IGraphicsCaptureItemInterop> interop;
	{
		HStringReference cls(RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureItem);
		ComPtr<IActivationFactory> factory;
		hr = RoGetActivationFactory(cls.Get(), IID_PPV_ARGS(&factory));
		if (FAILED(hr)) {
			U_LOG_W("leia_bg_capture: WGC item activation factory unavailable: 0x%08x — falling back to chroma-key",
			        (unsigned)hr);
			SetWindowDisplayAffinity(hwnd, WDA_NONE);
			return nullptr;
		}
		hr = factory.As(&interop);
		if (FAILED(hr)) {
			U_LOG_W("leia_bg_capture: IGraphicsCaptureItemInterop QI failed: 0x%08x", (unsigned)hr);
			SetWindowDisplayAffinity(hwnd, WDA_NONE);
			return nullptr;
		}
	}

	ComPtr<WGC::IGraphicsCaptureItem> capture_item;
	hr = interop->CreateForMonitor(monitor, IID_PPV_ARGS(capture_item.GetAddressOf()));
	if (FAILED(hr)) {
		U_LOG_W("leia_bg_capture: CreateForMonitor failed: 0x%08x", (unsigned)hr);
		SetWindowDisplayAffinity(hwnd, WDA_NONE);
		return nullptr;
	}

	WG::SizeInt32 item_size = {};
	capture_item->get_Size(&item_size);

	ComPtr<WGC::IDirect3D11CaptureFramePoolStatics2> pool_statics;
	{
		HStringReference cls(RuntimeClass_Windows_Graphics_Capture_Direct3D11CaptureFramePool);
		hr = RoGetActivationFactory(cls.Get(), IID_PPV_ARGS(&pool_statics));
		if (FAILED(hr)) {
			U_LOG_W("leia_bg_capture: FramePool statics unavailable: 0x%08x", (unsigned)hr);
			SetWindowDisplayAffinity(hwnd, WDA_NONE);
			return nullptr;
		}
	}

	ComPtr<WGC::IDirect3D11CaptureFramePool> frame_pool;
	hr = pool_statics->CreateFreeThreaded(wg_device.Get(),
	                                      WGDX::DirectXPixelFormat_B8G8R8A8UIntNormalized,
	                                      2,
	                                      item_size,
	                                      frame_pool.GetAddressOf());
	if (FAILED(hr)) {
		U_LOG_W("leia_bg_capture: FramePool::CreateFreeThreaded failed: 0x%08x", (unsigned)hr);
		SetWindowDisplayAffinity(hwnd, WDA_NONE);
		return nullptr;
	}

	ComPtr<WGC::IGraphicsCaptureSession> capture_session;
	hr = frame_pool->CreateCaptureSession(capture_item.Get(), capture_session.GetAddressOf());
	if (FAILED(hr)) {
		U_LOG_W("leia_bg_capture: CreateCaptureSession failed: 0x%08x", (unsigned)hr);
		SetWindowDisplayAffinity(hwnd, WDA_NONE);
		return nullptr;
	}

	// Don't capture the cursor — we draw our own UI; the desktop cursor under us
	// would be doubled.
	ComPtr<WGC::IGraphicsCaptureSession2> session2;
	if (SUCCEEDED(capture_session.As(&session2))) {
		session2->put_IsCursorCaptureEnabled(false);
	}
	// Suppress the yellow "this screen is being captured" border DWM draws
	// around the captured region. Requires Windows 11 22H2+; on older Windows
	// the QI fails or the put silently no-ops — fine, we just keep the border.
	ComPtr<WGC::IGraphicsCaptureSession3> session3;
	if (SUCCEEDED(capture_session.As(&session3))) {
		session3->put_IsBorderRequired(false);
	}

	ComPtr<ID3D11Texture2D> staging_tex;
	HANDLE staging_handle = nullptr;
	hr = create_staging_texture(d3d11_device.Get(), monitor_w, monitor_h,
	                            staging_tex.GetAddressOf(), &staging_handle);
	if (FAILED(hr)) {
		U_LOG_W("leia_bg_capture: staging tex create failed: 0x%08x", (unsigned)hr);
		SetWindowDisplayAffinity(hwnd, WDA_NONE);
		return nullptr;
	}

	ComPtr<ID3D11Fence> shared_fence;
	HANDLE shared_fence_handle = nullptr;
	hr = create_shared_fence(d3d11_device.Get(), shared_fence.GetAddressOf(), &shared_fence_handle);
	if (FAILED(hr)) {
		U_LOG_W("leia_bg_capture: shared fence create failed: 0x%08x", (unsigned)hr);
		CloseHandle(staging_handle);
		SetWindowDisplayAffinity(hwnd, WDA_NONE);
		return nullptr;
	}

	ComPtr<ID3D11DeviceContext4> ctx4;
	hr = d3d11_context.As(&ctx4);
	if (FAILED(hr)) {
		U_LOG_W("leia_bg_capture: ID3D11DeviceContext4 QI failed: 0x%08x", (unsigned)hr);
		CloseHandle(shared_fence_handle);
		CloseHandle(staging_handle);
		SetWindowDisplayAffinity(hwnd, WDA_NONE);
		return nullptr;
	}

	hr = capture_session->StartCapture();
	if (FAILED(hr)) {
		U_LOG_W("leia_bg_capture: StartCapture failed: 0x%08x", (unsigned)hr);
		CloseHandle(shared_fence_handle);
		CloseHandle(staging_handle);
		SetWindowDisplayAffinity(hwnd, WDA_NONE);
		return nullptr;
	}

	auto *c = new leia_bg_capture();
	c->hwnd = hwnd;
	c->monitor = monitor;
	c->monitor_w = monitor_w;
	c->monitor_h = monitor_h;
	c->monitor_rect = mi.rcMonitor;
	c->d3d11_device = d3d11_device;
	c->d3d11_context = d3d11_context;
	c->d3d11_context4 = ctx4;
	c->wg_device = wg_device;
	c->capture_item = capture_item;
	c->frame_pool = frame_pool;
	c->capture_session = capture_session;
	c->staging_tex = staging_tex;
	c->staging_shared_handle = staging_handle;
	c->shared_fence = shared_fence;
	c->shared_fence_handle = shared_fence_handle;
	c->signaled_value = 0;
	c->has_frame = false;

	U_LOG_W("leia_bg_capture: ready (monitor=%ux%u, hwnd=0x%p)", monitor_w, monitor_h, hwnd);
	return c;
}

extern "C" long
leia_bg_capture_open_d3d11(struct leia_bg_capture *c,
                           ID3D11Device *dev,
                           ID3D11Texture2D **out_tex,
                           ID3D11ShaderResourceView **out_srv)
{
	if (c == nullptr || dev == nullptr || out_tex == nullptr || out_srv == nullptr) {
		return E_INVALIDARG;
	}
	ComPtr<ID3D11Device1> dev1;
	HRESULT hr = dev->QueryInterface(IID_PPV_ARGS(&dev1));
	if (FAILED(hr)) {
		return hr;
	}
	hr = dev1->OpenSharedResource1(c->staging_shared_handle, IID_PPV_ARGS(out_tex));
	if (FAILED(hr)) {
		return hr;
	}
	D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
	sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	sd.Texture2D.MipLevels = 1;
	return dev->CreateShaderResourceView(*out_tex, &sd, out_srv);
}

extern "C" long
leia_bg_capture_open_d3d12(struct leia_bg_capture *c, ID3D12Device *dev, ID3D12Resource **out_res)
{
	if (c == nullptr || dev == nullptr || out_res == nullptr) {
		return E_INVALIDARG;
	}
	return dev->OpenSharedHandle(c->staging_shared_handle, IID_PPV_ARGS(out_res));
}

extern "C" long
leia_bg_capture_open_fence_d3d11(struct leia_bg_capture *c, ID3D11Device *dev, ID3D11Fence **out_fence)
{
	if (c == nullptr || dev == nullptr || out_fence == nullptr) {
		return E_INVALIDARG;
	}
	ComPtr<ID3D11Device5> dev5;
	HRESULT hr = dev->QueryInterface(IID_PPV_ARGS(&dev5));
	if (FAILED(hr)) {
		return hr;
	}
	return dev5->OpenSharedFence(c->shared_fence_handle, IID_PPV_ARGS(out_fence));
}

extern "C" long
leia_bg_capture_open_fence_d3d12(struct leia_bg_capture *c, ID3D12Device *dev, ID3D12Fence **out_fence)
{
	if (c == nullptr || dev == nullptr || out_fence == nullptr) {
		return E_INVALIDARG;
	}
	return dev->OpenSharedHandle(c->shared_fence_handle, IID_PPV_ARGS(out_fence));
}

extern "C" HANDLE
leia_bg_capture_get_shared_handle(struct leia_bg_capture *c)
{
	return c != nullptr ? c->staging_shared_handle : nullptr;
}

extern "C" void
leia_bg_capture_get_size(struct leia_bg_capture *c, uint32_t *out_w, uint32_t *out_h)
{
	if (c == nullptr) {
		if (out_w) *out_w = 0;
		if (out_h) *out_h = 0;
		return;
	}
	if (out_w) *out_w = c->monitor_w;
	if (out_h) *out_h = c->monitor_h;
}

extern "C" bool
leia_bg_capture_poll(struct leia_bg_capture *c,
                     float out_bg_uv_origin[2],
                     float out_bg_uv_extent[2],
                     uint64_t *out_fence_wait_value)
{
	if (c == nullptr) {
		return false;
	}

	// Drain framepool, keep newest.
	ComPtr<WGC::IDirect3D11CaptureFrame> latest_frame;
	for (;;) {
		ComPtr<WGC::IDirect3D11CaptureFrame> f;
		HRESULT hr = c->frame_pool->TryGetNextFrame(f.GetAddressOf());
		if (FAILED(hr) || f == nullptr) {
			break;
		}
		latest_frame = std::move(f);
	}
	if (latest_frame != nullptr) {
		ComPtr<WGD3D::IDirect3DSurface> wg_surface;
		if (SUCCEEDED(latest_frame->get_Surface(wg_surface.GetAddressOf()))) {
			ComPtr<IDxgiAccess> access;
			if (SUCCEEDED(wg_surface.As(&access))) {
				ComPtr<ID3D11Texture2D> wgc_tex;
				if (SUCCEEDED(access->GetInterface(IID_PPV_ARGS(&wgc_tex)))) {
					c->d3d11_context->CopyResource(c->staging_tex.Get(), wgc_tex.Get());
					// Signal after the copy queues — consumers wait on this value
					// before sampling the staging tex.
					UINT64 next = c->signaled_value.fetch_add(1, std::memory_order_acq_rel) + 1;
					c->d3d11_context4->Signal(c->shared_fence.Get(), next);
					// Flush the device context so the signal makes it to the GPU
					// queue before the consumer (on a different device) waits.
					c->d3d11_context->Flush();
					c->has_frame.store(true, std::memory_order_release);
				}
			}
		}
		ComPtr<WF::IClosable> closable;
		if (SUCCEEDED(latest_frame.As(&closable))) {
			closable->Close();
		}
	}

	if (out_fence_wait_value != nullptr) {
		*out_fence_wait_value = c->signaled_value.load(std::memory_order_acquire);
	}

	// Window-on-monitor rect → normalized UVs.
	HMONITOR cur = MonitorFromWindow(c->hwnd, MONITOR_DEFAULTTONEAREST);
	if (cur != c->monitor) {
		// Cross-monitor move: capture session is still bound to the old monitor.
		// Recreating the session mid-stream is non-trivial; for now skip compose
		// (caller can fall through to opaque-only weave).
		// TODO follow-up: tear-down + re-create on monitor change.
		out_bg_uv_origin[0] = 0;
		out_bg_uv_origin[1] = 0;
		out_bg_uv_extent[0] = 0;
		out_bg_uv_extent[1] = 0;
		return false;
	}
	// Compositor's swap chain renders into the window's CLIENT area only —
	// the title bar and borders are non-client and drawn by DWM. Mapping the
	// bg sample to GetWindowRect would shift / scale by the title-bar height.
	// Use GetClientRect + ClientToScreen so the UV rect covers exactly the
	// pixels the atlas's tile content maps onto. Pixel-perfect in both
	// titled-window and borderless cases.
	RECT cr;
	if (!GetClientRect(c->hwnd, &cr)) {
		return false;
	}
	POINT tl = {cr.left, cr.top};
	POINT br = {cr.right, cr.bottom};
	if (!ClientToScreen(c->hwnd, &tl) || !ClientToScreen(c->hwnd, &br)) {
		return false;
	}
	const float inv_w = 1.0f / (float)c->monitor_w;
	const float inv_h = 1.0f / (float)c->monitor_h;
	out_bg_uv_origin[0] = (float)(tl.x - c->monitor_rect.left) * inv_w;
	out_bg_uv_origin[1] = (float)(tl.y - c->monitor_rect.top) * inv_h;
	out_bg_uv_extent[0] = (float)(br.x - tl.x) * inv_w;
	out_bg_uv_extent[1] = (float)(br.y - tl.y) * inv_h;

	return c->has_frame.load(std::memory_order_acquire);
}

extern "C" void
leia_bg_capture_destroy(struct leia_bg_capture *c)
{
	if (c == nullptr) {
		return;
	}
	{
		ComPtr<WF::IClosable> closable;
		if (c->frame_pool != nullptr && SUCCEEDED(c->frame_pool.As(&closable))) {
			closable->Close();
		}
	}
	{
		ComPtr<WF::IClosable> closable;
		if (c->capture_session != nullptr && SUCCEEDED(c->capture_session.As(&closable))) {
			closable->Close();
		}
	}
	if (c->staging_shared_handle != nullptr) {
		CloseHandle(c->staging_shared_handle);
	}
	if (c->shared_fence_handle != nullptr) {
		CloseHandle(c->shared_fence_handle);
	}
	if (c->hwnd != nullptr) {
		SetWindowDisplayAffinity(c->hwnd, WDA_NONE);
	}
	delete c;
}
