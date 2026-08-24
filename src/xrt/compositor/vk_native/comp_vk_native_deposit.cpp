// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  VK-0 — the D3D11 deposit texture for the in-process Vulkan split (#1178).
 * @ingroup comp_vk_native
 *
 * Topology, the zero-copy claim and the synchronisation contract are all stated
 * in comp_vk_native_deposit.h — read that first.
 */

#include "comp_vk_native_deposit.h"

#include "xrt/xrt_vulkan_includes.h"
#include "vk/vk_helpers.h"

#include "util/u_logging.h"
#include "util/u_debug.h"
#include "util/u_misc.h"

#include <string.h>

// The deposit is a Windows mechanism end to end (D3D11 shared textures,
// ID3D11Fence, DXGI adapter LUIDs). Everything below compiles to nothing
// elsewhere; the header's entry points keep their signatures so call sites need
// no per-platform spelling beyond the one guard they already have.
#ifdef XRT_OS_WINDOWS

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan_win32.h>

/*
 * The gate. One env read for the whole process, same vocabulary as
 * DXR_WEAVE_ON_SCANOUT / DXR_VK_FORCE_GPU.
 */
DEBUG_GET_ONCE_BOOL_OPTION(vk_deposit, "DXR_VK_DEPOSIT", false)
DEBUG_GET_ONCE_BOOL_OPTION(vk_deposit_probe, "DXR_VK_DEPOSIT_PROBE", false)

struct comp_vk_deposit_slot
{
	ID3D11Texture2D *tex;
	HANDLE share_nt;
	VkImage image;
	VkImageView view;
	VkDeviceMemory memory;
};

struct comp_vk_deposit
{
	struct vk_bundle *vk;

	ID3D11Device *dx;
	ID3D11Device5 *dx5;
	ID3D11DeviceContext *ctx;
	IDXGIAdapter1 *adapter;
	uint64_t adapter_luid;

	struct comp_vk_deposit_slot ring[COMP_VK_DEPOSIT_RING];
	uint32_t slot;

	//! Shared D3D11 fence, imported into Vulkan as a timeline semaphore.
	ID3D11Fence *fence;
	HANDLE fence_nt;
	VkSemaphore timeline;

	//! Last value CLAIMED for a submit (== last value that will be signalled).
	uint64_t value;

	uint32_t width;
	uint32_t height;
	VkFormat format;

	bool probe_done;
};


/*
 *
 * Helpers.
 *
 */

//! Packed LUID (HighPart<<32 | LowPart) of the VkPhysicalDevice, 0 if unknown.
//! Same discipline as the DComp bridge's `vk_device_packed_luid` — a deposit on
//! a different adapter than the VkDevice shares no pixels at all.
static uint64_t
deposit_vk_packed_luid(struct vk_bundle *vk)
{
	if (vk->vkGetPhysicalDeviceProperties2 == NULL) {
		return 0;
	}
	VkPhysicalDeviceIDProperties id_props = {};
	id_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
	VkPhysicalDeviceProperties2 props2 = {};
	props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
	props2.pNext = &id_props;
	vk->vkGetPhysicalDeviceProperties2(vk->physical_device, &props2);
	if (!id_props.deviceLUIDValid) {
		return 0;
	}
	uint64_t luid = 0;
	memcpy(&luid, id_props.deviceLUID, sizeof(luid));
	return luid;
}

static DXGI_FORMAT
deposit_dxgi_format(VkFormat vk_format)
{
	switch (vk_format) {
	case VK_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM;
	case VK_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
	default: return DXGI_FORMAT_UNKNOWN;
	}
}

