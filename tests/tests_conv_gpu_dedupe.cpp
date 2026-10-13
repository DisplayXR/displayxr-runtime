// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  #2030: still-image dedupe of GPU PHOTO inputs, on WARP.
 *
 * The D3D11 glue keys a PHOTO stream's GPU input with the content check
 * (d3d11_conv_content_key.h: a private device, GenerateMips down to a
 * <= 16 x 16 block, asynchronous staging readback) and hands the key to the
 * core as conv_frame_submit::content_key. What must hold:
 *  - two identical inputs -> one conversion;
 *  - a changed pixel block -> another conversion, on the submit after the
 *    change (the readback is one submit late);
 *  - the key decides, not the slot's bytes (the core's CPU slot is scribbled
 *    differently every frame here, as a GPU slot would be invisible to it).
 *
 * SKIPs when no D3D11 device (WARP or hardware) exists.
 */

#include "catch_amalgamated.hpp"

#include "conv_fake_provider.h"

#include "conversion/conv_core.h"
#include "conversion/conv_stream.h"
#include "d3d11_service/d3d11_conv_content_key.h"

#include "os/os_time.h"

#include <d3d11_1.h>
#include <dxgi1_2.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <thread>
#include <vector>

namespace {

constexpr uint32_t kW = 256, kH = 256;

template <typename T>
void
rel(T *&p)
{
	if (p != nullptr) {
		p->Release();
		p = nullptr;
	}
}

bool
wait_until(const std::function<bool()> &pred, uint32_t timeout_ms)
{
	const uint64_t end = os_monotonic_get_ns() + (uint64_t)timeout_ms * 1000000ull;
	while (os_monotonic_get_ns() < end) {
		if (pred()) {
			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	return pred();
}

std::atomic<uint64_t> g_key{1};

bool
cpu_alloc(void *, const conv_image_request *req, xrt_conv_image *out)
{
	const uint64_t size = (uint64_t)req->width * req->height * 4u * (req->array_layers ? req->array_layers : 1);
	void *p = std::calloc(1, (size_t)size);
	if (p == nullptr) {
		return false;
	}
	std::memset(out, 0, sizeof(*out));
	out->memory_kind = XRT_CONV_MEM_CPU;
	out->cpu_ptr = p;
	out->byte_size = size;
	out->width = req->width;
	out->height = req->height;
	out->format = req->format;
	out->row_stride = req->width * 4u;
	out->import_key = g_key++;
	return true;
}

void
cpu_free(void *, xrt_conv_image *img)
{
	std::free(img->cpu_ptr);
	img->cpu_ptr = nullptr;
}

//! The "service" side: a shared NT-handle keyed-mutex slot image on WARP.
struct Gpu
{
	ID3D11Device *dev = nullptr;
	ID3D11DeviceContext *ctx = nullptr;
	ID3D11Texture2D *slot = nullptr;
	IDXGIKeyedMutex *km = nullptr;
	HANDLE share = nullptr;
	IDXGIAdapter *adapter = nullptr;

	~Gpu()
	{
		if (share != nullptr) {
			CloseHandle(share);
		}
		rel(km);
		rel(slot);
		rel(adapter);
		rel(ctx);
		rel(dev);
	}

	bool
	init()
	{
		HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
		                               &dev, nullptr, &ctx);
		if (FAILED(hr)) {
			hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
			                       D3D11_SDK_VERSION, &dev, nullptr, &ctx);
		}
		if (FAILED(hr)) {
			return false;
		}
		IDXGIDevice *dd = nullptr;
		if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&dd))) {
			dd->GetAdapter(&adapter);
		}
		rel(dd);
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = kW;
		td.Height = kH;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
		if (FAILED(dev->CreateTexture2D(&td, nullptr, &slot)) ||
		    FAILED(slot->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&km))) {
			return false;
		}
		IDXGIResource1 *r1 = nullptr;
		hr = slot->QueryInterface(__uuidof(IDXGIResource1), (void **)&r1);
		if (SUCCEEDED(hr)) {
			hr = r1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
			                            &share);
		}
		rel(r1);
		return SUCCEEDED(hr);
	}

	//! The S0 snapshot: picture A, or A with one 64 x 64 block changed.
	void
	fill(bool changed_block)
	{
		std::vector<uint8_t> px((size_t)kW * kH * 4);
		for (uint32_t y = 0; y < kH; y++) {
			for (uint32_t x = 0; x < kW; x++) {
				uint8_t *p = &px[((size_t)y * kW + x) * 4];
				const bool in_block = changed_block && x >= 64 && x < 128 && y >= 64 && y < 128;
				p[0] = in_block ? 250 : (uint8_t)x;
				p[1] = in_block ? 10 : (uint8_t)y;
				p[2] = 0x40;
				p[3] = 0xFF;
			}
		}
		REQUIRE(SUCCEEDED(km->AcquireSync(0, 1000)));
		ctx->UpdateSubresource(slot, 0, nullptr, px.data(), kW * 4, 0);
		REQUIRE(SUCCEEDED(km->ReleaseSync(0)));
	}
};

} // namespace

