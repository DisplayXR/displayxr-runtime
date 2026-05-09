// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Vulkan presentation target (Win32 surface + VkSwapchainKHR).
 * @author David Fattal
 * @ingroup comp_vk_native
 */

#include "comp_vk_native_target.h"
#include "comp_vk_native_compositor.h"

#include "xrt/xrt_vulkan_includes.h"
#include "vk/vk_helpers.h"

#include "util/u_logging.h"
#include "util/u_misc.h"

#ifdef XRT_OS_WINDOWS
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan_win32.h>
// Transparent-background present path: VK -> D3D11 KMT shared textures ->
// DComp + CreateSwapChainForComposition flip-model swapchain -> HWND.
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_3.h>
#include <dcomp.h>
#endif

#ifdef XRT_OS_MACOS
#include <vulkan/vulkan_metal.h>
#endif

#define DCOMP_RING 2 // Number of shared back-buffers in the bridge ring

#define MAX_TARGET_IMAGES 4

/*!
 * Vulkan target structure.
 */
struct comp_vk_native_target
{
	//! Vulkan bundle (borrowed).
	struct vk_bundle *vk;

	//! Win32 surface.
	VkSurfaceKHR surface;

	//! Swapchain.
	VkSwapchainKHR swapchain;

	//! Swapchain images.
	VkImage images[MAX_TARGET_IMAGES];

	//! Swapchain image views.
	VkImageView views[MAX_TARGET_IMAGES];

	//! Number of swapchain images.
	uint32_t image_count;

	//! Current acquired image index.
	uint32_t current_index;

	//! Semaphore signaled when image is available.
	VkSemaphore image_available;

	//! Semaphore signaled when rendering is done.
	VkSemaphore render_finished;

	//! Current dimensions.
	uint32_t width;
	uint32_t height;

	//! Surface format.
	VkFormat format;

	//! Window handle.
	void *hwnd;

	//! Queue family index for present support check.
	uint32_t queue_family_index;

	//! True if the swapchain was requested with a transparent compositeAlpha.
	bool transparent_background;

#ifdef XRT_OS_WINDOWS
	// VK -> D3D11 -> DComp transparent present bridge. Active when
	// transparent_background is set AND the bridge initialized successfully.
	// In this mode @ref swapchain stays VK_NULL_HANDLE — VK doesn't go through
	// WSI at all. The compositor renders into @ref dcomp_vk_image[i]; present
	// dispatches to the bridge, which copies to a flip-model DXGI swapchain
	// back buffer that DComp targets the HWND.
	bool dcomp_active;
	ID3D11Device *dcomp_dx_device;
	ID3D11DeviceContext *dcomp_dx_context;
	IDXGISwapChain1 *dcomp_swapchain;
	IDCompositionDevice *dcomp_dcomp_device;
	IDCompositionTarget *dcomp_dcomp_target;
	IDCompositionVisual *dcomp_dcomp_visual;
	ID3D11Texture2D *dcomp_shared_dx[DCOMP_RING];
	IDXGIKeyedMutex *dcomp_shared_mutex[DCOMP_RING];
	VkImage dcomp_vk_image[DCOMP_RING];
	VkImageView dcomp_vk_view[DCOMP_RING];
	VkDeviceMemory dcomp_vk_memory[DCOMP_RING];
	uint32_t dcomp_ring_idx;
#endif
};

static void
destroy_swapchain_views(struct comp_vk_native_target *target)
{
	struct vk_bundle *vk = target->vk;
	for (uint32_t i = 0; i < target->image_count; i++) {
		if (target->views[i] != VK_NULL_HANDLE) {
			vk->vkDestroyImageView(vk->device, target->views[i], NULL);
			target->views[i] = VK_NULL_HANDLE;
		}
	}
}

static xrt_result_t
create_swapchain_views(struct comp_vk_native_target *target)
{
	struct vk_bundle *vk = target->vk;
	for (uint32_t i = 0; i < target->image_count; i++) {
		VkImageViewCreateInfo ci = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		    .image = target->images[i],
		    .viewType = VK_IMAGE_VIEW_TYPE_2D,
		    .format = target->format,
		    .subresourceRange = {
		        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		        .baseMipLevel = 0,
		        .levelCount = 1,
		        .baseArrayLayer = 0,
		        .layerCount = 1,
		    },
		};

		VkResult res = vk->vkCreateImageView(vk->device, &ci, NULL, &target->views[i]);
		if (res != VK_SUCCESS) {
			U_LOG_E("Failed to create target image view %u: %d", i, res);
			return XRT_ERROR_VULKAN;
		}
	}
	return XRT_SUCCESS;
}

