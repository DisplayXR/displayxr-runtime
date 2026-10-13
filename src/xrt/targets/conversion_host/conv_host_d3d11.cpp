// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  displayxr-conversion-host D3D11 lane: device on a LUID, shared
 *         NT-handle outputs, shared fence.
 * @ingroup conv_host
 */

#include "conv_host_d3d11.h"

#include "util/u_logging.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <d3d11sdklayers.h>
#include <dxgi1_2.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct conv_host_d3d11
{
	ID3D11Device5 *dev;
	ID3D11DeviceContext4 *ctx;
	ID3D11Fence *fence;
	HANDLE fence_handle;
	ID3D11InfoQueue *info_queue; //!< DXR_CONV_HOST_D3D11_DEBUG=1 with the debug layer installed
	uint32_t debug_errors;       //!< ERROR / CORRUPTION messages logged so far
	uint64_t fence_value;
	SRWLOCK lock; //!< guards the immediate context + fence value
	char name[128];
};

static void
set_reason(char *out, uint32_t size, const char *fmt, unsigned long a)
{
	if (out != nullptr && size > 0) {
		(void)snprintf(out, size, fmt, a);
	}
}

extern "C" struct conv_host_d3d11 *
conv_host_d3d11_create(uint64_t luid, char *out_reason, uint32_t reason_size)
{
	IDXGIFactory1 *factory = nullptr;
	HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&factory);
	if (FAILED(hr)) {
		set_reason(out_reason, reason_size, "CreateDXGIFactory1 failed (0x%08lx)", (unsigned long)hr);
		return nullptr;
	}
	IDXGIAdapter1 *adapter = nullptr;
	DXGI_ADAPTER_DESC1 desc = {};
	for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
		if (SUCCEEDED(adapter->GetDesc1(&desc))) {
			const uint64_t l = ((uint64_t)(uint32_t)desc.AdapterLuid.HighPart << 32) | desc.AdapterLuid.LowPart;
			if (l == luid) {
				break;
			}
		}
		adapter->Release();
		adapter = nullptr;
	}
	factory->Release();
	if (adapter == nullptr) {
		set_reason(out_reason, reason_size, "no adapter with LUID 0x%lx (low part)",
		           (unsigned long)(luid & 0xffffffffu));
		return nullptr;
	}

	const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
	ID3D11Device *dev = nullptr;
	ID3D11DeviceContext *ctx = nullptr;
	// BGRA for interop with the service's swapchain formats; threading stays
	// the default (multithread-protected below): exec threads and the ring
	// worker both touch the immediate context.
	// DXR_CONV_HOST_D3D11_DEBUG=1 (CI / dev only): the D3D11 debug layer,
	// when installed; its errors are logged (and so forwarded) after every
	// fence signal and output allocation.
	const char *dbg_env = getenv("DXR_CONV_HOST_D3D11_DEBUG");
	const bool want_debug = dbg_env != nullptr && dbg_env[0] == '1';
	hr = E_FAIL;
	if (want_debug) {
		hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
		                       D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_DEBUG, levels,
		                       ARRAYSIZE(levels), D3D11_SDK_VERSION, &dev, nullptr, &ctx);
		U_LOG_W("conversion host: D3D11 debug layer %s (DXR_CONV_HOST_D3D11_DEBUG=1)",
		        SUCCEEDED(hr) ? "on" : "requested but not installed");
	}
	const bool debug_on = SUCCEEDED(hr);
	if (!debug_on) {
		hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
		                       levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &dev, nullptr, &ctx);
	}
	adapter->Release();
	if (FAILED(hr)) {
		set_reason(out_reason, reason_size, "D3D11CreateDevice on the adapter failed (0x%08lx)",
		           (unsigned long)hr);
		return nullptr;
	}

	struct conv_host_d3d11 *d = new conv_host_d3d11();
	InitializeSRWLock(&d->lock);
	if (debug_on) {
		(void)dev->QueryInterface(__uuidof(ID3D11InfoQueue), (void **)&d->info_queue);
	}
	hr = dev->QueryInterface(__uuidof(ID3D11Device5), (void **)&d->dev);
	if (SUCCEEDED(hr)) {
		hr = ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)&d->ctx);
	}
	dev->Release();
	ctx->Release();
	if (FAILED(hr)) {
		set_reason(out_reason, reason_size, "ID3D11Device5 / DeviceContext4 unavailable (0x%08lx)",
		           (unsigned long)hr);
		conv_host_d3d11_destroy(&d);
		return nullptr;
	}
	ID3D10Multithread *mt = nullptr;
	if (SUCCEEDED(d->dev->QueryInterface(__uuidof(ID3D10Multithread), (void **)&mt))) {
		mt->SetMultithreadProtected(TRUE);
		mt->Release();
	}
	(void)WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, d->name, (int)sizeof(d->name), nullptr, nullptr);
	return d;
}

/*!
 * Debug layer (CI / dev only): log every ERROR / CORRUPTION message the
 * layer stored since the last drain, at ERROR (so the line is forwarded to
 * the service and a test driving the host sees it). Caller holds d->lock.
 */
static void
drain_debug_locked(struct conv_host_d3d11 *d)
{
	if (d->info_queue == nullptr) {
		return;
	}
	const UINT64 n = d->info_queue->GetNumStoredMessages();
	for (UINT64 i = 0; i < n; i++) {
		SIZE_T len = 0;
		if (FAILED(d->info_queue->GetMessage(i, nullptr, &len)) || len == 0) {
			continue;
		}
		D3D11_MESSAGE *m = (D3D11_MESSAGE *)malloc(len);
		if (m == nullptr) {
			continue;
		}
		if (SUCCEEDED(d->info_queue->GetMessage(i, m, &len)) &&
		    (m->Severity == D3D11_MESSAGE_SEVERITY_ERROR || m->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION)) {
			d->debug_errors++;
			U_LOG_E("conversion host: d3d11 debug layer: %.*s", (int)m->DescriptionByteLength, m->pDescription);
		}
		free(m);
	}
	d->info_queue->ClearStoredMessages();
}