TEST_CASE("gpu dedupe: identical PHOTO inputs convert once; a changed block converts again", "[conv_gpu_dedupe][d3d11]")
{
	Gpu gpu;
	if (!gpu.init()) {
		SKIP("no D3D11 device (WARP or hardware) with shared NT keyed-mutex textures");
	}
	d3d11_conv_content_check *chk = d3d11_conv_content_check_create(gpu.adapter);
	REQUIRE(chk != nullptr);

	conv_fake_provider f;
	conv_fake_init(&f, "fake");
	conv_fake_cap *c = conv_fake_add_cap(&f, 4, XRT_CONV_TASK_VIEWS_MONO, XRT_CONV_LANE_STREAM,
	                                     XRT_CONV_OUT_VIEWS, XRT_CONV_CAP_EXPLICIT_VIEWPOINTS);
	c->convert_ms = 1;
	{
		conv_core_config cfg{};
		cfg.alloc.alloc = cpu_alloc;
		cfg.alloc.free = cpu_free;
		cfg.prewarm_on = CONV_PREWARM_FIRST_CREATE;
		cfg.prewarm_task_mask = 1ull << 63;
		cfg.rear_cap_m = -1.0f;
		cfg.tick_ms = 10;
		conv_core *core = nullptr;
		REQUIRE(conv_core_create(&cfg, &core) == XRT_SUCCESS);
		conv_provider_desc d{};
		d.id = f.id;
		d.probe_order = 100;
		d.enabled = true;
		d.iface = &f.iface;
		d.inst = conv_fake_inst(&f);
		REQUIRE(conv_core_add_provider(core, &d) >= 0);

		xrt_conv_wire_stream_desc sd{};
		sd.struct_size = sizeof(sd);
		sd.family_version = XRT_CONV_WIRE_FAMILY_VERSION_CURRENT;
		sd.task = XRT_CONV_TASK_VIEWS_MONO;
		sd.outputs = XRT_CONV_OUT_VIEWS;
		sd.view_count = 2;
		sd.views_layout = XRT_CONV_VIEWS_ROW;
		sd.input_kind = XRT_CONV_INPUT_MONO;
		sd.content_hint = XRT_CONV_CONTENT_PHOTO;
		xrt_conv_wire_stream_created cr{};
		REQUIRE(conv_core_stream_create(core, 1, &sd, nullptr, &cr) == XRT_SUCCESS);
		const uint64_t sid = cr.stream_id;

		auto stats = [&] {
			conv_stream_stats s{};
			REQUIRE(conv_core_stream_get_stats(core, sid, &s) == XRT_SUCCESS);
			return s;
		};
		uint8_t scribble = 0;
		std::vector<uint64_t> keys;
		// One page-rate submit: S0 into the GPU slot, the glue's key, commit.
		auto submit = [&](bool changed) {
			gpu.fill(changed);
			const uint64_t key = d3d11_conv_content_check_sample(chk, 7, gpu.share, 1234);
			keys.push_back(key);
			conv_submit_layout lay{};
			lay.image_count = 1;
			lay.width[0] = kW;
			lay.height[0] = kH;
			lay.format[0] = XRT_CONV_FMT_RGBA8_UNORM;
			lay.role[0] = XRT_CONV_IMG_COLOR;
			conv_submit_slot slot{};
			REQUIRE(conv_core_stream_begin_submit(core, sid, &lay, &slot) == XRT_SUCCESS);
			std::memset(slot.images[0].cpu_ptr, ++scribble, (size_t)slot.images[0].byte_size);
			conv_frame_submit fr{};
			fr.source_time = (int64_t)os_monotonic_get_ns();
			fr.content_key = key;
			uint64_t fid = 0;
			REQUIRE(conv_core_stream_commit_submit(core, sid, slot.slot, &fr, &fid) == XRT_SUCCESS);
			REQUIRE(wait_until(
			    [&] {
				    const conv_stream_stats s = stats();
				    return s.converted + s.deduped + s.dropped + s.failed >= s.submitted;
			    },
			    3000));
			// Page rate: the private device's tiny readback lands well within it.
			std::this_thread::sleep_for(std::chrono::milliseconds(40));
		};

		submit(false);
		REQUIRE(wait_until([&] { return stats().converted >= 1; }, 3000));
		submit(false);
		submit(false);
		CHECK(stats().converted == 1u); // two (three) identical inputs: one conversion
		CHECK(stats().deduped == 2u);

		submit(true);                   // the change: still last submit's key (one late)
		submit(true);                   // its key arrives: converts
		submit(true);
		CHECK(stats().converted == 2u);
		CHECK(stats().deduped == 4u);

		INFO("keys " << keys[0] << " " << keys[1] << " " << keys[3] << " " << keys[4]);
		CHECK(keys[0] != 0u);
		CHECK(keys[0] == keys[1]);
		CHECK(keys[0] == keys[3]);
		CHECK(keys[4] != keys[3]);
		CHECK(keys[4] != 0u);

		REQUIRE(conv_core_stream_destroy(core, sid) == XRT_SUCCESS);
		conv_core_destroy(&core);
	}
	d3d11_conv_content_check_forget_image(chk, 1234);
	d3d11_conv_content_check_forget_stream(chk, 7);
	d3d11_conv_content_check_destroy(&chk);
	CHECK(chk == nullptr);
	conv_fake_fini(&f);
}