static xrt_result_t
create_swapchain(struct comp_vk_native_target *target)
{
	struct vk_bundle *vk = target->vk;

	// Query surface capabilities
	VkSurfaceCapabilitiesKHR caps;
	VkResult res = vk->vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
	    vk->physical_device, target->surface, &caps);
	if (res != VK_SUCCESS) {
		U_LOG_E("Failed to get surface capabilities: %d", res);
		return XRT_ERROR_VULKAN;
	}

	// Use requested dimensions or surface extent
	VkExtent2D extent = {target->width, target->height};
	if (caps.currentExtent.width != UINT32_MAX) {
		extent = caps.currentExtent;
	}
	target->width = extent.width;
	target->height = extent.height;

	uint32_t image_count = caps.minImageCount + 1;
	if (caps.maxImageCount > 0 && image_count > caps.maxImageCount) {
		image_count = caps.maxImageCount;
	}
	if (image_count > MAX_TARGET_IMAGES) {
		image_count = MAX_TARGET_IMAGES;
	}

	// Pick surface format
	uint32_t format_count = 0;
	vk->vkGetPhysicalDeviceSurfaceFormatsKHR(vk->physical_device, target->surface,
	                                          &format_count, NULL);
	VkSurfaceFormatKHR formats[32];
	if (format_count > 32) format_count = 32;
	vk->vkGetPhysicalDeviceSurfaceFormatsKHR(vk->physical_device, target->surface,
	                                          &format_count, formats);

	// Prefer BGRA8_UNORM, fall back to first available
	target->format = formats[0].format;
	VkColorSpaceKHR color_space = formats[0].colorSpace;
	for (uint32_t i = 0; i < format_count; i++) {
		if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM) {
			target->format = formats[i].format;
			color_space = formats[i].colorSpace;
			break;
		}
	}

	// Pick present mode: FIFO (VSync) is always available
	VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;

	// Pick compositeAlpha. The DP's chroma-key strip pass writes premultiplied
	// alpha into the swapchain image, so we want PRE_MULTIPLIED. INHERIT works
	// on some Win32 WSI drivers where DWM still respects the alpha channel.
	// Most Win32 ICDs only expose OPAQUE — in that case transparency silently
	// no-ops at the WSI layer (the strip pass still runs but the alpha is
	// dropped on present); we log a one-time warning so the failure mode is
	// visible without spamming per-frame.
	VkCompositeAlphaFlagBitsKHR composite_alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	if (target->transparent_background) {
		if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR) {
			composite_alpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
			U_LOG_I("VK target: transparent_background using PRE_MULTIPLIED");
		} else if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) {
			composite_alpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
			U_LOG_I("VK target: transparent_background using INHERIT (PRE_MULTIPLIED unavailable)");
		} else {
			U_LOG_W("VK target: transparent_background requested but neither PRE_MULTIPLIED nor "
			        "INHERIT compositeAlpha is supported (caps=0x%x); falling back to OPAQUE — "
			        "alpha will be dropped at WSI present",
			        (unsigned)caps.supportedCompositeAlpha);
		}
	}

	VkSwapchainCreateInfoKHR ci = {
	    .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
	    .surface = target->surface,
	    .minImageCount = image_count,
	    .imageFormat = target->format,
	    .imageColorSpace = color_space,
	    .imageExtent = extent,
	    .imageArrayLayers = 1,
	    .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
	                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
	    .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
	    .preTransform = caps.currentTransform,
	    .compositeAlpha = composite_alpha,
	    .presentMode = present_mode,
	    .clipped = VK_TRUE,
	    .oldSwapchain = VK_NULL_HANDLE,
	};

	res = vk->vkCreateSwapchainKHR(vk->device, &ci, NULL, &target->swapchain);
	if (res != VK_SUCCESS) {
		U_LOG_E("Failed to create swapchain: %d", res);
		return XRT_ERROR_VULKAN;
	}

	// Get swapchain images
	target->image_count = MAX_TARGET_IMAGES;
	res = vk->vkGetSwapchainImagesKHR(vk->device, target->swapchain,
	                                    &target->image_count, target->images);
	if (res != VK_SUCCESS && res != VK_INCOMPLETE) {
		U_LOG_E("Failed to get swapchain images: %d", res);
		return XRT_ERROR_VULKAN;
	}

	// Create image views
	return create_swapchain_views(target);
}

#ifdef XRT_OS_WINDOWS

/*
 *
 * VK -> D3D11 -> DComp transparent present bridge.
 *
 * Win32 Vulkan WSI doesn't expose any path to alpha-correct desktop
 * composition (most ICDs only advertise OPAQUE compositeAlpha). To get real
 * desktop see-through under VK we side-step WSI entirely:
 *   1. Create a D3D11 device (anyone — DXGI factory + DComp don't care which
 *      adapter the VK GPU is on, we'll Copy across them via the system bus).
 *   2. For each ring slot create an ID3D11Texture2D with KMT_BIT shared NT
 *      handle + KEYEDMUTEX. Open the KMT handle in VK via
 *      VK_KHR_external_memory_win32 (which the runtime already requires).
 *   3. Create a flip-model DXGI swapchain via CreateSwapChainForComposition
 *      with PRE_MULTIPLIED alpha. Bind to the HWND through DComp visual+target.
 *   4. Each frame: VK renders into ring[i]'s VkImage. After vkQueueWaitIdle,
 *      D3D11 IDXGIKeyedMutex::AcquireSync(0,0) flushes the writer caches
 *      (per memory feedback_acquiresync_load_bearing.md), CopyResource into
 *      the swapchain back buffer, ReleaseSync, swapchain->Present(1, 0),
 *      dcomp_device->Commit().
 *
 * Ring index is bumped by acquire(); compositor renders into the slot
 * returned by get_current_image(); present() copies that slot.
 *
 */

