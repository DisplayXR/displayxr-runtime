// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  D3D11 NT-handle import path of the sim conversion provider (see
 *         sim_conv_d3d11_io.h).
 * @ingroup drv_sim_conversion
 */

#include "sim_conv_d3d11_io.h"

#include "util/u_logging.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>

#include <cstring>
#include <vector>

namespace {

//! An import unseen for this many reads/writes is dropped (contract §2.4).
constexpr uint64_t kCacheMaxAge = 64;
constexpr uint32_t kSlots = 4;

template <typename T>
void
safe_release(T *&p)
{
	if (p != nullptr) {
		p->Release();
		p = nullptr;
	}
}

struct cached_tex
{
	uint64_t key;
	ID3D11Texture2D *tex;
	uint64_t last_used;
};

struct cached_fence
{
	uint64_t key;
	ID3D11Fence *fence;
	uint64_t last_used;
};

uint32_t
fmt_bytes(uint32_t f)
{
	switch (f) {
	case XRT_CONV_FMT_R8_UNORM: return 1;
	case XRT_CONV_FMT_R16_FLOAT:
	case XRT_CONV_FMT_R16_UNORM: return 2;
	case XRT_CONV_FMT_RGBA16_FLOAT: return 8;
	case XRT_CONV_FMT_RGBA8_UNORM:
	case XRT_CONV_FMT_RGBA8_SRGB:
	case XRT_CONV_FMT_BGRA8_UNORM:
	case XRT_CONV_FMT_BGRA8_SRGB:
	case XRT_CONV_FMT_R32_FLOAT: return 4;
	default: return 0;
	}
}

} // namespace

struct sim_conv_d3d11_io
{
	uint64_t luid;
	ID3D11Device *device;
	ID3D11Device1 *device1;
	ID3D11Device5 *device5;
	ID3D11DeviceContext *ctx;
	ID3D11DeviceContext4 *ctx4;
	ID3D11Query *drain;
	uint64_t tick;
	std::vector<cached_tex> textures;
	std::vector<cached_fence> fences;
	std::vector<uint8_t> slot_buf[kSlots];
	ID3D11Texture2D *staging;
	D3D11_TEXTURE2D_DESC staging_desc;
};

static void
io_release_device(struct sim_conv_d3d11_io *io)
{
	sim_conv_d3d11_io_clear_cache(io);
	safe_release(io->staging);
	safe_release(io->drain);
	safe_release(io->ctx4);
	safe_release(io->ctx);
	safe_release(io->device5);
	safe_release(io->device1);
	safe_release(io->device);
	io->luid = 0;
}

static bool
io_ensure_device(struct sim_conv_d3d11_io *io, uint64_t luid)
{
	if (io->device != nullptr && (luid == 0 || luid == io->luid)) {
		return true;
	}
	io_release_device(io);

	IDXGIAdapter1 *adapter = nullptr;
	if (luid != 0) {
		IDXGIFactory1 *factory = nullptr;
		if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory)))) {
			IDXGIAdapter1 *a = nullptr;
			for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
				DXGI_ADAPTER_DESC1 d = {};
				a->GetDesc1(&d);
				const uint64_t l =
				    ((uint64_t)(uint32_t)d.AdapterLuid.HighPart << 32) | d.AdapterLuid.LowPart;
				if (l == luid) {
					adapter = a;
					break;
				}
				a->Release();
			}
			factory->Release();
		}
	}

	const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
	HRESULT hr = D3D11CreateDevice(adapter, adapter != nullptr ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
	                               nullptr, 0, levels, 2, D3D11_SDK_VERSION, &io->device, nullptr, &io->ctx);
	safe_release(adapter);
	if (FAILED(hr)) {
		hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
		                       &io->device, nullptr, &io->ctx);
	}
	if (FAILED(hr) ||
	    FAILED(io->device->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void **>(&io->device1)))) {
		U_LOG_E("sim conversion: no D3D11.1 device for the NT-handle path (hr 0x%08lx)", (unsigned long)hr);
		io_release_device(io);
		return false;
	}
	// Fences are optional (Windows 10 1703+); SYNC_FENCE_NT images fail without them.
	io->device->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void **>(&io->device5));
	io->ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void **>(&io->ctx4));
	D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
	io->device->CreateQuery(&qd, &io->drain);
	io->luid = luid;
	return true;
}

