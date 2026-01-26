// Copyright 2024-2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  D3D11 native swapchain implementation.
 * @author David Fattal
 * @ingroup comp_d3d11
 */

#include "comp_d3d11_swapchain.h"
#include "comp_d3d11_compositor.h"

#include "xrt/xrt_handles.h"

#include "util/u_logging.h"
#include "util/u_misc.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>

#include <cstring>

/*!
 * Maximum number of images in a swapchain.
 */
#define MAX_SWAPCHAIN_IMAGES 8

/*!
 * D3D11 swapchain structure.
 */
struct comp_d3d11_swapchain
{
	//! Base type - must be first!
	struct xrt_swapchain_native base;

	//! Parent compositor.
	struct comp_d3d11_compositor *c;

	//! D3D11 textures.
	ID3D11Texture2D *images[MAX_SWAPCHAIN_IMAGES];

	//! Shader resource views for each image.
	ID3D11ShaderResourceView *srvs[MAX_SWAPCHAIN_IMAGES];

	//! Render target views for each image.
	ID3D11RenderTargetView *rtvs[MAX_SWAPCHAIN_IMAGES];

	//! Number of images.
	uint32_t image_count;

	//! Creation info.
	struct xrt_swapchain_create_info info;

	//! Currently acquired image index (-1 if none).
	int32_t acquired_index;

	//! Currently waited image index (-1 if none).
	int32_t waited_index;

	//! Last released image index (for round-robin).
	uint32_t last_released_index;
};

// Access compositor internals
extern "C" {
struct comp_d3d11_compositor_internals
{
	struct xrt_compositor_native base;
	struct xrt_device *xdev;
	ID3D11Device5 *device;
	ID3D11DeviceContext4 *context;
	IDXGIFactory4 *dxgi_factory;
};
}

static inline struct comp_d3d11_compositor_internals *
get_internals(struct comp_d3d11_compositor *c)
{
	return reinterpret_cast<struct comp_d3d11_compositor_internals *>(c);
}

static inline struct comp_d3d11_swapchain *
d3d11_sc(struct xrt_swapchain *xsc)
{
	return reinterpret_cast<struct comp_d3d11_swapchain *>(xsc);
}

/*!
 * Convert format to DXGI format.
 *
 * The D3D11 native compositor enumerates DXGI formats directly, so apps using
 * XR_KHR_D3D11_enable will pass DXGI format values. We detect these and pass
 * them through. For Vulkan format values (used when going through the Vulkan
 * compositor path), we convert to the equivalent DXGI format.
 */
static DXGI_FORMAT
xrt_format_to_dxgi(int64_t format)
{
	// Check if this is already a DXGI format (common D3D11 formats are < 130)
	// DXGI formats we enumerate: 28, 29, 87, 91, 10, 11, 45, 40, 55
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

	// Convert VK_FORMAT values to DXGI (for Vulkan compositor interop)
	case 37: // VK_FORMAT_R8G8B8A8_UNORM
		return DXGI_FORMAT_R8G8B8A8_UNORM;
	case 43: // VK_FORMAT_R8G8B8A8_SRGB
		return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	case 44: // VK_FORMAT_B8G8R8A8_UNORM
		return DXGI_FORMAT_B8G8R8A8_UNORM;
	case 50: // VK_FORMAT_B8G8R8A8_SRGB
		return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
	case 64: // VK_FORMAT_A2B10G10R10_UNORM_PACK32
		return DXGI_FORMAT_R10G10B10A2_UNORM;
	case 97: // VK_FORMAT_R16G16B16A16_SFLOAT
		return DXGI_FORMAT_R16G16B16A16_FLOAT;
	case 100: // VK_FORMAT_R32_SFLOAT
		return DXGI_FORMAT_R32_FLOAT;
	case 129: // VK_FORMAT_D24_UNORM_S8_UINT
		return DXGI_FORMAT_D24_UNORM_S8_UINT;
	case 130: // VK_FORMAT_D32_SFLOAT
		return DXGI_FORMAT_D32_FLOAT;

	default:
		U_LOG_W("Unknown format %ld, using RGBA8", format);
		return DXGI_FORMAT_R8G8B8A8_UNORM;
	}
}