static void
dcomp_destroy(struct comp_vk_native_target *target)
{
	if (target == NULL) return;
	struct vk_bundle *vk = target->vk;

	for (uint32_t i = 0; i < DCOMP_RING; i++) {
		if (target->dcomp_vk_view[i] != VK_NULL_HANDLE) {
			vk->vkDestroyImageView(vk->device, target->dcomp_vk_view[i], NULL);
			target->dcomp_vk_view[i] = VK_NULL_HANDLE;
		}
		if (target->dcomp_vk_image[i] != VK_NULL_HANDLE) {
			vk->vkDestroyImage(vk->device, target->dcomp_vk_image[i], NULL);
			target->dcomp_vk_image[i] = VK_NULL_HANDLE;
		}
		if (target->dcomp_vk_memory[i] != VK_NULL_HANDLE) {
			vk->vkFreeMemory(vk->device, target->dcomp_vk_memory[i], NULL);
			target->dcomp_vk_memory[i] = VK_NULL_HANDLE;
		}
		if (target->dcomp_shared_mutex[i]) {
			target->dcomp_shared_mutex[i]->Release();
			target->dcomp_shared_mutex[i] = NULL;
		}
		if (target->dcomp_shared_dx[i]) {
			target->dcomp_shared_dx[i]->Release();
			target->dcomp_shared_dx[i] = NULL;
		}
	}
	if (target->dcomp_dcomp_visual) { target->dcomp_dcomp_visual->Release(); target->dcomp_dcomp_visual = NULL; }
	if (target->dcomp_dcomp_target) { target->dcomp_dcomp_target->Release(); target->dcomp_dcomp_target = NULL; }
	if (target->dcomp_dcomp_device) { target->dcomp_dcomp_device->Release(); target->dcomp_dcomp_device = NULL; }
	if (target->dcomp_swapchain)    { target->dcomp_swapchain->Release();    target->dcomp_swapchain = NULL; }
	if (target->dcomp_dx_context)   { target->dcomp_dx_context->Release();   target->dcomp_dx_context = NULL; }
	if (target->dcomp_dx_device)    { target->dcomp_dx_device->Release();    target->dcomp_dx_device = NULL; }
	target->dcomp_active = false;
}

// Import a single D3D11 KMT-shared texture as a VkImage in the ring.
static bool
dcomp_import_one(struct comp_vk_native_target *target,
                 uint32_t i,
                 ID3D11Texture2D *dx_tex,
                 HANDLE shared_kmt,
                 uint32_t w,
                 uint32_t h,
                 VkFormat vk_format)
{
	struct vk_bundle *vk = target->vk;

	VkExternalMemoryImageCreateInfo external_ci = {
	    .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
	    .pNext = NULL,
	    .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT,
	};

	VkImageCreateInfo image_ci = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .pNext = &external_ci,
	    .flags = 0,
	    .imageType = VK_IMAGE_TYPE_2D,
	    .format = vk_format,
	    .extent = {w, h, 1},
	    .mipLevels = 1,
	    .arrayLayers = 1,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .tiling = VK_IMAGE_TILING_OPTIMAL,
	    .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
	             VK_IMAGE_USAGE_TRANSFER_DST_BIT |
	             VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
	             VK_IMAGE_USAGE_SAMPLED_BIT,
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};

	VkResult res = vk->vkCreateImage(vk->device, &image_ci, NULL, &target->dcomp_vk_image[i]);
	if (res != VK_SUCCESS) {
		U_LOG_E("DComp bridge: vkCreateImage failed for ring[%u]: %d", i, res);
		return false;
	}

	VkMemoryRequirements requirements = {};
	vk->vkGetImageMemoryRequirements(vk->device, target->dcomp_vk_image[i], &requirements);

	VkImportMemoryWin32HandleInfoKHR import_info = {
	    .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
	    .pNext = NULL,
	    .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT,
	    .handle = shared_kmt,
	};
	VkMemoryDedicatedAllocateInfoKHR dedicated_info = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO_KHR,
	    .pNext = &import_info,
	    .image = target->dcomp_vk_image[i],
	    .buffer = VK_NULL_HANDLE,
	};

	VkPhysicalDeviceMemoryProperties mem_props = {};
	vk->vkGetPhysicalDeviceMemoryProperties(vk->physical_device, &mem_props);
	uint32_t memory_type_index = UINT32_MAX;
	for (uint32_t k = 0; k < mem_props.memoryTypeCount; k++) {
		if ((requirements.memoryTypeBits & (1u << k)) != 0) {
			memory_type_index = k;
			break;
		}
	}
	if (memory_type_index == UINT32_MAX) {
		U_LOG_E("DComp bridge: no compatible memory type for ring[%u]", i);
		return false;
	}

	VkMemoryAllocateInfo alloc_info = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .pNext = &dedicated_info,
	    .allocationSize = requirements.size,
	    .memoryTypeIndex = memory_type_index,
	};
	res = vk->vkAllocateMemory(vk->device, &alloc_info, NULL, &target->dcomp_vk_memory[i]);
	if (res != VK_SUCCESS) {
		U_LOG_E("DComp bridge: vkAllocateMemory failed for ring[%u]: %d", i, res);
		return false;
	}

	res = vk->vkBindImageMemory(vk->device, target->dcomp_vk_image[i],
	                            target->dcomp_vk_memory[i], 0);
	if (res != VK_SUCCESS) {
		U_LOG_E("DComp bridge: vkBindImageMemory failed for ring[%u]: %d", i, res);
		return false;
	}

	VkImageViewCreateInfo view_ci = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
	    .image = target->dcomp_vk_image[i],
	    .viewType = VK_IMAGE_VIEW_TYPE_2D,
	    .format = vk_format,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	res = vk->vkCreateImageView(vk->device, &view_ci, NULL, &target->dcomp_vk_view[i]);
	if (res != VK_SUCCESS) {
		U_LOG_E("DComp bridge: vkCreateImageView failed for ring[%u]: %d", i, res);
		return false;
	}

	// Cache the IDXGIKeyedMutex for the cross-API sync.
	HRESULT hr = dx_tex->QueryInterface(__uuidof(IDXGIKeyedMutex),
	                                     (void **)&target->dcomp_shared_mutex[i]);
	if (FAILED(hr) || target->dcomp_shared_mutex[i] == NULL) {
		U_LOG_E("DComp bridge: QueryInterface(IDXGIKeyedMutex) failed for ring[%u]: 0x%08x", i, hr);
		return false;
	}

	return true;
}

