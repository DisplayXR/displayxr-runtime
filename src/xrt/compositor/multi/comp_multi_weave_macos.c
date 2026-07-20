// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_weave on the macOS service path (#759) — the macOS analogue of
 *         the D3D11 service weave (#625).
 * @author David Fattal
 * @ingroup comp_multi
 *
 * A window-bound synchronous weave service for present-owners (a browser GPU
 * process, a CEF host): the caller owns its NSWindow and presents itself, but
 * hands the runtime pre-weave side-by-side stereo pixels + window-relative
 * rect(s) and composites back a weaved shared texture. The caller NEVER weaves
 * (ADR-007 / ADR-019).
 *
 * macOS platform mapping (vs the Windows/D3D11 original in
 * comp_d3d11_service.cpp):
 *
 *  - Input texture   = a caller-allocated IOSurface. It crosses the IPC as a
 *    global IOSurfaceID (ipc_message_channel_unix.c) and arrives here as a
 *    retained IOSurfaceRef, imported into a VkImage via VK_EXT_metal_objects +
 *    VK_EXT_external_memory_metal (same pattern as
 *    comp_vk_native_compositor.c import_shared_iosurface).
 *  - Output texture  = a service-allocated IOSurface-backed VkImage (the
 *    VkExportMetalObjectCreateInfoEXT allocation pattern from
 *    vk_image_allocator.c), exported back to the caller as an IOSurfaceRef.
 *  - Input-ready sync: there is no keyed mutex on macOS. The contract is that
 *    the caller completes its GPU writes into the input IOSurface before
 *    calling xrWeaveSubmitDXR (mirrors the existing macOS IPC submit_fallback
 *    model, comp_vk_client.c).
 *  - Completion sync: SYNCHRONOUS — the service vkWaitForFences before the IPC
 *    reply returns, so xrWeaveSubmitDXR returning IS the completion signal.
 *    weave_get_fence reports no fence; XrWeaveOutputDXR::fence stays NULL and
 *    fenceValue is a plain monotonic counter.
 *  - Output sizing: batch (v3) submits size the output from the INPUT IOSurface
 *    dims — the v3 contract makes the input window-client-sized, so no window
 *    geometry query is needed. Legacy single-rect submits size it from
 *    rect offset+extent (the Windows fallback rule).
 *  - Window bind: stored for future phase use only. sim_display has no
 *    snap_window_rect (anaglyph has no interlace lattice), so
 *    xrWeaveSnapWindowRectDXR is the well-defined identity snap.
 *
 * The weave itself is ONE xrt_display_processor process_atlas per submit over a
 * window-sized 2x1 SBS scratch atlas that all rects are blitted into — the same
 * one-weave-per-frame batch strategy as Windows (per-rect weave() calls degrade
 * a vendor weaver's predictor; see comp_d3d11_service.cpp). The DP instance is
 * created from the plug-in's Vulkan factory (dp_factory_vk), i.e. exactly the
 * DP family the macOS shared-surface path already drives — sim_display's
 * anaglyph SPIR-V pipeline runs on MoltenVK today.
 */

#include "xrt/xrt_compositor.h"
#include "xrt/xrt_display_processor.h"
#include "xrt/xrt_display_metrics.h"
#include "xrt/xrt_handles.h"

#include "util/u_misc.h"
#include "util/u_logging.h"

#include "vk/vk_helpers.h"

#include "comp_multi_private.h"

#ifdef XRT_OS_MACOS

#include <IOSurface/IOSurface.h>
#include <CoreFoundation/CoreFoundation.h>

/*
 *
 * Helpers.
 *
 */

//! Everything is BGRA8 end-to-end: IOSurfaces are canonically BGRA on macOS and
//! the DP factory gets the same format so its pipelines/render-pass match.
#define WEAVE_VK_FORMAT VK_FORMAT_B8G8R8A8_UNORM

static struct vk_bundle *
weave_get_vk(struct multi_compositor *mc)
{
	if (mc == NULL || mc->msc == NULL || mc->msc->target_service == NULL) {
		return NULL;
	}
	return comp_target_service_get_vk(mc->msc->target_service);
}

//! Lazily create the per-client engine lock (multi_compositor is zero-alloced).
static void
weave_ensure_mutex(struct multi_compositor *mc)
{
	os_mutex_lock(&mc->msc->list_and_timing_lock);
	if (!mc->weave.mutex_initialized) {
		os_mutex_init(&mc->weave.mutex);
		mc->weave.mutex_initialized = true;
	}
	os_mutex_unlock(&mc->msc->list_and_timing_lock);
}

/*!
 * Import a caller IOSurface as a VkImage usable as a blit source. Same
 * VK_EXT_metal_objects + VK_EXT_external_memory_metal dance as
 * comp_vk_native_compositor.c::import_shared_iosurface.
 */
static bool
weave_import_input(struct vk_bundle *vk, struct multi_compositor *mc, IOSurfaceRef surface)
{
#if defined(VK_EXT_metal_objects) && defined(VK_EXT_external_memory_metal)
	if (!vk->has_EXT_metal_objects || !vk->has_EXT_external_memory_metal) {
		U_LOG_E("weave(#759): VK_EXT_metal_objects / VK_EXT_external_memory_metal unavailable");
		return false;
	}

	uint32_t width = (uint32_t)IOSurfaceGetWidth(surface);
	uint32_t height = (uint32_t)IOSurfaceGetHeight(surface);
	if (width == 0 || height == 0) {
		U_LOG_E("weave(#759): input IOSurface has zero dimensions");
		return false;
	}

	VkExportMetalObjectCreateInfoEXT export_metal_tex_info = {
	    .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT,
	    .exportObjectType = VK_EXPORT_METAL_OBJECT_TYPE_METAL_TEXTURE_BIT_EXT,
	};
	VkImportMetalIOSurfaceInfoEXT import_iosurface_info = {
	    .sType = VK_STRUCTURE_TYPE_IMPORT_METAL_IO_SURFACE_INFO_EXT,
	    .pNext = &export_metal_tex_info,
	    .ioSurface = surface,
	};
	VkImageCreateInfo image_ci = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .pNext = &import_iosurface_info,
	    .imageType = VK_IMAGE_TYPE_2D,
	    .format = WEAVE_VK_FORMAT,
	    .extent = {width, height, 1},
	    .mipLevels = 1,
	    .arrayLayers = 1,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .tiling = VK_IMAGE_TILING_OPTIMAL,
	    .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};

	VkImage image = VK_NULL_HANDLE;
	VkResult ret = vk->vkCreateImage(vk->device, &image_ci, NULL, &image);
	if (ret != VK_SUCCESS) {
		U_LOG_E("weave(#759): vkCreateImage(input IOSurface) failed: %d", ret);
		return false;
	}

	// Export the MTLTexture MoltenVK created over the IOSurface — its handle
	// is what the memory import below is keyed on.
	VkExportMetalTextureInfoEXT export_tex_info = {
	    .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT,
	    .image = image,
	    .plane = VK_IMAGE_ASPECT_COLOR_BIT,
	};
	VkExportMetalObjectsInfoEXT export_objects_info = {
	    .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
	    .pNext = &export_tex_info,
	};
	vk->vkExportMetalObjectsEXT(vk->device, &export_objects_info);
	if (export_tex_info.mtlTexture == NULL) {
		U_LOG_E("weave(#759): failed to export MTLTexture from input VkImage");
		vk->vkDestroyImage(vk->device, image, NULL);
		return false;
	}

	VkMemoryRequirements requirements = {0};
	vk->vkGetImageMemoryRequirements(vk->device, image, &requirements);

	VkMemoryMetalHandlePropertiesEXT metal_props = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_METAL_HANDLE_PROPERTIES_EXT,
	};
	ret = vk->vkGetMemoryMetalHandlePropertiesEXT(vk->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLTEXTURE_BIT_EXT,
	                                              export_tex_info.mtlTexture, &metal_props);
	if (ret != VK_SUCCESS) {
		U_LOG_E("weave(#759): vkGetMemoryMetalHandlePropertiesEXT failed: %d", ret);
		vk->vkDestroyImage(vk->device, image, NULL);
		return false;
	}
	requirements.memoryTypeBits = metal_props.memoryTypeBits;

	VkImportMemoryMetalHandleInfoEXT import_memory_info = {
	    .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_METAL_HANDLE_INFO_EXT,
	    .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLTEXTURE_BIT_EXT,
	    .handle = export_tex_info.mtlTexture,
	};
	VkMemoryDedicatedAllocateInfoKHR dedicated_info = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO_KHR,
	    .pNext = &import_memory_info,
	    .image = image,
	};

	uint32_t memory_type_index = UINT32_MAX;
	VkPhysicalDeviceMemoryProperties mem_props;
	vk->vkGetPhysicalDeviceMemoryProperties(vk->physical_device, &mem_props);
	for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
		if ((requirements.memoryTypeBits & (1u << i)) != 0) {
			memory_type_index = i;
			break;
		}
	}
	if (memory_type_index == UINT32_MAX) {
		U_LOG_E("weave(#759): no valid memory type for input IOSurface");
		vk->vkDestroyImage(vk->device, image, NULL);
		return false;
	}

	VkMemoryAllocateInfo alloc_info = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .pNext = &dedicated_info,
	    .allocationSize = requirements.size,
	    .memoryTypeIndex = memory_type_index,
	};
	VkDeviceMemory memory = VK_NULL_HANDLE;
	ret = vk->vkAllocateMemory(vk->device, &alloc_info, NULL, &memory);
	if (ret != VK_SUCCESS) {
		U_LOG_E("weave(#759): vkAllocateMemory(input IOSurface) failed: %d", ret);
		vk->vkDestroyImage(vk->device, image, NULL);
		return false;
	}
	ret = vk->vkBindImageMemory(vk->device, image, memory, 0);
	if (ret != VK_SUCCESS) {
		U_LOG_E("weave(#759): vkBindImageMemory(input IOSurface) failed: %d", ret);
		vk->vkFreeMemory(vk->device, memory, NULL);
		vk->vkDestroyImage(vk->device, image, NULL);
		return false;
	}

	mc->weave.in_image = image;
	mc->weave.in_memory = memory;
	mc->weave.in_w = width;
	mc->weave.in_h = height;
	mc->weave.in_first_use = true;
	return true;
