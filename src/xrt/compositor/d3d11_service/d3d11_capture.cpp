// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Windows.Graphics.Capture implementation (C++/WinRT).
 * @author David Fattal
 * @ingroup comp_d3d11_service
 *
 * Uses the Windows.Graphics.Capture API to capture a window's
 * content as a D3D11 texture. This file is intentionally isolated
 * from the rest of the compositor to contain C++/WinRT dependencies.
 */

// C++/WinRT requires these before any Windows headers
#include <unknwn.h>
#include <inspectable.h>

// C++/WinRT headers
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

// WinRT ↔ D3D11 interop
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <d3d11_4.h>
#include <dxgi.h>

#include <wil/com.h>

#include "util/u_logging.h"

#include "d3d11_capture.h"

#include <mutex>
#include <atomic>

namespace winrt_cap = winrt::Windows::Graphics::Capture;
namespace winrt_dx = winrt::Windows::Graphics::DirectX;
namespace winrt_d3d = winrt::Windows::Graphics::DirectX::Direct3D11;

struct d3d11_capture_context
{
	// WinRT capture objects
	winrt_cap::GraphicsCaptureItem item{nullptr};
	winrt_cap::Direct3D11CaptureFramePool frame_pool{nullptr};
	winrt_cap::GraphicsCaptureSession session{nullptr};
	winrt_cap::Direct3D11CaptureFramePool::FrameArrived_revoker frame_arrived_revoker;

	// D3D11 device (AddRef'd)
	wil::com_ptr<ID3D11Device> device;

	// Staging texture: owned by us, safe to read from render thread.
	// Created on first frame, recreated on size change.
	wil::com_ptr<ID3D11Texture2D> staging_texture;
	uint32_t width;
	uint32_t height;

	// Thread safety: protects staging_texture, width, height.
	// Locked by FrameArrived callback (WinRT thread pool) and
	// d3d11_capture_get_texture() (compositor render thread).
	std::mutex mutex;

	// Set to false by d3d11_capture_stop() to reject late callbacks.
	std::atomic<bool> running{true};

	HWND hwnd;
};


// Helper: get IDirect3DDevice (WinRT) from ID3D11Device (raw COM).
static winrt_d3d::IDirect3DDevice
get_winrt_device(ID3D11Device *device)
{
	wil::com_ptr<IDXGIDevice> dxgi_device;
	HRESULT hr = device->QueryInterface(IID_PPV_ARGS(dxgi_device.put()));
	if (FAILED(hr)) {
		U_LOG_E("Capture: failed to get IDXGIDevice (hr=0x%08lx)", hr);
		return nullptr;
	}

	winrt::com_ptr<::IInspectable> inspectable;
	hr = CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.get(), inspectable.put());
	if (FAILED(hr)) {
		U_LOG_E("Capture: CreateDirect3D11DeviceFromDXGIDevice failed (hr=0x%08lx)", hr);
		return nullptr;
	}

	return inspectable.as<winrt_d3d::IDirect3DDevice>();
}


// Helper: get ID3D11Texture2D from a WinRT IDirect3DSurface.
static wil::com_ptr<ID3D11Texture2D>
get_d3d11_texture(const winrt_d3d::IDirect3DSurface &surface)
{
	auto access = surface.as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
	wil::com_ptr<ID3D11Texture2D> texture;
	HRESULT hr = access->GetInterface(IID_PPV_ARGS(texture.put()));
	if (FAILED(hr)) {
		U_LOG_E("Capture: IDirect3DDxgiInterfaceAccess::GetInterface failed (hr=0x%08lx)", hr);
		return nullptr;
	}
	return texture;
}


// FrameArrived callback — runs on WinRT thread pool.
static void
on_frame_arrived(struct d3d11_capture_context *ctx,
                 const winrt_cap::Direct3D11CaptureFramePool &pool)
{
	if (!ctx->running.load()) {
		return;
	}

	winrt_cap::Direct3D11CaptureFrame frame = pool.TryGetNextFrame();
	if (!frame) {
		return;
	}

	// Get the captured texture
	auto surface = frame.Surface();
	auto captured_tex = get_d3d11_texture(surface);
	if (!captured_tex) {
		frame.Close();
		return;
	}

	// Check for size change
	auto content_size = frame.ContentSize();
	uint32_t new_w = static_cast<uint32_t>(content_size.Width);
	uint32_t new_h = static_cast<uint32_t>(content_size.Height);
	if (new_w == 0 || new_h == 0) {
		frame.Close();
		return;
	}

	{
		std::lock_guard<std::mutex> lock(ctx->mutex);

		// Recreate staging texture if size changed
		if (!ctx->staging_texture || ctx->width != new_w || ctx->height != new_h) {
			D3D11_TEXTURE2D_DESC desc = {};
			desc.Width = new_w;
			desc.Height = new_h;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

			wil::com_ptr<ID3D11Texture2D> new_staging;
			HRESULT hr = ctx->device->CreateTexture2D(&desc, nullptr, new_staging.put());
			if (FAILED(hr)) {
				U_LOG_E("Capture: CreateTexture2D staging failed (hr=0x%08lx)", hr);
				frame.Close();
				return;
			}

			ctx->staging_texture = std::move(new_staging);
			ctx->width = new_w;
			ctx->height = new_h;

			U_LOG_W("Capture: frame size %ux%u (HWND=%p)", new_w, new_h, (void *)ctx->hwnd);
		}

		// Copy captured frame to our staging texture.
		// The captured texture is only valid until the frame is closed,
		// so we must copy now. ID3D11Multithread is enabled on the device,
		// making this safe from the WinRT thread pool.
		wil::com_ptr<ID3D11DeviceContext> imm_ctx;
		ctx->device->GetImmediateContext(imm_ctx.put());
		imm_ctx->CopyResource(ctx->staging_texture.get(), captured_tex.get());
	}

	frame.Close();
}


