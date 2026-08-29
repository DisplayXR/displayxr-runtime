// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The D3D11 deposit ring for the heavy-D3D12 reroute (#1264 / ADR-039).
 * @ingroup comp_d3d12
 *
 * Topology and the synchronisation contract are stated in
 * comp_d3d12_deposit.h — read that first.
 */

#include "comp_d3d12_deposit.h"

#include "util/u_logging.h"
#include "util/u_misc.h"

#include <stdio.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>

struct comp_d3d12_deposit_slot
{
	ID3D11Texture2D *tex;
	HANDLE share_nt;
	ID3D12Resource *res12; //!< The slot opened on the APP's D3D12 device.
	//! The fence value the consumer's release signal reaches once its copy of
	//! this slot is ordered. 0 while no consumer has taken it.
	uint64_t release_value;
};

struct comp_d3d12_deposit_plane_slot
{
	ID3D11Texture2D *tex;
	HANDLE share_nt;
	ID3D12Resource *res12;
	uint32_t width, height;
	uint64_t generation;
};

struct comp_d3d12_deposit
{
	ID3D12Device *app_dev; //!< BORROWED — the app's device, used for opens only.

	ID3D11Device *dx;
	ID3D11Device5 *dx5;
	ID3D11DeviceContext *ctx;
	ID3D11DeviceContext4 *ctx4;
	IDXGIAdapter1 *adapter;
	uint64_t adapter_luid;

	struct comp_d3d12_deposit_slot ring[COMP_D3D12_DEPOSIT_RING];
	uint32_t slot;

	//! #1264 plane transport — single-buffered, panel-sized once.
	struct comp_d3d12_deposit_plane_slot plane[COMP_D3D12_DEPOSIT_PLANE_COUNT];
	uint64_t plane_release_value;

	//! Shared D3D11 fence; the SAME object opened on the app device as fence12.
	ID3D11Fence *fence11;
	HANDLE fence_nt;
	ID3D12Fence *fence12;

	//! Last value claimed (== last that will be signalled), shared by the app
	//! queue's atlas signals and the consumer's release signals — monotonic on
	//! one timeline so a release can never be mistaken for an atlas signal.
	uint64_t value;

	uint32_t width;
	uint32_t height;
};

static void
deposit_free_plane(struct comp_d3d12_deposit *dep, uint32_t plane);

static void
deposit_free_ring(struct comp_d3d12_deposit *dep)
{
	for (uint32_t i = 0; i < COMP_D3D12_DEPOSIT_RING; i++) {
		struct comp_d3d12_deposit_slot *s = &dep->ring[i];
		if (s->res12 != NULL) {
			s->res12->Release();
			s->res12 = NULL;
		}
		if (s->share_nt != NULL) {
			CloseHandle(s->share_nt);
			s->share_nt = NULL;
		}
		if (s->tex != NULL) {
			s->tex->Release();
			s->tex = NULL;
		}
		s->release_value = 0;
	}
}

/*!
 * Allocate the ring: NT-shared D3D11 textures (`SHARED_NTHANDLE | SHARED`,
 * fence-synchronised — the same share recipe the VK deposit and the d3d11-ends
 * xbridge already prove on this hardware), each opened on the app's D3D12
 * device as a renderable resource. `BIND_RENDER_TARGET` on the D3D11 side is
 * what makes the opened `ID3D12Resource` accept an RTV.
 */
