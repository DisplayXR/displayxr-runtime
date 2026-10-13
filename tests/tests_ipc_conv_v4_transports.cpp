// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  #2054 — XR_DXR_lift spec-4 transports on WARP, through the service
 *         side of conv_submit_frame (ipc_server_conv_v4_submit) and the real
 *         D3D11 glue (d3d11_lift_glue_import_input_ex / _export_cpu).
 *
 * The sim_display FAKE lift module behind `dp-lift` makes the output a pure
 * function of the input picture (see tests_lift_dp_golden), so every
 * transport must reproduce the SBS bytes pinned on the pre-core path:
 *
 *  - a D3D11 NT texture through the spec-4 submit;
 *  - the same picture inside a larger texture, submitted with a region offset;
 *  - a D3D12 committed resource (NT handle, no keyed mutex) written by a D3D12
 *    copy and ordered by a shared ID3D12Fence the service waits on;
 *  - CPU bytes in a file mapping (RGBA8, and BGRA8 swizzled);
 *  - CPU export: the result read back into a mapping, hashed the same way.
 *
 * WARP only; no D3D11 device SKIPs, no D3D12 WARP device skips that section.
 *
 * @ingroup tests
 */

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "catch_amalgamated.hpp"

#include "d3d11_service/d3d11_lift.h"
#include "xrt/xrt_display_processor_d3d11.h"

#include "conversion/conv_core.h"
#include "conversion/conv_provider_legacy_dp.h"
#include "conversion/conv_registry.h"
#include "server/ipc_server_conv.h"
#include "shared/ipc_protocol.h"

#include "sim_display_lift_d3d11.h"

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace {

constexpr uint32_t kW = 96, kH = 64;
//! tests_lift_dp_golden's pin: the SBS bytes of the pre-core path, WARP.
constexpr uint64_t kGoldenSbs = 0x6c0461d3308e0febull;

template <typename T>
void
rel(T *&p)
{
	if (p != nullptr) {
		p->Release();
		p = nullptr;
	}
}

/*
 * The lift-only display processor carrying the fake module (as tests_lift_dp_golden).
 */
struct golden_dp
{
	xrt_display_processor_d3d11 base;
	sim_fake_lift *fl;
};

golden_dp *
gdp(xrt_display_processor_d3d11 *x)
{
	return reinterpret_cast<golden_dp *>(x);
}

void
gdp_destroy(xrt_display_processor_d3d11 *x)
{
	sim_fake_lift_destroy(gdp(x)->fl);
	free(x);
}

bool
gdp_get_caps(xrt_display_processor_d3d11 *x, xrt_dp_lift_caps *out)
{
	return sim_fake_lift_get_caps(gdp(x)->fl, out);
}

bool
gdp_stream_create(xrt_display_processor_d3d11 *x, const xrt_dp_lift_stream_info *info, uint64_t *out_id)
{
	return sim_fake_lift_stream_create(gdp(x)->fl, info, out_id);
}

void
gdp_stream_destroy(xrt_display_processor_d3d11 *x, uint64_t id)
{
	sim_fake_lift_stream_destroy(gdp(x)->fl, id);
}

bool
gdp_convert(xrt_display_processor_d3d11 *x,
            uint64_t id,
            void *ctx,
            void *in,
            uint32_t w,
            uint32_t h,
            const xrt_dp_lift_params *p,
            const float *vps,
            uint32_t vpf,
            void **out,
            uint32_t *ow,
            uint32_t *oh,
            uint32_t *of)
{
	return sim_fake_lift_convert(gdp(x)->fl, id, ctx, in, w, h, p, vps, vpf, out, ow, oh, of);
}

bool
gdp_get_depth(xrt_display_processor_d3d11 *x, uint64_t id, void *ctx, xrt_dp_lift_depth *out)
{
	return sim_fake_lift_get_depth(gdp(x)->fl, id, ctx, out);
}

xrt_result_t
golden_factory(void *device, void *context, void *window, xrt_display_processor_d3d11 **out)
{
	(void)context;
	(void)window;
	sim_fake_lift *fl = sim_fake_lift_create(device);
	if (fl == nullptr) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}
	auto *d = static_cast<golden_dp *>(calloc(1, sizeof(golden_dp)));
	d->base.struct_size = (uint32_t)sizeof(xrt_display_processor_d3d11);
	d->base.destroy = gdp_destroy;
	d->base.lift_get_caps = gdp_get_caps;
	d->base.lift_stream_create = gdp_stream_create;
	d->base.lift_stream_destroy = gdp_stream_destroy;
	d->base.lift_convert = gdp_convert;
	d->base.lift_get_depth = gdp_get_depth;
	d->fl = fl;
	*out = &d->base;
	return XRT_SUCCESS;
}

