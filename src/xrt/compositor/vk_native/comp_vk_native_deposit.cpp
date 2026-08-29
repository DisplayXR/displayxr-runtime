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

#include <stdio.h>
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
	//! KEYED-MUTEX mode only (ADR-039 Phase A): the slot's mutex, else NULL.
	IDXGIKeyedMutex *km;
	/*!
	 * VK-1 (#1178) — the timeline value the app-end D3D11 context signals once
	 * a consumer has finished reading this slot. Vulkan's next write into it
	 * waits for exactly this. 0 while no consumer has taken it.
	 */
	uint64_t release_value;
};

/*!
 * VK-1b (#1178) — one PLANE surface. Single-buffered on purpose; see
 * @ref comp_vk_deposit_note_planes_consumed for the fence edge that replaces the
 * spare texture a ring would be.
 */
struct comp_vk_deposit_plane_slot
{
	ID3D11Texture2D *tex;
	HANDLE share_nt;
	VkImage image;
	VkImageView view;
	VkDeviceMemory memory;
	uint32_t width, height;
	VkFormat format;
	//! Bumped on REALLOCATION only — `comp_xbridge_bind_plane`'s re-open key.
	uint64_t generation;
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

	//! VK-1b — the plane surfaces, and the one release value they all share.
	struct comp_vk_deposit_plane_slot plane[COMP_VK_DEPOSIT_PLANE_COUNT];
	uint64_t plane_release_value;

	//! Shared D3D11 fence, imported into Vulkan as a timeline semaphore.
	ID3D11Fence *fence;
	HANDLE fence_nt;
	VkSemaphore timeline;

	/*!
	 * KEYED-MUTEX mode (ADR-039 Phase A): the driver exposes no D3D12_FENCE
	 * import, so the ring slots are SHARED_KEYEDMUTEX and each side brackets
	 * its access with key 0 instead of signalling a timeline. See the mode
	 * selection in comp_vk_deposit_create for why it is NOT a 0/1 ping-pong.
	 */
	bool keyed_mutex_mode;
	//! Chain storage for @ref comp_vk_deposit_chain_km — must outlive the
	//! vkQueueSubmit it is chained into; one submit is built at a time.
	VkWin32KeyedMutexAcquireReleaseInfoKHR km_chain;
	VkDeviceMemory km_chain_mem;
	uint64_t km_chain_key;
	uint32_t km_chain_timeout_ms;

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
	// VK-1b: the authored zone mask is a scalar coverage plane, R8 on both ends.
	case VK_FORMAT_R8_UNORM: return DXGI_FORMAT_R8_UNORM;
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
		if (s->km != NULL) {
			s->km->Release();
			s->km = NULL;
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
deposit_import_shared(struct comp_vk_deposit *dep,
                      HANDLE share_nt,
                      uint32_t width,
                      uint32_t height,
                      VkFormat format,
                      const char *what,
                      VkImage *out_image,
                      VkDeviceMemory *out_memory,
                      VkImageView *out_view)
{
	struct vk_bundle *vk = dep->vk;

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
	    .format = format,
	    .extent = {width, height, 1},
	    .mipLevels = 1,
	    .arrayLayers = 1,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .tiling = VK_IMAGE_TILING_OPTIMAL,
	    .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
	             VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};

	VkResult res = vk->vkCreateImage(vk->device, &image_ci, NULL, out_image);
	if (res != VK_SUCCESS) {
		U_LOG_W("vk deposit: vkCreateImage(%s) failed: %d", what, res);
		return false;
	}

	VkMemoryRequirements requirements = {};
	vk->vkGetImageMemoryRequirements(vk->device, *out_image, &requirements);

	VkImportMemoryWin32HandleInfoKHR import_info = {
	    .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
	    .pNext = NULL,
	    .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,
	    .handle = share_nt,
	};
	VkMemoryDedicatedAllocateInfoKHR dedicated_info = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO_KHR,
	    .pNext = &import_info,
	    .image = *out_image,
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
		U_LOG_W("vk deposit: no compatible memory type for %s", what);
		return false;
	}

