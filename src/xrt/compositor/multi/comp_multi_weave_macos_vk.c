// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_weave on macOS (#759) — the Vulkan (MoltenVK) backend.
 * @author David Fattal
 * @ingroup comp_multi
 *
 * The original macOS weave engine, moved behind the backend vtable in
 * comp_multi_weave_macos_backend.h. The platform front end
 * (comp_multi_weave_macos.c) owns the entry points, locking, the IOSurface
 * refs, layout validation and output sizing; this file owns every Vulkan
 * object. Behaviour is unchanged from the single-file engine:
 *
 *  - Input texture   = the caller's IOSurface imported into a VkImage via
 *    VK_EXT_metal_objects + VK_EXT_external_memory_metal (same pattern as
 *    comp_vk_native_compositor.c import_shared_iosurface).
 *  - Output texture  = a service-allocated IOSurface-backed VkImage (the
 *    VkExportMetalObjectCreateInfoEXT allocation pattern from
 *    vk_image_allocator.c), exported back to the caller as an IOSurfaceRef.
 *  - The weave itself is ONE xrt_display_processor process_atlas per submit
 *    over a window-sized 2x1 SBS scratch atlas that all rects are blitted
 *    into (v3), or over the caller's packed N-view atlas (v6, #774). The DP
 *    comes from the plug-in's Vulkan factory (dp_factory_vk) — sim_display's
 *    anaglyph SPIR-V pipeline runs on MoltenVK.
 *  - Completion is synchronous: vkWaitForFences before the IPC reply.
 */

#include "xrt/xrt_compositor.h"
#include "xrt/xrt_display_processor.h"
#include "xrt/xrt_display_metrics.h"
#include "xrt/xrt_handles.h"

#include "util/u_misc.h"
#include "util/u_logging.h"

#include "vk/vk_helpers.h"

#include "comp_multi_private.h"
#include "comp_multi_weave_macos_backend.h"

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

	// Full-image view: the SBS path only blits the input (TRANSFER_SRC) so it
	// never needed one, but the v6 zero-copy path (#774) samples the input
	// atlas directly through the DP, which takes an atlas VkImageView.
	VkImageView view = VK_NULL_HANDLE;
	{
		VkImageViewCreateInfo view_ci = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		    .image = image,
		    .viewType = VK_IMAGE_VIEW_TYPE_2D,
		    .format = WEAVE_VK_FORMAT,
		    .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1},
		};
		ret = vk->vkCreateImageView(vk->device, &view_ci, NULL, &view);
		if (ret != VK_SUCCESS) {
			U_LOG_E("weave(#759): vkCreateImageView(input) failed: %d", ret);
			vk->vkFreeMemory(vk->device, memory, NULL);
			vk->vkDestroyImage(vk->device, image, NULL);
			return false;
		}
	}

	mc->weave.in_image = image;
	mc->weave.in_memory = memory;
	mc->weave.in_view = view;
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
	if (mc->weave.in_view != VK_NULL_HANDLE) {
		vk->vkDestroyImageView(vk->device, mc->weave.in_view, NULL);
		mc->weave.in_view = VK_NULL_HANDLE;
	}
	if (mc->weave.in_image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, mc->weave.in_image, NULL);
		mc->weave.in_image = VK_NULL_HANDLE;
	}
	if (mc->weave.in_memory != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, mc->weave.in_memory, NULL);
		mc->weave.in_memory = VK_NULL_HANDLE;
	}
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

/*!
 * Per-eye un-squeeze staging (struct comp_multi_weave_eye_stage): free one
 * slot's image. The caller guarantees no in-flight GPU work still reads it.
 */
static void
weave_eye_stage_release(struct vk_bundle *vk, struct comp_multi_weave_eye_stage *s)
{
	if (s->image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, s->image, NULL);
		s->image = VK_NULL_HANDLE;
	}
	if (s->memory != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, s->memory, NULL);
		s->memory = VK_NULL_HANDLE;
	}
	s->w = 0;
	s->h = 0;
	s->format = VK_FORMAT_UNDEFINED;
}