static bool
deposit_alloc_ring(struct comp_d3d12_deposit *dep)
{
	for (uint32_t i = 0; i < COMP_D3D12_DEPOSIT_RING; i++) {
		struct comp_d3d12_deposit_slot *s = &dep->ring[i];

		D3D11_TEXTURE2D_DESC td = {};
		td.Width = dep->width;
		td.Height = dep->height;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;

		HRESULT hr = dep->dx->CreateTexture2D(&td, NULL, &s->tex);
		if (FAILED(hr) || s->tex == NULL) {
			U_LOG_W("d3d12 deposit: CreateTexture2D(slot %u) failed: 0x%08lx", i, (unsigned long)hr);
			return false;
		}

		IDXGIResource1 *dxgi_res = NULL;
		hr = s->tex->QueryInterface(__uuidof(IDXGIResource1), (void **)&dxgi_res);
		if (FAILED(hr) || dxgi_res == NULL) {
			U_LOG_W("d3d12 deposit: QueryInterface(IDXGIResource1)(slot %u) failed: 0x%08lx", i,
			        (unsigned long)hr);
			return false;
		}
		hr = dxgi_res->CreateSharedHandle(NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, NULL,
		                                  &s->share_nt);
		dxgi_res->Release();
		if (FAILED(hr) || s->share_nt == NULL) {
			U_LOG_W("d3d12 deposit: CreateSharedHandle(slot %u) failed: 0x%08lx", i, (unsigned long)hr);
			return false;
		}

		// The handle stays OPEN: the split's ingress binds it by NT handle on
		// the consumer side; closed in destroy.
		hr = dep->app_dev->OpenSharedHandle(s->share_nt, __uuidof(ID3D12Resource), (void **)&s->res12);
		if (FAILED(hr) || s->res12 == NULL) {
			U_LOG_W("d3d12 deposit: OpenSharedHandle(slot %u) on the app device failed: 0x%08lx", i,
			        (unsigned long)hr);
			return false;
		}
		s->res12->SetName(L"DXR.d3d12_deposit_slot");
	}
	return true;
}

extern "C" xrt_result_t
comp_d3d12_deposit_create(void *app_d3d12_device,
                          uint32_t width,
                          uint32_t height,
                          struct comp_d3d12_deposit **out_deposit)
{
	*out_deposit = NULL;
	if (app_d3d12_device == NULL || width == 0 || height == 0) {
		return XRT_ERROR_D3D12;
	}
	ID3D12Device *app_dev = (ID3D12Device *)app_d3d12_device;

	const LUID app_luid = app_dev->GetAdapterLuid();
	const uint64_t want_luid =
	    ((uint64_t)(uint32_t)app_luid.HighPart << 32) | (uint64_t)(uint32_t)app_luid.LowPart;