	VkMemoryAllocateInfo alloc_info = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .pNext = &dedicated_info,
	    .allocationSize = requirements.size,
	    .memoryTypeIndex = memory_type_index,
	};
	res = vk->vkAllocateMemory(vk->device, &alloc_info, NULL, out_memory);
	if (res != VK_SUCCESS) {
		U_LOG_W("vk deposit: vkAllocateMemory(%s) failed: %d", what, res);
		return false;
	}

	res = vk->vkBindImageMemory(vk->device, *out_image, *out_memory, 0);
	if (res != VK_SUCCESS) {
		U_LOG_W("vk deposit: vkBindImageMemory(%s) failed: %d", what, res);
		return false;
	}

	VkImageViewCreateInfo view_ci = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
	    .image = *out_image,
	    .viewType = VK_IMAGE_VIEW_TYPE_2D,
	    .format = format,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	res = vk->vkCreateImageView(vk->device, &view_ci, NULL, out_view);
	if (res != VK_SUCCESS) {
		U_LOG_W("vk deposit: vkCreateImageView(%s) failed: %d", what, res);
		return false;
	}

	return true;
}

static bool
deposit_import_one(struct comp_vk_deposit *dep, uint32_t i)
{
	struct comp_vk_deposit_slot *s = &dep->ring[i];
	char what[32];
	snprintf(what, sizeof(what), "atlas slot %u", i);
	return deposit_import_shared(dep, s->share_nt, dep->width, dep->height, dep->format, what, &s->image,
	                             &s->memory, &s->view);
}

/*!
 * Create ONE NT-shared D3D11 texture on the deposit's device and return it with
 * its NT handle. In FENCE mode the share flags are the atlas ring's, verbatim:
 * fence-synchronised NT sharing, never `SHARED_KEYEDMUTEX` (a keyed mutex is
 * acquired from the CPU, and this ladder eliminated CPU waits by construction).
 * KEYED-MUTEX mode (ADR-039 Phase A) is the exception that proves the rule: on
 * a driver with no D3D12_FENCE import there is no fence to synchronise with,
 * and the mutex — bounded, skip-on-timeout — is the rung below nothing at all.
 */
static bool
deposit_make_shared_texture(struct comp_vk_deposit *dep,
                            uint32_t width,
                            uint32_t height,
                            DXGI_FORMAT dxgi_format,
                            bool keyed_mutex,
                            const char *what,
                            ID3D11Texture2D **out_tex,
                            HANDLE *out_share)
{
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = width;
	td.Height = height;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = dxgi_format;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	td.MiscFlags = keyed_mutex ? (D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX)
	                           : (D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED);

	HRESULT hr = dep->dx->CreateTexture2D(&td, NULL, out_tex);
	if (FAILED(hr) || *out_tex == NULL) {
		U_LOG_W("vk deposit: CreateTexture2D(%s) failed: 0x%08lx", what, (unsigned long)hr);
		return false;
	}

	IDXGIResource1 *dxgi_res = NULL;
	hr = (*out_tex)->QueryInterface(__uuidof(IDXGIResource1), (void **)&dxgi_res);
	if (FAILED(hr) || dxgi_res == NULL) {
		U_LOG_W("vk deposit: QueryInterface(IDXGIResource1)(%s) failed: 0x%08lx", what, (unsigned long)hr);
		return false;
	}
	hr =
	    dxgi_res->CreateSharedHandle(NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, NULL, out_share);
	dxgi_res->Release();
	if (FAILED(hr) || *out_share == NULL) {
		U_LOG_W("vk deposit: CreateSharedHandle(%s) failed: 0x%08lx", what, (unsigned long)hr);
		return false;
	}
	return true;
}

//! Free one plane surface. Idempotent. The caller has already idled the device.
static void
deposit_free_plane(struct comp_vk_deposit *dep, uint32_t plane)
{
	struct vk_bundle *vk = dep->vk;
	struct comp_vk_deposit_plane_slot *p = &dep->plane[plane];

	if (p->view != VK_NULL_HANDLE) {
		vk->vkDestroyImageView(vk->device, p->view, NULL);
		p->view = VK_NULL_HANDLE;
	}
	if (p->image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, p->image, NULL);
		p->image = VK_NULL_HANDLE;
	}
	if (p->memory != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, p->memory, NULL);
		p->memory = VK_NULL_HANDLE;
	}
	if (p->share_nt != NULL) {
		CloseHandle(p->share_nt);
		p->share_nt = NULL;
	}
	if (p->tex != NULL) {
		p->tex->Release();
		p->tex = NULL;
	}
	p->width = 0;
	p->height = 0;
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

		char what[32];
		snprintf(what, sizeof(what), "atlas slot %u", i);
		if (!deposit_make_shared_texture(dep, dep->width, dep->height, dxgi_format, dep->keyed_mutex_mode,
		                                 what, &s->tex, &s->share_nt)) {
			return false;
		}

		if (dep->keyed_mutex_mode) {
			HRESULT hr = s->tex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&s->km);
			if (FAILED(hr) || s->km == NULL) {
				U_LOG_W("vk deposit: QueryInterface(IDXGIKeyedMutex)(%s) failed: 0x%08lx", what,
				        (unsigned long)hr);
				return false;
			}
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
/*!
 * Is a TIMELINE semaphore importable from a D3D12 fence on this device?
 *
 * Asked BEFORE anything is created — the answer picks the deposit's sync mode
 * (a false yes becomes a consumer that waits forever). A missing query entry
 * point is treated as importable, exactly as the original setup did.
 */
