// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  D3D12 native swapchain implementation.
 * @author David Fattal
 * @ingroup comp_d3d12
 */

#include "comp_d3d12_swapchain.h"
#include "comp_d3d12_compositor.h"

#include "xrt/xrt_handles.h"

#include "util/u_logging.h"
#include "util/u_misc.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <cstring>

/*!
 * Maximum number of images in a swapchain.
 */
#define MAX_SWAPCHAIN_IMAGES 8

/*!
 * D3D12 swapchain structure.
 */
struct comp_d3d12_swapchain
{
	//! Base type - must be first!
	struct xrt_swapchain_native base;

	//! Parent compositor.
	struct comp_d3d12_compositor *c;

	//! D3D12 resources.
	ID3D12Resource *images[MAX_SWAPCHAIN_IMAGES];

	//! Number of images.
	uint32_t image_count;

	//! Creation info.
	struct xrt_swapchain_create_info info;

	//! Per-image state for multi-acquire tracking (#151).
	//! 0 = FREE, 1 = ACQUIRED, 2 = WAITED.
	uint8_t img_state[MAX_SWAPCHAIN_IMAGES];

	//! Round-robin hint for next acquire.
	uint32_t next_hint;
};

static constexpr uint8_t IMG_FREE = 0;
static constexpr uint8_t IMG_ACQUIRED = 1;
static constexpr uint8_t IMG_WAITED = 2;

// Access compositor internals
extern "C" {
struct comp_d3d12_compositor_internals
{
	struct xrt_compositor_native base;
	struct xrt_device *xdev;
	ID3D12Device *device;
	ID3D12CommandQueue *command_queue;
};
}

static inline struct comp_d3d12_compositor_internals *
get_internals(struct comp_d3d12_compositor *c)
{
	return reinterpret_cast<struct comp_d3d12_compositor_internals *>(c);
}

static inline struct comp_d3d12_swapchain *
d3d12_sc(struct xrt_swapchain *xsc)
{
	return reinterpret_cast<struct comp_d3d12_swapchain *>(xsc);
}

/*!
 * Convert format to DXGI format.
 */
static DXGI_FORMAT
xrt_format_to_dxgi(int64_t format)
{
	switch (format) {
	// Pass through DXGI formats directly
	case DXGI_FORMAT_R8G8B8A8_UNORM:        // 28
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:   // 29
	case DXGI_FORMAT_B8G8R8A8_UNORM:        // 87
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:   // 91
	case DXGI_FORMAT_R16G16B16A16_FLOAT:    // 10
	case DXGI_FORMAT_R16G16B16A16_UNORM:    // 11
	case DXGI_FORMAT_D24_UNORM_S8_UINT:     // 45
	case DXGI_FORMAT_D32_FLOAT:             // 40
	case DXGI_FORMAT_D16_UNORM:             // 55
	case DXGI_FORMAT_R10G10B10A2_UNORM:     // 24
		return static_cast<DXGI_FORMAT>(format);

	// Convert VK_FORMAT values to DXGI
	case 37: return DXGI_FORMAT_R8G8B8A8_UNORM;
	case 43: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	case 44: return DXGI_FORMAT_B8G8R8A8_UNORM;
	case 50: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
	case 64: return DXGI_FORMAT_R10G10B10A2_UNORM;
	case 97: return DXGI_FORMAT_R16G16B16A16_FLOAT;
	case 100: return DXGI_FORMAT_R32_FLOAT;
	case 129: return DXGI_FORMAT_D24_UNORM_S8_UINT;
	case 130: return DXGI_FORMAT_D32_FLOAT;

	default:
		U_LOG_W("Unknown format %lld, using RGBA8", (long long)format);
		return DXGI_FORMAT_R8G8B8A8_UNORM;
	}
}

/*
 *
 * xrt_swapchain member functions
 *
 */