static void
io_age_cache(struct sim_conv_d3d11_io *io)
{
	io->tick++;
	for (size_t i = 0; i < io->textures.size();) {
		if (io->tick - io->textures[i].last_used > kCacheMaxAge) {
			safe_release(io->textures[i].tex);
			io->textures.erase(io->textures.begin() + (ptrdiff_t)i);
		} else {
			i++;
		}
	}
	for (size_t i = 0; i < io->fences.size();) {
		if (io->tick - io->fences[i].last_used > kCacheMaxAge) {
			safe_release(io->fences[i].fence);
			io->fences.erase(io->fences.begin() + (ptrdiff_t)i);
		} else {
			i++;
		}
	}
}

static ID3D11Texture2D *
io_open_texture(struct sim_conv_d3d11_io *io, const struct xrt_conv_image *img)
{
	if (img->memory_kind != XRT_CONV_MEM_WIN32_NT_HANDLE || img->handle == 0 ||
	    !io_ensure_device(io, img->adapter_luid)) {
		return nullptr;
	}
	for (auto &c : io->textures) {
		if (c.key == img->import_key && img->import_key != 0) {
			c.last_used = io->tick;
			return c.tex;
		}
	}
	ID3D11Texture2D *tex = nullptr;
	if (FAILED(io->device1->OpenSharedResource1(reinterpret_cast<HANDLE>(img->handle), __uuidof(ID3D11Texture2D),
	                                            reinterpret_cast<void **>(&tex)))) {
		return nullptr;
	}
	if (img->import_key != 0) {
		io->textures.push_back({img->import_key, tex, io->tick});
	} else {
		// No key: cannot be recognised again; keep it for this call only.
		io->textures.push_back({0, tex, 0});
	}
	return tex;
}

static ID3D11Fence *
io_open_fence(struct sim_conv_d3d11_io *io, const struct xrt_conv_sync *sync)
{
	if (sync->kind != XRT_CONV_SYNC_FENCE_NT || sync->handle == 0 || io->device5 == nullptr ||
	    io->ctx4 == nullptr) {
		return nullptr;
	}
	for (auto &c : io->fences) {
		if (c.key == sync->import_key && sync->import_key != 0) {
			c.last_used = io->tick;
			return c.fence;
		}
	}
	ID3D11Fence *f = nullptr;
	if (FAILED(io->device5->OpenSharedFence(reinterpret_cast<HANDLE>(sync->handle), __uuidof(ID3D11Fence),
	                                        reinterpret_cast<void **>(&f)))) {
		return nullptr;
	}
	io->fences.push_back({sync->import_key, f, sync->import_key != 0 ? io->tick : 0});
	return f;
}

//! Wait the image's acquire point; false when its sync kind cannot be honoured.
static bool
io_acquire(struct sim_conv_d3d11_io *io, const struct xrt_conv_image *img)
{
	if (img->sync.kind == XRT_CONV_SYNC_NONE) {
		return true;
	}
	ID3D11Fence *f = io_open_fence(io, &img->sync);
	if (f == nullptr) {
		return false;
	}
	return SUCCEEDED(io->ctx4->Wait(f, img->sync.acquire_value));
}

extern "C" struct sim_conv_d3d11_io *
sim_conv_d3d11_io_create(void)
{
	return new sim_conv_d3d11_io{};
}

extern "C" void
sim_conv_d3d11_io_destroy(struct sim_conv_d3d11_io *io)
{
	if (io == nullptr) {
		return;
	}
	io_release_device(io);
	delete io;
}

extern "C" void
sim_conv_d3d11_io_clear_cache(struct sim_conv_d3d11_io *io)
{
	if (io == nullptr) {
		return;
	}
	for (auto &c : io->textures) {
		safe_release(c.tex);
	}
	io->textures.clear();
	for (auto &c : io->fences) {
		safe_release(c.fence);
	}
	io->fences.clear();
}