static void
weave_eye_stage_release_all(struct vk_bundle *vk, struct multi_compositor *mc)
{
	for (uint32_t i = 0; i < COMP_MULTI_WEAVE_EYE_STAGE_SLOTS; i++) {
		weave_eye_stage_release(vk, &mc->weave.eye_stage[i][0]);
		weave_eye_stage_release(vk, &mc->weave.eye_stage[i][1]);
	}
}

/*!
 * The staging image of rect @p slot / @p eye, EXACTLY @p w x @p h in @p format,
 * (re)allocated only when one of those changes (rects are stable frame to frame,
 * so steady state allocates nothing). VK_NULL_HANDLE on failure: the caller then
 * stretches straight out of the input (the pre-fix, edge-bleeding path) rather
 * than drop the rect. Called while recording, before this frame's first use.
 */
static VkImage
weave_eye_stage_get(struct vk_bundle *vk,
                    struct multi_compositor *mc,
                    uint32_t slot,
                    uint32_t eye,
                    uint32_t w,
                    uint32_t h,
                    VkFormat format)
{
	if (slot >= COMP_MULTI_WEAVE_EYE_STAGE_SLOTS || eye > 1 || w == 0 || h == 0) {
		return VK_NULL_HANDLE;
	}
	struct comp_multi_weave_eye_stage *s = &mc->weave.eye_stage[slot][eye];
	if (s->image != VK_NULL_HANDLE && s->w == w && s->h == h && s->format == format) {
		return s->image;
	}
	if (s->image != VK_NULL_HANDLE) {
		// A resized rect (rare): an earlier submit whose fence wait timed out may
		// still read the old image — never free it under the GPU.
		vk->vkQueueWaitIdle(vk->main_queue->queue);
		weave_eye_stage_release(vk, s);
	}

	VkExtent2D extent = {.width = w, .height = h};
	VkResult ret = vk_create_image_simple(vk, extent, format,
	                                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
	                                      &s->memory, &s->image);
	if (ret != VK_SUCCESS) {
		s->image = VK_NULL_HANDLE;
		s->memory = VK_NULL_HANDLE;
		static bool logged = false;
		if (!logged) {
			logged = true;
			U_LOG_E(
			    "weave(#759): eye staging vk_create_image_simple(%ux%u) failed: %s — "
			    "stretching from the input (edge bleed)",
			    w, h, vk_result_string(ret));
		}
		return VK_NULL_HANDLE;
	}
	s->w = w;
	s->h = h;
	s->format = format;
	return s->image;
}