	IDXGIAdapter1 *adapter = NULL;
	WCHAR desc[128] = L"UNKNOWN";
	{
		IDXGIFactory1 *factory = NULL;
		if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&factory))) {
			for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
				DXGI_ADAPTER_DESC1 ad = {};
				uint64_t luid = 0;
				if (SUCCEEDED(adapter->GetDesc1(&ad))) {
					luid = ((uint64_t)(uint32_t)ad.AdapterLuid.HighPart << 32) |
					       (uint64_t)(uint32_t)ad.AdapterLuid.LowPart;
				}
				if (luid == want_luid) {
					memcpy(desc, ad.Description, sizeof(desc) - sizeof(WCHAR));
					desc[(sizeof(desc) / sizeof(WCHAR)) - 1] = L'\0';
					break;
				}
				adapter->Release();
				adapter = NULL;
			}
			factory->Release();
		}
	}
	if (adapter == NULL) {
		U_LOG_W("d3d12 deposit: no DXGI adapter matches the app device LUID=0x%016llx — deposit DISABLED",
		        (unsigned long long)want_luid);
		return XRT_ERROR_D3D12;
	}

	struct comp_d3d12_deposit *dep = U_TYPED_CALLOC(struct comp_d3d12_deposit);
	if (dep == NULL) {
		adapter->Release();
		return XRT_ERROR_ALLOCATION;
	}
	dep->app_dev = app_dev;
	dep->adapter = adapter;
	dep->adapter_luid = want_luid;
	dep->width = width;
	dep->height = height;

	HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL,
	                               0, D3D11_SDK_VERSION, &dep->dx, NULL, &dep->ctx);
	if (FAILED(hr) || dep->dx == NULL) {
		U_LOG_W("d3d12 deposit: D3D11CreateDevice failed: 0x%08lx — deposit DISABLED", (unsigned long)hr);
		comp_d3d12_deposit_destroy(&dep);
		return XRT_ERROR_D3D12;
	}
	hr = dep->dx->QueryInterface(__uuidof(ID3D11Device5), (void **)&dep->dx5);
	if (FAILED(hr) || dep->dx5 == NULL) {
		U_LOG_W("d3d12 deposit: no ID3D11Device5 — no shared fence, deposit DISABLED");
		comp_d3d12_deposit_destroy(&dep);
		return XRT_ERROR_D3D12;
	}
	hr = dep->ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)&dep->ctx4);
	if (FAILED(hr) || dep->ctx4 == NULL) {
		// Without the queued-signal interface the back edge (slot release)
		// cannot exist and the ring would run timing-only — honest fail here,
		// the session keeps its stock path.
		U_LOG_W("d3d12 deposit: no ID3D11DeviceContext4 — no queued release channel, deposit DISABLED");
		comp_d3d12_deposit_destroy(&dep);
		return XRT_ERROR_D3D12;
	}

	// The one fence, shared both ways: D3D11 allocates, D3D12 opens.
	hr = dep->dx5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), (void **)&dep->fence11);
	if (FAILED(hr) || dep->fence11 == NULL) {
		U_LOG_W("d3d12 deposit: CreateFence failed: 0x%08lx — deposit DISABLED", (unsigned long)hr);
		comp_d3d12_deposit_destroy(&dep);
		return XRT_ERROR_D3D12;
	}
	hr = dep->fence11->CreateSharedHandle(NULL, GENERIC_ALL, NULL, &dep->fence_nt);
	if (FAILED(hr) || dep->fence_nt == NULL) {
		U_LOG_W("d3d12 deposit: fence CreateSharedHandle failed: 0x%08lx — deposit DISABLED",
		        (unsigned long)hr);
		comp_d3d12_deposit_destroy(&dep);
		return XRT_ERROR_D3D12;
	}
	hr = app_dev->OpenSharedHandle(dep->fence_nt, __uuidof(ID3D12Fence), (void **)&dep->fence12);
	if (FAILED(hr) || dep->fence12 == NULL) {
		U_LOG_W("d3d12 deposit: OpenSharedHandle(fence) on the app device failed: 0x%08lx — deposit DISABLED",
		        (unsigned long)hr);
		comp_d3d12_deposit_destroy(&dep);
		return XRT_ERROR_D3D12;
	}

	if (!deposit_alloc_ring(dep)) {
		comp_d3d12_deposit_destroy(&dep);
		return XRT_ERROR_D3D12;
	}

	U_LOG_W(
	    "d3d12 deposit: ACTIVE — atlas renders straight into the D3D11 texture ring on '%ls' "
	    "(LUID=0x%016llx, %ux%u x%u), shared ID3D11Fence opened natively as ID3D12Fence both "
	    "ways. No CPU wait added (#1264 heavy-d3d12 reroute).",
	    desc, (unsigned long long)want_luid, width, height, (unsigned)COMP_D3D12_DEPOSIT_RING);

	*out_deposit = dep;
	return XRT_SUCCESS;
}

extern "C" void
comp_d3d12_deposit_destroy(struct comp_d3d12_deposit **deposit_ptr)
{
	if (deposit_ptr == NULL || *deposit_ptr == NULL) {
		return;
	}
	struct comp_d3d12_deposit *dep = *deposit_ptr;

	deposit_free_ring(dep);
	for (uint32_t p = 0; p < COMP_D3D12_DEPOSIT_PLANE_COUNT; p++) {
		deposit_free_plane(dep, p);
	}

	if (dep->fence12 != NULL) {
		dep->fence12->Release();
		dep->fence12 = NULL;
	}
	if (dep->fence_nt != NULL) {
		CloseHandle(dep->fence_nt);
		dep->fence_nt = NULL;
	}
	if (dep->fence11 != NULL) {
		dep->fence11->Release();
		dep->fence11 = NULL;
	}
	if (dep->ctx4 != NULL) {
		dep->ctx4->Release();
		dep->ctx4 = NULL;
	}
	if (dep->ctx != NULL) {
		dep->ctx->Release();
		dep->ctx = NULL;
	}
	if (dep->dx5 != NULL) {
		dep->dx5->Release();
		dep->dx5 = NULL;
	}
	if (dep->dx != NULL) {
		dep->dx->Release();
		dep->dx = NULL;
	}
	if (dep->adapter != NULL) {
		dep->adapter->Release();
		dep->adapter = NULL;
	}
	// app_dev and fence12's underlying object are the app's / shared — only
	// our references were released above.

	free(dep);
	*deposit_ptr = NULL;
}

