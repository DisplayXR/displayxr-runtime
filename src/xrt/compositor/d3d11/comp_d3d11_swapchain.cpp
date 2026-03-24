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
#include "d3d/d3d_dxgi_formats.h"

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

	//! D3D11.4 fence for efficient GPU→CPU release notification.
	//! Signaled at xrReleaseSwapchainImage; waited in layer_commit.
	ID3D11Fence *release_fence;

	//! Auto-reset Win32 event; fired when GPU reaches release_fence.
	HANDLE release_event;

	//! Monotonic counter; incremented on each release.
	uint64_t release_fence_value;
};

// Access compositor internals
extern "C" {
struct comp_d3d11_compositor_internals
{
	struct xrt_compositor_native base;
	struct xrt_device *xdev;
	ID3D11Device *device;
	ID3D11DeviceContext *context;
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

	if (sc->acquired_index >= 0) {
		U_LOG_E("Image already acquired");
		return XRT_ERROR_IPC_FAILURE;
	}

	// Round-robin acquisition using per-swapchain counter
	// Find next available index (start from last released + 1)
	uint32_t index = (sc->last_released_index + 1) % sc->image_count;

	sc->acquired_index = static_cast<int32_t>(index);
	*out_index = index;

	return XRT_SUCCESS;
}

static xrt_result_t
d3d11_swapchain_wait_image(struct xrt_swapchain *xsc, int64_t timeout_ns, uint32_t index)
{
	struct comp_d3d11_swapchain *sc = d3d11_sc(xsc);
	(void)timeout_ns;

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
	struct comp_d3d11_swapchain *sc = d3d11_sc(xsc);

	// When transitioning from app to compositor, flush pending GPU commands then
	// signal a fence so layer_commit can wait efficiently via WaitForSingleObject.
	if (direction == XRT_BARRIER_TO_COMP && sc->c != nullptr) {
		auto internals = get_internals(sc->c);

		// Flush all pending immediate-context commands to the GPU.
		internals->context->Flush();

		// Signal fence after the flush.  When the GPU reaches this signal,
		// release_event fires and layer_commit's WaitForSingleObject returns,
		// guaranteeing all commands submitted up to this release are complete.
		if (sc->release_fence != nullptr && sc->release_event != nullptr) {
			ID3D11DeviceContext4 *ctx4 = nullptr;
			if (SUCCEEDED(internals->context->QueryInterface(
			        __uuidof(ID3D11DeviceContext4), (void **)&ctx4))) {
				uint64_t val = ++sc->release_fence_value;
				ctx4->Signal(sc->release_fence, val);
				sc->release_fence->SetEventOnCompletion(val, sc->release_event);
				ctx4->Release();
			}
		}
	}

	return XRT_SUCCESS;
}

static xrt_result_t
d3d11_swapchain_release_image(struct xrt_swapchain *xsc, uint32_t index)
{
	struct comp_d3d11_swapchain *sc = d3d11_sc(xsc);

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

	if (sc->release_event != nullptr) {
		CloseHandle(sc->release_event);
	}
	if (sc->release_fence != nullptr) {
		sc->release_fence->Release();
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
	sc->release_fence = nullptr;
	sc->release_event = nullptr;
	sc->release_fence_value = 0;

	// Create ID3D11Fence for efficient GPU→CPU sync (D3D11.4 / Windows 10+).
	// Falls back to D3D11_QUERY_EVENT spin-wait on older hardware.
	{
		ID3D11Device5 *dev5 = nullptr;
		if (SUCCEEDED(internals->device->QueryInterface(__uuidof(ID3D11Device5), (void **)&dev5))) {
			HRESULT fence_hr = dev5->CreateFence(
			    0, D3D11_FENCE_FLAG_NONE, __uuidof(ID3D11Fence), (void **)&sc->release_fence);
			if (SUCCEEDED(fence_hr)) {
				sc->release_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
				if (sc->release_event == nullptr) {
					sc->release_fence->Release();
					sc->release_fence = nullptr;
				}
			}
			dev5->Release();
		}
		if (sc->release_fence == nullptr) {
			U_LOG_I("D3D11: ID3D11Fence unavailable — will use D3D11_QUERY_EVENT fallback");
		}
	}

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

	// For depth formats that need both DSV and SRV, use typeless format for texture creation
	// This allows creating views with different typed formats
	DXGI_FORMAT texture_format = dxgi_format;
	DXGI_FORMAT dsv_format = dxgi_format;
	DXGI_FORMAT srv_format = dxgi_format;
	DXGI_FORMAT rtv_format = dxgi_format;
	bool is_depth = false;

	if (bind_flags & D3D11_BIND_DEPTH_STENCIL) {
		is_depth = true;
		switch (dxgi_format) {
		case DXGI_FORMAT_D24_UNORM_S8_UINT:
			texture_format = DXGI_FORMAT_R24G8_TYPELESS;
			dsv_format = DXGI_FORMAT_D24_UNORM_S8_UINT;
			srv_format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
			break;
		case DXGI_FORMAT_D32_FLOAT:
			texture_format = DXGI_FORMAT_R32_TYPELESS;
			dsv_format = DXGI_FORMAT_D32_FLOAT;
			srv_format = DXGI_FORMAT_R32_FLOAT;
			break;
		case DXGI_FORMAT_D16_UNORM:
			texture_format = DXGI_FORMAT_R16_TYPELESS;
			dsv_format = DXGI_FORMAT_D16_UNORM;
			srv_format = DXGI_FORMAT_R16_UNORM;
			break;
		default:
			// Use format as-is for other depth formats
			break;
		}
	}

	// For color textures, create with TYPELESS so apps can create their own typed views
	// (required by OpenXR D3D11 spec; fixes Unity D3D11 black screen, issue #91).
	if (!is_depth) {
		DXGI_FORMAT typeless = d3d_dxgi_format_to_typeless_dxgi(dxgi_format);
		if (typeless != dxgi_format) {
			texture_format = typeless;
			// srv_format and rtv_format remain as the original concrete format
		}
	}

	// Create textures
	D3D11_TEXTURE2D_DESC texDesc = {};
	texDesc.Width = info->width;
	texDesc.Height = info->height;
	texDesc.MipLevels = info->mip_count > 0 ? info->mip_count : 1;
	texDesc.ArraySize = info->array_size > 0 ? info->array_size : 1;
	texDesc.Format = texture_format; // TYPELESS for both depth and color textures
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
			srvDesc.Format = srv_format; // Use pre-computed SRV format (handles depth formats)

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

		// Create RTV if it's a render target (explicit format for TYPELESS textures)
		if (bind_flags & D3D11_BIND_RENDER_TARGET) {
			D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
			rtvDesc.Format = rtv_format;
			if (texDesc.ArraySize > 1) {
				rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
				rtvDesc.Texture2DArray.MipSlice = 0;
				rtvDesc.Texture2DArray.ArraySize = texDesc.ArraySize;
			} else {
				rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
				rtvDesc.Texture2D.MipSlice = 0;
			}
			hr = internals->device->CreateRenderTargetView(sc->images[i], &rtvDesc, &sc->rtvs[i]);
			if (FAILED(hr)) {
				U_LOG_W("Failed to create RTV for swapchain texture %u: 0x%08x", i, hr);
			}
		}

		// Set up native image
		sc->base.images[i].handle = reinterpret_cast<xrt_graphics_buffer_handle_t>(sc->images[i]);
		sc->base.images[i].size = 0; // Not applicable for D3D11
		sc->base.images[i].use_dedicated_allocation = false;
		sc->base.images[i].is_dxgi_handle = false;
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

	U_LOG_I("Created D3D11 swapchain: %ux%u, %u images, format %d (texture: %d)",
	        info->width, info->height, image_count, (int)dxgi_format, (int)texture_format);

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

extern "C" void
comp_d3d11_swapchain_get_dimensions(struct xrt_swapchain *xsc, uint32_t *out_w, uint32_t *out_h)
{
	struct comp_d3d11_swapchain *sc = d3d11_sc(xsc);
	*out_w = sc->info.width;
	*out_h = sc->info.height;
}

/*!
 * Wait for GPU completion of all commands submitted up to the most recent
 * xrReleaseSwapchainImage for this swapchain.
 *
 * Fast path (D3D11.4): OS-level WaitForSingleObject on an ID3D11Fence event —
 * zero CPU spin, no per-frame allocations.
 *
 * Fallback (pre-D3D11.4): Flush + spin-wait on D3D11_QUERY_EVENT.
 *
 * @param xsc       The swapchain.
 * @param timeout_ms Timeout in milliseconds (100 ms is recommended).
 */
extern "C" void
comp_d3d11_swapchain_wait_gpu_complete(struct xrt_swapchain *xsc, uint32_t timeout_ms)
{
	struct comp_d3d11_swapchain *sc = d3d11_sc(xsc);

	// Fast path: ID3D11Fence + Win32 auto-reset event (D3D11.4 / Windows 10+).
	// The fence was signaled in barrier_image(TO_COMP) after Flush(), so waiting
	// here guarantees all GPU commands up to xrReleaseSwapchainImage are done.
	if (sc->release_fence != nullptr && sc->release_event != nullptr &&
	    sc->release_fence_value > 0) {
		WaitForSingleObject(sc->release_event, timeout_ms);
		return;
	}

	// Fallback: Flush + spin-wait on D3D11_QUERY_EVENT.
	if (sc->c != nullptr) {
		auto internals = get_internals(sc->c);
		internals->context->Flush();
		ID3D11Query *eq = nullptr;
		D3D11_QUERY_DESC qd = {};
		qd.Query = D3D11_QUERY_EVENT;
		if (SUCCEEDED(internals->device->CreateQuery(&qd, &eq))) {
			internals->context->End(eq);
			BOOL done = FALSE;
			while (internals->context->GetData(eq, &done, sizeof(done), 0) == S_FALSE) {
				// Spin-wait for GPU to drain
			}
			eq->Release();
		}
	}
}