static void
deposit_free_ring(struct comp_vk_deposit *dep)
{
	struct vk_bundle *vk = dep->vk;

	for (uint32_t i = 0; i < COMP_VK_DEPOSIT_RING; i++) {
		struct comp_vk_deposit_slot *s = &dep->ring[i];
		if (s->view != VK_NULL_HANDLE) {
			vk->vkDestroyImageView(vk->device, s->view, NULL);
			s->view = VK_NULL_HANDLE;
		}
		if (s->image != VK_NULL_HANDLE) {
			vk->vkDestroyImage(vk->device, s->image, NULL);
			s->image = VK_NULL_HANDLE;
		}
		if (s->memory != VK_NULL_HANDLE) {
			vk->vkFreeMemory(vk->device, s->memory, NULL);
			s->memory = VK_NULL_HANDLE;
		}
		if (s->share_nt != NULL) {
			CloseHandle(s->share_nt);
			s->share_nt = NULL;
		}
		if (s->tex != NULL) {
			s->tex->Release();
			s->tex = NULL;
		}
	}
}

/*!
 * Import one NT-shared D3D11 texture as a renderable VkImage.
 *
 * `VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT` is the whole point: it makes the D3D11
 * texture a render target rather than a copy destination, which is what buys the
 * deposit its zero copies. The rest of the recipe (the D3D11_TEXTURE handle
 * type, the dedicated allocation) mirrors `dcomp_import_one` in
 * comp_vk_native_target.cpp, which is the import proven on this hardware.
 */
static bool
deposit_import_one(struct comp_vk_deposit *dep, uint32_t i)
{
	struct vk_bundle *vk = dep->vk;
	struct comp_vk_deposit_slot *s = &dep->ring[i];

	VkExternalMemoryImageCreateInfo external_ci = {
	    .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
	    .pNext = NULL,
	    .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,
	};

	VkImageCreateInfo image_ci = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .pNext = &external_ci,
	    .flags = 0,
	    .imageType = VK_IMAGE_TYPE_2D,
	    .format = dep->format,
	    .extent = {dep->width, dep->height, 1},
	    .mipLevels = 1,
	    .arrayLayers = 1,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .tiling = VK_IMAGE_TILING_OPTIMAL,
	    .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
	             VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};

	VkResult res = vk->vkCreateImage(vk->device, &image_ci, NULL, &s->image);
	if (res != VK_SUCCESS) {
		U_LOG_W("vk deposit: vkCreateImage[%u] failed: %d", i, res);
		return false;
	}

	VkMemoryRequirements requirements = {};
	vk->vkGetImageMemoryRequirements(vk->device, s->image, &requirements);

	VkImportMemoryWin32HandleInfoKHR import_info = {
	    .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
	    .pNext = NULL,
	    .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,
	    .handle = s->share_nt,
	};
	VkMemoryDedicatedAllocateInfoKHR dedicated_info = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO_KHR,
	    .pNext = &import_info,
	    .image = s->image,
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
		U_LOG_W("vk deposit: no compatible memory type for slot %u", i);
		return false;
	}

	VkMemoryAllocateInfo alloc_info = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .pNext = &dedicated_info,
	    .allocationSize = requirements.size,
	    .memoryTypeIndex = memory_type_index,
	};
	res = vk->vkAllocateMemory(vk->device, &alloc_info, NULL, &s->memory);
	if (res != VK_SUCCESS) {
		U_LOG_W("vk deposit: vkAllocateMemory[%u] failed: %d", i, res);
		return false;
	}

	res = vk->vkBindImageMemory(vk->device, s->image, s->memory, 0);
	if (res != VK_SUCCESS) {
		U_LOG_W("vk deposit: vkBindImageMemory[%u] failed: %d", i, res);
		return false;
	}

	VkImageViewCreateInfo view_ci = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
	    .image = s->image,
	    .viewType = VK_IMAGE_VIEW_TYPE_2D,
	    .format = dep->format,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	res = vk->vkCreateImageView(vk->device, &view_ci, NULL, &s->view);
	if (res != VK_SUCCESS) {
		U_LOG_W("vk deposit: vkCreateImageView[%u] failed: %d", i, res);
		return false;
	}

	return true;
}