#else
	(void)vk;
	(void)mc;
	(void)surface;
	return false;
#endif
}

static void
weave_release_input(struct vk_bundle *vk, struct multi_compositor *mc)
{
	if (mc->weave.in_image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, mc->weave.in_image, NULL);
		mc->weave.in_image = VK_NULL_HANDLE;
	}
	if (mc->weave.in_memory != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, mc->weave.in_memory, NULL);
		mc->weave.in_memory = VK_NULL_HANDLE;
	}
	if (mc->weave.in_iosurface != NULL) {
		CFRelease((IOSurfaceRef)mc->weave.in_iosurface);
		mc->weave.in_iosurface = NULL;
	}
	mc->weave.in_iosurface_id = 0;
	mc->weave.in_w = 0;
	mc->weave.in_h = 0;
}

/*!
 * Import a caller IOSurface as a sampler-source VkImage (+ view) for the v4
 * overlay atlas — the same VK_EXT_metal_objects dance as weave_import_input but
 * into the SEPARATE overlay cache (so it never clobbers the SBS input) and with
 * a view the premul-over blend samples. The atlas is premultiplied BGRA8.
 */
static bool
weave_import_overlay(struct vk_bundle *vk, struct multi_compositor *mc, IOSurfaceRef surface)
{
#if defined(VK_EXT_metal_objects) && defined(VK_EXT_external_memory_metal)
	if (!vk->has_EXT_metal_objects || !vk->has_EXT_external_memory_metal) {
		U_LOG_E("weave(#759) v4: VK_EXT_metal_objects / VK_EXT_external_memory_metal unavailable");
		return false;
	}

	uint32_t width = (uint32_t)IOSurfaceGetWidth(surface);
	uint32_t height = (uint32_t)IOSurfaceGetHeight(surface);
	if (width == 0 || height == 0) {
		U_LOG_E("weave(#759) v4: overlay IOSurface has zero dimensions");
		return false;
	}

	VkExportMetalObjectCreateInfoEXT export_metal_tex_info = {
	    .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT,
	    .exportObjectType = VK_EXPORT_METAL_OBJECT_TYPE_METAL_TEXTURE_BIT_EXT,
	};
	VkImportMetalIOSurfaceInfoEXT import_iosurface_info = {
	    .sType = VK_STRUCTURE_TYPE_IMPORT_METAL_IO_SURFACE_INFO_EXT,
	    .pNext = &export_metal_tex_info,
	    .ioSurface = surface,
	};
	VkImageCreateInfo image_ci = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .pNext = &import_iosurface_info,
	    .imageType = VK_IMAGE_TYPE_2D,
	    .format = WEAVE_VK_FORMAT,
	    .extent = {width, height, 1},
	    .mipLevels = 1,
	    .arrayLayers = 1,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .tiling = VK_IMAGE_TILING_OPTIMAL,
	    .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};

	VkImage image = VK_NULL_HANDLE;
	VkResult ret = vk->vkCreateImage(vk->device, &image_ci, NULL, &image);
	if (ret != VK_SUCCESS) {
		U_LOG_E("weave(#759) v4: vkCreateImage(overlay IOSurface) failed: %d", ret);
		return false;
	}

	VkExportMetalTextureInfoEXT export_tex_info = {
	    .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT,
	    .image = image,
	    .plane = VK_IMAGE_ASPECT_COLOR_BIT,
	};
	VkExportMetalObjectsInfoEXT export_objects_info = {
	    .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
	    .pNext = &export_tex_info,
	};
	vk->vkExportMetalObjectsEXT(vk->device, &export_objects_info);
	if (export_tex_info.mtlTexture == NULL) {
		U_LOG_E("weave(#759) v4: failed to export MTLTexture from overlay VkImage");
		vk->vkDestroyImage(vk->device, image, NULL);
		return false;
	}

	VkMemoryRequirements requirements = {0};
	vk->vkGetImageMemoryRequirements(vk->device, image, &requirements);

	VkMemoryMetalHandlePropertiesEXT metal_props = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_METAL_HANDLE_PROPERTIES_EXT,
	};
	ret = vk->vkGetMemoryMetalHandlePropertiesEXT(vk->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLTEXTURE_BIT_EXT,
	                                              export_tex_info.mtlTexture, &metal_props);
	if (ret != VK_SUCCESS) {
		U_LOG_E("weave(#759) v4: vkGetMemoryMetalHandlePropertiesEXT failed: %d", ret);
		vk->vkDestroyImage(vk->device, image, NULL);
		return false;
	}
	requirements.memoryTypeBits = metal_props.memoryTypeBits;

	VkImportMemoryMetalHandleInfoEXT import_memory_info = {
	    .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_METAL_HANDLE_INFO_EXT,
	    .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLTEXTURE_BIT_EXT,
	    .handle = export_tex_info.mtlTexture,
	};
	VkMemoryDedicatedAllocateInfoKHR dedicated_info = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO_KHR,
	    .pNext = &import_memory_info,
	    .image = image,
	};

	uint32_t memory_type_index = UINT32_MAX;
	VkPhysicalDeviceMemoryProperties mem_props;
	vk->vkGetPhysicalDeviceMemoryProperties(vk->physical_device, &mem_props);
	for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
		if ((requirements.memoryTypeBits & (1u << i)) != 0) {
			memory_type_index = i;
			break;
		}
	}
	if (memory_type_index == UINT32_MAX) {
		U_LOG_E("weave(#759) v4: no valid memory type for overlay IOSurface");
		vk->vkDestroyImage(vk->device, image, NULL);
		return false;
	}

	VkMemoryAllocateInfo alloc_info = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .pNext = &dedicated_info,
	    .allocationSize = requirements.size,
	    .memoryTypeIndex = memory_type_index,
	};
	VkDeviceMemory memory = VK_NULL_HANDLE;
	ret = vk->vkAllocateMemory(vk->device, &alloc_info, NULL, &memory);
	if (ret != VK_SUCCESS) {
		U_LOG_E("weave(#759) v4: vkAllocateMemory(overlay IOSurface) failed: %d", ret);
		vk->vkDestroyImage(vk->device, image, NULL);
		return false;
	}
	ret = vk->vkBindImageMemory(vk->device, image, memory, 0);
	if (ret != VK_SUCCESS) {
		U_LOG_E("weave(#759) v4: vkBindImageMemory(overlay IOSurface) failed: %d", ret);
		vk->vkFreeMemory(vk->device, memory, NULL);
		vk->vkDestroyImage(vk->device, image, NULL);
		return false;
	}

	VkImageViewCreateInfo view_ci = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
	    .image = image,
	    .viewType = VK_IMAGE_VIEW_TYPE_2D,
	    .format = WEAVE_VK_FORMAT,
	    .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1},
	};
	VkImageView view = VK_NULL_HANDLE;
	ret = vk->vkCreateImageView(vk->device, &view_ci, NULL, &view);
	if (ret != VK_SUCCESS) {
		U_LOG_E("weave(#759) v4: vkCreateImageView(overlay) failed: %d", ret);
		vk->vkFreeMemory(vk->device, memory, NULL);
		vk->vkDestroyImage(vk->device, image, NULL);
		return false;
	}

	mc->weave.overlay_image = image;
	mc->weave.overlay_memory = memory;
	mc->weave.overlay_view = view;
	mc->weave.overlay_w = width;
	mc->weave.overlay_h = height;
	mc->weave.overlay_first_use = true;
	return true;