static void
weave_release_scratch(struct vk_bundle *vk, struct multi_compositor *mc)
{
	weave_eye_stage_release_all(vk, mc);
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

//! v6 crop staging (#774): a device-local image + view holding the top-left
//! packed region (tile_columns*cvw x tile_rows*cvh) when the input atlas is
//! larger than the active mode's tiles. Sampled by the DP (SAMPLED) after a
//! single box copy from the input (TRANSFER_DST). No zero-copy = no crop image.
static bool
weave_create_crop(struct vk_bundle *vk, struct multi_compositor *mc, uint32_t w, uint32_t h)
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
	VkResult ret = vk->vkCreateImage(vk->device, &image_ci, NULL, &mc->weave.crop_image);
	if (ret != VK_SUCCESS) {
		return false;
	}

	VkMemoryRequirements reqs;
	vk->vkGetImageMemoryRequirements(vk->device, mc->weave.crop_image, &reqs);
	uint32_t mti = UINT32_MAX;
	if (!vk_get_memory_type(vk, reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &mti)) {
		vk->vkDestroyImage(vk->device, mc->weave.crop_image, NULL);
		mc->weave.crop_image = VK_NULL_HANDLE;
		return false;
	}
	VkMemoryAllocateInfo alloc = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .allocationSize = reqs.size,
	    .memoryTypeIndex = mti,
	};
	ret = vk->vkAllocateMemory(vk->device, &alloc, NULL, &mc->weave.crop_memory);
	if (ret != VK_SUCCESS ||
	    vk->vkBindImageMemory(vk->device, mc->weave.crop_image, mc->weave.crop_memory, 0) != VK_SUCCESS) {
		vk->vkDestroyImage(vk->device, mc->weave.crop_image, NULL);
		mc->weave.crop_image = VK_NULL_HANDLE;
		if (mc->weave.crop_memory != VK_NULL_HANDLE) {
			vk->vkFreeMemory(vk->device, mc->weave.crop_memory, NULL);
			mc->weave.crop_memory = VK_NULL_HANDLE;
		}
		return false;
	}

	VkImageViewCreateInfo view_ci = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
	    .image = mc->weave.crop_image,
	    .viewType = VK_IMAGE_VIEW_TYPE_2D,
	    .format = WEAVE_VK_FORMAT,
	    .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1},
	};
	ret = vk->vkCreateImageView(vk->device, &view_ci, NULL, &mc->weave.crop_view);
	if (ret != VK_SUCCESS) {
		return false;
	}

	mc->weave.crop_w = w;
	mc->weave.crop_h = h;
	mc->weave.crop_first_use = true;
	return true;
}

static void
weave_release_crop(struct vk_bundle *vk, struct multi_compositor *mc)
{
	if (mc->weave.crop_view != VK_NULL_HANDLE) {
		vk->vkDestroyImageView(vk->device, mc->weave.crop_view, NULL);
		mc->weave.crop_view = VK_NULL_HANDLE;
	}
	if (mc->weave.crop_image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, mc->weave.crop_image, NULL);
		mc->weave.crop_image = VK_NULL_HANDLE;
	}
	if (mc->weave.crop_memory != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, mc->weave.crop_memory, NULL);
		mc->weave.crop_memory = VK_NULL_HANDLE;
	}
	mc->weave.crop_w = 0;
	mc->weave.crop_h = 0;
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
 * Backend vtable.
 *
 */

static bool
vkb_ensure_engine(struct multi_compositor *mc)
{
	struct vk_bundle *vk = weave_get_vk(mc);
	if (vk == NULL) {
		return false;
	}
	return weave_ensure_engine(vk, mc);
}

static bool
vkb_import_input(struct multi_compositor *mc, IOSurfaceRef surface)
{
	struct vk_bundle *vk = weave_get_vk(mc);
	return vk != NULL && weave_import_input(vk, mc, surface);
}

static void
vkb_release_input(struct multi_compositor *mc)
{
	struct vk_bundle *vk = weave_get_vk(mc);
	if (vk != NULL) {
		weave_release_input(vk, mc);
	}
}

static bool
vkb_import_overlay(struct multi_compositor *mc, IOSurfaceRef surface)
{
	struct vk_bundle *vk = weave_get_vk(mc);
	return vk != NULL && weave_import_overlay(vk, mc, surface);
}

static void
vkb_release_overlay(struct multi_compositor *mc)
{
	struct vk_bundle *vk = weave_get_vk(mc);
	if (vk != NULL) {
		weave_release_overlay(vk, mc);
	}
}


static bool
vkb_output_tolerates_resample(struct multi_compositor *mc)
{
	// Unchanged behaviour: the Vulkan path always sizes the v6 output to one
	// content view (sim_display anaglyph has no lattice to protect).
	(void)mc;
	return true;
}

