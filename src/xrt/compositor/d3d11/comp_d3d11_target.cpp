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
                         struct comp_d3d11_target **out_target)
{
	auto internals = get_internals(c);

	comp_d3d11_target *target = new comp_d3d11_target();
	target->c = c;
	target->hwnd = static_cast<HWND>(hwnd);
	target->width = width;
	target->height = height;
	target->current_index = 0;

	// Create swapchain
	DXGI_SWAP_CHAIN_DESC1 desc = {};
	desc.Width = width;
	desc.Height = height;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.BufferCount = 2;
	desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	desc.Flags = 0;

	DXGI_SWAP_CHAIN_FULLSCREEN_DESC fsDesc = {};
	fsDesc.Windowed = TRUE;

	HRESULT hr = internals->dxgi_factory->CreateSwapChainForHwnd(
	    internals->device, target->hwnd, &desc, &fsDesc, nullptr, &target->swapchain);
	if (FAILED(hr)) {
		U_LOG_E("Failed to create swapchain: 0x%08x", hr);
		delete target;
		return XRT_ERROR_D3D;
	}

	// Disable Alt-Enter fullscreen toggle
	internals->dxgi_factory->MakeWindowAssociation(target->hwnd, DXGI_MWA_NO_ALT_ENTER);

	// Create initial RTV
	xrt_result_t xret = create_rtv(target);
	if (xret != XRT_SUCCESS) {
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