#else
	(void)vk;
	(void)mc;
	(void)surface;
	return false;
#endif
}

static void
weave_release_overlay(struct vk_bundle *vk, struct multi_compositor *mc)
{
	if (mc->weave.overlay_view != VK_NULL_HANDLE) {
		vk->vkDestroyImageView(vk->device, mc->weave.overlay_view, NULL);
		mc->weave.overlay_view = VK_NULL_HANDLE;
	}
	if (mc->weave.overlay_image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, mc->weave.overlay_image, NULL);
		mc->weave.overlay_image = VK_NULL_HANDLE;
	}
	if (mc->weave.overlay_memory != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, mc->weave.overlay_memory, NULL);
		mc->weave.overlay_memory = VK_NULL_HANDLE;
	}
	if (mc->weave.overlay_iosurface != NULL) {
		CFRelease((IOSurfaceRef)mc->weave.overlay_iosurface);
		mc->weave.overlay_iosurface = NULL;
	}
	mc->weave.overlay_iosurface_id = 0;
	mc->weave.overlay_w = 0;
	mc->weave.overlay_h = 0;
}

//! Plain device-local image + view (the SBS scratch atlas).
static bool
weave_create_scratch(struct vk_bundle *vk, struct multi_compositor *mc, uint32_t w, uint32_t h)
{
	VkImageCreateInfo image_ci = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .imageType = VK_IMAGE_TYPE_2D,
	    .format = WEAVE_VK_FORMAT,
	    .extent = {w, h, 1},
	    .mipLevels = 1,
	    .arrayLayers = 1,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .tiling = VK_IMAGE_TILING_OPTIMAL,
	    .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	VkResult ret = vk->vkCreateImage(vk->device, &image_ci, NULL, &mc->weave.sbs_image);
	if (ret != VK_SUCCESS) {
		return false;
	}

	VkMemoryRequirements reqs;
	vk->vkGetImageMemoryRequirements(vk->device, mc->weave.sbs_image, &reqs);
	uint32_t mti = UINT32_MAX;
	if (!vk_get_memory_type(vk, reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &mti)) {
		vk->vkDestroyImage(vk->device, mc->weave.sbs_image, NULL);
		mc->weave.sbs_image = VK_NULL_HANDLE;
		return false;
	}
	VkMemoryAllocateInfo alloc = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .allocationSize = reqs.size,
	    .memoryTypeIndex = mti,
	};
	ret = vk->vkAllocateMemory(vk->device, &alloc, NULL, &mc->weave.sbs_memory);
	if (ret != VK_SUCCESS || vk->vkBindImageMemory(vk->device, mc->weave.sbs_image, mc->weave.sbs_memory, 0) !=
	                             VK_SUCCESS) {
		vk->vkDestroyImage(vk->device, mc->weave.sbs_image, NULL);
		mc->weave.sbs_image = VK_NULL_HANDLE;
		if (mc->weave.sbs_memory != VK_NULL_HANDLE) {
			vk->vkFreeMemory(vk->device, mc->weave.sbs_memory, NULL);
			mc->weave.sbs_memory = VK_NULL_HANDLE;
		}
		return false;
	}

	VkImageViewCreateInfo view_ci = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
	    .image = mc->weave.sbs_image,
	    .viewType = VK_IMAGE_VIEW_TYPE_2D,
	    .format = WEAVE_VK_FORMAT,
	    .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1},
	};
	ret = vk->vkCreateImageView(vk->device, &view_ci, NULL, &mc->weave.sbs_view);
	if (ret != VK_SUCCESS) {
		return false;
	}

	mc->weave.sbs_w = w;
	mc->weave.sbs_h = h;
	mc->weave.sbs_first_use = true;
	return true;
}