extern "C" xrt_result_t
comp_d3d12_deposit_resize(struct comp_d3d12_deposit *dep, uint32_t width, uint32_t height)
{
	if (dep == NULL || width == 0 || height == 0) {
		return XRT_ERROR_D3D12;
	}
	if (dep->width == width && dep->height == height) {
		return XRT_SUCCESS;
	}
	deposit_free_ring(dep);
	dep->width = width;
	dep->height = height;
	dep->slot = 0;
	if (!deposit_alloc_ring(dep)) {
		U_LOG_W("d3d12 deposit: resize to %ux%u failed — deposit is now INACTIVE", width, height);
		deposit_free_ring(dep);
		return XRT_ERROR_D3D12;
	}
	U_LOG_W("d3d12 deposit: resized to %ux%u", width, height);
	return XRT_SUCCESS;
}

extern "C" void
comp_d3d12_deposit_advance(struct comp_d3d12_deposit *dep, void *app_queue)
{
	if (dep == NULL) {
		return;
	}
	dep->slot = (dep->slot + 1) % COMP_D3D12_DEPOSIT_RING;
	struct comp_d3d12_deposit_slot *s = &dep->ring[dep->slot];
	if (s->release_value != 0 && app_queue != NULL && dep->fence12 != NULL) {
		// GPU-side back-pressure: nothing on this queue may rewrite the slot
		// before the consumer's recorded copy of it has resolved. Free on the
		// frames it is already satisfied — the common case at D>=2.
		((ID3D12CommandQueue *)app_queue)->Wait(dep->fence12, s->release_value);
	}
}

extern "C" void *
comp_d3d12_deposit_current_resource(struct comp_d3d12_deposit *dep)
{
	return (dep != NULL) ? (void *)dep->ring[dep->slot].res12 : NULL;
}

extern "C" void
comp_d3d12_deposit_get_dims(struct comp_d3d12_deposit *dep, uint32_t *out_w, uint32_t *out_h)
{
	if (out_w != NULL) {
		*out_w = (dep != NULL) ? dep->width : 0;
	}
	if (out_h != NULL) {
		*out_h = (dep != NULL) ? dep->height : 0;
	}
}

extern "C" void
comp_d3d12_deposit_signal(struct comp_d3d12_deposit *dep, void *app_queue)
{
	if (dep == NULL || app_queue == NULL || dep->fence12 == NULL) {
		return;
	}
	dep->value += 1;
	((ID3D12CommandQueue *)app_queue)->Signal(dep->fence12, dep->value);
}

extern "C" bool
comp_d3d12_deposit_get_handoff(struct comp_d3d12_deposit *dep, struct comp_vk_deposit_handoff *out)
{
	if (dep == NULL || out == NULL || dep->dx == NULL) {
		return false;
	}
	const struct comp_d3d12_deposit_slot *s = &dep->ring[dep->slot];
	if (s->tex == NULL) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	out->d3d11_device = dep->dx;
	out->d3d11_context = dep->ctx;
	out->dxgi_adapter = dep->adapter;
	out->texture = s->tex;
	out->shared_handle = s->share_nt;
	out->keyed_mutex = NULL; // always fence mode on this flavour
	out->fence = dep->fence11;
	out->fence_shared_handle = dep->fence_nt;
	out->fence_value = dep->value;
	out->adapter_luid = dep->adapter_luid;
	out->width = dep->width;
	out->height = dep->height;
	out->slot = dep->slot;
	out->dxgi_format = (uint32_t)DXGI_FORMAT_R8G8B8A8_UNORM;
	return true;
}

extern "C" void
comp_d3d12_deposit_note_consumed(struct comp_d3d12_deposit *dep, uint32_t slot)
{
	if (dep == NULL || dep->ctx4 == NULL || dep->fence11 == NULL || slot >= COMP_D3D12_DEPOSIT_RING) {
		return;
	}
	// Past every value claimed so far, so a release can never be mistaken for
	// (or overtaken by) an atlas signal on the same timeline.
	dep->value += 1;
	dep->ctx4->Signal(dep->fence11, dep->value);
	dep->ring[slot].release_value = dep->value;
}

static void
deposit_free_plane(struct comp_d3d12_deposit *dep, uint32_t plane)
{
	struct comp_d3d12_deposit_plane_slot *p = &dep->plane[plane];
	if (p->res12 != NULL) {
		p->res12->Release();
		p->res12 = NULL;
	}
	if (p->share_nt != NULL) {
		CloseHandle(p->share_nt);
		p->share_nt = NULL;
	}
	if (p->tex != NULL) {
		p->tex->Release();
		p->tex = NULL;
	}
	p->width = 0;
	p->height = 0;
}