extern "C" bool
sim_conv_d3d11_io_read(struct sim_conv_d3d11_io *io,
                       const struct xrt_conv_image *img,
                       uint32_t slot,
                       const uint8_t **out_pixels,
                       uint32_t *out_stride)
{
	if (io == nullptr || img == nullptr || slot >= kSlots || out_pixels == nullptr || out_stride == nullptr) {
		return false;
	}
	io_age_cache(io);
	ID3D11Texture2D *tex = io_open_texture(io, img);
	const uint32_t bpp = fmt_bytes(img->format);
	if (tex == nullptr || bpp == 0 || !io_acquire(io, img)) {
		return false;
	}
	D3D11_TEXTURE2D_DESC td = {};
	tex->GetDesc(&td);
	if (td.Width < img->width || td.Height < img->height || img->array_layer >= td.ArraySize) {
		return false;
	}
	D3D11_TEXTURE2D_DESC sd = {};
	sd.Width = img->width;
	sd.Height = img->height;
	sd.MipLevels = 1;
	sd.ArraySize = 1;
	sd.Format = td.Format;
	sd.SampleDesc.Count = 1;
	sd.Usage = D3D11_USAGE_STAGING;
	sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	if (io->staging == nullptr || memcmp(&sd, &io->staging_desc, sizeof(sd)) != 0) {
		safe_release(io->staging);
		if (FAILED(io->device->CreateTexture2D(&sd, nullptr, &io->staging))) {
			return false;
		}
		io->staging_desc = sd;
	}
	D3D11_BOX box = {0, 0, 0, img->width, img->height, 1};
	io->ctx->CopySubresourceRegion(io->staging, 0, 0, 0, 0, tex,
	                               D3D11CalcSubresource(0, img->array_layer, td.MipLevels), &box);
	D3D11_MAPPED_SUBRESOURCE map = {};
	if (FAILED(io->ctx->Map(io->staging, 0, D3D11_MAP_READ, 0, &map))) {
		return false;
	}
	const uint32_t stride = img->width * bpp;
	std::vector<uint8_t> &buf = io->slot_buf[slot];
	buf.resize((size_t)stride * img->height);
	for (uint32_t y = 0; y < img->height; y++) {
		memcpy(buf.data() + (size_t)y * stride,
		       static_cast<const uint8_t *>(map.pData) + (size_t)y * map.RowPitch, stride);
	}
	io->ctx->Unmap(io->staging, 0);
	*out_pixels = buf.data();
	*out_stride = stride;
	return true;
}

extern "C" bool
sim_conv_d3d11_io_write(struct sim_conv_d3d11_io *io,
                        const struct xrt_conv_image *img,
                        int32_t layer_override,
                        const void *pixels,
                        uint32_t stride)
{
	if (io == nullptr || img == nullptr || pixels == nullptr) {
		return false;
	}
	io_age_cache(io);
	ID3D11Texture2D *tex = io_open_texture(io, img);
	if (tex == nullptr) {
		return false;
	}
	// Order our writes after whoever produced / last read the target.
	if (!io_acquire(io, img)) {
		return false;
	}
	D3D11_TEXTURE2D_DESC td = {};
	tex->GetDesc(&td);
	const uint32_t layer = layer_override >= 0 ? (uint32_t)layer_override : img->array_layer;
	if (layer >= td.ArraySize || td.Usage != D3D11_USAGE_DEFAULT) {
		return false;
	}
	D3D11_BOX box = {
	    0, 0, 0, img->width < td.Width ? img->width : td.Width, img->height < td.Height ? img->height : td.Height,
	    1};
	io->ctx->UpdateSubresource(tex, D3D11CalcSubresource(0, layer, td.MipLevels), &box, pixels, stride, 0);
	return true;
}

extern "C" bool
sim_conv_d3d11_io_finish(struct sim_conv_d3d11_io *io, const struct xrt_conv_image *const *written, uint32_t count)
{
	if (io == nullptr || io->ctx == nullptr) {
		return false;
	}
	bool drain = false;
	bool ok = true;
	for (uint32_t i = 0; i < count; i++) {
		const struct xrt_conv_image *img = written[i];
		if (img == nullptr || img->memory_kind != XRT_CONV_MEM_WIN32_NT_HANDLE) {
			continue;
		}
		if (img->sync.kind == XRT_CONV_SYNC_FENCE_NT) {
			ID3D11Fence *f = io_open_fence(io, &img->sync);
			ok = ok && f != nullptr && SUCCEEDED(io->ctx4->Signal(f, img->sync.release_value));
		} else {
			drain = true;
		}
	}
	io->ctx->Flush();
	if (drain && io->drain != nullptr) {
		io->ctx->End(io->drain);
		BOOL done = FALSE;
		while (io->ctx->GetData(io->drain, &done, sizeof(done), 0) == S_FALSE) {
			Sleep(0);
		}
	}
	// Keyless imports were for this call only.
	for (size_t i = 0; i < io->textures.size();) {
		if (io->textures[i].key == 0) {
			safe_release(io->textures[i].tex);
			io->textures.erase(io->textures.begin() + (ptrdiff_t)i);
		} else {
			i++;
		}
	}
	return ok;
}