static void
weave_release_scratch(struct vk_bundle *vk, struct multi_compositor *mc)
{
	if (mc->weave.sbs_view != VK_NULL_HANDLE) {
		vk->vkDestroyImageView(vk->device, mc->weave.sbs_view, NULL);
		mc->weave.sbs_view = VK_NULL_HANDLE;
	}
	if (mc->weave.sbs_image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, mc->weave.sbs_image, NULL);
		mc->weave.sbs_image = VK_NULL_HANDLE;
	}
	if (mc->weave.sbs_memory != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, mc->weave.sbs_memory, NULL);
		mc->weave.sbs_memory = VK_NULL_HANDLE;
	}
	mc->weave.sbs_w = 0;
	mc->weave.sbs_h = 0;
}

/*!
 * IOSurface-backed output image + view + framebuffer, IOSurfaceRef exported for
 * the caller (vk_image_allocator.c export pattern).
 */
static bool
weave_create_output(struct vk_bundle *vk, struct multi_compositor *mc, uint32_t w, uint32_t h)
{
#if defined(VK_EXT_metal_objects)
	if (!vk->has_EXT_metal_objects) {
		U_LOG_E("weave(#759): VK_EXT_metal_objects unavailable for output export");
		return false;
	}

	VkExportMetalObjectCreateInfoEXT export_metal_info = {
	    .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT,
	    .exportObjectType = VK_EXPORT_METAL_OBJECT_TYPE_METAL_IOSURFACE_BIT_EXT,
	};
	VkImageCreateInfo image_ci = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .pNext = &export_metal_info,
	    .imageType = VK_IMAGE_TYPE_2D,
	    .format = WEAVE_VK_FORMAT,
	    .extent = {w, h, 1},
	    .mipLevels = 1,
	    .arrayLayers = 1,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .tiling = VK_IMAGE_TILING_OPTIMAL,
	    .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
	             VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	VkResult ret = vk->vkCreateImage(vk->device, &image_ci, NULL, &mc->weave.out_image);
	if (ret != VK_SUCCESS) {
		U_LOG_E("weave(#759): vkCreateImage(output) failed: %d", ret);
		return false;
	}

	VkMemoryRequirements reqs;
	vk->vkGetImageMemoryRequirements(vk->device, mc->weave.out_image, &reqs);
	uint32_t mti = UINT32_MAX;
	if (!vk_get_memory_type(vk, reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &mti)) {
		vk->vkDestroyImage(vk->device, mc->weave.out_image, NULL);
		mc->weave.out_image = VK_NULL_HANDLE;
		return false;
	}
	VkMemoryDedicatedAllocateInfoKHR dedicated = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO_KHR,
	    .image = mc->weave.out_image,
	};
	VkMemoryAllocateInfo alloc = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .pNext = &dedicated,
	    .allocationSize = reqs.size,
	    .memoryTypeIndex = mti,
	};
	ret = vk->vkAllocateMemory(vk->device, &alloc, NULL, &mc->weave.out_memory);
	if (ret != VK_SUCCESS || vk->vkBindImageMemory(vk->device, mc->weave.out_image, mc->weave.out_memory, 0) !=
	                             VK_SUCCESS) {
		U_LOG_E("weave(#759): output memory alloc/bind failed");
		vk->vkDestroyImage(vk->device, mc->weave.out_image, NULL);
		mc->weave.out_image = VK_NULL_HANDLE;
		if (mc->weave.out_memory != VK_NULL_HANDLE) {
			vk->vkFreeMemory(vk->device, mc->weave.out_memory, NULL);
			mc->weave.out_memory = VK_NULL_HANDLE;
		}
		return false;
	}

	// Export the backing IOSurface for the caller (retained; released on
	// resize/teardown — the IPC send only reads its IOSurfaceID).
	VkExportMetalIOSurfaceInfoEXT export_surface = {
	    .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_IO_SURFACE_INFO_EXT,
	    .image = mc->weave.out_image,
	};
	VkExportMetalObjectsInfoEXT export_info = {
	    .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
	    .pNext = &export_surface,
	};
	vk->vkExportMetalObjectsEXT(vk->device, &export_info);
	if (export_surface.ioSurface == NULL) {
		U_LOG_E("weave(#759): failed to export output IOSurface");
		return false;
	}
	CFRetain(export_surface.ioSurface);
	mc->weave.out_iosurface = (void *)export_surface.ioSurface;

	VkImageViewCreateInfo view_ci = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
	    .image = mc->weave.out_image,
	    .viewType = VK_IMAGE_VIEW_TYPE_2D,
	    .format = WEAVE_VK_FORMAT,
	    .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1},
	};
	ret = vk->vkCreateImageView(vk->device, &view_ci, NULL, &mc->weave.out_view);
	if (ret != VK_SUCCESS) {
		return false;
	}

	VkFramebufferCreateInfo fb_ci = {
	    .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
	    .renderPass = mc->weave.render_pass,
	    .attachmentCount = 1,
	    .pAttachments = &mc->weave.out_view,
	    .width = w,
	    .height = h,
	    .layers = 1,
	};
	ret = vk->vkCreateFramebuffer(vk->device, &fb_ci, NULL, &mc->weave.out_fb);
	if (ret != VK_SUCCESS) {
		return false;
	}

	mc->weave.out_w = w;
	mc->weave.out_h = h;
	return true;