static xrt_result_t
d3d12_swapchain_acquire_image(struct xrt_swapchain *xsc, uint32_t *out_index)
{
	struct comp_d3d12_swapchain *sc = d3d12_sc(xsc);

	// Find a FREE image starting from next_hint (#151: support multiple outstanding acquires).
	for (uint32_t offset = 0; offset < sc->image_count; offset++) {
		uint32_t idx = (sc->next_hint + offset) % sc->image_count;
		if (sc->img_state[idx] == IMG_FREE) {
			sc->img_state[idx] = IMG_ACQUIRED;
			sc->next_hint = (idx + 1) % sc->image_count;
			*out_index = idx;
			return XRT_SUCCESS;
		}
	}

	U_LOG_E("No FREE swapchain image available (all %u acquired/waited)", sc->image_count);
	return XRT_ERROR_IPC_FAILURE;
}

static xrt_result_t
d3d12_swapchain_wait_image(struct xrt_swapchain *xsc, int64_t timeout_ns, uint32_t index)
{
	struct comp_d3d12_swapchain *sc = d3d12_sc(xsc);
	(void)timeout_ns;

	if (index >= sc->image_count || sc->img_state[index] != IMG_ACQUIRED) {
		U_LOG_E("Wait on non-acquired image index %u (state=%u)", index,
		        index < sc->image_count ? sc->img_state[index] : 0xff);
		return XRT_ERROR_IPC_FAILURE;
	}

	sc->img_state[index] = IMG_WAITED;
	return XRT_SUCCESS;
}

static xrt_result_t
d3d12_swapchain_barrier_image(struct xrt_swapchain *xsc, enum xrt_barrier_direction direction, uint32_t index)
{
	// D3D12 native compositor: app and compositor share the same device.
	// Resource barriers are managed by the compositor at commit time.
	(void)xsc;
	(void)direction;
	(void)index;
	return XRT_SUCCESS;
}

static xrt_result_t
d3d12_swapchain_release_image(struct xrt_swapchain *xsc, uint32_t index)
{
	struct comp_d3d12_swapchain *sc = d3d12_sc(xsc);

	if (index >= sc->image_count || sc->img_state[index] != IMG_WAITED) {
		U_LOG_E("Release on non-waited image index %u (state=%u)", index,
		        index < sc->image_count ? sc->img_state[index] : 0xff);
		return XRT_ERROR_IPC_FAILURE;
	}

	sc->img_state[index] = IMG_FREE;
	return XRT_SUCCESS;
}

static void
d3d12_swapchain_destroy(struct xrt_swapchain *xsc)
{
	struct comp_d3d12_swapchain *sc = d3d12_sc(xsc);

	for (uint32_t i = 0; i < sc->image_count; i++) {
		if (sc->images[i] != nullptr) {
			sc->images[i]->Release();
		}
	}

	delete sc;
}

/*
 *
 * Exported functions
 *
 */

extern "C" xrt_result_t
comp_d3d12_swapchain_create(struct comp_d3d12_compositor *c,
                            const struct xrt_swapchain_create_info *info,
                            struct xrt_swapchain **out_xsc)
{
	auto internals = get_internals(c);

	uint32_t image_count = 3; // Triple buffering
	if (image_count > MAX_SWAPCHAIN_IMAGES) {
		image_count = MAX_SWAPCHAIN_IMAGES;
	}

	comp_d3d12_swapchain *sc = new comp_d3d12_swapchain();
	memset(sc, 0, sizeof(*sc));

	sc->c = c;
	sc->info = *info;
	sc->image_count = image_count;
	// All images start FREE (memset already zeroed, but explicit for clarity).
	for (uint32_t i = 0; i < image_count; i++) {
		sc->img_state[i] = IMG_FREE;
	}
	sc->next_hint = 0;

	DXGI_FORMAT dxgi_format = xrt_format_to_dxgi(info->format);

	// Determine D3D12 resource flags
	D3D12_RESOURCE_FLAGS resource_flags = D3D12_RESOURCE_FLAG_NONE;
	if (info->bits & XRT_SWAPCHAIN_USAGE_COLOR) {
		resource_flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	}
	if (info->bits & XRT_SWAPCHAIN_USAGE_DEPTH_STENCIL) {
		resource_flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
	}
	if (info->bits & XRT_SWAPCHAIN_USAGE_UNORDERED_ACCESS) {
		resource_flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	}

	// For depth formats that need DSV, use typeless format for resource creation
	DXGI_FORMAT resource_format = dxgi_format;
	bool is_depth = (info->bits & XRT_SWAPCHAIN_USAGE_DEPTH_STENCIL) != 0;

