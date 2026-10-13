// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-053 (#2003, #2028) — the legacy lift output's golden replay on WARP.
 *
 * Drives the XR_DXR_lift v3 route the way the service does — ipc_server_conv
 * over the conversion core, the `dp-lift` adapter on d3d11_lift's one lift
 * thread, the D3D11 glue (d3d11_conv_gpu) for the slots / targets / export —
 * with the sim_display FAKE lift module behind a minimal display processor,
 * on WARP: a client-shared input texture is submitted, the result is acquired
 * into the client export texture, read back and hashed.
 *
 * The hashes were pinned on the PRE-CORE path before the conversion core took
 * over the lift thread (#2003), so the refactors (P1a, then the P1b split that
 * deleted the pre-core path) are checked against the shipping bytes, not
 * against themselves. The fake's pictures depend only on the input, the mode,
 * the view count and the strength (no eyes, no rect: the historical fan), so
 * the pin is deterministic.
 *
 * WARP means no GPU is required; no D3D11 device at all SKIPs.
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

#include <d3d11_1.h>
#include <dxgi1_2.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace {

constexpr uint32_t kW = 96, kH = 64;

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
 * A display processor that carries only the fake lift module (the lift-only
 * factory's contract: no weaver, no tracker).
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

//! The plug-in's factory pair: the fake as the lift-only factory, no fallback.
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

//! The input picture: a coordinate gradient + a bright square (no black bars).
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

struct Warp
{
	ID3D11Device *dev = nullptr;
	ID3D11Device1 *dev1 = nullptr;
	ID3D11DeviceContext *ctx = nullptr;
	ID3D11Texture2D *input = nullptr;

	~Warp()
	{
		rel(input);
		rel(ctx);
		rel(dev1);
		rel(dev);
	}

	bool
	init()
	{
		D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
		if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, fl,
		                             2, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
			return false;
		}
		if (FAILED(dev->QueryInterface(__uuidof(ID3D11Device1), (void **)&dev1))) {
			return false;
		}
		std::vector<uint32_t> px = make_input();
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = kW;
		td.Height = kH;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
		D3D11_SUBRESOURCE_DATA sd = {px.data(), kW * 4, 0};
		return SUCCEEDED(dev->CreateTexture2D(&td, &sd, &input));
	}

	//! A fresh NT handle to the input (the submit path takes ownership).
	HANDLE
	input_handle()
	{
		IDXGIResource1 *r1 = nullptr;
		HANDLE h = nullptr;
		if (SUCCEEDED(input->QueryInterface(__uuidof(IDXGIResource1), (void **)&r1))) {
			(void)r1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
			                             &h);
		}
		rel(r1);
		return h;
	}

	//! Read back a texture shared by NT handle; hash its rows.
	bool
	hash_shared(HANDLE h, uint32_t w, uint32_t hgt, uint32_t bpp, uint64_t *out)
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
			ctx->CopyResource(st, t);
			D3D11_MAPPED_SUBRESOURCE m = {};
			ok = SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m));
			if (ok) {
				uint64_t hv = 1469598103934665603ull;
				for (uint32_t y = 0; y < hgt; y++) {
					hv = fnv1a((const uint8_t *)m.pData + (size_t)y * m.RowPitch, (size_t)w * bpp, hv);
				}
				*out = hv;
				ctx->Unmap(st, 0);
			}
		}
		rel(st);
		rel(t);
		return ok;
	}
};

struct golden_result
{
	uint32_t w = 0, h = 0, format = 0, view_count = 0;
	uint64_t hash = 0;
};