static bool
vkb_ensure_output(struct multi_compositor *mc, uint32_t want_w, uint32_t want_h, bool nview)
{
	struct vk_bundle *vk = weave_get_vk(mc);
	if (vk == NULL) {
		return false;
	}
	// (Re)allocate output (+ SBS scratch on the non-v6 paths) on resize. The
	// scratch is the window-sized 2x1 SBS atlas (2*w x h) — ONE weave per
	// submit. v6 samples the input (or the crop image) directly, so it needs
	// no scratch.
	if (mc->weave.out_image == VK_NULL_HANDLE || mc->weave.out_w != want_w || mc->weave.out_h != want_h) {
		// Never yank resources out from under in-flight GPU work.
		vk->vkQueueWaitIdle(vk->main_queue->queue);
		weave_release_output(vk, mc);
		weave_release_scratch(vk, mc);
		if (!weave_create_output(vk, mc, want_w, want_h)) {
			return false;
		}
		if (!nview && !weave_create_scratch(vk, mc, want_w * 2, want_h)) {
			return false;
		}
	}
	return true;
}

static bool
vkb_record_and_wait(struct multi_compositor *mc, const struct comp_multi_weave_macos_params *p)
{
	struct vk_bundle *vk = weave_get_vk(mc);
	if (vk == NULL) {
		return false;
	}
	// Same names as the single-file engine so the recording below is unchanged.
	const uint32_t rect_count = p->rect_count;
	const struct xrt_rect *rects = p->rects;
	const uint32_t want_w = p->want_w;
	const uint32_t want_h = p->want_h;
	const bool weave_frame_first = p->first_chunk;
	const bool nview = p->nview;
	const bool v6_zero_copy = p->v6_zero_copy;
	const uint32_t cvw = p->cvw, cvh = p->cvh, packed_w = p->packed_w, packed_h = p->packed_h;

	bool ok = false;
	do {
		// v6 crop staging: (re)create when the packed region is smaller than the
		// input (the common ADR-010 case). Zero-copy (packed == input) samples
		// the input directly and needs no crop image.
		if (nview && !v6_zero_copy &&
		    (mc->weave.crop_image == VK_NULL_HANDLE || mc->weave.crop_w != packed_w ||
		     mc->weave.crop_h != packed_h)) {
			vk->vkQueueWaitIdle(vk->main_queue->queue);
			weave_release_crop(vk, mc);
			if (!weave_create_crop(vk, mc, packed_w, packed_h)) {
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

		// Atlas source for the DP sample + the atlas grid it describes. Legacy /
		// batch weaves the window-sized 2x1 SBS scratch; v6 (#774) weaves the
		// caller's already-packed N-view atlas — the input directly (zero-copy)
		// or a cropped copy of its top-left packed region.
		VkImage dp_src_image = mc->weave.sbs_image;
		VkImageView dp_src_view = mc->weave.sbs_view;
		uint32_t atlas_view_w = mc->weave.out_w;
		uint32_t atlas_view_h = mc->weave.out_h;
		uint32_t grid_cols = 2, grid_rows = 1;

		if (nview) {
			// v6: no SBS scratch, no per-rect unpack blits, no firstChunk clear.
			// Transparency between elements is carried by the caller's own atlas
			// alpha, which reaches the DP untouched.
			if (v6_zero_copy) {
				// The packed atlas fills the input exactly — sample it directly.
				// Kept GENERAL across frames otherwise (UNDEFINED would discard
				// the caller's pixels); restored to GENERAL after the weave.
				VkImageMemoryBarrier in_to_read = {
				    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				    .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
				    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
				    .oldLayout = mc->weave.in_first_use ? VK_IMAGE_LAYOUT_UNDEFINED
				                                        : VK_IMAGE_LAYOUT_GENERAL,
				    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				    .image = mc->weave.in_image,
				    .subresourceRange = range,
				};
				mc->weave.in_first_use = false;
				vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
				                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1,
				                         &in_to_read);
				dp_src_image = mc->weave.in_image;
				dp_src_view = mc->weave.in_view;
			} else {
				// Crop the top-left packed region (tiles are contiguous, so it
				// is a single rectangle — ONE box copy, not a per-tile gather).
				VkImageMemoryBarrier in_to_src = {
				    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				    .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
				    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
				    .oldLayout = mc->weave.in_first_use ? VK_IMAGE_LAYOUT_UNDEFINED
				                                        : VK_IMAGE_LAYOUT_GENERAL,
				    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				    .image = mc->weave.in_image,
				    .subresourceRange = range,
				};
				mc->weave.in_first_use = false;
				vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
				                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
				                         &in_to_src);

				VkImageMemoryBarrier crop_to_dst = {
				    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				    .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
				    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
				    .oldLayout = mc->weave.crop_first_use ? VK_IMAGE_LAYOUT_UNDEFINED
				                                          : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				    .image = mc->weave.crop_image,
				    .subresourceRange = range,
				};
				mc->weave.crop_first_use = false;
				vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
				                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
				                         &crop_to_dst);

				VkImageCopy copy = {
				    .srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
				    .srcOffset = {0, 0, 0},
				    .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
				    .dstOffset = {0, 0, 0},
				    .extent = {packed_w, packed_h, 1},
				};
				vk->vkCmdCopyImage(cmd, mc->weave.in_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				                   mc->weave.crop_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

				VkImageMemoryBarrier crop_to_read = {
				    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
				    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
				    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				    .image = mc->weave.crop_image,
				    .subresourceRange = range,
				};
				vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
				                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1,
				                         &crop_to_read);
				dp_src_image = mc->weave.crop_image;
				dp_src_view = mc->weave.crop_view;
			}
			atlas_view_w = cvw;
			atlas_view_h = cvh;
			grid_cols = p->tile_columns;
			grid_rows = p->tile_rows;
		} else {

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

		// Un-squeeze each rect's SBS halves into the two atlas tiles: left half ->
		// left tile at the rect's window position (stretched to full rect width),
		// right half -> right tile offset by out_w. A LINEAR blit clamps at the
		// edge of the whole SOURCE IMAGE, not at srcOffsets, so stretching straight
		// out of the input would read 25 % of the texel beyond each half-rect (the
		// caller's never-cleared ring on the outer edges = a dark 1-px border; the
		// other eye at the midline). Each half is therefore first copied 1:1 into
		// an image of exactly its size and that WHOLE image is stretched: the edge
		// clamp is then the half-rect's own outermost texel centres. Odd widths
		// keep the split they always had: left = rw / 2, right = the rest.
		struct xrt_rect legacy_rect = {
		    .offset = {.w = 0, .h = 0},
		    .extent = {.w = (int)want_w, .h = (int)want_h},
		};
		const struct xrt_rect *blit_rects = rect_count > 0 ? rects : &legacy_rect;
		uint32_t blit_count = rect_count > 0 ? rect_count : 1;
		if (blit_count > COMP_MULTI_WEAVE_EYE_STAGE_SLOTS) {
			blit_count = COMP_MULTI_WEAVE_EYE_STAGE_SLOTS; // The IPC server already rejects more.
		}

		VkImageBlit eye_blits[COMP_MULTI_WEAVE_EYE_STAGE_SLOTS * 2];
		VkImage eye_src[COMP_MULTI_WEAVE_EYE_STAGE_SLOTS * 2]; // VK_NULL_HANDLE = straight from the input.
		VkImage stage_images[COMP_MULTI_WEAVE_EYE_STAGE_SLOTS * 2];
		VkImageCopy stage_copies[COMP_MULTI_WEAVE_EYE_STAGE_SLOTS * 2];
		VkImageMemoryBarrier stage_barriers[COMP_MULTI_WEAVE_EYE_STAGE_SLOTS * 2];
		uint32_t eye_count = 0;
		uint32_t staged_count = 0;
		const VkImageSubresourceLayers color_layer = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1};

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

			// Tile position = the rect's own window position (1:1 vertically).
			const int32_t dx0 = rx;
			const int32_t dy0 = ry;
			const int32_t dx1 = rx + rw;
			const int32_t dy1 = ry + rh;
			const int32_t src_x[2] = {rx, rx + half};
			const int32_t src_w[2] = {half, rw - half};
			const int32_t tile_x[2] = {0, (int32_t)mc->weave.out_w}; // Right tile (+out_w).
			for (uint32_t e = 0; e < 2; e++) {
				VkImage stage = weave_eye_stage_get(vk, mc, i, e, (uint32_t)src_w[e], (uint32_t)rh,
				                                    WEAVE_VK_FORMAT);
				eye_blits[eye_count] = (VkImageBlit){
				    .srcSubresource = color_layer,
				    .srcOffsets = {{src_x[e], ry, 0}, {src_x[e] + src_w[e], ry + rh, 1}},
				    .dstSubresource = color_layer,
				    .dstOffsets = {{tile_x[e] + dx0, dy0, 0}, {tile_x[e] + dx1, dy1, 1}},
				};
				if (stage != VK_NULL_HANDLE) {
					// Stretch the WHOLE staged image instead of the input sub-rect.
					eye_blits[eye_count].srcOffsets[0] = (VkOffset3D){0, 0, 0};
					eye_blits[eye_count].srcOffsets[1] = (VkOffset3D){src_w[e], rh, 1};
					stage_images[staged_count] = stage;
					stage_copies[staged_count] = (VkImageCopy){
					    .srcSubresource = color_layer,
					    .srcOffset = {src_x[e], ry, 0},
					    .dstSubresource = color_layer,
					    .dstOffset = {0, 0, 0},
					    .extent = {(uint32_t)src_w[e], (uint32_t)rh, 1},
					};
					// Fully overwritten by the copy: discard from UNDEFINED. The
					// TRANSFER src scope orders it after an earlier submit's
					// stretch read of the same image (WAR — execution only).
					stage_barriers[staged_count] = (VkImageMemoryBarrier){
					    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
					    .srcAccessMask = 0,
					    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
					    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
					    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					    .image = stage,
					    .subresourceRange = range,
					};
					staged_count++;
				}
				eye_src[eye_count] = stage;
				eye_count++;
			}
		}

		if (staged_count > 0) {
			vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
			                         0, NULL, 0, NULL, staged_count, stage_barriers);
			for (uint32_t k = 0; k < staged_count; k++) {
				vk->vkCmdCopyImage(cmd, mc->weave.in_image, VK_IMAGE_LAYOUT_GENERAL, stage_images[k],
				                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &stage_copies[k]);
			}
			// Copy writes -> stretch reads.
			for (uint32_t k = 0; k < staged_count; k++) {
				stage_barriers[k].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
				stage_barriers[k].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
				stage_barriers[k].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
				stage_barriers[k].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			}
			vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
			                         0, NULL, 0, NULL, staged_count, stage_barriers);
		}

		// Stretches, in rect order (a later rect still overwrites an overlapping
		// earlier one).
		for (uint32_t k = 0; k < eye_count; k++) {
			const bool staged = eye_src[k] != VK_NULL_HANDLE;
			VkImage src = staged ? eye_src[k] : mc->weave.in_image;
			VkImageLayout src_layout =
			    staged ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
			vk->vkCmdBlitImage(cmd, src, src_layout, mc->weave.sbs_image,
			                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &eye_blits[k], VK_FILTER_LINEAR);
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
		} // end !nview (legacy/batch SBS record)

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

		// ONE process_atlas per submit. Legacy/batch: 2x1 SBS, per-eye dims = the
		// window. v6: the caller's grid at content_view dims — output is one
		// content view (cvw x cvh = the whole window).
		xrt_display_processor_set_target_color_view(mc->weave.dp, mc->weave.out_view);
		xrt_display_processor_process_atlas(mc->weave.dp, cmd,                             //
		                                    (VkImage_XDP)dp_src_image, dp_src_view,        //
		                                    atlas_view_w, atlas_view_h,                    //
		                                    grid_cols, grid_rows,                          //
		                                    (VkFormat_XDP)WEAVE_VK_FORMAT,                 //
		                                    mc->weave.out_fb,                              //
		                                    (VkImage_XDP)mc->weave.out_image,              //
		                                    mc->weave.out_w, mc->weave.out_h,              //
		                                    (VkFormat_XDP)WEAVE_VK_FORMAT,                 //
		                                    0, 0, 0, 0);

		// v6: restore the input atlas to GENERAL so the next frame's barrier
		// (oldLayout=GENERAL) is correct and the caller's external Metal writes
		// land into a defined layout. (In the crop path the input's last use was
		// the box copy; in zero-copy it was the DP sample.)
		if (nview) {
			VkImageMemoryBarrier in_restore = {
			    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			    .srcAccessMask = v6_zero_copy ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_TRANSFER_READ_BIT,
			    .dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
			    .oldLayout = v6_zero_copy ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
			                              : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
			    .image = mc->weave.in_image,
			    .subresourceRange = range,
			};
			vk->vkCmdPipelineBarrier(cmd,
			                         v6_zero_copy ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
			                                      : VK_PIPELINE_STAGE_TRANSFER_BIT,
			                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &in_restore);
		}

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

		ok = true;
	} while (false);
	return ok;
}