// Initialize the DComp bridge: D3D11 device, swapchain, DComp visual+target,
// and the ring of KMT-shared textures imported as VkImages. Returns false
// (with a U_LOG_W) if any prerequisite is missing — caller falls back to
// opaque WSI.
static bool
dcomp_setup(struct comp_vk_native_target *target, HWND hwnd, uint32_t w, uint32_t h)
{
	struct vk_bundle *vk = target->vk;

	HRESULT hr = D3D11CreateDevice(
	    NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
	    D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0, D3D11_SDK_VERSION,
	    &target->dcomp_dx_device, NULL, &target->dcomp_dx_context);
	if (FAILED(hr) || target->dcomp_dx_device == NULL) {
		U_LOG_W("DComp bridge: D3D11CreateDevice failed: 0x%08x — falling back to opaque WSI", hr);
		return false;
	}

	// Create flip-model swapchain via DXGI factory bound to the D3D11 device.
	IDXGIDevice *dxgi_device = NULL;
	hr = target->dcomp_dx_device->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgi_device);
	if (FAILED(hr) || dxgi_device == NULL) {
		U_LOG_W("DComp bridge: QueryInterface(IDXGIDevice) failed: 0x%08x", hr);
		return false;
	}
	IDXGIAdapter *dxgi_adapter = NULL;
	dxgi_device->GetAdapter(&dxgi_adapter);
	dxgi_device->Release();
	if (dxgi_adapter == NULL) {
		U_LOG_W("DComp bridge: GetAdapter failed");
		return false;
	}
	IDXGIFactory2 *dxgi_factory = NULL;
	hr = dxgi_adapter->GetParent(__uuidof(IDXGIFactory2), (void **)&dxgi_factory);
	dxgi_adapter->Release();
	if (FAILED(hr) || dxgi_factory == NULL) {
		U_LOG_W("DComp bridge: GetParent(IDXGIFactory2) failed: 0x%08x", hr);
		return false;
	}

	DXGI_SWAP_CHAIN_DESC1 desc = {};
	desc.Width = w;
	desc.Height = h;
	desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.BufferCount = DCOMP_RING;
	desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

	hr = dxgi_factory->CreateSwapChainForComposition(target->dcomp_dx_device, &desc, NULL,
	                                                  &target->dcomp_swapchain);
	dxgi_factory->Release();
	if (FAILED(hr) || target->dcomp_swapchain == NULL) {
		U_LOG_W("DComp bridge: CreateSwapChainForComposition failed: 0x%08x", hr);
		return false;
	}

	hr = DCompositionCreateDevice2(NULL, __uuidof(IDCompositionDevice),
	                                (void **)&target->dcomp_dcomp_device);
	if (FAILED(hr) || target->dcomp_dcomp_device == NULL) {
		U_LOG_W("DComp bridge: DCompositionCreateDevice2 failed: 0x%08x", hr);
		return false;
	}
	hr = target->dcomp_dcomp_device->CreateTargetForHwnd(hwnd, /*topmost*/ TRUE,
	                                                      &target->dcomp_dcomp_target);
	if (FAILED(hr) || target->dcomp_dcomp_target == NULL) {
		U_LOG_W("DComp bridge: CreateTargetForHwnd failed: 0x%08x", hr);
		return false;
	}
	hr = target->dcomp_dcomp_device->CreateVisual(&target->dcomp_dcomp_visual);
	if (FAILED(hr) || target->dcomp_dcomp_visual == NULL) {
		U_LOG_W("DComp bridge: CreateVisual failed: 0x%08x", hr);
		return false;
	}
	if (FAILED(target->dcomp_dcomp_visual->SetContent(target->dcomp_swapchain)) ||
	    FAILED(target->dcomp_dcomp_target->SetRoot(target->dcomp_dcomp_visual)) ||
	    FAILED(target->dcomp_dcomp_device->Commit())) {
		U_LOG_W("DComp bridge: visual setup failed");
		return false;
	}

	// Create the ring of KMT-shared D3D11 textures and import each as a VkImage.
	for (uint32_t i = 0; i < DCOMP_RING; i++) {
		D3D11_TEXTURE2D_DESC tdesc = {};
		tdesc.Width = w;
		tdesc.Height = h;
		tdesc.MipLevels = 1;
		tdesc.ArraySize = 1;
		tdesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		tdesc.SampleDesc.Count = 1;
		tdesc.Usage = D3D11_USAGE_DEFAULT;
		tdesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		// Use the legacy KMT path (NOT NTHANDLE) so we can call
		// IDXGIResource::GetSharedHandle and import on the VK side via
		// VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT — matching
		// the existing import_shared_d3d11_texture pattern. NTHANDLE
		// would require IDXGIResource1::CreateSharedHandle (returns
		// E_INVALIDARG from the legacy GetSharedHandle path).
		// SHARED_KEYEDMUTEX implies legacy SHARED — they're mutually
		// exclusive at the D3D11 API surface (combining with NTHANDLE is
		// also possible but requires CreateSharedHandle).
		tdesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

		hr = target->dcomp_dx_device->CreateTexture2D(&tdesc, NULL, &target->dcomp_shared_dx[i]);
		if (FAILED(hr)) {
			U_LOG_W("DComp bridge: CreateTexture2D[%u] failed: 0x%08x", i, hr);
			return false;
		}

		// Get the KMT-style legacy shared HANDLE for VK import.
		IDXGIResource *dxgi_res = NULL;
		hr = target->dcomp_shared_dx[i]->QueryInterface(__uuidof(IDXGIResource),
		                                                 (void **)&dxgi_res);
		if (FAILED(hr) || dxgi_res == NULL) {
			U_LOG_W("DComp bridge: QueryInterface(IDXGIResource)[%u] failed: 0x%08x", i, hr);
			return false;
		}
		HANDLE shared_kmt = NULL;
		hr = dxgi_res->GetSharedHandle(&shared_kmt);
		dxgi_res->Release();
		if (FAILED(hr) || shared_kmt == NULL) {
			U_LOG_W("DComp bridge: GetSharedHandle[%u] failed: 0x%08x", i, hr);
			return false;
		}

		if (!dcomp_import_one(target, i, target->dcomp_shared_dx[i], shared_kmt,
		                       w, h, VK_FORMAT_B8G8R8A8_UNORM)) {
			return false;
		}
	}

	// Match the public target fields so the rest of the compositor sees the
	// imported VkImages as if they were swapchain images.
	target->image_count = DCOMP_RING;
	for (uint32_t i = 0; i < DCOMP_RING; i++) {
		target->images[i] = target->dcomp_vk_image[i];
		target->views[i] = target->dcomp_vk_view[i];
	}
	target->format = VK_FORMAT_B8G8R8A8_UNORM;
	target->width = w;
	target->height = h;
	target->dcomp_ring_idx = 0;
	target->current_index = 0;
	target->dcomp_active = true;

	U_LOG_W("DComp bridge active: %ux%u, %u-deep ring, KMT shared, "
	        "PRE_MULTIPLIED + DComp -> HWND",
	        w, h, (unsigned)DCOMP_RING);
	return true;
}

