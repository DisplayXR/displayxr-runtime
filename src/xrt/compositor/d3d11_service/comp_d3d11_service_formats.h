// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Swapchain image formats of the D3D11 service compositor — which formats
 *         it advertises to IPC clients, what each wire format allocates, and
 *         whether a depth format can be shared cross-process. Pure, and
 *         therefore testable (#1956).
 * @ingroup comp_d3d11_service
 *
 * The service is the "server-creates-swapchain" half of every Windows IPC
 * client (D3D11, D3D12, Vulkan, OpenGL): it allocates each swapchain image as
 * an NT-handle-shared, keyed-mutex texture and the client opens that handle in
 * its own API. Three facts about that allocation live here so
 * `tests/tests_comp_d3d11_service_formats.cpp` can pin them without the
 * 36k-line service unit, a window, a panel, or a plug-in:
 *
 *  1. **The wire format is a VkFormat.** The service advertises VkFormat values
 *     (`d3d_dxgi_format_to_vk`) and every client converts its API's format to a
 *     VkFormat before `create_swapchain` crosses IPC. The old converter mixed
 *     the two namespaces and got depth wrong: VkFormat 130 (`D32_SFLOAT_S8_UINT`)
 *     mapped to `DXGI_FORMAT_D32_FLOAT`, and 124 (`D16_UNORM`) / 126
 *     (`D32_SFLOAT`) fell through to "unknown, using RGBA8" — a depth request
 *     silently allocating a colour texture.
 *
 *  2. **Depth images are TYPELESS.** A texture that carries both
 *     `D3D11_BIND_DEPTH_STENCIL` and `D3D11_BIND_SHADER_RESOURCE` must be created
 *     with the typeless member of its family (a typed depth format cannot have
 *     an SRV), and the OpenXR D3D11/D3D12 bindings — and the CTS `Swapchains`
 *     check — expect the app to receive the typeless resource so it can build
 *     its own typed DSV/SRV. That is what the in-process D3D11 (e9f2844da) and
 *     D3D12 compositors already do.
 *
 *  3. **Depth sharing is probed, not assumed.** `D3D11_RESOURCE_MISC_SHARED_NTHANDLE`
 *     makes the D3D runtime strictly validate a shared resource's format and
 *     flags, and Monado's allocator records that "depth formats are not
 *     supported" with NT handles on some drivers. So the service advertises a
 *     depth format only after creating one with the exact production recipe
 *     and getting an NT handle for it (d3d11_service_depth_format_shareable).
 *     Measured on WARP (tests_comp_d3d11_service_formats): D32_FLOAT and
 *     D16_UNORM share, open in a second D3D11 device and in D3D12, and the
 *     depth one device writes is what the other reads; D24_UNORM_S8_UINT and
 *     D32_FLOAT_S8X24_UINT are refused by the probe. So on such a device
 *     the service offers D32 + D16 and never enumerates D24S8.
 */

#pragma once

#include "xrt/xrt_compositor.h"
#include "d3d/d3d_dxgi_formats.h"

#include <d3d11.h>
#include <dxgi1_2.h>

#include <stdint.h>


/*!
 * The depth formats the service offers, in advertise order, AFTER its colour
 * formats (an app that takes the first enumerated format must keep getting
 * colour). This is exactly the set — and order — the in-process D3D11 and D3D12
 * compositors enumerate; the in-process Vulkan compositor lists the same three.
 * `DXGI_FORMAT_D32_FLOAT_S8X24_UINT` is deliberately absent for that parity: no
 * in-process compositor offers it, so no CTS arm has ever exercised it.
 */
static const DXGI_FORMAT d3d11_service_depth_formats[] = {
    DXGI_FORMAT_D32_FLOAT,
    DXGI_FORMAT_D24_UNORM_S8_UINT,
    DXGI_FORMAT_D16_UNORM,
};

#define D3D11_SERVICE_DEPTH_FORMAT_COUNT (sizeof(d3d11_service_depth_formats) / sizeof(d3d11_service_depth_formats[0]))

//! Is @p format one of the four DXGI depth(-stencil) formats?
static inline bool
d3d11_service_is_depth_format(DXGI_FORMAT format)
{
	switch (format) {
	case DXGI_FORMAT_D16_UNORM:
	case DXGI_FORMAT_D24_UNORM_S8_UINT:
	case DXGI_FORMAT_D32_FLOAT:
	case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return true;
	default: return false;
	}
}

/*!
 * The DXGI format a swapchain create request names. The value on the wire is a
 * VkFormat (see the file comment), so the VkFormat table is authoritative.
 *
 * Kept from the old converter, for compatibility only: a pass-through of the
 * DXGI colour values a pre-VkFormat caller could have sent, and VkFormat 64
 * (`A2B10G10R10_UNORM_PACK32`), which the shared table leaves unmapped. None of
 * the pass-through DXGI values collides with a VkFormat any client sends.
 *
 * Returns `DXGI_FORMAT_UNKNOWN` for anything else — the caller must FAIL the
 * create (XRT_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED) rather than substitute RGBA8,
 * which is how a depth request used to come back as a colour texture.
 */
static inline DXGI_FORMAT
d3d11_service_wire_format_to_dxgi(int64_t format)
{
	switch (format) {
	// VkFormat — what the service advertises and clients send.
	case VK_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
	case VK_FORMAT_R8G8B8A8_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	case VK_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM;
	case VK_FORMAT_B8G8R8A8_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
	case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return DXGI_FORMAT_R10G10B10A2_UNORM;
	case VK_FORMAT_R16G16B16A16_SFLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
	case VK_FORMAT_D16_UNORM: return DXGI_FORMAT_D16_UNORM;
	case VK_FORMAT_D32_SFLOAT: return DXGI_FORMAT_D32_FLOAT;
	case VK_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_D24_UNORM_S8_UINT;
	case VK_FORMAT_D32_SFLOAT_S8_UINT: return DXGI_FORMAT_D32_FLOAT;

	// Legacy DXGI pass-through (colour only).
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
	case DXGI_FORMAT_R16G16B16A16_UNORM:
	case DXGI_FORMAT_R10G10B10A2_UNORM: return static_cast<DXGI_FORMAT>(format);

	default: return DXGI_FORMAT_UNKNOWN;
	}
}

/*!
 * The three formats one service swapchain image needs: what the texture is
 * created as, what the service's own SRV reads it as, and (depth only) the
 * typed depth-stencil-view format an app would bind it with.
 */
struct d3d11_service_image_formats
{
	DXGI_FORMAT texture; //!< CreateTexture2D format — what the app receives.
	DXGI_FORMAT srv;     //!< The service's sampling view.
	DXGI_FORMAT dsv;     //!< DXGI_FORMAT_UNKNOWN for colour.
	bool depth;
};

/*!
 * Formats for an image of the typed @p format. Colour is unchanged from the
 * pre-#1956 service (typed texture, same-format SRV). Depth is TYPELESS storage
 * with the SRV-legal typed member as the view, matching the in-process D3D11
 * compositor's comp_d3d11_swapchain_create.
 */
static inline struct d3d11_service_image_formats
d3d11_service_image_formats_for(DXGI_FORMAT format)
{
	struct d3d11_service_image_formats f = {format, format, DXGI_FORMAT_UNKNOWN, false};
	if (d3d11_service_is_depth_format(format)) {
		f.depth = true;
		f.texture = format;
		f.srv = d3d_dxgi_typeless_to_typed_dxgi(f.texture);
		f.dsv = format;
	}
	return f;
}

/*!
 * Bind flags for a service image. The service always samples (SRV). A depth
 * image never gets RENDER_TARGET (illegal on a depth family) and always gets
 * DEPTH_STENCIL — an app creating a depth-format swapchain renders depth into
 * it even when it only declared SAMPLED. UNORDERED_ACCESS is honoured for colour
 * only, for the same legality reason.
 */
static inline UINT
d3d11_service_bind_flags(DXGI_FORMAT format, enum xrt_swapchain_usage_bits bits)
{
	UINT bind = D3D11_BIND_SHADER_RESOURCE;
	if (d3d11_service_is_depth_format(format)) {
		return bind | D3D11_BIND_DEPTH_STENCIL |
		       (((bits & XRT_SWAPCHAIN_USAGE_COLOR) != 0) ? D3D11_BIND_RENDER_TARGET : 0);
	}
	if ((bits & XRT_SWAPCHAIN_USAGE_COLOR) != 0) {
		bind |= D3D11_BIND_RENDER_TARGET;
	}
	if ((bits & XRT_SWAPCHAIN_USAGE_DEPTH_STENCIL) != 0) {
		// A colour format with depth usage cannot be created; keep the old
		// behaviour (pass the flag through, let CreateTexture2D refuse it).
		bind |= D3D11_BIND_DEPTH_STENCIL;
	}
	if ((bits & XRT_SWAPCHAIN_USAGE_UNORDERED_ACCESS) != 0) {
		bind |= D3D11_BIND_UNORDERED_ACCESS;
	}
	return bind;
}

/*!
 * The texture description of a cross-process service image: NT-handle shared,
 * keyed-mutex synchronised, single sample by default.
 */
static inline D3D11_TEXTURE2D_DESC
d3d11_service_shared_texture_desc(uint32_t width,
                                  uint32_t height,
                                  uint32_t mip_count,
                                  uint32_t array_size,
                                  uint32_t sample_count,
                                  DXGI_FORMAT format,
                                  enum xrt_swapchain_usage_bits bits)
{
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = width;
	desc.Height = height;
	desc.MipLevels = mip_count > 0 ? mip_count : 1;
	desc.ArraySize = array_size > 0 ? array_size : 1;
	desc.Format = d3d11_service_image_formats_for(format).texture;
	desc.SampleDesc.Count = sample_count > 0 ? sample_count : 1;
	desc.SampleDesc.Quality = 0;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = d3d11_service_bind_flags(format, bits);
	desc.CPUAccessFlags = 0;
	// SHARED_KEYEDMUTEX: cross-process sharing with synchronisation.
	// SHARED_NTHANDLE: real kernel handles that DuplicateHandle across processes.
	desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
	return desc;
}

/*!
 * Can @p device allocate a depth image of the typed @p format the way
 * compositor_create_swapchain does, AND hand out an NT handle for it?
 *
 * Creates a 16x16 image with the production description, asks DXGI for an NT
 * handle, and frees both. A `false` here means the format must not be
 * advertised: an app would see it in xrEnumerateSwapchainFormats and then fail
 * xrCreateSwapchain.
 */
static inline bool
d3d11_service_depth_format_shareable(ID3D11Device *device, DXGI_FORMAT format)
{
	if (device == nullptr || !d3d11_service_is_depth_format(format)) {
		return false;
	}

	D3D11_TEXTURE2D_DESC desc = d3d11_service_shared_texture_desc(
	    16, 16, 1, 1, 1, format,
	    (enum xrt_swapchain_usage_bits)(XRT_SWAPCHAIN_USAGE_DEPTH_STENCIL | XRT_SWAPCHAIN_USAGE_SAMPLED));

	ID3D11Texture2D *tex = nullptr;
	if (FAILED(device->CreateTexture2D(&desc, nullptr, &tex)) || tex == nullptr) {
		return false;
	}

	bool ok = false;
	IDXGIResource1 *res1 = nullptr;
	if (SUCCEEDED(tex->QueryInterface(__uuidof(IDXGIResource1), (void **)&res1)) && res1 != nullptr) {
		HANDLE h = nullptr;
		if (SUCCEEDED(res1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
		                                       nullptr, &h)) &&
		    h != nullptr) {
			ok = true;
			CloseHandle(h);
		}
		res1->Release();
	}
	tex->Release();
	return ok;
}