/*
 * The core-routed path (#2003): the v3 adapter -> the conversion core -> the
 * dp-lift provider -> the lift thread, with the D3D11 glue.
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

bool
core_convert(Warp &wp, uint32_t mode, uint32_t view_count, golden_result *out)
{
	std::mutex ctx_mtx;
	d3d11_lift *l = d3d11_lift_create(wp.dev, wp.ctx, &ctx_mtx, golden_resolve, nullptr);
	if (l == nullptr) {
		return false;
	}
	conv_core_config cfg{};
	d3d11_lift_glue_alloc_ops(l, &cfg.alloc);
	d3d11_lift_glue_gpu_ops(l, &cfg.viewsyn, &cfg.readback);
	cfg.prewarm_on = CONV_PREWARM_FIRST_CREATE;
	cfg.rear_cap_m = -1.0f;
	cfg.tick_ms = 10;
	conv_core *core = nullptr;
	bool ok = conv_core_create(&cfg, &core) == XRT_SUCCESS;

	conv_dp_lift_backend be;
	d3d11_lift_get_dp_backend(l, &be);
	const xrt_conversion_plugin_iface *iface = nullptr;
	xrt_conversion_plugin_instance *inst = nullptr;
	ok = ok && conv_provider_legacy_dp_create(&be, &iface, &inst);
	if (ok) {
		conv_provider_desc d{};
		d.id = CONV_DP_LIFT_ID;
		d.probe_order = CONV_DP_LIFT_PROBE_ORDER;
		d.hosting = CONV_HOSTING_IN_SERVICE;
		d.enabled = true;
		d.flags = CONV_PROV_LICENCE_EXEMPT | CONV_PROV_LEGACY_UNITS;
		d.iface = iface;
		d.inst = inst;
		ok = conv_core_add_provider(core, &d) >= 0;
	}
	ipc_server_conv *sc = nullptr;
	ok = ok && ipc_server_conv_create(core, &sc) == XRT_SUCCESS;
	if (ok) {
		ipc_server_conv_io_ops io{};
		io.import_input = io_import;
		io.release_input = io_release;
		io.publish_result = io_publish;
		io.export_output = io_export;
		io.export_fence = io_fence;
		io.stream_gone = io_gone;
		io.ud = l;
		ipc_server_conv_set_io(sc, &io);
	}
	uint64_t id = 0;
	ok = ok && ipc_server_conv_routes_create(sc, mode, XRT_DP_LIFT_CONTENT_VIDEO) &&
	     ipc_server_conv_lift_stream_create(sc, 1, mode, XRT_DP_LIFT_CONTENT_VIDEO, 1.0f, 0, 0, &id) == XRT_SUCCESS;

	bool ready = false;
	ipc_lift_result r{};
	const auto t0 = std::chrono::steady_clock::now();
	while (ok && !ready && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(30)) {
		ipc_arg_lift_submit a{};
		a.stream_id = id;
		a.source_time = 1000;
		a.width = kW;
		a.height = kH;
		a.has_params = 1;
		a.params.convergence = -1.0f;
		a.params.strength = 1.0f;
		a.params.inpaint = 1;
		a.params.view_count = view_count;
		uint64_t fid = 0;
		const xrt_result_t sr = ipc_server_conv_lift_submit(
		    sc, 1, &a, (xrt_graphics_buffer_handle_t)wp.input_handle(), false, &fid);
		ok = sr == XRT_SUCCESS || sr == XRT_ERROR_WEAVE_REFUSED;
		ok = ok && ipc_server_conv_lift_acquire(sc, 1, id, &ready, &r) == XRT_SUCCESS;
		if (!ready) {
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
	}
	xrt_graphics_buffer_handle_t eh{};
	uint32_t ew = 0, ehh = 0, ef = 0;
	ok = ok && ready && ipc_server_conv_lift_export_output(sc, 1, id, false, &eh, &ew, &ehh, &ef);
	if (ok) {
		out->w = ew;
		out->h = ehh;
		out->format = ef;
		out->view_count = mode == XRT_DP_LIFT_MODE_DEPTH ? 0u : r.view_count;
		ok = wp.hash_shared((HANDLE)eh, ew, ehh, 4u, &out->hash);
	}
	// Teardown order (as the service does it): the route, the adapter backend,
	// the core (its streams free their images through the glue), the lift module.
	ipc_server_conv_destroy(&sc);
	if (inst != nullptr) {
		conv_provider_legacy_dp_detach(inst);
	}
	conv_core_destroy(&core);
	d3d11_lift_destroy(&l);
	return ok;
}

// Pinned on the pre-core path (commit before #2003's refactor), WARP.
constexpr uint64_t kGoldenSbs = 0x6c0461d3308e0febull;
constexpr uint64_t kGoldenNview4 = 0x79a52a4f4fbc3d43ull;
constexpr uint64_t kGoldenDepth = 0x1dbbe066f7193183ull;

} // namespace

TEST_CASE("lift_dp_golden: the core-routed dp-lift path reproduces the pinned pre-core bytes", "[lift][dp_golden]")
{
	_putenv("SIM_DISPLAY_FAKE_LIFT=1");
	_putenv("SIM_DISPLAY_FAKE_LIFT_LATENCY_MS=0");
	Warp wp;
	if (!wp.init()) {
		SKIP("no WARP D3D11 device");
	}
	golden_result sbs, nview, depth;
	REQUIRE(core_convert(wp, XRT_DP_LIFT_MODE_SBS, 2, &sbs));
	REQUIRE(core_convert(wp, XRT_DP_LIFT_MODE_NVIEW, 4, &nview));
	REQUIRE(core_convert(wp, XRT_DP_LIFT_MODE_DEPTH, 0, &depth));
	UNSCOPED_INFO("core SBS " << sbs.w << "x" << sbs.h << " hash 0x" << std::hex << sbs.hash);
	UNSCOPED_INFO("core NVIEW4 " << std::dec << nview.w << "x" << nview.h << " hash 0x" << std::hex << nview.hash);
	UNSCOPED_INFO("core DEPTH " << std::dec << depth.w << "x" << depth.h << " hash 0x" << std::hex << depth.hash);
	CHECK(sbs.w == 2 * kW);
	CHECK(sbs.h == kH);
	CHECK(sbs.view_count == 2);
	CHECK(nview.w == 4 * kW);
	CHECK(nview.view_count == 4);
	CHECK(depth.w == kW);
	CHECK(depth.format == DXGI_FORMAT_R32_FLOAT);
	CHECK(sbs.hash == kGoldenSbs);
	CHECK(nview.hash == kGoldenNview4);
	CHECK(depth.hash == kGoldenDepth);
}