extern "C" void
conv_host_d3d11_destroy(struct conv_host_d3d11 **d_ptr)
{
	if (d_ptr == nullptr || *d_ptr == nullptr) {
		return;
	}
	struct conv_host_d3d11 *d = *d_ptr;
	if (d->fence_handle != nullptr) {
		CloseHandle(d->fence_handle);
	}
	if (d->fence != nullptr) {
		d->fence->Release();
	}
	if (d->info_queue != nullptr) {
		drain_debug_locked(d);
		d->info_queue->Release();
	}
	if (d->ctx != nullptr) {
		d->ctx->Release();
	}
	if (d->dev != nullptr) {
		d->dev->Release();
	}
	delete d;
	*d_ptr = nullptr;
}

extern "C" const char *
conv_host_d3d11_adapter_name(const struct conv_host_d3d11 *d)
{
	return d != nullptr ? d->name : "";
}

extern "C" bool
conv_host_d3d11_alloc_image(struct conv_host_d3d11 *d,
                            uint32_t width,
                            uint32_t height,
                            uint32_t array_size,
                            uint32_t dxgi_format,
                            struct conv_host_d3d11_image *out)
{
	if (d == nullptr || out == nullptr || width == 0 || height == 0) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = width;
	td.Height = height;
	td.MipLevels = 1;
	td.ArraySize = array_size > 0 ? array_size : 1;
	td.Format = (DXGI_FORMAT)dxgi_format;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
	ID3D11Texture2D *tex = nullptr;
	HRESULT hr = d->dev->CreateTexture2D(&td, nullptr, &tex);
	if (d->info_queue != nullptr) {
		AcquireSRWLockExclusive(&d->lock);
		drain_debug_locked(d);
		ReleaseSRWLockExclusive(&d->lock);
	}
	if (FAILED(hr)) {
		U_LOG_W("conversion host: output texture %ux%ux%u fmt %u failed (0x%08lx)", width, height, td.ArraySize,
		        dxgi_format, (unsigned long)hr);
		return false;
	}
	IDXGIResource1 *res = nullptr;
	HANDLE h = nullptr;
	hr = tex->QueryInterface(__uuidof(IDXGIResource1), (void **)&res);
	if (SUCCEEDED(hr)) {
		hr = res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &h);
		res->Release();
	}
	if (FAILED(hr)) {
		tex->Release();
		U_LOG_W("conversion host: CreateSharedHandle failed (0x%08lx)", (unsigned long)hr);
		return false;
	}
	out->texture = tex;
	out->nt_handle = (uint64_t)(uintptr_t)h;
	out->width = width;
	out->height = height;
	out->array_size = td.ArraySize;
	out->dxgi_format = dxgi_format;
	return true;
}

extern "C" void
conv_host_d3d11_free_image(struct conv_host_d3d11 *d, struct conv_host_d3d11_image *img)
{
	(void)d;
	if (img == nullptr) {
		return;
	}
	if (img->nt_handle != 0) {
		CloseHandle((HANDLE)(uintptr_t)img->nt_handle);
	}
	if (img->texture != nullptr) {
		((ID3D11Texture2D *)img->texture)->Release();
	}
	memset(img, 0, sizeof(*img));
}

extern "C" bool
conv_host_d3d11_fence(struct conv_host_d3d11 *d, uint64_t *out_nt_handle)
{
	if (d == nullptr) {
		return false;
	}
	AcquireSRWLockExclusive(&d->lock);
	if (d->fence == nullptr) {
		HRESULT hr = d->dev->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), (void **)&d->fence);
		if (SUCCEEDED(hr)) {
			hr = d->fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &d->fence_handle);
		}
		if (FAILED(hr)) {
			U_LOG_W("conversion host: shared fence creation failed (0x%08lx)", (unsigned long)hr);
			if (d->fence != nullptr) {
				d->fence->Release();
				d->fence = nullptr;
			}
			d->fence_handle = nullptr;
		}
	}
	const bool ok = d->fence_handle != nullptr;
	if (ok && out_nt_handle != nullptr) {
		*out_nt_handle = (uint64_t)(uintptr_t)d->fence_handle;
	}
	ReleaseSRWLockExclusive(&d->lock);
	return ok;
}

extern "C" uint64_t
conv_host_d3d11_reserve(struct conv_host_d3d11 *d)
{
	if (d == nullptr || !conv_host_d3d11_fence(d, nullptr)) {
		return 0;
	}
	AcquireSRWLockExclusive(&d->lock);
	const uint64_t v = ++d->fence_value;
	drain_debug_locked(d); // what the previous frame left (per frame: no-op without the layer)
	ReleaseSRWLockExclusive(&d->lock);
	return v;
}

extern "C" bool
conv_host_d3d11_signal_value(struct conv_host_d3d11 *d, uint64_t value)
{
	if (d == nullptr || d->fence == nullptr) {
		return false;
	}
	AcquireSRWLockExclusive(&d->lock);
	const HRESULT hr = d->ctx->Signal(d->fence, value);
	d->ctx->Flush();
	drain_debug_locked(d);
	ReleaseSRWLockExclusive(&d->lock);
	return SUCCEEDED(hr);
}

extern "C" uint64_t
conv_host_d3d11_completed(struct conv_host_d3d11 *d)
{
	return d != nullptr && d->fence != nullptr ? d->fence->GetCompletedValue() : 0;
}