// Submit the just-rendered ring slot to DComp: D3D11 acquires sync on the
// shared texture (which flushes VK writer caches per
// feedback_acquiresync_load_bearing.md), CopyResource into the next swapchain
// back buffer, releases sync, Present, Commit.
static xrt_result_t
dcomp_present(struct comp_vk_native_target *target)
{
	uint32_t idx = target->current_index;
	if (idx >= DCOMP_RING || target->dcomp_shared_mutex[idx] == NULL) {
		U_LOG_E("DComp bridge: present called with invalid ring index %u", idx);
		return XRT_ERROR_VULKAN;
	}

	// Acquire (key=0) — also flushes VK writer caches into the shared resource.
	HRESULT hr = target->dcomp_shared_mutex[idx]->AcquireSync(0, INFINITE);
	if (FAILED(hr)) {
		U_LOG_E("DComp bridge: AcquireSync[%u] failed: 0x%08x", idx, hr);
		return XRT_ERROR_VULKAN;
	}

	ID3D11Texture2D *back = NULL;
	hr = target->dcomp_swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back);
	if (FAILED(hr) || back == NULL) {
		U_LOG_E("DComp bridge: GetBuffer failed: 0x%08x", hr);
		target->dcomp_shared_mutex[idx]->ReleaseSync(0);
		return XRT_ERROR_VULKAN;
	}

	target->dcomp_dx_context->CopyResource(back, target->dcomp_shared_dx[idx]);
	back->Release();

	target->dcomp_shared_mutex[idx]->ReleaseSync(0);

	hr = target->dcomp_swapchain->Present(/*SyncInterval*/ 1, /*Flags*/ 0);
	if (FAILED(hr)) {
		U_LOG_E("DComp bridge: Present failed: 0x%08x", hr);
		return XRT_ERROR_VULKAN;
	}
	target->dcomp_dcomp_device->Commit();
	return XRT_SUCCESS;
}