bool
golden_resolve(void *ud, void **out_lift, void **out_fallback)
{
	(void)ud;
	*out_lift = (void *)golden_factory;
	*out_fallback = nullptr;
	return true;
}

uint64_t
fnv1a(const uint8_t *p, size_t n, uint64_t h = 1469598103934665603ull)
{
	for (size_t i = 0; i < n; i++) {
		h ^= p[i];
		h *= 1099511628211ull;
	}
	return h;
}

//! tests_lift_dp_golden's input picture (RGBA8 words).
std::vector<uint32_t>
make_input()
{
	std::vector<uint32_t> px(kW * kH);
	for (uint32_t y = 0; y < kH; y++) {
		for (uint32_t x = 0; x < kW; x++) {
			uint32_t r = 40 + (x * 200) / kW;
			uint32_t g = 40 + (y * 200) / kH;
			uint32_t b = (x >= 30 && x < 60 && y >= 20 && y < 44) ? 250 : 60;
			px[y * kW + x] = r | (g << 8) | (b << 16) | (255u << 24);
		}
	}
	return px;
}

/*
 * The service side: d3d11_lift + core + dp-lift + the v4 adapter, real glue.
 */

xrt_result_t
io_import(void *ud,
          uint64_t sid,
          xrt_graphics_buffer_handle_t h,
          bool dx,
          uint32_t w,
          uint32_t hh,
          const conv_submit_slot *slot)
{
	return d3d11_lift_glue_import_input((d3d11_lift *)ud, sid, (HANDLE)h, dx, w, hh, slot);
}

xrt_result_t
io_import_ex(void *ud, uint64_t sid, const conv_input_import *in, const conv_submit_slot *slot)
{
	return d3d11_lift_glue_import_input_ex((d3d11_lift *)ud, sid, in, slot);
}

void
io_release(void *, xrt_graphics_buffer_handle_t h, bool dx)
{
	d3d11_lift_glue_release_input((HANDLE)h, dx);
}

xrt_result_t
io_publish(void *ud, uint64_t sid, const conv_result *res, ipc_server_conv_published *out)
{
	d3d11_lift_published p = {};
	const xrt_result_t r = d3d11_lift_glue_publish((d3d11_lift *)ud, sid, res, &p);
	out->fence_value = p.fence_value;
	out->width = p.width;
	out->height = p.height;
	out->format = p.format;
	out->realloc = p.realloc;
	out->depth_width = p.depth_width;
	out->depth_height = p.depth_height;
	out->depth_format = p.depth_format;
	out->depth_realloc = p.depth_realloc;
	return r;
}

bool
io_export(void *ud, uint64_t sid, bool depth, xrt_graphics_buffer_handle_t *h, uint32_t *w, uint32_t *hh, uint32_t *f)
{
	HANDLE x = nullptr;
	if (!d3d11_lift_glue_export_output((d3d11_lift *)ud, sid, depth, &x, w, hh, f)) {
		return false;
	}
	*h = (xrt_graphics_buffer_handle_t)x;
	return true;
}

bool
io_export_cpu(void *ud,
              uint64_t sid,
              bool copy,
              xrt_graphics_buffer_handle_t *h,
              uint32_t *w,
              uint32_t *hh,
              uint32_t *f,
              uint64_t *bytes,
              bool *realloc)
{
	HANDLE x = nullptr;
	if (!d3d11_lift_glue_export_cpu((d3d11_lift *)ud, sid, copy, &x, w, hh, f, bytes, realloc)) {
		return false;
	}
	*h = (xrt_graphics_buffer_handle_t)x;
	return true;
}

bool
io_fence(void *ud, uint64_t sid, xrt_graphics_sync_handle_t *h)
{
	HANDLE x = nullptr;
	if (!d3d11_lift_glue_export_fence((d3d11_lift *)ud, sid, &x)) {
		return false;
	}
	*h = (xrt_graphics_sync_handle_t)x;
	return true;
}

void
io_gone(void *ud, uint64_t sid)
{
	d3d11_lift_glue_stream_gone((d3d11_lift *)ud, sid);
}