/*!
 * Allocate the ring at the current dims.
 *
 * The share flags are `SHARED_NTHANDLE | SHARED` — NOT the DComp bridge's
 * `SHARED_NTHANDLE | SHARED_KEYEDMUTEX`. This is deliberate and it is the
 * difference that matters for this ladder: a keyed mutex is acquired from the
 * CPU, and the D3D legs eliminated CPU waits by construction. Fence-synchronised
 * NT sharing is the recipe the shipped cross-adapter bridge already uses for its
 * ingress (comp_xbridge.cpp `xb_make_egress_texture` / the atlas ring), so the
 * deposit hands VK-1 a texture in exactly the shape the bridge expects.
 *
 * The NT handle (not the legacy KMT one) is load-bearing: a comment in
 * comp_vk_native_target.cpp records KMT silently yielding a never-updated
 * surface on this box's Intel driver while NT shares pixels correctly.
 */
static bool
deposit_alloc_ring(struct comp_vk_deposit *dep)
{
	const DXGI_FORMAT dxgi_format = deposit_dxgi_format(dep->format);
	if (dxgi_format == DXGI_FORMAT_UNKNOWN) {
		U_LOG_W("vk deposit: no DXGI equivalent for VkFormat %d", (int)dep->format);
		return false;
	}

	for (uint32_t i = 0; i < COMP_VK_DEPOSIT_RING; i++) {
		struct comp_vk_deposit_slot *s = &dep->ring[i];

		D3D11_TEXTURE2D_DESC td = {};
		td.Width = dep->width;
		td.Height = dep->height;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = dxgi_format;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;

		HRESULT hr = dep->dx->CreateTexture2D(&td, NULL, &s->tex);
		if (FAILED(hr)) {
			U_LOG_W("vk deposit: CreateTexture2D[%u] failed: 0x%08lx", i, (unsigned long)hr);
			return false;
		}

		IDXGIResource1 *dxgi_res = NULL;
		hr = s->tex->QueryInterface(__uuidof(IDXGIResource1), (void **)&dxgi_res);
		if (FAILED(hr) || dxgi_res == NULL) {
			U_LOG_W("vk deposit: QueryInterface(IDXGIResource1)[%u] failed: 0x%08lx", i, (unsigned long)hr);
			return false;
		}
		hr = dxgi_res->CreateSharedHandle(NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, NULL,
		                                  &s->share_nt);
		dxgi_res->Release();
		if (FAILED(hr) || s->share_nt == NULL) {
			U_LOG_W("vk deposit: CreateSharedHandle[%u] failed: 0x%08lx", i, (unsigned long)hr);
			return false;
		}

		// The handle stays OPEN: Vulkan does not adopt NT handles on import,
		// and the handoff hands it to the D3D consumer. Closed in destroy.
		if (!deposit_import_one(dep, i)) {
			return false;
		}
	}

	return true;
}

/*!
 * Create the shared D3D11 fence and import it as a Vulkan timeline semaphore.
 *
 * Direction: **D3D allocates, Vulkan imports** — the same direction
 * `comp_vk_client.c` already uses for the workspace sync fence, and the only one
 * this tree has hardware evidence for. Vulkan then signals values on it from the
 * atlas submit and a D3D consumer waits on the GPU with
 * `ID3D11DeviceContext4::Wait`.
 *
 * Not routed through `vk_create_timeline_semaphore_from_native`: that helper
 * gates on `vk->features.timeline_semaphore`, which the in-process VK bundle
 * hardcodes false (comp_vk_native_compositor.c passes
 * `timeline_semaphore_enabled = false` to `vk_init_from_given`). The capability
 * gate here is the app's ACTUAL device state, plumbed in from the session.
 */