static bool
deposit_fence_importable(struct vk_bundle *vk)
{
	if (vk->vkImportSemaphoreWin32HandleKHR == NULL) {
		U_LOG_W(
		    "vk deposit: no vkImportSemaphoreWin32HandleKHR "
		    "(VK_KHR_external_semaphore_win32 missing) — no D3D fence import");
		return false;
	}
	if (vk->vkGetPhysicalDeviceExternalSemaphorePropertiesKHR == NULL) {
		return true;
	}
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
		    "(features=0x%x)",
		    (unsigned)props.externalSemaphoreFeatures);
		return false;
	}
	return true;
}

static bool
deposit_setup_sync(struct comp_vk_deposit *dep)
{
	struct vk_bundle *vk = dep->vk;

	// FENCE mode only, and create() has already run deposit_fence_importable.

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
                       bool app_keyed_mutex,
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

	/*
	 * Pick the SYNC MODE before anything is allocated — the ring's share flags
	 * depend on it.
	 *
	 * FENCE mode is the design (header §Synchronisation): zero CPU waits, a
	 * shared D3D11 fence imported as a timeline semaphore. Some drivers (this
	 * box's Intel UHD VK ICD, ADR-039 Phase A) expose no D3D12_FENCE import at
	 * all; the rung below is KEYED-MUTEX mode — every ring slot is
	 * SHARED_KEYEDMUTEX and both sides bracket their access with KEY 0.
	 *
	 * Key 0 on both sides is deliberate: plain GPU-scoped mutual exclusion, NOT
	 * a 0/1 ready-handshake. A ping-pong wedges the queue the first time either
	 * side skips a beat (consumer acquire timeout, split retirement) because
	 * the slot is left at the key the other side will never release — and a
	 * timed-out in-submit Vulkan acquire is effectively device loss. Mutual
	 * exclusion alone closes the one edge the 2-slot ring cannot (a slot
	 * REWRITE overtaking the bridge's in-flight copy, VK-1's tear); the
	 * write-before-read direction is carried by the frame path's existing
	 * per-frame CPU wait (the vkQueueWaitIdle after every atlas submit, #837).
	 * If #837 ever removes that wait, the worst case here becomes a
	 * one-frame-stale copy — never a tear, never a wedge (#925: no unbounded
	 * waits).
	 */
	const bool fence_importable = deposit_fence_importable(vk);
	bool keyed_mutex_mode = false;
	if (fence_importable) {
		if (!app_timeline_semaphores) {
			U_LOG_W(
			    "vk deposit: the app's VkDevice has no VK_KHR_timeline_semaphore — the deposit's only "
			    "GPU-side sync is a D3D fence imported as a timeline semaphore, and a CPU wait is not "
			    "an acceptable substitute (#1178). Deposit DISABLED.");
			adapter->Release();
			return XRT_ERROR_VULKAN;
		}
	} else if (app_keyed_mutex) {
		keyed_mutex_mode = true;
	} else {
		U_LOG_W(
		    "vk deposit: device can import neither a D3D12_FENCE timeline semaphore nor bracket a "
		    "keyed mutex (VK_KHR_win32_keyed_mutex not enabled on the app's VkDevice) — no GPU-side "
		    "sync. Deposit DISABLED.");
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
	dep->keyed_mutex_mode = keyed_mutex_mode;

	HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL,
	                               0, D3D11_SDK_VERSION, &dep->dx, NULL, &dep->ctx);
	if (FAILED(hr) || dep->dx == NULL) {
		U_LOG_W("vk deposit: D3D11CreateDevice failed: 0x%08lx — deposit DISABLED", (unsigned long)hr);
		comp_vk_deposit_destroy(&dep);
		return XRT_ERROR_VULKAN;
	}

	if (!deposit_alloc_ring(dep) || (!keyed_mutex_mode && !deposit_setup_sync(dep))) {
		comp_vk_deposit_destroy(&dep);
		return XRT_ERROR_VULKAN;
	}

	if (keyed_mutex_mode) {
		U_LOG_W(
		    "vk deposit: ACTIVE (KEYED-MUTEX mode, ADR-039 Phase A) — atlas renders straight into the "
		    "D3D11 texture (COLOR_ATTACHMENT, zero copies); no D3D12_FENCE import on this driver, so "
		    "each side brackets its access with the slot's keyed mutex (key 0, bounded, "
		    "skip-on-timeout) instead of a timeline signal. Plane deposits run timing-only (#1274).");
	} else {
		U_LOG_W(
		    "vk deposit: ACTIVE — atlas renders straight into the D3D11 texture "
		    "(COLOR_ATTACHMENT, zero copies), completion published on a shared ID3D11Fence "
		    "imported as a VK timeline semaphore. No CPU wait added.");
	}

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
	for (uint32_t p = 0; p < COMP_VK_DEPOSIT_PLANE_COUNT; p++) {
		deposit_free_plane(dep, p);
	}

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

extern "C" void
comp_vk_deposit_chain_km(struct comp_vk_deposit *dep, VkSubmitInfo *submit_info)
{
	if (dep == NULL || !dep->keyed_mutex_mode || submit_info == NULL) {
		return;
	}
	struct comp_vk_deposit_slot *s = &dep->ring[dep->slot];
	if (s->memory == VK_NULL_HANDLE) {
		return;
	}

	/*
	 * Key 0 on both acquire and release — mutual exclusion, not a handshake;
	 * the mode-selection comment in comp_vk_deposit_create carries the wedge
	 * argument. The timeout bounds the acquire against a consumer copy that is
	 * milliseconds long, so it is generous without being INFINITE (a timed-out
	 * in-submit acquire is not a recoverable event, and must never happen).
	 */
	dep->km_chain_mem = s->memory;
	dep->km_chain_key = 0;
	dep->km_chain_timeout_ms = 512;
	VkWin32KeyedMutexAcquireReleaseInfoKHR info = {
	    .sType = VK_STRUCTURE_TYPE_WIN32_KEYED_MUTEX_ACQUIRE_RELEASE_INFO_KHR,
	    .pNext = submit_info->pNext,
	    .acquireCount = 1,
	    .pAcquireSyncs = &dep->km_chain_mem,
	    .pAcquireKeys = &dep->km_chain_key,
	    .pAcquireTimeouts = &dep->km_chain_timeout_ms,
	    .releaseCount = 1,
	    .pReleaseSyncs = &dep->km_chain_mem,
	    .pReleaseKeys = &dep->km_chain_key,
	};
	dep->km_chain = info;
	submit_info->pNext = &dep->km_chain;
}

extern "C" void
comp_vk_deposit_note_consumed(struct comp_vk_deposit *dep, uint32_t slot)
{
	if (dep == NULL || dep->fence == NULL || dep->ctx == NULL || slot >= COMP_VK_DEPOSIT_RING) {
		return;
	}

	ID3D11DeviceContext4 *ctx4 = NULL;
	if (FAILED(dep->ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)&ctx4)) || ctx4 == NULL) {
		/*
		 * Without ID3D11DeviceContext4 there is no queued signal to give, and
		 * a value RECORDED but never signalled would hang Vulkan forever —
		 * strictly worse than no back-pressure. Leave release_value at 0: the
		 * ring then keeps VK-0's timing-only separation, which is what a
		 * machine lacking the interface had anyway.
		 */
		static bool warned = false;
		if (!warned) {
			warned = true;
			U_LOG_W("vk deposit: no ID3D11DeviceContext4 — the ring keeps VK-0's timing-only "
			        "separation instead of a queued release (#1178)");
		}
		return;
	}

	// Past every value claimed so far, so a release can never be mistaken for
	// (or overtaken by) one of Vulkan's own atlas signals on the same timeline.
	dep->value += 1;
	ctx4->Signal(dep->fence, dep->value);
	dep->ring[slot].release_value = dep->value;
	ctx4->Release();
}