struct Service
{
	ID3D11Device *dev = nullptr;
	ID3D11Device1 *dev1 = nullptr;
	ID3D11Device5 *dev5 = nullptr;
	ID3D11DeviceContext *ctx = nullptr;
	ID3D11DeviceContext4 *ctx4 = nullptr;
	std::mutex ctx_mtx;
	d3d11_lift *l = nullptr;
	conv_core *core = nullptr;
	ipc_server_conv *sc = nullptr;
	xrt_conversion_plugin_instance *inst = nullptr;

	~Service()
	{
		ipc_server_conv_destroy(&sc);
		if (inst != nullptr) {
			conv_provider_legacy_dp_detach(inst);
		}
		conv_core_destroy(&core);
		d3d11_lift_destroy(&l);
		rel(ctx4);
		rel(ctx);
		rel(dev5);
		rel(dev1);
		rel(dev);
	}

	bool
	init()
	{
		D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
		if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
		                             fl, 2, D3D11_SDK_VERSION, &dev, nullptr, &ctx)) ||
		    FAILED(dev->QueryInterface(__uuidof(ID3D11Device1), (void **)&dev1)) ||
		    FAILED(dev->QueryInterface(__uuidof(ID3D11Device5), (void **)&dev5)) ||
		    FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)&ctx4))) {
			return false;
		}
		l = d3d11_lift_create(dev, ctx, &ctx_mtx, golden_resolve, nullptr);
		if (l == nullptr) {
			return false;
		}
		conv_core_config cfg{};
		d3d11_lift_glue_alloc_ops(l, &cfg.alloc);
		d3d11_lift_glue_gpu_ops(l, &cfg.viewsyn, &cfg.readback);
		cfg.prewarm_on = CONV_PREWARM_FIRST_CREATE;
		cfg.rear_cap_m = -1.0f;
		cfg.tick_ms = 10;
		if (conv_core_create(&cfg, &core) != XRT_SUCCESS) {
			return false;
		}
		conv_dp_lift_backend be;
		d3d11_lift_get_dp_backend(l, &be);
		const xrt_conversion_plugin_iface *iface = nullptr;
		if (!conv_provider_legacy_dp_create(&be, &iface, &inst)) {
			return false;
		}
		conv_provider_desc d{};
		d.id = CONV_DP_LIFT_ID;
		d.probe_order = CONV_DP_LIFT_PROBE_ORDER;
		d.hosting = CONV_HOSTING_IN_SERVICE;
		d.enabled = true;
		d.flags = CONV_PROV_LICENCE_EXEMPT | CONV_PROV_LEGACY_UNITS;
		d.iface = iface;
		d.inst = inst;
		if (conv_core_add_provider(core, &d) < 0 || ipc_server_conv_create(core, &sc) != XRT_SUCCESS) {
			return false;
		}
		ipc_server_conv_io_ops io{};
		io.import_input = io_import;
		io.release_input = io_release;
		io.publish_result = io_publish;
		io.export_output = io_export;
		io.export_fence = io_fence;
		io.stream_gone = io_gone;
		io.import_input_ex = io_import_ex;
		io.export_cpu = io_export_cpu;
		io.ud = l;
		ipc_server_conv_set_io(sc, &io);
		return true;
	}

	//! A VIEWS_MONO task stream, 2 views (the golden SBS), export @p export_kind.
	uint64_t
	stream(uint32_t export_kind = 0)
	{
		xrt_conv_wire_stream_desc sd{};
		sd.struct_size = sizeof(sd);
		sd.family_version = XRT_CONV_WIRE_FAMILY_VERSION_CURRENT;
		sd.task = XRT_CONV_TASK_VIEWS_MONO;
		sd.input_kind = XRT_CONV_INPUT_MONO;
		sd.view_count = 2;
		sd.content_hint = XRT_CONV_CONTENT_VIDEO;
		sd.export_memory_kind = export_kind;
		xrt_conv_wire_stream_created c{};
		if (ipc_server_conv_v4_stream_create(sc, 1, &sd, 0, 0, 1.0f, &c) != XRT_SUCCESS) {
			return 0;
		}
		return c.stream_id;
	}

	/*!
	 * Submit with @p make_input (a fresh descriptor each try: the glue owns
	 * its handles) until a result comes back; the views export hashed.
	 */
	bool
	convert(uint64_t id,
	        const std::function<conv_input_import()> &make_in,
	        uint64_t *out_hash,
	        bool cpu_export = false)
	{
		bool ready = false;
		const auto t0 = std::chrono::steady_clock::now();
		while (!ready && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(30)) {
			ipc_arg_lift_submit a{};
			a.stream_id = id;
			a.source_time = 1000;
			a.width = kW;
			a.height = kH;
			a.has_params = 1;
			a.params.convergence = -1.0f;
			a.params.strength = 1.0f;
			a.params.inpaint = 1;
			a.params.view_count = 2;
			conv_input_import in = make_in();
			uint64_t fid = 0;
			const xrt_result_t sr = ipc_server_conv_v4_submit(sc, 1, &a, &in, nullptr, &fid);
			if (sr != XRT_SUCCESS && sr != XRT_ERROR_WEAVE_REFUSED) {
				UNSCOPED_INFO("submit: " << (int)sr);
				return false;
			}
			bool disc = false;
			ipc_lift_result r3{};
			xrt_conv_wire_result r4{};
			if (ipc_server_conv_v4_acquire(sc, 1, id, &ready, &disc, &r3, &r4) != XRT_SUCCESS) {
				ready = false;
			}
			if (!ready) {
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
		}
		if (!ready) {
			return false;
		}
		if (cpu_export) {
			xrt_graphics_buffer_handle_t mh{};
			uint32_t w = 0, h = 0, f = 0;
			uint64_t bytes = 0;
			if (!ipc_server_conv_v4_cpu_output(sc, 1, id, &mh, &w, &h, &f, &bytes) || w != 2 * kW ||
			    h != kH) {
				return false;
			}
			const uint8_t *v =
			    (const uint8_t *)MapViewOfFile((HANDLE)mh, FILE_MAP_READ, 0, 0, (SIZE_T)bytes);
			if (v == nullptr) {
				return false;
			}
			uint64_t hv = 1469598103934665603ull;
			for (uint32_t y = 0; y < h; y++) {
				hv = fnv1a(v + (size_t)y * w * 4u, (size_t)w * 4u, hv); // tightly packed rows
			}
			UnmapViewOfFile(v);
			*out_hash = hv;
			return true;
		}
		xrt_graphics_buffer_handle_t eh{};
		uint32_t ew = 0, ehh = 0, ef = 0;
		if (!ipc_server_conv_lift_export_output(sc, 1, id, false, &eh, &ew, &ehh, &ef)) {
			return false;
		}
		return hash_shared((HANDLE)eh, ew, ehh, out_hash);
	}

	bool
	hash_shared(HANDLE h, uint32_t w, uint32_t hgt, uint64_t *out)
	{
		ID3D11Texture2D *t = nullptr;
		if (FAILED(dev1->OpenSharedResource1(h, __uuidof(ID3D11Texture2D), (void **)&t))) {
			return false;
		}
		D3D11_TEXTURE2D_DESC td = {};
		t->GetDesc(&td);
		td.Usage = D3D11_USAGE_STAGING;
		td.BindFlags = 0;
		td.MiscFlags = 0;
		td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		ID3D11Texture2D *st = nullptr;
		bool ok = SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &st));
		if (ok) {
			std::lock_guard<std::mutex> lk(ctx_mtx);
			ctx->CopyResource(st, t);
			D3D11_MAPPED_SUBRESOURCE m = {};
			ok = SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m));
			if (ok) {
				uint64_t hv = 1469598103934665603ull;
				for (uint32_t y = 0; y < hgt; y++) {
					hv = fnv1a((const uint8_t *)m.pData + (size_t)y * m.RowPitch, (size_t)w * 4u,
					           hv);
				}
				*out = hv;
				ctx->Unmap(st, 0);
			}
		}
		rel(st);
		rel(t);
		return ok;
	}

	//! A D3D11 texture of @p tw x @p th with the input picture at (@p ox, @p oy).
	ID3D11Texture2D *
	texture(uint32_t tw, uint32_t th, uint32_t ox, uint32_t oy, bool keyed_mutex)
	{
		const std::vector<uint32_t> px = make_input();
		std::vector<uint32_t> big((size_t)tw * th, 0xff000000u);
		for (uint32_t y = 0; y < kH; y++) {
			memcpy(&big[(size_t)(oy + y) * tw + ox], &px[(size_t)y * kW], kW * 4u);
		}
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = tw;
		td.Height = th;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
		               (keyed_mutex ? D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX : D3D11_RESOURCE_MISC_SHARED);
		D3D11_SUBRESOURCE_DATA sd = {big.data(), tw * 4, 0};
		ID3D11Texture2D *t = nullptr;
		return SUCCEEDED(dev->CreateTexture2D(&td, &sd, &t)) ? t : nullptr;
	}
};

