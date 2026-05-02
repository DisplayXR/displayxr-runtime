// Copyright 2024-2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  D3D11 DXGI swapchain target implementation.
 * @author David Fattal
 * @ingroup comp_d3d11
 */

#include "comp_d3d11_target.h"
#include "comp_d3d11_compositor.h"

#include "util/u_logging.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <dcomp.h>

/*!
 * D3D11 target structure.
 */
struct comp_d3d11_target
{
	//! Parent compositor.
	struct comp_d3d11_compositor *c;

	//! DXGI swapchain.
	IDXGISwapChain1 *swapchain;

	//! Current render target view (for current back buffer).
	ID3D11RenderTargetView *rtv;

	//! Current back buffer texture.
	ID3D11Texture2D *back_buffer;

	//! Window handle.
	HWND hwnd;

	//! Current dimensions.
	uint32_t width;
	uint32_t height;

	//! Current back buffer index.
	uint32_t current_index;

	//! DirectComposition resources (transparent path only — null on default path).
	//! On the transparent path the swapchain is created via
	//! CreateSwapChainForComposition (HWND-less) and bound to the app's HWND
	//! through DComp instead of via DXGI, so DWM can blend per-pixel alpha.
	IDCompositionDevice *dcomp_device;
	IDCompositionTarget *dcomp_target;
	IDCompositionVisual *dcomp_visual;
};

// Access compositor internals
extern "C" {
struct comp_d3d11_compositor_internals
{
	struct xrt_compositor_native base;
	struct xrt_device *xdev;
	ID3D11Device *device;
	ID3D11DeviceContext *context;
	IDXGIFactory4 *dxgi_factory;
};
}

static inline struct comp_d3d11_compositor_internals *
get_internals(struct comp_d3d11_compositor *c)
{
	return reinterpret_cast<struct comp_d3d11_compositor_internals *>(c);
}

static xrt_result_t
create_rtv(struct comp_d3d11_target *target)
{
	auto internals = get_internals(target->c);

	// Release existing RTV and back buffer
	if (target->rtv != nullptr) {
		target->rtv->Release();
		target->rtv = nullptr;
	}
	if (target->back_buffer != nullptr) {
		target->back_buffer->Release();
		target->back_buffer = nullptr;
	}

	// Get back buffer
	HRESULT hr = target->swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D),
	                                           reinterpret_cast<void **>(&target->back_buffer));
	if (FAILED(hr)) {
		U_LOG_E("Failed to get back buffer: 0x%08x", hr);
		return XRT_ERROR_D3D;
	}

	// Create RTV
	hr = internals->device->CreateRenderTargetView(target->back_buffer, nullptr, &target->rtv);
	if (FAILED(hr)) {
		U_LOG_E("Failed to create RTV: 0x%08x", hr);
		return XRT_ERROR_D3D;
	}

	return XRT_SUCCESS;
}

extern "C" xrt_result_t
comp_d3d11_target_create(struct comp_d3d11_compositor *c,
                         void *hwnd,
                         uint32_t width,
                         uint32_t height,
                         bool transparent,
                         struct comp_d3d11_target **out_target)
{
	auto internals = get_internals(c);

	comp_d3d11_target *target = new comp_d3d11_target();
	target->c = c;
	target->hwnd = static_cast<HWND>(hwnd);
	target->width = width;
	target->height = height;
	target->current_index = 0;
	target->dcomp_device = nullptr;
	target->dcomp_target = nullptr;
	target->dcomp_visual = nullptr;

	// Create swapchain.
	//
	// Default: flip-model + ALPHA_MODE_IGNORE (#163) — opaque present, no DWM bleed-through.
	// Transparent opt-in (only when an HWND was provided): flip-model +
	// ALPHA_MODE_PREMULTIPLIED via CreateSwapChainForComposition, bound to the app's
	// HWND through DirectComposition (IDCompositionTarget::SetRoot(visual)).
	// DComp gives us per-pixel alpha — no chroma-key, no disocclusion fringes, no
	// LWA_COLORKEY on the plugin side.
	const bool use_transparent = transparent && hwnd != nullptr;

	DXGI_SWAP_CHAIN_DESC1 desc = {};
	desc.Width = width;
	desc.Height = height;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.BufferCount = 2;
	desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	if (use_transparent) {
		desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
	} else {
		// IGNORE so DWM doesn't composite the desktop through the bound HWND (#163).
		desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
	}
	desc.Flags = 0;

