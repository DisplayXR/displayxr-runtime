// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The D3D11 half of a conversion-host satellite lane (see d3d11_conv_host_lane.h).
 * @ingroup comp_d3d11_service
 */

#include "d3d11_conv_host_lane.h"

#include "util/u_logging.h"

#include <d3d11_4.h>
#include <dxgi1_2.h>

#include <mutex>
#include <unordered_map>

namespace {

constexpr UINT kCopyTimeoutMs = 2000;

template <typename T>
void
release(T *&p)
{
	if (p != nullptr) {
		p->Release();
		p = nullptr;
	}
}

struct opened_tex
{
	ID3D11Texture2D *tex = nullptr;
	IDXGIKeyedMutex *km = nullptr;
};

struct lane
{
	ID3D11Device5 *dev = nullptr;
	ID3D11DeviceContext *ctx = nullptr;
	ID3D11Query *done = nullptr;
	HANDLE event = nullptr;
	std::mutex mtx;                                   // the proxy's worker is the only caller; belt and braces
	std::unordered_map<uint64_t, opened_tex> targets; // by import_key
};

lane *
L(void *ud)
{
	return static_cast<lane *>(ud);
}

bool
open_tex(lane *l, HANDLE h, opened_tex *out)
{
	ID3D11Texture2D *tex = nullptr;
	if (FAILED(l->dev->OpenSharedResource1(h, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&tex)))) {
		return false;
	}
	out->tex = tex;
	(void)tex->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void **>(&out->km));
	return true;
}

bool
lane_open_output(void *ud, uint64_t handle, const xrt_conv_image *desc, void **out_ctx)
{
	(void)desc;
	lane *l = L(ud);
	auto *o = new opened_tex();
	const bool ok = open_tex(l, reinterpret_cast<HANDLE>(handle), o);
	CloseHandle(reinterpret_cast<HANDLE>(handle)); // the opened resource keeps the memory alive
	if (!ok) {
		delete o;
		U_LOG_W("[conv-lane] could not open a host output texture (wrong adapter?)");
		return false;
	}
	*out_ctx = o;
	return true;
}

bool
lane_open_fence(void *ud, uint64_t handle, void **out_fence)
{
	lane *l = L(ud);
	ID3D11Fence *f = nullptr;
	const HRESULT hr =
	    l->dev->OpenSharedFence(reinterpret_cast<HANDLE>(handle), __uuidof(ID3D11Fence), reinterpret_cast<void **>(&f));
	CloseHandle(reinterpret_cast<HANDLE>(handle));
	if (FAILED(hr)) {
		U_LOG_W("[conv-lane] could not open the host's output fence (0x%08lx)", (unsigned long)hr);
		return false;
	}
	*out_fence = f;
	return true;
}

//! THE FENCE RULE: a CPU wait, never a GPU one.
bool
lane_observe_fence(void *ud, void *fence, uint64_t value, uint32_t timeout_ms)
{
	lane *l = L(ud);
	auto *f = static_cast<ID3D11Fence *>(fence);
	if (f->GetCompletedValue() >= value) {
		return true;
	}
	std::lock_guard<std::mutex> lk(l->mtx);
	if (FAILED(f->SetEventOnCompletion(value, l->event))) {
		return false;
	}
	if (WaitForSingleObject(l->event, timeout_ms) != WAIT_OBJECT_0) {
		return false;
	}
	return f->GetCompletedValue() >= value;
}