static bool
deposit_setup_sync(struct comp_vk_deposit *dep)
{
	struct vk_bundle *vk = dep->vk;

	if (vk->vkImportSemaphoreWin32HandleKHR == NULL) {
		U_LOG_W(
		    "vk deposit: no vkImportSemaphoreWin32HandleKHR "
		    "(VK_KHR_external_semaphore_win32 missing) — no GPU-side sync");
		return false;
	}

	// Is a TIMELINE semaphore importable from a D3D12 fence on this device?
	// Ask before creating anything: a false yes here becomes a consumer that
	// waits forever.
	if (vk->vkGetPhysicalDeviceExternalSemaphorePropertiesKHR != NULL) {
		VkSemaphoreTypeCreateInfo type_query = {
		    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
		    .pNext = NULL,
		    .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
		    .initialValue = 0,
		};
		VkPhysicalDeviceExternalSemaphoreInfo query = {
		    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
		    .pNext = &type_query,
		    .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT,
		};
		VkExternalSemaphoreProperties props = {
		    .sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES,
		};
		vk->vkGetPhysicalDeviceExternalSemaphorePropertiesKHR(vk->physical_device, &query, &props);
		if ((props.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT) == 0) {
			U_LOG_W(
			    "vk deposit: device cannot import a D3D12_FENCE timeline semaphore "
			    "(features=0x%x) — no GPU-side sync",
			    (unsigned)props.externalSemaphoreFeatures);
			return false;
		}
	}

	HRESULT hr = dep->dx->QueryInterface(__uuidof(ID3D11Device5), (void **)&dep->dx5);
	if (FAILED(hr) || dep->dx5 == NULL) {
		U_LOG_W("vk deposit: QueryInterface(ID3D11Device5) failed: 0x%08lx — no shared fence",
		        (unsigned long)hr);
		return false;
	}

	hr = dep->dx5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), (void **)&dep->fence);
	if (FAILED(hr) || dep->fence == NULL) {
		U_LOG_W("vk deposit: CreateFence failed: 0x%08lx", (unsigned long)hr);
		return false;
	}

	hr = dep->fence->CreateSharedHandle(NULL, GENERIC_ALL, NULL, &dep->fence_nt);
	if (FAILED(hr) || dep->fence_nt == NULL) {
		U_LOG_W("vk deposit: fence CreateSharedHandle failed: 0x%08lx", (unsigned long)hr);
		return false;
	}

	VkSemaphoreTypeCreateInfo type_info = {
	    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
	    .pNext = NULL,
	    .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
	    .initialValue = 0,
	};
	VkSemaphoreCreateInfo sem_ci = {
	    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
	    .pNext = &type_info,
	};
	VkResult res = vk->vkCreateSemaphore(vk->device, &sem_ci, NULL, &dep->timeline);
	if (res != VK_SUCCESS) {
		U_LOG_W("vk deposit: vkCreateSemaphore(TIMELINE) failed: %d", res);
		dep->timeline = VK_NULL_HANDLE;
		return false;
	}

	// Import does NOT consume the handle — the deposit keeps ownership and
	// hands the same handle to the consumer through the handoff.
	VkImportSemaphoreWin32HandleInfoKHR import = {
	    .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR,
	    .semaphore = dep->timeline,
	    .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT,
	    .handle = dep->fence_nt,
	};
	res = vk->vkImportSemaphoreWin32HandleKHR(vk->device, &import);
	if (res != VK_SUCCESS) {
		U_LOG_W("vk deposit: vkImportSemaphoreWin32HandleKHR failed: %d", res);
		vk->vkDestroySemaphore(vk->device, dep->timeline, NULL);
		dep->timeline = VK_NULL_HANDLE;
		return false;
	}

	return true;
}


/*
 *
 * 'Exported' functions.
 *
 */

extern "C" bool
comp_vk_deposit_requested(void)
{
	return debug_get_bool_option_vk_deposit();
}

extern "C" xrt_result_t
comp_vk_deposit_create(struct vk_bundle *vk,
                       bool app_timeline_semaphores,
                       uint32_t width,
                       uint32_t height,
                       VkFormat format,
                       struct comp_vk_deposit **out_deposit)
{
	*out_deposit = NULL;

	if (vk == NULL || vk->device == VK_NULL_HANDLE || width == 0 || height == 0) {
		return XRT_ERROR_VULKAN;
	}

	const uint64_t want_luid = deposit_vk_packed_luid(vk);

	/*
	 * THE DIAGNOSTIC LINE. One grep away, in the style of `weave placement:`:
	 * both LUIDs and the verdict, so "send me the `vk deposit:` line" is a
	 * complete answer to "is the deposit on the right adapter?".
	 */
	IDXGIAdapter1 *adapter = NULL;
	uint64_t got_luid = 0;
	WCHAR desc[128] = L"UNKNOWN";
	if (want_luid != 0) {
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
					got_luid = luid;
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
		U_LOG_W(
		    "vk deposit: deposit adapter=NONE, VkDevice LUID=0x%016llx, match=NO — "
		    "no same-adapter D3D11 device, deposit DISABLED (the compositor keeps its own atlas)",
		    (unsigned long long)want_luid);
		return XRT_ERROR_VULKAN;
	}