extern "C" uint64_t
comp_vk_deposit_current_slot_wait(struct comp_vk_deposit *dep)
{
	if (dep == NULL || dep->timeline == VK_NULL_HANDLE) {
		return 0;
	}
	return dep->ring[dep->slot].release_value;
}

extern "C" VkSemaphore
comp_vk_deposit_get_timeline(struct comp_vk_deposit *dep)
{
	return dep != NULL ? dep->timeline : VK_NULL_HANDLE;
}


/*
 *
 * VK-1b — the plane surfaces.
 *
 */

extern "C" bool
comp_vk_deposit_plane_ensure(
    struct comp_vk_deposit *dep, uint32_t plane, uint32_t width, uint32_t height, VkFormat format)
{
	if (dep == NULL || dep->dx == NULL || plane >= COMP_VK_DEPOSIT_PLANE_COUNT || width == 0 || height == 0) {
		return false;
	}
	if (dep->keyed_mutex_mode) {
		/*
		 * #1274 — TIMING-ONLY planes. The planes' fence edges (the flatten's
		 * timeline signal/wait, note_planes_consumed's release) all no-op
		 * naturally in this mode, and what carries correctness instead is
		 * the frame path's structure: the flatten submits BEFORE the frame's
		 * per-frame CPU wait (#837), so the bridge's read — recorded at
		 * submit_atlas time on the D3D11 immediate context — always sees a
		 * complete plane; and the reverse edge (the next flatten overwriting
		 * an in-flight read) has a full app-frame period of separation on an
		 * ON-CHANGE surface. The same argument, weaker inputs, than the KM
		 * atlas ring's — and the same degrade family as the missing-ctx4
		 * rung. If #837's wait is ever removed, revisit both together.
		 */
		static bool km_plane_logged = false;
		if (!km_plane_logged) {
			km_plane_logged = true;
			U_LOG_W(
			    "vk deposit: plane deposits run TIMING-ONLY in KEYED-MUTEX mode (#1274) — "
			    "Local2D/backdrop/mask transport with no fence edges; ordering rides the "
			    "per-frame CPU wait (#837)");
		}
	}
	struct comp_vk_deposit_plane_slot *p = &dep->plane[plane];