	HRESULT hr;
	if (use_transparent) {
		hr = internals->dxgi_factory->CreateSwapChainForComposition(
		    internals->device, &desc, nullptr, &target->swapchain);
		U_LOG_W("Transparent HWND opt-in: DComp + flip-model swapchain "
		        "(FLIP_DISCARD + PREMULTIPLIED, bc=2)");
	} else {
		DXGI_SWAP_CHAIN_FULLSCREEN_DESC fsDesc = {};
		fsDesc.Windowed = TRUE;
		hr = internals->dxgi_factory->CreateSwapChainForHwnd(
		    internals->device, target->hwnd, &desc, &fsDesc, nullptr, &target->swapchain);
	}
	if (FAILED(hr)) {
		U_LOG_E("Failed to create swapchain: 0x%08x", hr);
		delete target;
		return XRT_ERROR_D3D;
	}

	// Disable Alt-Enter fullscreen toggle (HWND-bound only — composition swapchains
	// have no HWND association).
	if (!use_transparent) {
		internals->dxgi_factory->MakeWindowAssociation(target->hwnd, DXGI_MWA_NO_ALT_ENTER);
	}

	// Transparent path: bind the composition swapchain to the HWND through DComp.
	if (use_transparent) {
		hr = DCompositionCreateDevice2(
		    /*renderingDevice*/ nullptr,
		    __uuidof(IDCompositionDevice),
		    reinterpret_cast<void **>(&target->dcomp_device));
		if (FAILED(hr) || target->dcomp_device == nullptr) {
			U_LOG_E("DCompositionCreateDevice2 failed: 0x%08x", hr);
			target->swapchain->Release();
			delete target;
			return XRT_ERROR_D3D;
		}

		hr = target->dcomp_device->CreateTargetForHwnd(
		    target->hwnd, /*topmost*/ TRUE, &target->dcomp_target);
		if (FAILED(hr) || target->dcomp_target == nullptr) {
			U_LOG_E("IDCompositionDevice::CreateTargetForHwnd failed: 0x%08x", hr);
			target->dcomp_device->Release();
			target->swapchain->Release();
			delete target;
			return XRT_ERROR_D3D;
		}

		hr = target->dcomp_device->CreateVisual(&target->dcomp_visual);
		if (FAILED(hr) || target->dcomp_visual == nullptr) {
			U_LOG_E("IDCompositionDevice::CreateVisual failed: 0x%08x", hr);
			target->dcomp_target->Release();
			target->dcomp_device->Release();
			target->swapchain->Release();
			delete target;
			return XRT_ERROR_D3D;
		}

		hr = target->dcomp_visual->SetContent(target->swapchain);
		if (SUCCEEDED(hr)) {
			hr = target->dcomp_target->SetRoot(target->dcomp_visual);
		}
		if (SUCCEEDED(hr)) {
			hr = target->dcomp_device->Commit();
		}
		if (FAILED(hr)) {
			U_LOG_E("DComp visual setup failed: 0x%08x", hr);
			target->dcomp_visual->Release();
			target->dcomp_target->Release();
			target->dcomp_device->Release();
			target->swapchain->Release();
			delete target;
			return XRT_ERROR_D3D;
		}
	}

	// Create initial RTV
	xrt_result_t xret = create_rtv(target);
	if (xret != XRT_SUCCESS) {
		if (target->dcomp_visual != nullptr) target->dcomp_visual->Release();
		if (target->dcomp_target != nullptr) target->dcomp_target->Release();
		if (target->dcomp_device != nullptr) target->dcomp_device->Release();
		target->swapchain->Release();
		delete target;
		return xret;
	}

	*out_target = target;

	U_LOG_I("Created D3D11 target: %ux%u", width, height);

	return XRT_SUCCESS;
}