	U_LOG_W(
	    "vk deposit: deposit adapter LUID=0x%016llx ('%ls'), VkDevice LUID=0x%016llx, match=YES — "
	    "same-adapter D3D11 deposit, %ux%u x%u ring",
	    (unsigned long long)got_luid, desc, (unsigned long long)want_luid, width, height,
	    (unsigned)COMP_VK_DEPOSIT_RING);

	if (!app_timeline_semaphores) {
		U_LOG_W(
		    "vk deposit: the app's VkDevice has no VK_KHR_timeline_semaphore — the deposit's only "
		    "GPU-side sync is a D3D fence imported as a timeline semaphore, and a CPU wait is not an "
		    "acceptable substitute (#1178). Deposit DISABLED.");
		adapter->Release();
		return XRT_ERROR_VULKAN;
	}

	struct comp_vk_deposit *dep = U_TYPED_CALLOC(struct comp_vk_deposit);
	if (dep == NULL) {
		adapter->Release();
		return XRT_ERROR_ALLOCATION;
	}
	dep->vk = vk;
	dep->adapter = adapter;
	dep->adapter_luid = got_luid;
	dep->width = width;
	dep->height = height;
	dep->format = format;

	HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL,
	                               0, D3D11_SDK_VERSION, &dep->dx, NULL, &dep->ctx);
	if (FAILED(hr) || dep->dx == NULL) {
		U_LOG_W("vk deposit: D3D11CreateDevice failed: 0x%08lx — deposit DISABLED", (unsigned long)hr);
		comp_vk_deposit_destroy(&dep);
		return XRT_ERROR_VULKAN;
	}

	if (!deposit_alloc_ring(dep) || !deposit_setup_sync(dep)) {
		comp_vk_deposit_destroy(&dep);
		return XRT_ERROR_VULKAN;
	}

	U_LOG_W(
	    "vk deposit: ACTIVE — atlas renders straight into the D3D11 texture "
	    "(COLOR_ATTACHMENT, zero copies), completion published on a shared ID3D11Fence "
	    "imported as a VK timeline semaphore. No CPU wait added.");

	*out_deposit = dep;
	return XRT_SUCCESS;
}

extern "C" void
comp_vk_deposit_destroy(struct comp_vk_deposit **deposit_ptr)
{
	if (deposit_ptr == NULL || *deposit_ptr == NULL) {
		return;
	}
	struct comp_vk_deposit *dep = *deposit_ptr;
	struct vk_bundle *vk = dep->vk;

	// The ring is imported memory backed by D3D11 textures that are about to
	// be released — nothing may still be reading it.
	if (vk != NULL && vk->device != VK_NULL_HANDLE && vk->vkDeviceWaitIdle != NULL) {
		vk->vkDeviceWaitIdle(vk->device);
	}

	deposit_free_ring(dep);

	if (dep->timeline != VK_NULL_HANDLE) {
		vk->vkDestroySemaphore(vk->device, dep->timeline, NULL);
		dep->timeline = VK_NULL_HANDLE;
	}
	if (dep->fence_nt != NULL) {
		CloseHandle(dep->fence_nt);
		dep->fence_nt = NULL;
	}
	if (dep->fence != NULL) {
		dep->fence->Release();
		dep->fence = NULL;
	}
	if (dep->dx5 != NULL) {
		dep->dx5->Release();
		dep->dx5 = NULL;
	}
	if (dep->ctx != NULL) {
		dep->ctx->Release();
		dep->ctx = NULL;
	}
	if (dep->dx != NULL) {
		dep->dx->Release();
		dep->dx = NULL;
	}
	if (dep->adapter != NULL) {
		dep->adapter->Release();
		dep->adapter = NULL;
	}

	free(dep);
	*deposit_ptr = NULL;
}