	if (is_depth) {
		switch (dxgi_format) {
		case DXGI_FORMAT_D24_UNORM_S8_UINT:
			resource_format = DXGI_FORMAT_R24G8_TYPELESS;
			break;
		case DXGI_FORMAT_D32_FLOAT:
			resource_format = DXGI_FORMAT_R32_TYPELESS;
			break;
		case DXGI_FORMAT_D16_UNORM:
			resource_format = DXGI_FORMAT_R16_TYPELESS;
			break;
		default:
			break;
		}
	}

	// Create committed resources
	D3D12_HEAP_PROPERTIES heap_props = {};
	heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC res_desc = {};
	res_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	res_desc.Width = info->width;
	res_desc.Height = info->height;
	res_desc.DepthOrArraySize = info->array_size > 0 ? info->array_size : 1;
	res_desc.MipLevels = info->mip_count > 0 ? info->mip_count : 1;
	res_desc.Format = resource_format;
	res_desc.SampleDesc.Count = info->sample_count > 0 ? info->sample_count : 1;
	res_desc.SampleDesc.Quality = 0;
	res_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	res_desc.Flags = resource_flags;

	D3D12_RESOURCE_STATES initial_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
	if (is_depth) {
		initial_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
	}

	D3D12_CLEAR_VALUE clear_value = {};
	clear_value.Format = dxgi_format;
	D3D12_CLEAR_VALUE *clear_ptr = nullptr;

	if (resource_flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) {
		clear_value.Color[0] = 0.0f;
		clear_value.Color[1] = 0.0f;
		clear_value.Color[2] = 0.0f;
		clear_value.Color[3] = 1.0f;
		clear_ptr = &clear_value;
	} else if (resource_flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) {
		clear_value.DepthStencil.Depth = 1.0f;
		clear_value.DepthStencil.Stencil = 0;
		clear_ptr = &clear_value;
	}

	for (uint32_t i = 0; i < image_count; i++) {
		HRESULT hr = internals->device->CreateCommittedResource(
		    &heap_props, D3D12_HEAP_FLAG_NONE, &res_desc, initial_state,
		    clear_ptr, __uuidof(ID3D12Resource),
		    reinterpret_cast<void **>(&sc->images[i]));
		if (FAILED(hr)) {
			U_LOG_E("Failed to create swapchain resource %u: 0x%08x", i, hr);
			d3d12_swapchain_destroy(&sc->base.base);
			return XRT_ERROR_D3D;
		}

		// Store resource pointer as native image handle
		sc->base.images[i].handle = reinterpret_cast<xrt_graphics_buffer_handle_t>(sc->images[i]);
		sc->base.images[i].size = 0;
		sc->base.images[i].use_dedicated_allocation = false;
		sc->base.images[i].is_dxgi_handle = false;
	}

	// Set up swapchain interface
	sc->base.base.image_count = image_count;
	sc->base.base.wait_image = d3d12_swapchain_wait_image;
	sc->base.base.acquire_image = d3d12_swapchain_acquire_image;
	sc->base.base.barrier_image = d3d12_swapchain_barrier_image;
	sc->base.base.release_image = d3d12_swapchain_release_image;
	sc->base.base.destroy = d3d12_swapchain_destroy;
	sc->base.base.reference.count = 1;

	*out_xsc = &sc->base.base;

	U_LOG_I("Created D3D12 swapchain: %ux%u, %u images, format %d",
	        info->width, info->height, image_count, (int)dxgi_format);

	return XRT_SUCCESS;
}

extern "C" void
comp_d3d12_swapchain_get_dimensions(struct xrt_swapchain *xsc, uint32_t *out_w, uint32_t *out_h)
{
	struct comp_d3d12_swapchain *sc = d3d12_sc(xsc);
	*out_w = sc->info.width;
	*out_h = sc->info.height;
}

extern "C" void *
comp_d3d12_swapchain_get_resource(struct xrt_swapchain *xsc, uint32_t index)
{
	struct comp_d3d12_swapchain *sc = d3d12_sc(xsc);
	if (index >= sc->image_count) {
		return nullptr;
	}
	return sc->images[index];
}