#else
	(void)vk;
	(void)mc;
	(void)w;
	(void)h;
	return false;
#endif
}

static void
weave_release_output(struct vk_bundle *vk, struct multi_compositor *mc)
{
	if (mc->weave.out_fb != VK_NULL_HANDLE) {
		vk->vkDestroyFramebuffer(vk->device, mc->weave.out_fb, NULL);
		mc->weave.out_fb = VK_NULL_HANDLE;
	}
	if (mc->weave.out_view != VK_NULL_HANDLE) {
		vk->vkDestroyImageView(vk->device, mc->weave.out_view, NULL);
		mc->weave.out_view = VK_NULL_HANDLE;
	}
	if (mc->weave.out_image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, mc->weave.out_image, NULL);
		mc->weave.out_image = VK_NULL_HANDLE;
	}
	if (mc->weave.out_memory != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, mc->weave.out_memory, NULL);
		mc->weave.out_memory = VK_NULL_HANDLE;
	}
	if (mc->weave.out_iosurface != NULL) {
		CFRelease((IOSurfaceRef)mc->weave.out_iosurface);
		mc->weave.out_iosurface = NULL;
	}
	mc->weave.out_w = 0;
	mc->weave.out_h = 0;
}

/*!
 * One-time engine bring-up: command pool + buffer, fence, render pass
 * (compatible with the DP's own — same single BGRA8 color attachment), and the
 * DP instance from the plug-in's Vulkan factory. Mirrors shared_surface_init.
 */
static bool
weave_ensure_engine(struct vk_bundle *vk, struct multi_compositor *mc)
{
	if (mc->weave.engine_initialized) {
		return true;
	}

	VkCommandPoolCreateInfo pool_info = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
	    .queueFamilyIndex = vk->main_queue->family_index,
	    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
	};
	if (vk->vkCreateCommandPool(vk->device, &pool_info, NULL, &mc->weave.cmd_pool) != VK_SUCCESS) {
		return false;
	}
	VkCommandBufferAllocateInfo cb_info = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	    .commandPool = mc->weave.cmd_pool,
	    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
	    .commandBufferCount = 1,
	};
	if (vk->vkAllocateCommandBuffers(vk->device, &cb_info, &mc->weave.cmd) != VK_SUCCESS) {
		return false;
	}
	VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	if (vk->vkCreateFence(vk->device, &fence_info, NULL, &mc->weave.fence) != VK_SUCCESS) {
		return false;
	}

	// Render pass the output framebuffer is created against. Compatibility with
	// the DP's internal render pass only needs matching attachment count /
	// format / samples (load-store ops and layouts don't participate).
	VkAttachmentDescription color_attachment = {
	    .format = WEAVE_VK_FORMAT,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
	    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
	    .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
	    .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
	    .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	    .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	};
	VkAttachmentReference color_ref = {.attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
	VkSubpassDescription subpass = {
	    .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
	    .colorAttachmentCount = 1,
	    .pColorAttachments = &color_ref,
	};
	VkRenderPassCreateInfo rp_info = {
	    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
	    .attachmentCount = 1,
	    .pAttachments = &color_attachment,
	    .subpassCount = 1,
	    .pSubpasses = &subpass,
	};
	if (vk->vkCreateRenderPass(vk->device, &rp_info, NULL, &mc->weave.render_pass) != VK_SUCCESS) {
		return false;
	}

	// The DP that weaves — same Vulkan plug-in factory family the macOS
	// shared-surface path drives (sim_display anaglyph runs on MoltenVK).
	xrt_dp_factory_vk_fn_t factory = (xrt_dp_factory_vk_fn_t)mc->msc->base.info.dp_factory_vk;
	if (factory == NULL) {
		U_LOG_E("weave(#759): no Vulkan DP factory — cannot weave");
		return false;
	}
	xrt_result_t xret = factory(vk,                                       // vk_bundle
	                            (void *)(uintptr_t)mc->weave.cmd_pool,    // cmd_pool
	                            NULL,                                     // window_handle (present-owner's, not ours)
	                            (int32_t)WEAVE_VK_FORMAT,                 // target_format
	                            &mc->weave.dp);
	if (xret != XRT_SUCCESS || mc->weave.dp == NULL) {
		U_LOG_E("weave(#759): Vulkan DP factory failed: %d", xret);
		return false;
	}

	mc->weave.engine_initialized = true;
	U_LOG_W("weave(#759): macOS weave engine initialized (BGRA8, synchronous)");
	return true;
}

/*
 *
 * Public entry points (called from ipc_server_handler.c).
 *
 */

bool
comp_multi_weave_bind_window(struct xrt_compositor *xc, uint64_t window_id)
{
	struct multi_compositor *mc = multi_compositor(xc);
	if (mc == NULL || mc->msc == NULL) {
		return false;
	}
	weave_ensure_mutex(mc);
	os_mutex_lock(&mc->weave.mutex);
	// Stored for future interlace-phase use only: sim_display (anaglyph) has no
	// lattice, and macOS window geometry is derived from the input surface.
	mc->weave.window_id = window_id;
	os_mutex_unlock(&mc->weave.mutex);
	U_LOG_W("weave(#759): bound present-owner window id 0x%" PRIx64, window_id);
	return true;
}

