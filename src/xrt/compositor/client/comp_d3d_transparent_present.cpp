// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Render-API-agnostic transparent DirectComposition present for IPC clients.
 * @ingroup comp_client
 */

#include "comp_d3d_transparent_present.h"

#include "d3d/d3d_d3d11_helpers.hpp"

#include "util/u_logging.h"

#include <d3d11_4.h> // ID3D11Fence / ID3D11DeviceContext4
#include <dxgi1_2.h> // CreateSwapChainForComposition
#include <dcomp.h>   // DirectComposition

#include <wil/com.h>
#include <wil/result_macros.h>

#include <new>

using wil::com_ptr;

struct comp_d3d_transparent_presenter
{
	//! Device + context used for the fence wait and the shared→back-buffer copy. Either the
	//! caller's device (D3D11 client) or one we created (D3D12/GL/VK clients).
	com_ptr<ID3D11Device5> device;
	com_ptr<ID3D11DeviceContext4> context;
	//! Keeps our self-created device's base interfaces alive (NULL when the caller's device is reused).
	com_ptr<ID3D11Device> owned_device;
	com_ptr<ID3D11DeviceContext> owned_context;

	com_ptr<ID3D11Texture2D> output_texture; //!< imported shared service output
	com_ptr<ID3D11Fence> output_fence;       //!< imported service→client fence
	uint64_t present_value = 0;              //!< client wait counter (lockstep with the service)

	com_ptr<IDXGISwapChain1> swapchain; //!< DComp composition swap chain
	com_ptr<IDCompositionDevice> dcomp_device;
	com_ptr<IDCompositionTarget> dcomp_target;
	com_ptr<IDCompositionVisual> dcomp_visual;

	uint32_t width = 0, height = 0;
};