/*
 *
 * xrt_swapchain member functions
 *
 */

static xrt_result_t
d3d11_swapchain_acquire_image(struct xrt_swapchain *xsc, uint32_t *out_index)
{
	struct comp_d3d11_swapchain *sc = d3d11_sc(xsc);

	// Debug logging for first few acquires
	static int acquire_count = 0;
	if (acquire_count < 10) {
		U_LOG_W("[DEBUG] d3d11_swapchain_acquire_image: sc=%p, image_count=%u, last_released=%u, acquired_index=%d",
		        (void *)sc, sc->image_count, sc->last_released_index, sc->acquired_index);
		acquire_count++;
	}

	if (sc->acquired_index >= 0) {
		U_LOG_E("Image already acquired");
		return XRT_ERROR_IPC_FAILURE;
	}

	// Round-robin acquisition using per-swapchain counter
	// Find next available index (start from last released + 1)
	uint32_t index = (sc->last_released_index + 1) % sc->image_count;

	sc->acquired_index = static_cast<int32_t>(index);
	*out_index = index;

	if (acquire_count <= 10) {
		U_LOG_W("[DEBUG] d3d11_swapchain_acquire_image: returning index=%u", index);
	}

	return XRT_SUCCESS;
}

static xrt_result_t
d3d11_swapchain_wait_image(struct xrt_swapchain *xsc, int64_t timeout_ns, uint32_t index)
{
	struct comp_d3d11_swapchain *sc = d3d11_sc(xsc);
	(void)timeout_ns;

	// Debug logging for first few waits
	static int wait_count = 0;
	if (wait_count < 10) {
		U_LOG_W("[DEBUG] d3d11_swapchain_wait_image: sc=%p, index=%u, acquired_index=%d",
		        (void *)sc, index, sc->acquired_index);
		wait_count++;
	}

	if (sc->acquired_index < 0) {
		U_LOG_E("No image acquired");
		return XRT_ERROR_IPC_FAILURE;
	}

	if (static_cast<uint32_t>(sc->acquired_index) != index) {
		U_LOG_E("Wait index %u doesn't match acquired index %d", index, sc->acquired_index);
		return XRT_ERROR_IPC_FAILURE;
	}

	sc->waited_index = sc->acquired_index;
	sc->acquired_index = -1;

	return XRT_SUCCESS;
}

static xrt_result_t
d3d11_swapchain_barrier_image(struct xrt_swapchain *xsc, enum xrt_barrier_direction direction, uint32_t index)
{
	// D3D11 native compositor: app and compositor share the same D3D11 device and textures
	// No explicit barrier/synchronization is needed - D3D11 handles this internally
	(void)xsc;
	(void)direction;
	(void)index;
	return XRT_SUCCESS;
}

static xrt_result_t
d3d11_swapchain_release_image(struct xrt_swapchain *xsc, uint32_t index)
{
	struct comp_d3d11_swapchain *sc = d3d11_sc(xsc);

	// Debug logging for first few releases
	static int release_count = 0;
	if (release_count < 10) {
		U_LOG_W("[DEBUG] d3d11_swapchain_release_image: sc=%p, index=%u, waited_index=%d",
		        (void *)sc, index, sc->waited_index);
		release_count++;
	}

	if (sc->waited_index < 0) {
		U_LOG_E("No image to release");
		return XRT_ERROR_IPC_FAILURE;
	}

	if (static_cast<uint32_t>(sc->waited_index) != index) {
		U_LOG_E("Release index %u doesn't match waited index %d", index, sc->waited_index);
		return XRT_ERROR_IPC_FAILURE;
	}

	// Track the last released index for round-robin acquisition
	sc->last_released_index = index;
	sc->waited_index = -1;

	return XRT_SUCCESS;
}