extern "C" void
comp_d3d11_target_destroy(struct comp_d3d11_target **target_ptr)
{
	if (target_ptr == nullptr || *target_ptr == nullptr) {
		return;
	}

	comp_d3d11_target *target = *target_ptr;

	if (target->rtv != nullptr) {
		target->rtv->Release();
	}
	if (target->back_buffer != nullptr) {
		target->back_buffer->Release();
	}

	// Release DComp resources before the swapchain (visual holds a swapchain reference;
	// target holds the visual). DWM tears down the on-screen content when target releases.
	if (target->dcomp_visual != nullptr) {
		target->dcomp_visual->Release();
	}
	if (target->dcomp_target != nullptr) {
		target->dcomp_target->Release();
	}
	if (target->dcomp_device != nullptr) {
		target->dcomp_device->Release();
	}

	if (target->swapchain != nullptr) {
		target->swapchain->Release();
	}

	delete target;
	*target_ptr = nullptr;
}

extern "C" xrt_result_t
comp_d3d11_target_acquire(struct comp_d3d11_target *target, uint32_t *out_index)
{
	auto internals = get_internals(target->c);

	// For FLIP_DISCARD swapchain, we always render to buffer 0
	// The swapchain handles double-buffering internally
	*out_index = 0;
	target->current_index = 0;

	// Bind the render target
	internals->context->OMSetRenderTargets(1, &target->rtv, nullptr);

	// Set viewport
	D3D11_VIEWPORT viewport = {};
	viewport.TopLeftX = 0.0f;
	viewport.TopLeftY = 0.0f;
	viewport.Width = static_cast<float>(target->width);
	viewport.Height = static_cast<float>(target->height);
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	internals->context->RSSetViewports(1, &viewport);

	// Clear to black
	float clear_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
	internals->context->ClearRenderTargetView(target->rtv, clear_color);

	return XRT_SUCCESS;
}

extern "C" void
comp_d3d11_target_bind(struct comp_d3d11_target *target)
{
	auto internals = get_internals(target->c);

	// Re-bind the render target and viewport (no clear)
	internals->context->OMSetRenderTargets(1, &target->rtv, nullptr);

	D3D11_VIEWPORT viewport = {};
	viewport.TopLeftX = 0.0f;
	viewport.TopLeftY = 0.0f;
	viewport.Width = static_cast<float>(target->width);
	viewport.Height = static_cast<float>(target->height);
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	internals->context->RSSetViewports(1, &viewport);
}

extern "C" xrt_result_t
comp_d3d11_target_present(struct comp_d3d11_target *target, uint32_t sync_interval)
{
	HRESULT hr = target->swapchain->Present(sync_interval, 0);
	if (FAILED(hr)) {
		U_LOG_E("Present failed: 0x%08x", hr);

		// Check for device removed
		if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
			return XRT_ERROR_D3D;
		}
		return XRT_ERROR_D3D;
	}

	// DComp path: publish the new frame to dwm.exe. Cheap — IPC of delta state, no GPU work.
	if (target->dcomp_device != nullptr) {
		target->dcomp_device->Commit();
	}

	return XRT_SUCCESS;
}

extern "C" void
comp_d3d11_target_get_dimensions(struct comp_d3d11_target *target,
                                 uint32_t *out_width,
                                 uint32_t *out_height)
{
	*out_width = target->width;
	*out_height = target->height;
}

extern "C" void *
comp_d3d11_target_get_back_buffer(struct comp_d3d11_target *target)
{
	if (target == nullptr) {
		return nullptr;
	}
	return target->back_buffer;
}

extern "C" xrt_result_t
comp_d3d11_target_resize(struct comp_d3d11_target *target,
                         uint32_t width,
                         uint32_t height)
{
	if (width == target->width && height == target->height) {
		return XRT_SUCCESS;
	}

	auto internals = get_internals(target->c);

	// Release current back buffer and RTV
	internals->context->OMSetRenderTargets(0, nullptr, nullptr);
	if (target->rtv != nullptr) {
		target->rtv->Release();
		target->rtv = nullptr;
	}
	if (target->back_buffer != nullptr) {
		target->back_buffer->Release();
		target->back_buffer = nullptr;
	}

	// Resize swapchain
	HRESULT hr = target->swapchain->ResizeBuffers(0, width, height,
	                                               DXGI_FORMAT_UNKNOWN,
	                                               0);
	if (FAILED(hr)) {
		U_LOG_E("Failed to resize swapchain: 0x%08x", hr);
		return XRT_ERROR_D3D;
	}

	target->width = width;
	target->height = height;

	// Recreate RTV
	return create_rtv(target);
}