HANDLE
nt_handle(ID3D11Texture2D *t)
{
	IDXGIResource1 *r1 = nullptr;
	HANDLE h = nullptr;
	if (SUCCEEDED(t->QueryInterface(__uuidof(IDXGIResource1), (void **)&r1))) {
		(void)r1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
		                             &h);
	}
	rel(r1);
	return h;
}

HANDLE
dup(HANDLE h)
{
	HANDLE out = nullptr;
	DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &out, 0, FALSE, DUPLICATE_SAME_ACCESS);
	return out;
}

conv_input_import
base_in(uint32_t kind)
{
	conv_input_import in{};
	in.kind = kind;
	in.sync_handle = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
	in.w = kW;
	in.h = kH;
	return in;
}

//! A file mapping holding @p px (@p bgra: swizzled), rows of @p pitch bytes.
HANDLE
cpu_mapping(const std::vector<uint32_t> &px, uint32_t pitch, bool bgra)
{
	const uint64_t size = (uint64_t)pitch * kH;
	HANDLE m = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, (DWORD)size, nullptr);
	uint8_t *v = m != nullptr ? (uint8_t *)MapViewOfFile(m, FILE_MAP_WRITE, 0, 0, 0) : nullptr;
	if (v == nullptr) {
		return nullptr;
	}
	memset(v, 0x33, (size_t)size);
	for (uint32_t y = 0; y < kH; y++) {
		uint8_t *row = v + (size_t)y * pitch;
		memcpy(row, &px[(size_t)y * kW], kW * 4u);
		if (bgra) {
			for (uint32_t x = 0; x < kW; x++) {
				const uint8_t b = row[4 * x + 0];
				row[4 * x + 0] = row[4 * x + 2];
				row[4 * x + 2] = b;
			}
		}
	}
	UnmapViewOfFile(v);
	return m;
}