static void
d3d11_swapchain_destroy(struct xrt_swapchain *xsc)
{
	struct comp_d3d11_swapchain *sc = d3d11_sc(xsc);

	for (uint32_t i = 0; i < sc->image_count; i++) {
		if (sc->rtvs[i] != nullptr) {
			sc->rtvs[i]->Release();
		}
		if (sc->srvs[i] != nullptr) {
			sc->srvs[i]->Release();
		}
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
comp_d3d11_swapchain_create(struct comp_d3d11_compositor *c,
                            const struct xrt_swapchain_create_info *info,
                            struct xrt_swapchain **out_xsc)
{
	auto internals = get_internals(c);

	// Validate image count
	uint32_t image_count = 3; // Default to triple buffering
	if (image_count > MAX_SWAPCHAIN_IMAGES) {
		image_count = MAX_SWAPCHAIN_IMAGES;
	}

	// Allocate swapchain
	comp_d3d11_swapchain *sc = new comp_d3d11_swapchain();
	memset(sc, 0, sizeof(*sc));

	sc->c = c;
	sc->info = *info;
	sc->image_count = image_count;
	sc->acquired_index = -1;
	sc->waited_index = -1;
	sc->last_released_index = image_count - 1; // Start so first acquire returns index 0

	// Convert format
	DXGI_FORMAT dxgi_format = xrt_format_to_dxgi(info->format);

	// Determine bind flags
	UINT bind_flags = 0;
	if (info->bits & XRT_SWAPCHAIN_USAGE_COLOR) {
		bind_flags |= D3D11_BIND_RENDER_TARGET;
	}
	if (info->bits & XRT_SWAPCHAIN_USAGE_DEPTH_STENCIL) {
		bind_flags |= D3D11_BIND_DEPTH_STENCIL;
	}
	if (info->bits & XRT_SWAPCHAIN_USAGE_SAMPLED) {
		bind_flags |= D3D11_BIND_SHADER_RESOURCE;
	}
	if (info->bits & XRT_SWAPCHAIN_USAGE_UNORDERED_ACCESS) {
		bind_flags |= D3D11_BIND_UNORDERED_ACCESS;
	}

	// Always allow shader resource if it's a color texture
	if ((info->bits & XRT_SWAPCHAIN_USAGE_COLOR) && !(info->bits & XRT_SWAPCHAIN_USAGE_DEPTH_STENCIL)) {
		bind_flags |= D3D11_BIND_SHADER_RESOURCE;
	}

	U_LOG_W("[DEBUG] Swapchain creation: bits=0x%x, bind_flags=0x%x (RT=%d, SR=%d, DS=%d), device=%p",
	        info->bits, bind_flags,
	        (bind_flags & D3D11_BIND_RENDER_TARGET) ? 1 : 0,
	        (bind_flags & D3D11_BIND_SHADER_RESOURCE) ? 1 : 0,
	        (bind_flags & D3D11_BIND_DEPTH_STENCIL) ? 1 : 0,
	        (void *)internals->device);

	// Create textures
	D3D11_TEXTURE2D_DESC texDesc = {};
	texDesc.Width = info->width;
	texDesc.Height = info->height;
	texDesc.MipLevels = info->mip_count > 0 ? info->mip_count : 1;
	texDesc.ArraySize = info->array_size > 0 ? info->array_size : 1;
	texDesc.Format = dxgi_format;
	texDesc.SampleDesc.Count = info->sample_count > 0 ? info->sample_count : 1;
	texDesc.SampleDesc.Quality = 0;
	texDesc.Usage = D3D11_USAGE_DEFAULT;
	texDesc.BindFlags = bind_flags;
	texDesc.CPUAccessFlags = 0;
	texDesc.MiscFlags = 0;

	for (uint32_t i = 0; i < image_count; i++) {
		HRESULT hr = internals->device->CreateTexture2D(&texDesc, nullptr, &sc->images[i]);
		if (FAILED(hr)) {
			U_LOG_E("Failed to create swapchain texture %u: 0x%08x", i, hr);
			d3d11_swapchain_destroy(&sc->base.base);
			return XRT_ERROR_D3D;
		}

		// Create SRV if it's a shader resource
		if (bind_flags & D3D11_BIND_SHADER_RESOURCE) {
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};

			// For depth formats, use a compatible format for SRV
			if (dxgi_format == DXGI_FORMAT_D24_UNORM_S8_UINT) {
				srvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
			} else if (dxgi_format == DXGI_FORMAT_D32_FLOAT) {
				srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
			} else {
				srvDesc.Format = dxgi_format;
			}

			if (texDesc.ArraySize > 1) {
				srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
				srvDesc.Texture2DArray.MipLevels = texDesc.MipLevels;
				srvDesc.Texture2DArray.ArraySize = texDesc.ArraySize;
			} else {
				srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
				srvDesc.Texture2D.MipLevels = texDesc.MipLevels;
			}

			hr = internals->device->CreateShaderResourceView(sc->images[i], &srvDesc, &sc->srvs[i]);
			if (FAILED(hr)) {
				U_LOG_W("Failed to create SRV for swapchain texture %u: 0x%08x", i, hr);
				// Non-fatal, continue without SRV
			}
		}

		// Create RTV if it's a render target
		if (bind_flags & D3D11_BIND_RENDER_TARGET) {
			hr = internals->device->CreateRenderTargetView(sc->images[i], nullptr, &sc->rtvs[i]);
			if (FAILED(hr)) {
				U_LOG_W("Failed to create RTV for swapchain texture %u: 0x%08x", i, hr);
				// Non-fatal, continue without RTV
			}
		}

		// Set up native image
		sc->base.images[i].handle = reinterpret_cast<xrt_graphics_buffer_handle_t>(sc->images[i]);
		sc->base.images[i].size = 0; // Not applicable for D3D11
		sc->base.images[i].use_dedicated_allocation = false;
		sc->base.images[i].is_dxgi_handle = false;

		U_LOG_W("[DEBUG] Swapchain image[%u]: texture=%p, handle=%p",
		        i, (void *)sc->images[i], (void *)sc->base.images[i].handle);
	}

	// Set up swapchain interface
	sc->base.base.image_count = image_count;
	sc->base.base.wait_image = d3d11_swapchain_wait_image;
	sc->base.base.acquire_image = d3d11_swapchain_acquire_image;
	sc->base.base.barrier_image = d3d11_swapchain_barrier_image;
	sc->base.base.release_image = d3d11_swapchain_release_image;
	sc->base.base.destroy = d3d11_swapchain_destroy;
	sc->base.base.reference.count = 1;

	*out_xsc = &sc->base.base;

	U_LOG_W("[DEBUG] Created D3D11 swapchain: %ux%u, %u images, format %d (DXGI), sc=%p, base=%p",
	        info->width, info->height, image_count, (int)dxgi_format, (void *)sc, (void *)&sc->base);

	return XRT_SUCCESS;
}

/*!
 * Get the SRV for a swapchain image.
 *
 * @param xsc The swapchain.
 * @param index Image index.
 * @return The SRV or nullptr.
 */
extern "C" void *
comp_d3d11_swapchain_get_srv(struct xrt_swapchain *xsc, uint32_t index)
{
	struct comp_d3d11_swapchain *sc = d3d11_sc(xsc);

	if (index >= sc->image_count) {
		return nullptr;
	}

	return sc->srvs[index];
}

/*!
 * Get the RTV for a swapchain image.
 *
 * @param xsc The swapchain.
 * @param index Image index.
 * @return The RTV or nullptr.
 */
extern "C" void *
comp_d3d11_swapchain_get_rtv(struct xrt_swapchain *xsc, uint32_t index)
{
	struct comp_d3d11_swapchain *sc = d3d11_sc(xsc);

	if (index >= sc->image_count) {
		return nullptr;
	}

	return sc->rtvs[index];
}

/*!
 * Get the D3D11 texture for a swapchain image.
 *
 * @param xsc The swapchain.
 * @param index Image index.
 * @return The texture or nullptr.
 */
extern "C" void *
comp_d3d11_swapchain_get_texture(struct xrt_swapchain *xsc, uint32_t index)
{
	struct comp_d3d11_swapchain *sc = d3d11_sc(xsc);

	if (index >= sc->image_count) {
		return nullptr;
	}

	return sc->images[index];
}