bool
comp_multi_weave_submit(struct xrt_compositor *xc,
                        xrt_graphics_buffer_handle_t in_handle,
                        int32_t rect_x,
                        int32_t rect_y,
                        uint32_t rect_w,
                        uint32_t rect_h,
                        uint32_t rect_count,
                        const struct xrt_rect *rects,
                        xrt_graphics_buffer_handle_t overlay_handle,
                        bool weave_frame_first,
                        uint32_t *out_width,
                        uint32_t *out_height,
                        uint64_t *out_fence_value,
                        struct xrt_eye_positions *out_eyes)
{
	struct multi_compositor *mc = multi_compositor(xc);
	if (mc == NULL || mc->msc == NULL || in_handle == NULL) {
		return false;
	}
	struct vk_bundle *vk = weave_get_vk(mc);
	if (vk == NULL) {
		return false;
	}

	// The handler hands us ownership of the retained IOSurfaceRef the IPC
	// receive looked up; we either adopt it into the cache or release it.
	IOSurfaceRef surface = (IOSurfaceRef)in_handle;
	uint32_t surface_id = (uint32_t)IOSurfaceGetID(surface);

	// v4 overlay atlas (browser#18): the handler passes a second retained
	// IOSurfaceRef when the caller chained XrWeaveSubmitOverlaysDXR. We own it —
	// adopt into the overlay cache (keyed by IOSurfaceID) or release it below.
	IOSurfaceRef overlay = (IOSurfaceRef)overlay_handle; // may be NULL
	uint32_t overlay_id = overlay != NULL ? (uint32_t)IOSurfaceGetID(overlay) : 0;

	weave_ensure_mutex(mc);
	os_mutex_lock(&mc->weave.mutex);