/*
 * D3D12: a shared committed texture written by a copy, ordered by a shared fence.
 */
struct D3D12Source
{
	ID3D12Device *dev = nullptr;
	ID3D12CommandQueue *q = nullptr;
	ID3D12CommandAllocator *alloc = nullptr;
	ID3D12GraphicsCommandList *list = nullptr;
	ID3D12Resource *tex = nullptr;
	ID3D12Resource *upload = nullptr;
	ID3D12Fence *fence = nullptr;
	HANDLE tex_handle = nullptr;
	HANDLE fence_handle = nullptr;
	uint64_t value = 0;

	~D3D12Source()
	{
		if (tex_handle != nullptr) {
			CloseHandle(tex_handle);
		}
		if (fence_handle != nullptr) {
			CloseHandle(fence_handle);
		}
		rel(fence);
		rel(upload);
		rel(tex);
		rel(list);
		rel(alloc);
		rel(q);
		rel(dev);
	}

	bool
	init()
	{
		IDXGIFactory4 *f = nullptr;
		IDXGIAdapter *warp = nullptr;
		bool ok =
		    SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void **)&f)) &&
		    SUCCEEDED(f->EnumWarpAdapter(__uuidof(IDXGIAdapter), (void **)&warp)) &&
		    SUCCEEDED(D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void **)&dev));
		rel(warp);
		rel(f);
		if (!ok) {
			return false;
		}
		D3D12_COMMAND_QUEUE_DESC qd = {};
		qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		ok = SUCCEEDED(dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void **)&q)) &&
		     SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
		                                           __uuidof(ID3D12CommandAllocator), (void **)&alloc)) &&
		     SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr,
		                                      __uuidof(ID3D12GraphicsCommandList), (void **)&list));
		if (!ok) {
			return false;
		}
		D3D12_HEAP_PROPERTIES hp = {};
		hp.Type = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_RESOURCE_DESC rd = {};
		rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		rd.Width = kW;
		rd.Height = kH;
		rd.DepthOrArraySize = 1;
		rd.MipLevels = 1;
		rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		rd.SampleDesc.Count = 1;
		rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
		ok = SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd,
		                                            D3D12_RESOURCE_STATE_COMMON, nullptr,
		                                            __uuidof(ID3D12Resource), (void **)&tex)) &&
		     SUCCEEDED(dev->CreateSharedHandle(tex, nullptr, GENERIC_ALL, nullptr, &tex_handle)) &&
		     SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence), (void **)&fence)) &&
		     SUCCEEDED(dev->CreateSharedHandle(fence, nullptr, GENERIC_ALL, nullptr, &fence_handle));
		if (!ok) {
			return false;
		}
		// The picture, through an upload buffer and a copy on the queue.
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
		UINT64 total = 0;
		dev->GetCopyableFootprints(&rd, 0, 1, 0, &fp, nullptr, nullptr, &total);
		D3D12_HEAP_PROPERTIES up = {};
		up.Type = D3D12_HEAP_TYPE_UPLOAD;
		D3D12_RESOURCE_DESC bd = {};
		bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bd.Width = total;
		bd.Height = 1;
		bd.DepthOrArraySize = 1;
		bd.MipLevels = 1;
		bd.SampleDesc.Count = 1;
		bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		ok = SUCCEEDED(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd,
		                                            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
		                                            __uuidof(ID3D12Resource), (void **)&upload));
		uint8_t *map = nullptr;
		ok = ok && SUCCEEDED(upload->Map(0, nullptr, (void **)&map));
		if (!ok) {
			return false;
		}
		const std::vector<uint32_t> px = make_input();
		for (uint32_t y = 0; y < kH; y++) {
			memcpy(map + fp.Offset + (size_t)y * fp.Footprint.RowPitch, &px[(size_t)y * kW], kW * 4u);
		}
		upload->Unmap(0, nullptr);
		D3D12_TEXTURE_COPY_LOCATION dst = {};
		dst.pResource = tex;
		dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		D3D12_TEXTURE_COPY_LOCATION src = {};
		src.pResource = upload;
		src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		src.PlacedFootprint = fp;
		list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
		ok = SUCCEEDED(list->Close());
		if (ok) {
			ID3D12CommandList *lists[] = {list};
			q->ExecuteCommandLists(1, lists);
			value = 1;
			ok = SUCCEEDED(q->Signal(fence, value)); // the service waits for this value
		}
		return ok;
	}
};

} // namespace