extern "C" xrt_result_t
comp_vk_deposit_resize(struct comp_vk_deposit *dep, uint32_t width, uint32_t height)
{
	if (dep == NULL || width == 0 || height == 0) {
		return XRT_ERROR_VULKAN;
	}
	if (dep->width == width && dep->height == height) {
		return XRT_SUCCESS;
	}

	struct vk_bundle *vk = dep->vk;
	if (vk->vkDeviceWaitIdle != NULL) {
		vk->vkDeviceWaitIdle(vk->device);
	}

	deposit_free_ring(dep);
	dep->width = width;
	dep->height = height;
	dep->slot = 0;

	if (!deposit_alloc_ring(dep)) {
		U_LOG_W("vk deposit: resize to %ux%u failed — deposit is now INACTIVE", width, height);
		deposit_free_ring(dep);
		return XRT_ERROR_VULKAN;
	}

	U_LOG_W("vk deposit: resized to %ux%u", width, height);
	return XRT_SUCCESS;
}

extern "C" void
comp_vk_deposit_advance(struct comp_vk_deposit *dep, uint64_t *out_image, uint64_t *out_view)
{
	if (dep == NULL) {
		return;
	}
	dep->slot = (dep->slot + 1) % COMP_VK_DEPOSIT_RING;
	comp_vk_deposit_get_current(dep, out_image, out_view);
}

extern "C" void
comp_vk_deposit_get_current(struct comp_vk_deposit *dep, uint64_t *out_image, uint64_t *out_view)
{
	if (dep == NULL) {
		if (out_image != NULL) {
			*out_image = 0;
		}
		if (out_view != NULL) {
			*out_view = 0;
		}
		return;
	}
	const struct comp_vk_deposit_slot *s = &dep->ring[dep->slot];
	if (out_image != NULL) {
		*out_image = (uint64_t)(uintptr_t)s->image;
	}
	if (out_view != NULL) {
		*out_view = (uint64_t)(uintptr_t)s->view;
	}
}

extern "C" void
comp_vk_deposit_claim_signal(struct comp_vk_deposit *dep, VkSemaphore *out_semaphore, uint64_t *out_value)
{
	if (dep == NULL || dep->timeline == VK_NULL_HANDLE) {
		if (out_semaphore != NULL) {
			*out_semaphore = VK_NULL_HANDLE;
		}
		if (out_value != NULL) {
			*out_value = 0;
		}
		return;
	}
	dep->value += 1;
	if (out_semaphore != NULL) {
		*out_semaphore = dep->timeline;
	}
	if (out_value != NULL) {
		*out_value = dep->value;
	}
}

extern "C" void
comp_vk_deposit_abandon_signal(struct comp_vk_deposit *dep)
{
	if (dep == NULL || dep->value == 0) {
		return;
	}
	dep->value -= 1;
}

extern "C" bool
comp_vk_deposit_get_handoff(struct comp_vk_deposit *dep, struct comp_vk_deposit_handoff *out)
{
	if (dep == NULL || out == NULL || dep->dx == NULL) {
		return false;
	}
	const struct comp_vk_deposit_slot *s = &dep->ring[dep->slot];
	if (s->tex == NULL) {
		return false;
	}

	out->d3d11_device = dep->dx;
	out->d3d11_context = dep->ctx;
	out->dxgi_adapter = dep->adapter;
	out->texture = s->tex;
	out->shared_handle = s->share_nt;
	out->fence = dep->fence;
	out->fence_shared_handle = dep->fence_nt;
	out->fence_value = dep->value;
	out->adapter_luid = dep->adapter_luid;
	out->width = dep->width;
	out->height = dep->height;
	out->slot = dep->slot;
	return true;
}