#endif // XRT_OS_WINDOWS


xrt_result_t
comp_vk_native_target_create(struct comp_vk_native_compositor *c,
                              void *hwnd,
                              uint32_t width,
                              uint32_t height,
                              bool transparent_background,
                              struct comp_vk_native_target **out_target)
{
	struct vk_bundle *vk = comp_vk_native_compositor_get_vk(c);
	uint32_t queue_family_index = comp_vk_native_compositor_get_queue_family(c);

	struct comp_vk_native_target *target = U_TYPED_CALLOC(struct comp_vk_native_target);
	if (target == NULL) {
		return XRT_ERROR_ALLOCATION;
	}

	target->vk = vk;
	target->hwnd = hwnd;
	target->width = width;
	target->height = height;
	target->queue_family_index = queue_family_index;
	target->transparent_background = transparent_background;

#ifdef XRT_OS_WINDOWS
	// Transparent-background path: VK -> D3D11 KMT shared -> DComp bridge,
	// no WSI swapchain at all. Falls back to opaque WSI on failure.
	if (transparent_background && hwnd != NULL) {
		if (dcomp_setup(target, (HWND)hwnd, width, height)) {
			*out_target = target;
			return XRT_SUCCESS;
		}
		// Setup failed: tear down anything dcomp_setup partially created and
		// fall through to the standard WSI path below (will end up OPAQUE).
		dcomp_destroy(target);
	}

	// Create Win32 surface
	// Note: vkCreateWin32SurfaceKHR is an instance-level function loaded
	// into vk_bundle by vk_get_instance_functions(). Access via vk->vkCreateWin32SurfaceKHR.
	PFN_vkCreateWin32SurfaceKHR pvkCreateWin32SurfaceKHR =
	    (PFN_vkCreateWin32SurfaceKHR)vk->vkGetInstanceProcAddr(vk->instance, "vkCreateWin32SurfaceKHR");
	if (pvkCreateWin32SurfaceKHR == NULL) {
		U_LOG_E("Failed to load vkCreateWin32SurfaceKHR");
		free(target);
		return XRT_ERROR_VULKAN;
	}

	VkWin32SurfaceCreateInfoKHR surface_ci = {
	    .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
	    .hinstance = GetModuleHandle(NULL),
	    .hwnd = (HWND)hwnd,
	};

	VkResult res = pvkCreateWin32SurfaceKHR(vk->instance, &surface_ci, NULL, &target->surface);
	if (res != VK_SUCCESS) {
		U_LOG_E("Failed to create Win32 surface: %d", res);
		free(target);
		return XRT_ERROR_VULKAN;
	}

	// Check present support
	VkBool32 present_support = VK_FALSE;
	vk->vkGetPhysicalDeviceSurfaceSupportKHR(vk->physical_device,
	                                          queue_family_index,
	                                          target->surface, &present_support);
	if (!present_support) {
		U_LOG_E("Queue family does not support presentation to Win32 surface");
		vk->vkDestroySurfaceKHR(vk->instance, target->surface, NULL);
		free(target);
		return XRT_ERROR_VULKAN;
	}
#elif defined(XRT_OS_MACOS)
	// Create Metal surface via VK_EXT_metal_surface
	// hwnd parameter is actually a CAMetalLayer* on macOS
	// Try vk_bundle's pre-loaded function pointer first,
	// fall back to runtime lookup via vkGetInstanceProcAddr
	PFN_vkCreateMetalSurfaceEXT pfnCreateMetalSurface = vk->vkCreateMetalSurfaceEXT;
	if (pfnCreateMetalSurface == NULL) {
		pfnCreateMetalSurface = (PFN_vkCreateMetalSurfaceEXT)
		    vk->vkGetInstanceProcAddr(vk->instance, "vkCreateMetalSurfaceEXT");
	}
	if (pfnCreateMetalSurface == NULL) {
		U_LOG_E("vkCreateMetalSurfaceEXT not available — VK_EXT_metal_surface must be enabled");
		free(target);
		return XRT_ERROR_VULKAN;
	}