	// The steady-state call. The 2D planes are panel-sized once, so after
	// warmup this is every frame's answer and it costs a compare.
	if (p->image != VK_NULL_HANDLE && p->width == width && p->height == height && p->format == format) {
		return true;
	}

	const DXGI_FORMAT dxgi_format = deposit_dxgi_format(format);
	if (dxgi_format == DXGI_FORMAT_UNKNOWN) {
		U_LOG_W("vk deposit: plane %u — no DXGI equivalent for VkFormat %d", plane, (int)format);
		return false;
	}

	/*
	 * A REALLOCATION. The old surface is imported memory the GPU may still be
	 * reading (Vulkan's flatten, or the bridge producer's copy), so idle before
	 * freeing — the same reason comp_vk_deposit_resize does. This is an
	 * on-change event: the 2D planes reach it once, and the authored mask only
	 * when the app authors one at new dims.
	 */
	struct vk_bundle *vk = dep->vk;
	if (p->image != VK_NULL_HANDLE && vk->vkDeviceWaitIdle != NULL) {
		vk->vkDeviceWaitIdle(vk->device);
	}
	deposit_free_plane(dep, plane);

	char what[40];
	snprintf(what, sizeof(what), "plane %u", plane);
	if (!deposit_make_shared_texture(dep, width, height, dxgi_format, false, what, &p->tex, &p->share_nt) ||
	    !deposit_import_shared(dep, p->share_nt, width, height, format, what, &p->image, &p->memory, &p->view)) {
		// Feature-local: the caller stops staging this plane and the 3D weave
		// is untouched. Never a reason to give the scanout adapter back.
		U_LOG_W(
		    "vk deposit: plane %u could not be allocated at %ux%u — THAT FEATURE is off for this "
		    "session; the weave is unaffected (#1178 VK-1b)",
		    plane, width, height);
		deposit_free_plane(dep, plane);
		return false;
	}

	p->width = width;
	p->height = height;
	p->format = format;
	p->generation++;
	U_LOG_W("vk deposit: plane %u up — %ux%u, generation %llu (#1178 VK-1b)", plane, width, height,
	        (unsigned long long)p->generation);
	return true;
}

extern "C" bool
comp_vk_deposit_plane_get(struct comp_vk_deposit *dep, uint32_t plane, struct comp_vk_deposit_plane *out)
{
	if (dep == NULL || out == NULL || plane >= COMP_VK_DEPOSIT_PLANE_COUNT) {
		return false;
	}
	const struct comp_vk_deposit_plane_slot *p = &dep->plane[plane];
	if (p->image == VK_NULL_HANDLE || p->tex == NULL) {
		return false;
	}
	out->texture = p->tex;
	out->shared_handle = p->share_nt;
	out->image = (uint64_t)(uintptr_t)p->image;
	out->view = (uint64_t)(uintptr_t)p->view;
	out->width = p->width;
	out->height = p->height;
	out->generation = p->generation;
	return true;
}