	bool ok = false;
	do {
		if (!weave_ensure_engine(vk, mc)) {
			break;
		}

		// (Re)import the input on identity change (new IOSurface = new id).
		if (mc->weave.in_image == VK_NULL_HANDLE || mc->weave.in_iosurface_id != surface_id) {
			weave_release_input(vk, mc);
			if (!weave_import_input(vk, mc, surface)) {
				break;
			}
			mc->weave.in_iosurface = (void *)surface; // adopt the retained ref
			mc->weave.in_iosurface_id = surface_id;
			surface = NULL; // ownership transferred
		}

		// v4 overlay: (re)import on identity change. On a matching id we keep the
		// cached import and release the (redundant) per-call ref at the epilogue.
		if (overlay != NULL &&
		    (mc->weave.overlay_image == VK_NULL_HANDLE || mc->weave.overlay_iosurface_id != overlay_id)) {
			weave_release_overlay(vk, mc);
			if (weave_import_overlay(vk, mc, overlay)) {
				mc->weave.overlay_iosurface = (void *)overlay; // adopt the retained ref
				mc->weave.overlay_iosurface_id = overlay_id;
				overlay = NULL; // ownership transferred
				U_LOG_W("weave(#759) v4: overlay import cached (%ux%u)", mc->weave.overlay_w,
				        mc->weave.overlay_h);
			}
		}

		// Output dims: batch = the (window-client-sized) input; legacy =
		// rect offset+extent (the Windows GetClientRect-less fallback).
		uint32_t want_w = 0, want_h = 0;
		if (rect_count > 0) {
			want_w = mc->weave.in_w;
			want_h = mc->weave.in_h;
		} else {
			want_w = (uint32_t)rect_x + rect_w;
			want_h = (uint32_t)rect_y + rect_h;
		}
		if (want_w == 0 || want_h == 0) {
			break;
		}

		// (Re)allocate output + scratch on resize. The scratch is the
		// window-sized 2x1 SBS atlas (2*w x h) — ONE weave per submit.
		if (mc->weave.out_image == VK_NULL_HANDLE || mc->weave.out_w != want_w ||
		    mc->weave.out_h != want_h) {
			// Never yank resources out from under in-flight GPU work.
			vk->vkQueueWaitIdle(vk->main_queue->queue);
			weave_release_output(vk, mc);
			weave_release_scratch(vk, mc);
			if (!weave_create_output(vk, mc, want_w, want_h) ||
			    !weave_create_scratch(vk, mc, want_w * 2, want_h)) {
				break;
			}
		}

		// ---- Record ----
		VkCommandBuffer cmd = mc->weave.cmd;
		vk->vkResetCommandBuffer(cmd, 0);
		VkCommandBufferBeginInfo begin = {
		    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		};
		if (vk->vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS) {
			break;
		}

		VkImageSubresourceRange range = {
		    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1};

		// Input: keep GENERAL across frames (UNDEFINED would discard the
		// caller's pixels); the barrier makes external writes visible.
		VkImageMemoryBarrier in_barrier = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
		    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		    .oldLayout = mc->weave.in_first_use ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
		    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
		    .image = mc->weave.in_image,
		    .subresourceRange = range,
		};
		mc->weave.in_first_use = false;
		vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
		                         NULL, 0, NULL, 1, &in_barrier);

		// Scratch -> TRANSFER_DST (persists across frames: stale regions from
		// closed elements re-weave harmlessly; the caller composites back only
		// its current rects — same contract as the Windows output).
		VkImageMemoryBarrier sbs_to_dst = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
		    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		    .oldLayout = mc->weave.sbs_first_use ? VK_IMAGE_LAYOUT_UNDEFINED
		                                         : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		    .image = mc->weave.sbs_image,
		    .subresourceRange = range,
		};
		mc->weave.sbs_first_use = false;
		vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
		                         0, NULL, 0, NULL, 1, &sbs_to_dst);

		// v5 firstChunk (browser#22): clear the SBS scratch to premultiplied
		// transparent (0,0,0,0) on the first submit of a frame, so regions BETWEEN
		// the woven tiles come out alpha 0 instead of stale — the present-owner can
		// then draw the woven output back WHOLE-WINDOW (opaque tiles replace the
		// page, transparent gaps show it through). The DP is alpha-native (passes
		// the atlas alpha through the weave), so cleared gaps stay transparent while
		// blitted tiles keep the page's opaque alpha. Opt-in: legacy present-owners
		// draw back only their own tiles and skip it (accumulate-across-submits).
		if (weave_frame_first) {
			VkClearColorValue sbs_transparent = {.float32 = {0.0f, 0.0f, 0.0f, 0.0f}};
			vk->vkCmdClearColorImage(cmd, mc->weave.sbs_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			                         &sbs_transparent, 1, &range);
			// Order the whole-image clear before the per-rect blits (both TRANSFER
			// writes to overlapping regions — no implicit ordering within a stage).
			VkImageMemoryBarrier clear_to_blit = {
			    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			    .image = mc->weave.sbs_image,
			    .subresourceRange = range,
			};
			vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
			                         0, NULL, 0, NULL, 1, &clear_to_blit);
		}

		// Blit each rect's squeezed-SBS halves into the two atlas tiles:
		// left half -> left tile at the rect's window position (stretched to
		// full rect width), right half -> right tile offset by out_w.
		struct xrt_rect legacy_rect = {
		    .offset = {.w = 0, .h = 0},
		    .extent = {.w = (int)want_w, .h = (int)want_h},
		};
		const struct xrt_rect *blit_rects = rect_count > 0 ? rects : &legacy_rect;
		uint32_t blit_count = rect_count > 0 ? rect_count : 1;

		for (uint32_t i = 0; i < blit_count; i++) {
			// xrt_offset names its fields w/h; they hold x/y here.
			int32_t rx = blit_rects[i].offset.w;
			int32_t ry = blit_rects[i].offset.h;
			int32_t rw = blit_rects[i].extent.w;
			int32_t rh = blit_rects[i].extent.h;
			if (rw <= 0 || rh <= 0) {
				continue;
			}
			// Clamp to the input.
			if (rx < 0 || ry < 0 || (uint32_t)(rx + rw) > mc->weave.in_w ||
			    (uint32_t)(ry + rh) > mc->weave.in_h) {
				continue;
			}
			int32_t half = rw / 2;
			if (half <= 0) {
				continue;
			}

			VkImageBlit blits[2] = {
			    // Left eye: input rect's left half -> left tile, unsqueezed.
			    {
			        .srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
			        .srcOffsets = {{rx, ry, 0}, {rx + half, ry + rh, 1}},
			        .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
			        .dstOffsets = {{rx, ry, 0}, {rx + rw, ry + rh, 1}},
			    },
			    // Right eye: input rect's right half -> right tile (+out_w).
			    {
			        .srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
			        .srcOffsets = {{rx + half, ry, 0}, {rx + rw, ry + rh, 1}},
			        .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
			        .dstOffsets = {{(int32_t)mc->weave.out_w + rx, ry, 0},
			                       {(int32_t)mc->weave.out_w + rx + rw, ry + rh, 1}},
			    },
			};
			vk->vkCmdBlitImage(cmd, mc->weave.in_image, VK_IMAGE_LAYOUT_GENERAL, mc->weave.sbs_image,
			                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, blits, VK_FILTER_LINEAR);
		}

		// Scratch -> SHADER_READ for the DP sample.
		VkImageMemoryBarrier sbs_to_read = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
		    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		    .image = mc->weave.sbs_image,
		    .subresourceRange = range,
		};
		vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
		                         0, NULL, 0, NULL, 1, &sbs_to_read);

		// Output -> COLOR_ATTACHMENT (fully re-rendered every submit, so the
		// discard from UNDEFINED is fine).
		VkImageMemoryBarrier out_to_attach = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = 0,
		    .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		    .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		    .image = mc->weave.out_image,
		    .subresourceRange = range,
		};
		vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1,
		                         &out_to_attach);

		// ONE process_atlas per submit — 2x1 SBS, per-eye dims = the window.
		xrt_display_processor_set_target_color_view(mc->weave.dp, mc->weave.out_view);
		xrt_display_processor_process_atlas(mc->weave.dp, cmd,                             //
		                                    (VkImage_XDP)mc->weave.sbs_image, mc->weave.sbs_view, //
		                                    mc->weave.out_w, mc->weave.out_h,              //
		                                    2, 1,                                          //
		                                    (VkFormat_XDP)WEAVE_VK_FORMAT,                 //
		                                    mc->weave.out_fb,                              //
		                                    (VkImage_XDP)mc->weave.out_image,              //
		                                    mc->weave.out_w, mc->weave.out_h,              //
		                                    (VkFormat_XDP)WEAVE_VK_FORMAT,                 //
		                                    0, 0, 0, 0);

		// v4 overlay atlas (browser#18): composite the caller's window-sized
		// premultiplied-alpha 2D atlas OVER the woven output with a premul "over"
		// blend (out = overlay + (1-overlay.a)*out), so crisp 2D lands on top of
		// the interlaced 3D at screen depth. The overlay is NOT woven — it is drawn
		// after process_atlas onto the same output attachment. Reuses aux_vk's
		// vk_local2d_composite flatten_premul pipeline (One / OneMinusSrcAlpha, all
		// RGBA). process_atlas leaves out_image in COLOR_ATTACHMENT_OPTIMAL.
		if (mc->weave.overlay_image != VK_NULL_HANDLE) {
			bool blend_ready = mc->weave.overlay_blend_initialized;
			if (!blend_ready) {
				blend_ready = vk_local2d_composite_init(&mc->weave.overlay_blend, vk,
				                                        WEAVE_VK_FORMAT, WEAVE_VK_FORMAT);
				mc->weave.overlay_blend_initialized = blend_ready;
				if (blend_ready) {
					U_LOG_W("weave(#759) v4: premul-over blend pipeline ready");
				} else {
					U_LOG_E("weave(#759) v4: premul-over blend init failed");
				}
			}
			if (blend_ready) {
				// Overlay -> SHADER_READ (make the caller's external write visible).
				VkImageMemoryBarrier ov_to_read = {
				    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				    .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
				    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
				    .oldLayout = mc->weave.overlay_first_use
				                     ? VK_IMAGE_LAYOUT_UNDEFINED
				                     : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				    .image = mc->weave.overlay_image,
				    .subresourceRange = range,
				};
				mc->weave.overlay_first_use = false;
				vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
				                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1,
				                         &ov_to_read);

				// Make the weave's color writes available to the blend pass's
				// LOAD_OP_LOAD + "over" (out stays COLOR_ATTACHMENT_OPTIMAL).
				VkImageMemoryBarrier out_weave_to_blend = {
				    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				    .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
				    .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
				                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
				    .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				    .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				    .image = mc->weave.out_image,
				    .subresourceRange = range,
				};
				vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
				                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0,
				                         NULL, 1, &out_weave_to_blend);

				// One whole-window premul "over": the atlas is transparent
				// (alpha 0) everywhere except the 2D regions, so a single
				// full-window composite is correct. out_fb is render-pass
				// compatible with the flatten pass (same BGRA8 single attachment).
				vk_local2d_composite_begin_frame(&mc->weave.overlay_blend, vk);
				vk_local2d_composite_flatten_draw(&mc->weave.overlay_blend, vk, cmd, mc->weave.out_fb,
				                                  mc->weave.out_w, mc->weave.out_h,
				                                  mc->weave.overlay_view,       //
				                                  0, 0, mc->weave.out_w, mc->weave.out_h, // dst = full window
				                                  0.0f, 0.0f, 1.0f, 1.0f,       // src = whole atlas, no flip
				                                  /*unpremultiplied*/ false);
			}
		}

		// Output -> GENERAL for the caller's cross-API (Metal) read.
		VkImageMemoryBarrier out_to_general = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		    .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
		    .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
		    .image = mc->weave.out_image,
		    .subresourceRange = range,
		};
		vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &out_to_general);

		if (vk->vkEndCommandBuffer(cmd) != VK_SUCCESS) {
			break;
		}

		// ---- Submit + synchronous completion (the macOS sync contract). ----
		VkSubmitInfo submit = {
		    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		    .commandBufferCount = 1,
		    .pCommandBuffers = &cmd,
		};
		vk_queue_lock(vk->main_queue);
		VkResult ret = vk->vkQueueSubmit(vk->main_queue->queue, 1, &submit, mc->weave.fence);
		vk_queue_unlock(vk->main_queue);
		if (ret != VK_SUCCESS) {
			U_LOG_E("weave(#759): vkQueueSubmit failed: %d", ret);
			break;
		}
		vk->vkWaitForFences(vk->device, 1, &mc->weave.fence, VK_TRUE, UINT64_MAX);
		vk->vkResetFences(vk->device, 1, &mc->weave.fence);

		mc->weave.fence_value++;

		*out_width = mc->weave.out_w;
		*out_height = mc->weave.out_h;
		*out_fence_value = mc->weave.fence_value;

		// Eyes flow OUT (runtime -> caller) for the caller's next off-axis
		// frame; the weave itself reads the tracker DP-internally.
		U_ZERO(out_eyes);
		if (!xrt_display_processor_get_predicted_eye_positions(mc->weave.dp, out_eyes)) {
			U_ZERO(out_eyes);
		}

		ok = true;
	} while (false);

	os_mutex_unlock(&mc->weave.mutex);

	// Release the per-call refs that weren't adopted into a cache.
	if (surface != NULL) {
		CFRelease(surface);
	}
	if (overlay != NULL) {
		CFRelease(overlay);
	}
	return ok;
}