	U_LOG_I("Creating Metal surface from CAMetalLayer %p", hwnd);

	VkMetalSurfaceCreateInfoEXT surface_ci = {
	    .sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT,
	    .pLayer = (const CAMetalLayer *)hwnd,
	};

	VkResult res = pfnCreateMetalSurface(vk->instance, &surface_ci, NULL, &target->surface);
	if (res != VK_SUCCESS) {
		U_LOG_E("Failed to create Metal surface: %d", res);
		free(target);
		return XRT_ERROR_VULKAN;
	}

	// Check present support
	VkBool32 present_support = VK_FALSE;
	vk->vkGetPhysicalDeviceSurfaceSupportKHR(vk->physical_device,
	                                          queue_family_index,
	                                          target->surface, &present_support);
	if (!present_support) {
		U_LOG_E("Queue family does not support presentation to Metal surface");
		vk->vkDestroySurfaceKHR(vk->instance, target->surface, NULL);
		free(target);
		return XRT_ERROR_VULKAN;
	}
#else
	U_LOG_E("VK native target: no supported surface type on this platform");
	free(target);
	return XRT_ERROR_DEVICE_CREATION_FAILED;
#endif

	// Create synchronization primitives
	VkSemaphoreCreateInfo sem_ci = {
	    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
	};
	VkResult vk_res;
	vk_res = vk->vkCreateSemaphore(vk->device, &sem_ci, NULL, &target->image_available);
	if (vk_res != VK_SUCCESS) {
		U_LOG_E("Failed to create image_available semaphore");
		vk->vkDestroySurfaceKHR(vk->instance, target->surface, NULL);
		free(target);
		return XRT_ERROR_VULKAN;
	}
	vk_res = vk->vkCreateSemaphore(vk->device, &sem_ci, NULL, &target->render_finished);
	if (vk_res != VK_SUCCESS) {
		U_LOG_E("Failed to create render_finished semaphore");
		vk->vkDestroySemaphore(vk->device, target->image_available, NULL);
		vk->vkDestroySurfaceKHR(vk->instance, target->surface, NULL);
		free(target);
		return XRT_ERROR_VULKAN;
	}

	// Create swapchain
	xrt_result_t xret = create_swapchain(target);
	if (xret != XRT_SUCCESS) {
		vk->vkDestroySemaphore(vk->device, target->render_finished, NULL);
		vk->vkDestroySemaphore(vk->device, target->image_available, NULL);
		vk->vkDestroySurfaceKHR(vk->instance, target->surface, NULL);
		free(target);
		return xret;
	}

	*out_target = target;

	U_LOG_I("Created VK native target: %ux%u, %u images, format %d",
	        target->width, target->height, target->image_count, target->format);

	return XRT_SUCCESS;
}

void
comp_vk_native_target_destroy(struct comp_vk_native_target **target_ptr)
{
	if (target_ptr == NULL || *target_ptr == NULL) {
		return;
	}

	struct comp_vk_native_target *target = *target_ptr;
	struct vk_bundle *vk = target->vk;

	vk->vkDeviceWaitIdle(vk->device);

#ifdef XRT_OS_WINDOWS
	if (target->dcomp_active) {
		dcomp_destroy(target);
		// dcomp_active path doesn't allocate semaphores / surface / swapchain.
		free(target);
		*target_ptr = NULL;
		return;
	}
#endif

	destroy_swapchain_views(target);

	if (target->swapchain != VK_NULL_HANDLE) {
		vk->vkDestroySwapchainKHR(vk->device, target->swapchain, NULL);
	}
	if (target->render_finished != VK_NULL_HANDLE) {
		vk->vkDestroySemaphore(vk->device, target->render_finished, NULL);
	}
	if (target->image_available != VK_NULL_HANDLE) {
		vk->vkDestroySemaphore(vk->device, target->image_available, NULL);
	}
	if (target->surface != VK_NULL_HANDLE) {
		vk->vkDestroySurfaceKHR(vk->instance, target->surface, NULL);
	}

	free(target);
	*target_ptr = NULL;
}