extern "C" void
comp_vk_deposit_note_planes_consumed(struct comp_vk_deposit *dep)
{
	if (dep == NULL || dep->fence == NULL || dep->ctx == NULL) {
		return;
	}

	ID3D11DeviceContext4 *ctx4 = NULL;
	if (FAILED(dep->ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)&ctx4)) || ctx4 == NULL) {
		// Same reasoning as comp_vk_deposit_note_consumed's: a value RECORDED
		// but never signalled hangs Vulkan forever, which is strictly worse
		// than the timing-only separation a machine without the interface had.
		return;
	}

	/*
	 * Past every value claimed so far, on the APP IMMEDIATE CONTEXT — which is
	 * the whole mechanism. The caller has just taken
	 * `comp_xbridge_pre_plane_write` for every live plane on this same context,
	 * so this signal sits behind the producer's read of them in one ordered
	 * stream. One signal covers every plane for exactly that reason.
	 */
	dep->value += 1;
	ctx4->Signal(dep->fence, dep->value);
	dep->plane_release_value = dep->value;
	ctx4->Release();
}

extern "C" uint64_t
comp_vk_deposit_plane_wait_value(struct comp_vk_deposit *dep)
{
	if (dep == NULL || dep->timeline == VK_NULL_HANDLE) {
		return 0;
	}
	return dep->plane_release_value;
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
	out->keyed_mutex = s->km;
	out->fence = dep->fence;
	out->fence_shared_handle = dep->fence_nt;
	out->fence_value = dep->value;
	out->adapter_luid = dep->adapter_luid;
	out->width = dep->width;
	out->height = dep->height;
	out->slot = dep->slot;
	out->dxgi_format = (uint32_t)deposit_dxgi_format(dep->format);
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
                       bool app_keyed_mutex,
                       uint32_t width,
                       uint32_t height,
                       VkFormat format,
                       struct comp_vk_deposit **out_deposit)
{
	(void)vk;
	(void)app_timeline_semaphores;
	(void)app_keyed_mutex;
	(void)width;
	(void)height;
	(void)format;
	*out_deposit = NULL;
	return XRT_ERROR_VULKAN;
}

extern "C" void
comp_vk_deposit_chain_km(struct comp_vk_deposit *dep, VkSubmitInfo *submit_info)
{
	(void)dep;
	(void)submit_info;
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

extern "C" void
comp_vk_deposit_note_consumed(struct comp_vk_deposit *dep, uint32_t slot)
{
	(void)dep;
	(void)slot;
}

extern "C" uint64_t
comp_vk_deposit_current_slot_wait(struct comp_vk_deposit *dep)
{
	(void)dep;
	return 0;
}

extern "C" VkSemaphore
comp_vk_deposit_get_timeline(struct comp_vk_deposit *dep)
{
	(void)dep;
	return VK_NULL_HANDLE;
}

extern "C" bool
comp_vk_deposit_get_handoff(struct comp_vk_deposit *dep, struct comp_vk_deposit_handoff *out)
{
	(void)dep;
	(void)out;
	return false;
}

extern "C" bool
comp_vk_deposit_plane_ensure(
    struct comp_vk_deposit *dep, uint32_t plane, uint32_t width, uint32_t height, VkFormat format)
{
	(void)dep;
	(void)plane;
	(void)width;
	(void)height;
	(void)format;
	return false;
}

extern "C" bool
comp_vk_deposit_plane_get(struct comp_vk_deposit *dep, uint32_t plane, struct comp_vk_deposit_plane *out)
{
	(void)dep;
	(void)plane;
	(void)out;
	return false;
}

extern "C" void
comp_vk_deposit_note_planes_consumed(struct comp_vk_deposit *dep)
{
	(void)dep;
}

extern "C" uint64_t
comp_vk_deposit_plane_wait_value(struct comp_vk_deposit *dep)
{
	(void)dep;
	return 0;
}

extern "C" void
comp_vk_deposit_probe_once(struct comp_vk_deposit *dep, VkQueue queue)
{
	(void)dep;
	(void)queue;
}

#endif /* XRT_OS_WINDOWS */