static bool
vkb_get_eyes(struct multi_compositor *mc, struct xrt_eye_positions *out_eyes)
{
	return mc->weave.dp != NULL && xrt_display_processor_get_predicted_eye_positions(mc->weave.dp, out_eyes);
}

static bool
vkb_request_display_mode(struct multi_compositor *mc, bool enable_3d, bool *out_has_slot)
{
	struct xrt_display_processor *dp = mc->weave.dp;
	*out_has_slot = dp != NULL && XRT_DP_HAS_SLOT(dp, request_display_mode) && dp->request_display_mode != NULL;
	return *out_has_slot ? xrt_display_processor_request_display_mode(dp, enable_3d) : true;
}

static bool
vkb_get_hardware_3d_state(struct multi_compositor *mc, bool *out_is_3d)
{
	return mc->weave.dp != NULL && xrt_display_processor_get_hardware_3d_state(mc->weave.dp, out_is_3d);
}

static void
vkb_fini(struct multi_compositor *mc)
{
	struct vk_bundle *vk = weave_get_vk(mc);
	if (vk == NULL) {
		return;
	}
	if (mc->weave.engine_initialized) {
		// Nothing may be in flight (submits are synchronous), but a
		// belt-and-braces idle keeps teardown safe if that changes.
		vk->vkQueueWaitIdle(vk->main_queue->queue);
	}
	weave_release_input(vk, mc);
	weave_release_overlay(vk, mc);
	weave_release_scratch(vk, mc);
	weave_release_crop(vk, mc);
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
	mc->weave.engine_initialized = false;
}

const struct comp_multi_weave_macos_backend comp_multi_weave_macos_backend_vk = {
    .name = "vk",
    .feeds_present_origin = false,
    .ensure_engine = vkb_ensure_engine,
    .import_input = vkb_import_input,
    .release_input = vkb_release_input,
    .import_overlay = vkb_import_overlay,
    .release_overlay = vkb_release_overlay,
    .output_tolerates_resample = vkb_output_tolerates_resample,
    .ensure_output = vkb_ensure_output,
    .record_and_wait = vkb_record_and_wait,
    .get_eyes = vkb_get_eyes,
    .request_display_mode = vkb_request_display_mode,
    .get_hardware_3d_state = vkb_get_hardware_3d_state,
    .snap_window_rect = NULL, // unchanged: no snap on the vk weave path (identity)
    .fini = vkb_fini,
};

#endif // XRT_OS_MACOS