xrt_result_t
comp_vk_native_target_acquire(struct comp_vk_native_target *target, uint32_t *out_index)
{
	struct vk_bundle *vk = target->vk;

#ifdef XRT_OS_WINDOWS
	if (target->dcomp_active) {
		// Round-robin through the bridge ring. No WSI to acquire from.
		// vkQueueWaitIdle in the compositor's render path covers the GPU
		// fence; D3D11's IDXGIKeyedMutex::AcquireSync in dcomp_present
		// flushes the writer caches before D3D11 reads the shared resource.
		target->dcomp_ring_idx = (target->dcomp_ring_idx + 1) % DCOMP_RING;
		target->current_index = target->dcomp_ring_idx;
		*out_index = target->current_index;
		return XRT_SUCCESS;
	}
#endif

	// Use the semaphore for acquire, then do a dummy submit that waits on it
	// to ensure the image is actually available before the compositor renders.
	VkResult res = vk->vkAcquireNextImageKHR(vk->device, target->swapchain,
	                                          UINT64_MAX, target->image_available,
	                                          VK_NULL_HANDLE, &target->current_index);
	if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR) {
		// Swapchain invalidated (window resize, minimize, etc.) — recreate and retry
		U_LOG_I("Swapchain out of date, recreating");

		vk->vkDeviceWaitIdle(vk->device);
		destroy_swapchain_views(target);

		// Destroy old swapchain BEFORE creating new one — MoltenVK requires
		// the native window to be free (VK_ERROR_NATIVE_WINDOW_IN_USE_KHR).
		if (target->swapchain != VK_NULL_HANDLE) {
			vk->vkDestroySwapchainKHR(vk->device, target->swapchain, NULL);
			target->swapchain = VK_NULL_HANDLE;
		}

		xrt_result_t xret = create_swapchain(target);
		if (xret != XRT_SUCCESS) {
			U_LOG_E("Failed to recreate swapchain");
			return XRT_ERROR_VULKAN;
		}

		// Retry acquire with new swapchain
		res = vk->vkAcquireNextImageKHR(vk->device, target->swapchain,
		                                 UINT64_MAX, target->image_available,
		                                 VK_NULL_HANDLE, &target->current_index);
		if (res != VK_SUCCESS) {
			U_LOG_E("Failed to acquire after swapchain recreation: %d", res);
			return XRT_ERROR_VULKAN;
		}
	} else if (res != VK_SUCCESS) {
		U_LOG_E("Failed to acquire swapchain image: %d", res);
		return XRT_ERROR_VULKAN;
	}

	// Wait for the acquired image to be available by doing a dummy submit
	// that waits on the image_available semaphore.
	VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
	VkSubmitInfo wait_submit = {
	    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	    .waitSemaphoreCount = 1,
	    .pWaitSemaphores = &target->image_available,
	    .pWaitDstStageMask = &wait_stage,
	};
	vk->vkQueueSubmit(vk->main_queue->queue, 1, &wait_submit, VK_NULL_HANDLE);
	vk->vkQueueWaitIdle(vk->main_queue->queue);

	*out_index = target->current_index;
	return XRT_SUCCESS;
}

xrt_result_t
comp_vk_native_target_present(struct comp_vk_native_target *target)
{
	struct vk_bundle *vk = target->vk;

#ifdef XRT_OS_WINDOWS
	if (target->dcomp_active) {
		return dcomp_present(target);
	}
#endif

	// No semaphore wait needed — the compositor calls vkQueueWaitIdle
	// after all rendering commands before presenting.
	VkPresentInfoKHR present_info = {
	    .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
	    .waitSemaphoreCount = 0,
	    .pWaitSemaphores = NULL,
	    .swapchainCount = 1,
	    .pSwapchains = &target->swapchain,
	    .pImageIndices = &target->current_index,
	};

	VkResult res = vk->vkQueuePresentKHR(vk->main_queue->queue, &present_info);
	if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR) {
		return XRT_SUCCESS;
	}
	if (res != VK_SUCCESS) {
		U_LOG_E("Present failed: %d", res);
		return XRT_ERROR_VULKAN;
	}

	return XRT_SUCCESS;
}

void
comp_vk_native_target_get_dimensions(struct comp_vk_native_target *target,
                                      uint32_t *out_width,
                                      uint32_t *out_height)
{
	*out_width = target->width;
	*out_height = target->height;
}

void
comp_vk_native_target_get_current_image(struct comp_vk_native_target *target,
                                         uint64_t *out_image,
                                         uint64_t *out_view)
{
	*out_image = (uint64_t)(uintptr_t)target->images[target->current_index];
	*out_view = (uint64_t)(uintptr_t)target->views[target->current_index];
}

VkFormat
comp_vk_native_target_get_format(struct comp_vk_native_target *target)
{
	return target->format;
}

xrt_result_t
comp_vk_native_target_resize(struct comp_vk_native_target *target,
                               uint32_t width,
                               uint32_t height)
{
	struct vk_bundle *vk = target->vk;

	if (width == target->width && height == target->height) {
		return XRT_SUCCESS;
	}

	vk->vkDeviceWaitIdle(vk->device);

	destroy_swapchain_views(target);

	// Destroy old swapchain BEFORE creating new one — only one active
	// swapchain per surface is allowed (VK_ERROR_NATIVE_WINDOW_IN_USE_KHR).
	// This matches the destroy-before-create pattern in target_acquire.
	if (target->swapchain != VK_NULL_HANDLE) {
		vk->vkDestroySwapchainKHR(vk->device, target->swapchain, NULL);
		target->swapchain = VK_NULL_HANDLE;
	}

	target->width = width;
	target->height = height;

	return create_swapchain(target);
}