extern "C" bool
comp_d3d12_deposit_plane_ensure(struct comp_d3d12_deposit *dep, uint32_t plane, uint32_t width, uint32_t height)
{
	if (dep == NULL || dep->dx == NULL || plane >= COMP_D3D12_DEPOSIT_PLANE_COUNT || width == 0 || height == 0) {
		return false;
	}
	struct comp_d3d12_deposit_plane_slot *p = &dep->plane[plane];
	// Steady state after warmup: a compare (panel-sized once).
	if (p->tex != NULL && p->width == width && p->height == height) {
		return true;
	}
	// A REALLOCATION — an on-change event. The caller has idled the queues
	// (the same discipline every shared-surface realloc in this tree follows).
	deposit_free_plane(dep, plane);

	D3D11_TEXTURE2D_DESC td = {};
	td.Width = width;
	td.Height = height;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;

	HRESULT hr = dep->dx->CreateTexture2D(&td, NULL, &p->tex);
	IDXGIResource1 *dxgi_res = NULL;
	if (SUCCEEDED(hr) && p->tex != NULL &&
	    SUCCEEDED(p->tex->QueryInterface(__uuidof(IDXGIResource1), (void **)&dxgi_res)) && dxgi_res != NULL) {
		hr = dxgi_res->CreateSharedHandle(NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, NULL,
		                                  &p->share_nt);
		dxgi_res->Release();
	} else if (SUCCEEDED(hr)) {
		hr = E_FAIL;
	}
	if (SUCCEEDED(hr) && p->share_nt != NULL) {
		hr = dep->app_dev->OpenSharedHandle(p->share_nt, __uuidof(ID3D12Resource), (void **)&p->res12);
	}
	if (FAILED(hr) || p->res12 == NULL) {
		// Feature-local: the caller stops staging this plane; the 3D weave is
		// untouched — the same degrade contract as the VK deposit's planes.
		U_LOG_W("d3d12 deposit: plane %u could not be allocated at %ux%u (0x%08lx) — that feature is "
		        "off for this session; the weave is unaffected (#1264)",
		        plane, width, height, (unsigned long)hr);
		deposit_free_plane(dep, plane);
		return false;
	}
	p->res12->SetName(L"DXR.d3d12_deposit_plane");
	p->width = width;
	p->height = height;
	p->generation++;
	U_LOG_W("d3d12 deposit: plane %u up — %ux%u, generation %llu (#1264)", plane, width, height,
	        (unsigned long long)p->generation);
	return true;
}

extern "C" bool
comp_d3d12_deposit_plane_get(struct comp_d3d12_deposit *dep, uint32_t plane, struct comp_d3d12_deposit_plane *out)
{
	if (dep == NULL || out == NULL || plane >= COMP_D3D12_DEPOSIT_PLANE_COUNT) {
		return false;
	}
	const struct comp_d3d12_deposit_plane_slot *p = &dep->plane[plane];
	if (p->tex == NULL || p->res12 == NULL) {
		return false;
	}
	out->shared_handle = p->share_nt;
	out->resource12 = p->res12;
	out->width = p->width;
	out->height = p->height;
	out->generation = p->generation;
	return true;
}

extern "C" void
comp_d3d12_deposit_note_planes_consumed(struct comp_d3d12_deposit *dep)
{
	if (dep == NULL || dep->ctx4 == NULL || dep->fence11 == NULL) {
		return;
	}
	// One signal covers every plane: the immediate context is one ordered
	// stream and the producer's reads were just recorded ahead of it.
	dep->value += 1;
	dep->ctx4->Signal(dep->fence11, dep->value);
	dep->plane_release_value = dep->value;
}

extern "C" void
comp_d3d12_deposit_plane_wait(struct comp_d3d12_deposit *dep, void *app_queue)
{
	if (dep == NULL || app_queue == NULL || dep->fence12 == NULL || dep->plane_release_value == 0) {
		return;
	}
	((ID3D12CommandQueue *)app_queue)->Wait(dep->fence12, dep->plane_release_value);
}