extern "C" struct comp_d3d_transparent_presenter *
comp_d3d_transparent_presenter_create(void *existing_d3d11_device,
                                      uint64_t hwnd_val,
                                      uint32_t width,
                                      uint32_t height,
                                      xrt_graphics_buffer_handle_t shared_tex,
                                      xrt_graphics_sync_handle_t shared_fence)
{
	// Consume the handles unconditionally: on every early-out below we still close them.
	HANDLE tex_h = (HANDLE)shared_tex;
	HANDLE fence_h = (HANDLE)shared_fence;

	auto fail = [&]() -> comp_d3d_transparent_presenter * {
		if (tex_h != NULL && tex_h != INVALID_HANDLE_VALUE) {
			CloseHandle(tex_h);
		}
		if (fence_h != NULL && fence_h != INVALID_HANDLE_VALUE) {
			CloseHandle(fence_h);
		}
		return nullptr;
	};

	if (hwnd_val == 0 || tex_h == NULL || tex_h == INVALID_HANDLE_VALUE) {
		return fail();
	}

	auto *p = new (std::nothrow) comp_d3d_transparent_presenter();
	if (p == nullptr) {
		return fail();
	}
	p->width = width;
	p->height = height;

	HRESULT hr = S_OK;
	try {
		// 1. Resolve the D3D11 device used for the present.
		if (existing_d3d11_device != nullptr) {
			auto *base = reinterpret_cast<ID3D11Device *>(existing_d3d11_device);
			p->device = wil::com_query<ID3D11Device5>(base);
			com_ptr<ID3D11DeviceContext> ctx;
			base->GetImmediateContext(ctx.put());
			p->context = ctx.query<ID3D11DeviceContext4>();
		} else {
			auto dev = xrt::auxiliary::d3d::d3d11::createDevice();
			p->owned_device = dev.first;
			p->owned_context = dev.second;
			if (!p->owned_device || !p->owned_context) {
				delete p;
				return fail();
			}
			p->device = p->owned_device.query<ID3D11Device5>();
			p->context = p->owned_context.query<ID3D11DeviceContext4>();
		}
		if (!p->device || !p->context) {
			delete p;
			return fail();
		}

		// 2. Import the shared output texture + service→client fence.
		THROW_IF_FAILED(p->device->OpenSharedResource1(tex_h, IID_PPV_ARGS(p->output_texture.put())));
		CloseHandle(tex_h);
		tex_h = NULL;

		if (fence_h == NULL || fence_h == INVALID_HANDLE_VALUE) {
			// No fence ⇒ we cannot lockstep with the service; bail (stays opaque).
			delete p;
			return fail();
		}
		THROW_IF_FAILED(p->device->OpenSharedFence(fence_h, IID_PPV_ARGS(p->output_fence.put())));
		CloseHandle(fence_h);
		fence_h = NULL;

		// 3. Composition swap chain on the device's DXGI factory.
		com_ptr<IDXGIDevice> dxgi_dev;
		com_ptr<IDXGIAdapter> adapter;
		com_ptr<IDXGIFactory2> factory;
		THROW_IF_FAILED(p->device->QueryInterface(IID_PPV_ARGS(dxgi_dev.put())));
		THROW_IF_FAILED(dxgi_dev->GetAdapter(adapter.put()));
		THROW_IF_FAILED(adapter->GetParent(IID_PPV_ARGS(factory.put())));

		DXGI_SWAP_CHAIN_DESC1 sd = {};
		sd.Width = width;
		sd.Height = height;
		sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		sd.SampleDesc.Count = 1;
		sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sd.BufferCount = 2;
		sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
		THROW_IF_FAILED(
		    factory->CreateSwapChainForComposition(p->device.get(), &sd, nullptr, p->swapchain.put()));

		// 4. DComp device/target/visual on the app's HWND.
		THROW_IF_FAILED(DCompositionCreateDevice2(nullptr, IID_PPV_ARGS(p->dcomp_device.put())));
		THROW_IF_FAILED(
		    p->dcomp_device->CreateTargetForHwnd((HWND)(uintptr_t)hwnd_val, TRUE, p->dcomp_target.put()));
		THROW_IF_FAILED(p->dcomp_device->CreateVisual(p->dcomp_visual.put()));
		THROW_IF_FAILED(p->dcomp_visual->SetContent(p->swapchain.get()));
		THROW_IF_FAILED(p->dcomp_target->SetRoot(p->dcomp_visual.get()));
		THROW_IF_FAILED(p->dcomp_device->Commit());
	} catch (const wil::ResultException &e) {
		hr = e.GetErrorCode();
		U_LOG_E("transparent presenter setup failed: 0x%08lx — staying on opaque present", hr);
		delete p;
		return fail();
	} catch (...) {
		U_LOG_E("transparent presenter setup failed (exception) — staying on opaque present");
		delete p;
		return fail();
	}

	U_LOG_W("transparent presenter ready (hwnd=0x%llx %ux%u) — DWM blends live desktop into the "
	        "DP alpha-gate holes",
	        (unsigned long long)hwnd_val, width, height);
	return p;
}

extern "C" void
comp_d3d_transparent_presenter_present(struct comp_d3d_transparent_presenter *p)
{
	if (p == nullptr || !p->swapchain || !p->output_fence || !p->context) {
		return;
	}
	// Lockstep with the service: it bumps + signals once per commit, we bump + wait once per
	// commit, so this value always matches the frame just weaved.
	p->present_value++;
	p->context->Wait(p->output_fence.get(), p->present_value);

	com_ptr<ID3D11Texture2D> back;
	if (SUCCEEDED(p->swapchain->GetBuffer(0, IID_PPV_ARGS(back.put()))) && back) {
		p->context->CopyResource(back.get(), p->output_texture.get());
	}
	p->swapchain->Present(1, 0);
	// Publish the new frame to dwm.exe (cheap delta-state IPC, no GPU work).
	if (p->dcomp_device) {
		p->dcomp_device->Commit();
	}
}

extern "C" void
comp_d3d_transparent_presenter_destroy(struct comp_d3d_transparent_presenter **p_ptr)
{
	if (p_ptr == nullptr || *p_ptr == nullptr) {
		return;
	}
	delete *p_ptr;
	*p_ptr = nullptr;
}