extern "C" void
comp_vk_deposit_probe_once(struct comp_vk_deposit *dep, VkQueue queue)
{
	if (dep == NULL || dep->probe_done || !debug_get_bool_option_vk_deposit_probe()) {
		return;
	}
	if (dep->timeline == VK_NULL_HANDLE || dep->value == 0 || dep->ctx == NULL) {
		return;
	}
	dep->probe_done = true;

	struct vk_bundle *vk = dep->vk;
	struct comp_vk_deposit_slot *s = &dep->ring[dep->slot];

	/*
	 * Release the slot to the external (D3D) queue family and put it in
	 * GENERAL — the layout a D3D device may read. This is the transition
	 * VK-1 does every frame in place of the VK weave; here it runs once,
	 * after the frame has already presented, so it disturbs nothing.
	 *
	 * The image comes back next frame through the renderer's existing
	 * UNDEFINED -> TRANSFER_DST barrier, which discards contents and so
	 * needs no matching acquire.
	 */
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandPoolCreateInfo pool_ci = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
	    .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
	    .queueFamilyIndex = vk->main_queue->family_index,
	};
	if (vk->vkCreateCommandPool(vk->device, &pool_ci, NULL, &pool) != VK_SUCCESS) {
		U_LOG_W("vk deposit probe: vkCreateCommandPool failed");
		return;
	}

	VkCommandBuffer cmd = VK_NULL_HANDLE;
	VkCommandBufferAllocateInfo cba = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	    .commandPool = pool,
	    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
	    .commandBufferCount = 1,
	};
	if (vk->vkAllocateCommandBuffers(vk->device, &cba, &cmd) != VK_SUCCESS) {
		vk->vkDestroyCommandPool(vk->device, pool, NULL);
		U_LOG_W("vk deposit probe: vkAllocateCommandBuffers failed");
		return;
	}

	VkCommandBufferBeginInfo begin = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	vk->vkBeginCommandBuffer(cmd, &begin);

	VkImageMemoryBarrier release = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
	    .dstAccessMask = 0,
	    .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
	    .srcQueueFamilyIndex = vk->main_queue->family_index,
	    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
	    .image = s->image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
	                         NULL, 0, NULL, 1, &release);
	vk->vkEndCommandBuffer(cmd);

	const uint64_t signal_value = dep->value + 1;
	VkTimelineSemaphoreSubmitInfo timeline_info = {
	    .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
	    .signalSemaphoreValueCount = 1,
	    .pSignalSemaphoreValues = &signal_value,
	};
	VkSubmitInfo submit = {
	    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	    .pNext = &timeline_info,
	    .commandBufferCount = 1,
	    .pCommandBuffers = &cmd,
	    .signalSemaphoreCount = 1,
	    .pSignalSemaphores = &dep->timeline,
	};
	if (vk->vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) {
		U_LOG_W("vk deposit probe: vkQueueSubmit failed");
		vk->vkDestroyCommandPool(vk->device, pool, NULL);
		return;
	}
	dep->value = signal_value;

	/*
	 * THE CONSUMER SIDE, for real: a GPU-side wait on the imported fence.
	 * Nothing here blocks the CPU on Vulkan's completion — `Wait` queues an
	 * ordering constraint on the D3D device and returns immediately.
	 */
	ID3D11DeviceContext4 *ctx4 = NULL;
	HRESULT hr = dep->ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)&ctx4);
	if (FAILED(hr) || ctx4 == NULL) {
		U_LOG_W("vk deposit probe: QueryInterface(ID3D11DeviceContext4) failed: 0x%08lx", (unsigned long)hr);
		vk->vkDestroyCommandPool(vk->device, pool, NULL);
		return;
	}
	ctx4->Wait(dep->fence, signal_value);

	D3D11_TEXTURE2D_DESC td = {};
	s->tex->GetDesc(&td);
	td.Usage = D3D11_USAGE_STAGING;
	td.BindFlags = 0;
	td.MiscFlags = 0;
	td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

	ID3D11Texture2D *staging = NULL;
	hr = dep->dx->CreateTexture2D(&td, NULL, &staging);
	if (FAILED(hr) || staging == NULL) {
		U_LOG_W("vk deposit probe: staging CreateTexture2D failed: 0x%08lx", (unsigned long)hr);
		ctx4->Release();
		vk->vkDestroyCommandPool(vk->device, pool, NULL);
		return;
	}
	ctx4->CopyResource(staging, s->tex);

	// The one CPU wait in this file, and it is a one-shot diagnostic that
	// runs after present — never on the steady-state path.
	D3D11_MAPPED_SUBRESOURCE map = {};
	hr = ctx4->Map(staging, 0, D3D11_MAP_READ, 0, &map);
	if (SUCCEEDED(hr)) {
		uint64_t nonzero = 0;
		uint64_t sum = 0;
		for (uint32_t y = 0; y < dep->height; y++) {
			const uint8_t *row = (const uint8_t *)map.pData + (size_t)y * map.RowPitch;
			for (uint32_t x = 0; x < dep->width; x++) {
				const uint8_t *px = row + (size_t)x * 4;
				const uint32_t v = ((uint32_t)px[0] | ((uint32_t)px[1] << 8) | ((uint32_t)px[2] << 16));
				if (v != 0) {
					nonzero++;
				}
				sum += v;
			}
		}
		ctx4->Unmap(staging, 0);
		const uint64_t total = (uint64_t)dep->width * dep->height;
		U_LOG_W(
		    "vk deposit probe: fence wait resolved at value %llu; deposit %ux%u has %llu/%llu "
		    "non-black pixels (%.1f%%), checksum 0x%016llx — %s",
		    (unsigned long long)signal_value, dep->width, dep->height, (unsigned long long)nonzero,
		    (unsigned long long)total, total ? (100.0 * (double)nonzero / (double)total) : 0.0,
		    (unsigned long long)sum,
		    nonzero > 0 ? "VULKAN PIXELS ARE IN THE D3D11 TEXTURE" : "EMPTY (deposit not written)");
	} else {
		U_LOG_W("vk deposit probe: Map failed: 0x%08lx", (unsigned long)hr);
	}

	staging->Release();
	ctx4->Release();
	vk->vkDestroyCommandPool(vk->device, pool, NULL);
}