// Recreate the frame pool when the captured window is resized.
// Must be called outside the mutex to avoid deadlock with FrameArrived.
static void
recreate_frame_pool(struct d3d11_capture_context *ctx, uint32_t w, uint32_t h)
{
	if (!ctx->frame_pool) return;
	ctx->frame_pool.Recreate(
	    get_winrt_device(ctx->device.get()),
	    winrt_dx::DirectXPixelFormat::B8G8R8A8UIntNormalized,
	    1,
	    {static_cast<int32_t>(w), static_cast<int32_t>(h)});
}


struct d3d11_capture_context *
d3d11_capture_start(struct ID3D11Device *device, HWND hwnd)
{
	if (!device || !hwnd || !IsWindow(hwnd)) {
		U_LOG_E("Capture: invalid device or HWND");
		return nullptr;
	}

	// Initialize WinRT (safe to call multiple times).
	winrt::init_apartment(winrt::apartment_type::multi_threaded);

	auto ctx = new (std::nothrow) d3d11_capture_context();
	if (!ctx) return nullptr;

	device->AddRef();
	ctx->device.attach(device);
	ctx->hwnd = hwnd;
	ctx->width = 0;
	ctx->height = 0;

	try {
		// Create WinRT device from D3D11 device
		auto winrt_device = get_winrt_device(device);
		if (!winrt_device) {
			delete ctx;
			return nullptr;
		}

		// Create capture item from HWND.
		// GraphicsCaptureItem::CreateFromWindowId requires Win10 2004+.
		auto interop_factory = winrt::get_activation_factory<
		    winrt_cap::GraphicsCaptureItem,
		    IGraphicsCaptureItemInterop>();

		HRESULT hr = interop_factory->CreateForWindow(
		    hwnd,
		    winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
		    winrt::put_abi(ctx->item));
		if (FAILED(hr) || !ctx->item) {
			U_LOG_E("Capture: CreateForWindow failed (hr=0x%08lx). "
			         "Window may be protected or elevated.", hr);
			delete ctx;
			return nullptr;
		}

		// Get initial window size
		auto size = ctx->item.Size();
		if (size.Width <= 0 || size.Height <= 0) {
			U_LOG_E("Capture: window has zero size");
			delete ctx;
			return nullptr;
		}

		// Create frame pool (free-threaded: callbacks on thread pool)
		ctx->frame_pool = winrt_cap::Direct3D11CaptureFramePool::CreateFreeThreaded(
		    winrt_device,
		    winrt_dx::DirectXPixelFormat::B8G8R8A8UIntNormalized,
		    1,  // buffer count — latest-frame model
		    size);

		// Register FrameArrived callback
		ctx->frame_arrived_revoker = ctx->frame_pool.FrameArrived(
		    winrt::auto_revoke,
		    [ctx](const winrt_cap::Direct3D11CaptureFramePool &pool,
		          const winrt::Windows::Foundation::IInspectable &) {
			    on_frame_arrived(ctx, pool);
		    });

		// Create and start session
		ctx->session = ctx->frame_pool.CreateCaptureSession(ctx->item);

		// Suppress yellow capture border on Win11+ (optional, ignore if unavailable)
		try {
			ctx->session.IsBorderRequired(false);
		} catch (...) {
			// IsBorderRequired not available on older Windows — ignore
		}

		ctx->session.StartCapture();

		U_LOG_W("Capture: started for HWND=%p (%dx%d)",
		         (void *)hwnd, size.Width, size.Height);

	} catch (const winrt::hresult_error &e) {
		U_LOG_E("Capture: WinRT error 0x%08lx: %ls",
		         static_cast<unsigned long>(e.code()),
		         e.message().c_str());
		delete ctx;
		return nullptr;

	} catch (...) {
		U_LOG_E("Capture: unexpected exception");
		delete ctx;
		return nullptr;
	}

	return ctx;
}


struct ID3D11Texture2D *
d3d11_capture_get_texture(struct d3d11_capture_context *ctx,
                          uint32_t *out_width,
                          uint32_t *out_height)
{
	if (!ctx) return nullptr;

	std::lock_guard<std::mutex> lock(ctx->mutex);

	if (!ctx->staging_texture) return nullptr;

	if (out_width) *out_width = ctx->width;
	if (out_height) *out_height = ctx->height;

	return ctx->staging_texture.get();
}


void
d3d11_capture_stop(struct d3d11_capture_context *ctx)
{
	if (!ctx) return;

	// Signal callbacks to stop
	ctx->running.store(false);

	// Revoke callback before closing session/pool
	ctx->frame_arrived_revoker.revoke();

	try {
		if (ctx->session) {
			ctx->session.Close();
			ctx->session = nullptr;
		}
		if (ctx->frame_pool) {
			ctx->frame_pool.Close();
			ctx->frame_pool = nullptr;
		}
		ctx->item = nullptr;
	} catch (...) {
		// Ignore errors during teardown
	}

	{
		std::lock_guard<std::mutex> lock(ctx->mutex);
		ctx->staging_texture = nullptr;
	}

	U_LOG_W("Capture: stopped for HWND=%p", (void *)ctx->hwnd);

	delete ctx;
}