TEST_CASE("spec-4 transports on WARP reproduce the pinned SBS bytes", "[ipc_conv_v4_transports]")
{
	_putenv("SIM_DISPLAY_FAKE_LIFT=1");
	_putenv("SIM_DISPLAY_FAKE_LIFT_LATENCY_MS=0");
	Service svc;
	if (!svc.init()) {
		SKIP("no WARP D3D11 device");
	}
	// The kinds the service now offers to a spec-4 caller.
	CHECK((ipc_server_conv_v4_image_kinds(svc.sc, false) & (1ull << IPC_SERVER_CONV_IMAGE_KIND_D3D12_NT)) != 0u);
	CHECK((ipc_server_conv_v4_image_kinds(svc.sc, false) & (1ull << IPC_SERVER_CONV_IMAGE_KIND_CPU)) != 0u);
	CHECK((ipc_server_conv_v4_image_kinds(svc.sc, true) & (1ull << IPC_SERVER_CONV_IMAGE_KIND_CPU)) != 0u);

	SECTION("D3D11 NT through the spec-4 submit")
	{
		ID3D11Texture2D *t = svc.texture(kW, kH, 0, 0, true);
		REQUIRE(t != nullptr);
		const uint64_t id = svc.stream();
		REQUIRE(id != 0);
		uint64_t hv = 0;
		REQUIRE(svc.convert(
		    id,
		    [&] {
			    conv_input_import in = base_in(CONV_INPUT_NT);
			    in.handle = nt_handle(t);
			    return in;
		    },
		    &hv));
		CHECK(hv == kGoldenSbs);
		rel(t);
	}
	SECTION("a region offset: the picture inside a larger texture")
	{
		ID3D11Texture2D *t = svc.texture(kW + 24, kH + 10, 17, 9, true);
		REQUIRE(t != nullptr);
		const uint64_t id = svc.stream();
		REQUIRE(id != 0);
		uint64_t hv = 0;
		REQUIRE(svc.convert(
		    id,
		    [&] {
			    conv_input_import in = base_in(CONV_INPUT_NT);
			    in.handle = nt_handle(t);
			    in.x = 17;
			    in.y = 9;
			    return in;
		    },
		    &hv));
		CHECK(hv == kGoldenSbs);
		rel(t);
	}
	SECTION("an NT texture without a keyed mutex, ordered by a shared D3D11 fence")
	{
		ID3D11Texture2D *t = svc.texture(kW, kH, 0, 0, false);
		REQUIRE(t != nullptr);
		ID3D11Fence *fence = nullptr;
		REQUIRE(SUCCEEDED(
		    svc.dev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), (void **)&fence)));
		HANDLE fh = nullptr;
		REQUIRE(SUCCEEDED(fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fh)));
		{
			std::lock_guard<std::mutex> lk(svc.ctx_mtx);
			svc.ctx4->Signal(fence, 5);
		}
		const uint64_t id = svc.stream();
		REQUIRE(id != 0);
		uint64_t hv = 0;
		REQUIRE(svc.convert(
		    id,
		    [&] {
			    conv_input_import in = base_in(CONV_INPUT_NT);
			    in.handle = nt_handle(t);
			    in.sync_kind = XRT_CONV_SYNC_FENCE_NT;
			    in.sync_handle = dup(fh);
			    in.sync_value = 5;
			    return in;
		    },
		    &hv));
		CHECK(hv == kGoldenSbs);
		CloseHandle(fh);
		rel(fence);
		rel(t);
	}
	SECTION("a D3D12 resource + its shared ID3D12Fence")
	{
		D3D12Source d12;
		if (!d12.init()) {
			SKIP("no D3D12 WARP device");
		}
		const uint64_t id = svc.stream();
		REQUIRE(id != 0);
		uint64_t hv = 0;
		REQUIRE(svc.convert(
		    id,
		    [&] {
			    conv_input_import in = base_in(CONV_INPUT_NT);
			    in.handle = dup(d12.tex_handle);
			    in.sync_kind = XRT_CONV_SYNC_FENCE_NT;
			    in.sync_handle = dup(d12.fence_handle);
			    in.sync_value = d12.value;
			    return in;
		    },
		    &hv));
		CHECK(hv == kGoldenSbs);
	}
	SECTION("CPU bytes in a mapping: RGBA8, a padded pitch, BGRA8 swizzled")
	{
		const std::vector<uint32_t> px = make_input();
		for (bool bgra : {false, true}) {
			CAPTURE(bgra);
			const uint64_t id = svc.stream();
			REQUIRE(id != 0);
			uint64_t hv = 0;
			const uint32_t pitch = kW * 4u + 64u;
			REQUIRE(svc.convert(
			    id,
			    [&] {
				    conv_input_import in = base_in(CONV_INPUT_CPU);
				    in.handle = cpu_mapping(px, pitch, bgra);
				    in.cpu_row_pitch = pitch;
				    in.cpu_bgra = bgra;
				    return in;
			    },
			    &hv));
			CHECK(hv == kGoldenSbs);
		}
	}
	SECTION("a region past the image is refused, the handle disposed of")
	{
		ID3D11Texture2D *t = svc.texture(kW, kH, 0, 0, true);
		REQUIRE(t != nullptr);
		const uint64_t id = svc.stream();
		REQUIRE(id != 0);
		ipc_arg_lift_submit a{};
		a.stream_id = id;
		a.width = kW;
		a.height = kH;
		conv_input_import in = base_in(CONV_INPUT_NT);
		in.handle = nt_handle(t);
		in.x = kW; // starts outside the texture
		uint64_t fid = 0;
		CHECK(ipc_server_conv_v4_submit(svc.sc, 1, &a, &in, nullptr, &fid) == XRT_ERROR_OUTPUT_REQUEST_FAILURE);
		rel(t);
	}
	SECTION("CPU export: the same bytes, tightly packed, in a mapping")
	{
		ID3D11Texture2D *t = svc.texture(kW, kH, 0, 0, true);
		REQUIRE(t != nullptr);
		const uint64_t id = svc.stream(XRT_CONV_MEM_CPU);
		REQUIRE(id != 0);
		uint64_t hv = 0;
		REQUIRE(svc.convert(
		    id,
		    [&] {
			    conv_input_import in = base_in(CONV_INPUT_NT);
			    in.handle = nt_handle(t);
			    return in;
		    },
		    &hv, /*cpu_export*/ true));
		CHECK(hv == kGoldenSbs);
		rel(t);
	}
}