#else /* !XRT_OS_WINDOWS */

extern "C" bool
comp_vk_deposit_requested(void)
{
	return false;
}

extern "C" xrt_result_t
comp_vk_deposit_create(struct vk_bundle *vk,
                       bool app_timeline_semaphores,
                       uint32_t width,
                       uint32_t height,
                       VkFormat format,
                       struct comp_vk_deposit **out_deposit)
{
	(void)vk;
	(void)app_timeline_semaphores;
	(void)width;
	(void)height;
	(void)format;
	*out_deposit = NULL;
	return XRT_ERROR_VULKAN;
}

extern "C" void
comp_vk_deposit_destroy(struct comp_vk_deposit **deposit_ptr)
{
	(void)deposit_ptr;
}

extern "C" xrt_result_t
comp_vk_deposit_resize(struct comp_vk_deposit *dep, uint32_t width, uint32_t height)
{
	(void)dep;
	(void)width;
	(void)height;
	return XRT_ERROR_VULKAN;
}

extern "C" void
comp_vk_deposit_advance(struct comp_vk_deposit *dep, uint64_t *out_image, uint64_t *out_view)
{
	(void)dep;
	if (out_image != NULL) {
		*out_image = 0;
	}
	if (out_view != NULL) {
		*out_view = 0;
	}
}

extern "C" void
comp_vk_deposit_get_current(struct comp_vk_deposit *dep, uint64_t *out_image, uint64_t *out_view)
{
	comp_vk_deposit_advance(dep, out_image, out_view);
}

extern "C" void
comp_vk_deposit_claim_signal(struct comp_vk_deposit *dep, VkSemaphore *out_semaphore, uint64_t *out_value)
{
	(void)dep;
	if (out_semaphore != NULL) {
		*out_semaphore = VK_NULL_HANDLE;
	}
	if (out_value != NULL) {
		*out_value = 0;
	}
}

extern "C" void
comp_vk_deposit_abandon_signal(struct comp_vk_deposit *dep)
{
	(void)dep;
}

extern "C" bool
comp_vk_deposit_get_handoff(struct comp_vk_deposit *dep, struct comp_vk_deposit_handoff *out)
{
	(void)dep;
	(void)out;
	return false;
}

extern "C" void
comp_vk_deposit_probe_once(struct comp_vk_deposit *dep, VkQueue queue)
{
	(void)dep;
	(void)queue;
}

#endif /* XRT_OS_WINDOWS */