bool
lane_copy_output(void *ud, void *src_ctx, const xrt_conv_image *dst)
{
	lane *l = L(ud);
	auto *src = static_cast<opened_tex *>(src_ctx);
	if (dst->memory_kind != XRT_CONV_MEM_WIN32_NT_HANDLE || dst->handle == 0) {
		return false;
	}
	std::lock_guard<std::mutex> lk(l->mtx);
	auto it = l->targets.find(dst->import_key);
	if (it == l->targets.end()) {
		opened_tex t;
		if (!open_tex(l, reinterpret_cast<HANDLE>(dst->handle), &t)) {
			U_LOG_W("[conv-lane] could not open a ring target (wrong adapter?)");
			return false;
		}
		it = l->targets.emplace(dst->import_key, t).first;
	}
	opened_tex &t = it->second;
	const bool keyed = t.km != nullptr && dst->sync.kind == XRT_CONV_SYNC_KEYED_MUTEX;
	if (keyed && t.km->AcquireSync(dst->sync.acquire_value, kCopyTimeoutMs) != S_OK) {
		return false;
	}
	// The host's output is complete (its fence was observed on the CPU):
	// a plain copy on the lane's own context, with no GPU wait queued.
	const bool src_locked = src->km != nullptr && src->km->AcquireSync(0, kCopyTimeoutMs) == S_OK;
	l->ctx->CopyResource(t.tex, src->tex);
	if (src_locked) {
		(void)src->km->ReleaseSync(0);
	}
	l->ctx->End(l->done);
	l->ctx->Flush();
	if (keyed) {
		(void)t.km->ReleaseSync(dst->sync.release_value);
	}
	// The copy itself is observed on the CPU before the result is published.
	const ULONGLONG until = GetTickCount64() + kCopyTimeoutMs;
	BOOL finished = FALSE;
	while (l->ctx->GetData(l->done, &finished, sizeof(finished), 0) != S_OK || !finished) {
		if (GetTickCount64() > until) {
			return false;
		}
		Sleep(0);
	}
	return true;
}

void
lane_close_output(void *ud, void *ctx)
{
	(void)ud;
	auto *o = static_cast<opened_tex *>(ctx);
	release(o->km);
	release(o->tex);
	delete o;
}

void
lane_close_fence(void *ud, void *fence)
{
	(void)ud;
	static_cast<ID3D11Fence *>(fence)->Release();
}

void
lane_destroy(void *ud)
{
	lane *l = L(ud);
	for (auto &kv : l->targets) {
		release(kv.second.km);
		release(kv.second.tex);
	}
	release(l->done);
	release(l->ctx);
	release(l->dev);
	if (l->event != nullptr) {
		CloseHandle(l->event);
	}
	delete l;
}

IDXGIAdapter1 *
find_adapter(uint64_t luid)
{
	if (luid == 0) {
		return nullptr;
	}
	IDXGIFactory1 *f = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&f)))) {
		return nullptr;
	}
	IDXGIAdapter1 *found = nullptr;
	IDXGIAdapter1 *a = nullptr;
	for (UINT i = 0; found == nullptr && f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
		DXGI_ADAPTER_DESC1 d;
		if (SUCCEEDED(a->GetDesc1(&d)) &&
		    ((uint64_t)(uint32_t)d.AdapterLuid.HighPart << 32 | d.AdapterLuid.LowPart) == luid) {
			found = a;
		} else {
			a->Release();
		}
	}
	f->Release();
	return found;
}

} // namespace

extern "C" bool
d3d11_conv_host_lane_create(uint64_t adapter_luid, struct conv_host_proxy_gpu_ops *out)
{
	auto *l = new lane();
	IDXGIAdapter1 *adapter = find_adapter(adapter_luid);
	if (adapter_luid != 0 && adapter == nullptr) {
		U_LOG_W("[conv-lane] no adapter with LUID %llx: the satellite stays on the CPU lane",
		        (unsigned long long)adapter_luid);
		delete l;
		return false;
	}
	ID3D11Device *dev = nullptr;
	const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
	HRESULT hr = D3D11CreateDevice(adapter, adapter != nullptr ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
	                               nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &l->ctx);
	release(adapter);
	if (FAILED(hr) || FAILED(dev->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void **>(&l->dev)))) {
		release(dev);
		release(l->ctx);
		delete l;
		U_LOG_W("[conv-lane] no D3D11.4 device with shared fences: the satellite stays on the CPU lane");
		return false;
	}
	release(dev);
	D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
	l->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	if (FAILED(l->dev->CreateQuery(&qd, &l->done)) || l->event == nullptr) {
		lane_destroy(l);
		return false;
	}
	out->ud = l;
	out->open_output = lane_open_output;
	out->open_fence = lane_open_fence;
	out->observe_fence = lane_observe_fence;
	out->copy_output = lane_copy_output;
	out->close_output = lane_close_output;
	out->close_fence = lane_close_fence;
	out->destroy = lane_destroy;
	return true;
}