bool
comp_multi_weave_export_output(struct xrt_compositor *xc,
                               xrt_graphics_buffer_handle_t *out_handle,
                               uint32_t *out_width,
                               uint32_t *out_height)
{
	struct multi_compositor *mc = multi_compositor(xc);
	if (mc == NULL || !mc->weave.mutex_initialized) {
		return false;
	}
	os_mutex_lock(&mc->weave.mutex);
	bool ok = false;
	if (mc->weave.out_iosurface != NULL && mc->weave.out_w != 0) {
		// The IPC send path only reads the IOSurfaceID out of the ref; the
		// cache keeps ownership (released on resize/teardown).
		CFRetain((IOSurfaceRef)mc->weave.out_iosurface);
		*out_handle = (xrt_graphics_buffer_handle_t)mc->weave.out_iosurface;
		*out_width = mc->weave.out_w;
		*out_height = mc->weave.out_h;
		ok = true;
	}
	os_mutex_unlock(&mc->weave.mutex);
	return ok;
}

bool
comp_multi_weave_export_fence(struct xrt_compositor *xc, xrt_graphics_sync_handle_t *out_handle)
{
	// No cross-process GPU fence on macOS — completion is synchronous
	// (xrWeaveSubmitDXR returns after the weave finished on the GPU).
	(void)xc;
	(void)out_handle;
	return false;
}

bool
comp_multi_weave_snap_window_rect(struct xrt_compositor *xc,
                                  int32_t origin_x,
                                  int32_t origin_y,
                                  int32_t target_x,
                                  int32_t target_y,
                                  int32_t *out_snapped_x,
                                  int32_t *out_snapped_y)
{
	// sim_display has no snap_window_rect (anaglyph has no interlace lattice)
	// and the generic VK DP vtable carries no snap slot yet — identity snap.
	(void)xc;
	(void)origin_x;
	(void)origin_y;
	(void)target_x;
	(void)target_y;
	(void)out_snapped_x;
	(void)out_snapped_y;
	return false;
}

void
comp_multi_weave_fini(struct multi_compositor *mc)
{
	if (mc == NULL || !mc->weave.mutex_initialized) {
		return;
	}
	struct vk_bundle *vk = weave_get_vk(mc);
	os_mutex_lock(&mc->weave.mutex);
	if (vk != NULL) {
		if (mc->weave.engine_initialized) {
			// Nothing may be in flight (submits are synchronous), but a
			// belt-and-braces idle keeps teardown safe if that changes.
			vk->vkQueueWaitIdle(vk->main_queue->queue);
		}
		weave_release_input(vk, mc);
		weave_release_overlay(vk, mc);
		weave_release_scratch(vk, mc);
		weave_release_output(vk, mc);
		if (mc->weave.overlay_blend_initialized) {
			vk_local2d_composite_fini(&mc->weave.overlay_blend, vk);
			mc->weave.overlay_blend_initialized = false;
		}
		if (mc->weave.dp != NULL) {
			xrt_display_processor_destroy(&mc->weave.dp);
		}
		if (mc->weave.render_pass != VK_NULL_HANDLE) {
			vk->vkDestroyRenderPass(vk->device, mc->weave.render_pass, NULL);
			mc->weave.render_pass = VK_NULL_HANDLE;
		}
		if (mc->weave.fence != VK_NULL_HANDLE) {
			vk->vkDestroyFence(vk->device, mc->weave.fence, NULL);
			mc->weave.fence = VK_NULL_HANDLE;
		}
		if (mc->weave.cmd_pool != VK_NULL_HANDLE) {
			vk->vkDestroyCommandPool(vk->device, mc->weave.cmd_pool, NULL);
			mc->weave.cmd_pool = VK_NULL_HANDLE;
		}
	}
	mc->weave.engine_initialized = false;
	os_mutex_unlock(&mc->weave.mutex);
	os_mutex_destroy(&mc->weave.mutex);
	mc->weave.mutex_initialized = false;
}

#endif // XRT_OS_MACOS
