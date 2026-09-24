// Copyright 2026, DisplayXR contributors.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Desktop-Linux dma-buf + DRM-format-modifier + sync_file helpers (#1699 R5).
 *
 * Runtime form of the consumer/exporter code the week-1 cross-process dma-buf
 * spike proved bit-exact on LINEAR, X-tiled, 4-tiled and compressed (flat CCS)
 * modifiers. See vk_dmabuf.h for the contracts, fd ownership above all.
 *
 * @ingroup aux_vk
 */

#include "xrt/xrt_config_os.h"

#ifdef XRT_OS_LINUX_DESKTOP

#include "vk/vk_dmabuf.h"

#include "util/u_misc.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>


/*
 *
 * Helpers.
 *
 */

static void
close_fd(int *fd)
{
	if (*fd >= 0) {
		close(*fd);
	}
	*fd = -1;
}

/*!
 * vkGetMemoryFdPropertiesKHR is not carried by @ref vk_bundle, and adding it
 * would change sizeof(struct vk_bundle), which the plug-in loader checks. So it
 * is looked up here, per call. Deliberately not cached behind a static keyed by
 * VkDevice: a destroyed device's handle value can be reused by the next
 * vkCreateDevice, and a stale entry point would be a use-after-free. The
 * lookup is a loader hash probe, noise next to a dma-buf import.
 */
static PFN_vkGetMemoryFdPropertiesKHR
get_memory_fd_properties_fn(struct vk_bundle *vk)
{
	if (vk->vkGetDeviceProcAddr == NULL || vk->device == VK_NULL_HANDLE) {
		return NULL;
	}
	return (PFN_vkGetMemoryFdPropertiesKHR)vk->vkGetDeviceProcAddr(vk->device, "vkGetMemoryFdPropertiesKHR");
}

static bool
physical_device_has_extension(struct vk_bundle *vk, const char *name)
{
	uint32_t count = 0;
	if (vk->vkEnumerateDeviceExtensionProperties(vk->physical_device, NULL, &count, NULL) != VK_SUCCESS ||
	    count == 0) {
		return false;
	}
	VkExtensionProperties *props = U_TYPED_ARRAY_CALLOC(VkExtensionProperties, count);
	if (props == NULL) {
		return false;
	}
	bool found = false;
	if (vk->vkEnumerateDeviceExtensionProperties(vk->physical_device, NULL, &count, props) == VK_SUCCESS) {
		for (uint32_t i = 0; i < count && !found; i++) {
			found = strcmp(props[i].extensionName, name) == 0;
		}
	}
	free(props);
	return found;
}

/*!
 * Ask the driver whether a 2D DRM_FORMAT_MODIFIER_EXT image of this
 * format/modifier/usage with a DMA_BUF handle is supported, and with which
 * external-memory features and maximum extent.
 */
static VkResult
modifier_image_support(struct vk_bundle *vk,
                       VkFormat format,
                       uint64_t modifier,
                       VkImageUsageFlags usage,
                       VkExternalMemoryFeatureFlags *out_features,
                       VkExtent3D *out_max_extent)
{
	VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier_info = {
	    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
	    .drmFormatModifier = modifier,
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	VkPhysicalDeviceExternalImageFormatInfo external_info = {
	    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
	    .pNext = &modifier_info,
	    .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	};
	VkPhysicalDeviceImageFormatInfo2 format_info = {
	    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
	    .pNext = &external_info,
	    .format = format,
	    .type = VK_IMAGE_TYPE_2D,
	    .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
	    .usage = usage,
	};
	VkExternalImageFormatProperties external_props = {
	    .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
	};
	VkImageFormatProperties2 props = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
	    .pNext = &external_props,
	};

	VkResult ret = vk->vkGetPhysicalDeviceImageFormatProperties2(vk->physical_device, &format_info, &props);
	if (out_features != NULL) {
		*out_features = ret == VK_SUCCESS ? external_props.externalMemoryProperties.externalMemoryFeatures : 0;
	}
	if (out_max_extent != NULL) {
		*out_max_extent = props.imageFormatProperties.maxExtent;
	}
	return ret;
}

/*!
 * The device's full modifier list for a format (no usage filtering).
 * *out_props is calloc'd; the caller frees it.
 */
static VkResult
get_format_modifier_properties(struct vk_bundle *vk,
                               VkFormat format,
                               VkDrmFormatModifierPropertiesEXT **out_props,
                               uint32_t *out_count)
{
	*out_props = NULL;
	*out_count = 0;

	VkDrmFormatModifierPropertiesListEXT list = {
	    .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
	};
	VkFormatProperties2 format_props = {
	    .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
	    .pNext = &list,
	};
	vk->vkGetPhysicalDeviceFormatProperties2(vk->physical_device, format, &format_props);
	if (list.drmFormatModifierCount == 0) {
		return VK_SUCCESS;
	}

	VkDrmFormatModifierPropertiesEXT *props =
	    U_TYPED_ARRAY_CALLOC(VkDrmFormatModifierPropertiesEXT, list.drmFormatModifierCount);
	if (props == NULL) {
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}
	list.pDrmFormatModifierProperties = props;
	vk->vkGetPhysicalDeviceFormatProperties2(vk->physical_device, format, &format_props);

	*out_props = props;
	*out_count = list.drmFormatModifierCount;
	return VK_SUCCESS;
}

//! Plane count of @p modifier for @p format, 0 if the device does not offer it.
static uint32_t
get_modifier_plane_count(struct vk_bundle *vk, VkFormat format, uint64_t modifier)
{
	VkDrmFormatModifierPropertiesEXT *props = NULL;
	uint32_t count = 0;
	if (get_format_modifier_properties(vk, format, &props, &count) != VK_SUCCESS) {
		return 0;
	}
	uint32_t planes = 0;
	for (uint32_t i = 0; i < count; i++) {
		if (props[i].drmFormatModifier == modifier) {
			planes = props[i].drmFormatModifierPlaneCount;
			break;
		}
	}
	free(props);
	return planes;
}

static const VkImageAspectFlagBits memory_plane_aspects[XRT_WEAVE_DMABUF_MAX_PLANES] = {
    VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT,
    VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT,
    VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT,
    VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT,
};


/*
 *
 * Capability.
 *
 */

bool
vk_dmabuf_supported(struct vk_bundle *vk)
{
	if (!vk->has_EXT_external_memory_dma_buf || !vk->has_EXT_image_drm_format_modifier) {
		return false;
	}
	if (vk->vkGetMemoryFdKHR == NULL || vk->vkGetImageDrmFormatModifierPropertiesEXT == NULL ||
	    vk->vkGetPhysicalDeviceFormatProperties2 == NULL || vk->vkGetPhysicalDeviceImageFormatProperties2 == NULL ||
	    vk->vkGetImageMemoryRequirements2 == NULL || get_memory_fd_properties_fn(vk) == NULL) {
		return false;
	}
	return physical_device_has_extension(vk, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
}

bool
vk_dmabuf_sync_fd_supported(struct vk_bundle *vk)
{
	return vk_has_external_semaphore_fd(vk) && vk->external.binary_semaphore_sync_fd;
}


/*
 *
 * Formats and modifiers.
 *
 */

VkFormat
vk_dmabuf_fourcc_to_vk_format(uint32_t fourcc, bool *out_has_alpha)
{
	bool has_alpha = false;
	VkFormat format = VK_FORMAT_UNDEFINED;

	switch (fourcc) {
	case DRM_FORMAT_ARGB8888:
		has_alpha = true;
		format = VK_FORMAT_B8G8R8A8_UNORM;
		break;
	case DRM_FORMAT_XRGB8888:
		has_alpha = false;
		format = VK_FORMAT_B8G8R8A8_UNORM;
		break;
	case DRM_FORMAT_ABGR8888:
		has_alpha = true;
		format = VK_FORMAT_R8G8B8A8_UNORM;
		break;
	case DRM_FORMAT_XBGR8888:
		has_alpha = false;
		format = VK_FORMAT_R8G8B8A8_UNORM;
		break;
	default: break;
	}

	if (out_has_alpha != NULL) {
		*out_has_alpha = has_alpha;
	}
	return format;
}

uint32_t
vk_dmabuf_vk_format_to_fourcc(VkFormat format, bool has_alpha)
{
	switch (format) {
	case VK_FORMAT_B8G8R8A8_UNORM: return has_alpha ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_XRGB8888;
	case VK_FORMAT_R8G8B8A8_UNORM: return has_alpha ? DRM_FORMAT_ABGR8888 : DRM_FORMAT_XBGR8888;
	default: return 0;
	}
}

VkResult
vk_dmabuf_query_modifiers(struct vk_bundle *vk,
                          VkFormat format,
                          VkImageUsageFlags usage,
                          struct vk_dmabuf_modifier_info *out_list,
                          uint32_t max,
                          uint32_t *out_count)
{
	*out_count = 0;
	if (!vk_dmabuf_supported(vk)) {
		return VK_ERROR_EXTENSION_NOT_PRESENT;
	}

	VkDrmFormatModifierPropertiesEXT *props = NULL;
	uint32_t count = 0;
	VkResult ret = get_format_modifier_properties(vk, format, &props, &count);
	if (ret != VK_SUCCESS) {
		return ret;
	}

	uint32_t n = 0;
	for (uint32_t i = 0; i < count; i++) {
		VkExternalMemoryFeatureFlags features = 0;
		if (modifier_image_support(vk, format, props[i].drmFormatModifier, usage, &features, NULL) !=
		    VK_SUCCESS) {
			continue;
		}
		if (n < max && out_list != NULL) {
			out_list[n] = (struct vk_dmabuf_modifier_info){
			    .modifier = props[i].drmFormatModifier,
			    .plane_count = props[i].drmFormatModifierPlaneCount,
			    .features = props[i].drmFormatModifierTilingFeatures,
			    .importable = (features & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) != 0,
			    .exportable = (features & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) != 0,
			};
		}
		n++;
	}
	free(props);

	*out_count = n;
	return n > max ? VK_INCOMPLETE : VK_SUCCESS;
}


/*
 *
 * Images.
 *
 */

VkResult
vk_create_image_from_dmabuf(struct vk_bundle *vk,
                            const struct xrt_weave_dmabuf_desc *desc,
                            VkImageUsageFlags usage,
                            VkImage *out_image,
                            VkDeviceMemory *out_memory,
                            VkFormat *out_format)
{
	/*
	 * Ownership: `fd` is ours until a vkAllocateMemory import SUCCEEDS
	 * (VkImportMemoryFdInfoKHR: "Importing memory from a file descriptor
	 * transfers ownership of the file descriptor from the application to the
	 * Vulkan implementation" -- on success; a failed import leaves it with the
	 * application). From then on only freeing the memory releases it. Every
	 * early return therefore goes through `fail`, which closes `fd` while it is
	 * still ours and never after.
	 */
	int fd = desc != NULL ? desc->fd : -1;
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkResult ret;

	*out_image = VK_NULL_HANDLE;
	*out_memory = VK_NULL_HANDLE;
	if (out_format != NULL) {
		*out_format = VK_FORMAT_UNDEFINED;
	}

	if (desc == NULL || fd < 0) {
		VK_ERROR(vk, "vk_create_image_from_dmabuf: no descriptor or no fd");
		ret = VK_ERROR_INVALID_EXTERNAL_HANDLE;
		goto fail;
	}
	if (!vk_dmabuf_supported(vk)) {
		VK_ERROR(vk, "vk_create_image_from_dmabuf: dma-buf + DRM format modifier import not enabled on device");
		ret = VK_ERROR_EXTENSION_NOT_PRESENT;
		goto fail;
	}

	bool has_alpha = false;
	const VkFormat format = vk_dmabuf_fourcc_to_vk_format(desc->drm_fourcc, &has_alpha);
	if (format == VK_FORMAT_UNDEFINED) {
		VK_ERROR(vk, "vk_create_image_from_dmabuf: unsupported DRM fourcc 0x%08x", desc->drm_fourcc);
		ret = VK_ERROR_FORMAT_NOT_SUPPORTED;
		goto fail;
	}
	if (desc->drm_modifier == DRM_FORMAT_MOD_INVALID) {
		VK_ERROR(vk, "vk_create_image_from_dmabuf: DRM_FORMAT_MOD_INVALID is not an importable modifier");
		ret = VK_ERROR_FORMAT_NOT_SUPPORTED;
		goto fail;
	}
	if (desc->width == 0 || desc->height == 0) {
		VK_ERROR(vk, "vk_create_image_from_dmabuf: zero extent %ux%u", desc->width, desc->height);
		ret = VK_ERROR_INITIALIZATION_FAILED;
		goto fail;
	}

	// Offered for this format + usage + DMA_BUF import, and big enough?
	VkExternalMemoryFeatureFlags ext_features = 0;
	VkExtent3D max_extent = {0};
	ret = modifier_image_support(vk, format, desc->drm_modifier, usage, &ext_features, &max_extent);
	if (ret != VK_SUCCESS || (ext_features & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) == 0) {
		VK_ERROR(vk,
		         "vk_create_image_from_dmabuf: modifier 0x%016" PRIx64
		         " not importable for fourcc 0x%08x usage 0x%x (%s)",
		         desc->drm_modifier, desc->drm_fourcc, usage, vk_result_string(ret));
		if (ret == VK_SUCCESS) {
			ret = VK_ERROR_FORMAT_NOT_SUPPORTED;
		}
		goto fail;
	}
	if (desc->width > max_extent.width || desc->height > max_extent.height) {
		VK_ERROR(vk, "vk_create_image_from_dmabuf: %ux%u exceeds the device maximum %ux%u", desc->width,
		         desc->height, max_extent.width, max_extent.height);
		ret = VK_ERROR_FORMAT_NOT_SUPPORTED;
		goto fail;
	}

	// The explicit create info must carry exactly the modifier's plane count.
	const uint32_t planes = get_modifier_plane_count(vk, format, desc->drm_modifier);
	if (planes == 0 || planes > XRT_WEAVE_DMABUF_MAX_PLANES || desc->plane_count != planes) {
		VK_ERROR(vk, "vk_create_image_from_dmabuf: modifier 0x%016" PRIx64 " has %u planes, descriptor says %u",
		         desc->drm_modifier, planes, desc->plane_count);
		ret = VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT;
		goto fail;
	}

	VkSubresourceLayout plane_layouts[XRT_WEAVE_DMABUF_MAX_PLANES] = {0};
	for (uint32_t i = 0; i < planes; i++) {
		// size, arrayPitch and depthPitch are ignored for explicit modifier layouts.
		plane_layouts[i].offset = desc->offsets[i];
		plane_layouts[i].rowPitch = desc->strides[i];
	}

	VkImageDrmFormatModifierExplicitCreateInfoEXT explicit_info = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
	    .drmFormatModifier = desc->drm_modifier,
	    .drmFormatModifierPlaneCount = planes,
	    .pPlaneLayouts = plane_layouts,
	};
	VkExternalMemoryImageCreateInfo external_info = {
	    .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
	    .pNext = &explicit_info,
	    .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	};
	VkImageCreateInfo image_info = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .pNext = &external_info,
	    .imageType = VK_IMAGE_TYPE_2D,
	    .format = format,
	    .extent = {.width = desc->width, .height = desc->height, .depth = 1},
	    .mipLevels = 1,
	    .arrayLayers = 1,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
	    .usage = usage,
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	ret = vk->vkCreateImage(vk->device, &image_info, NULL, &image);
	if (ret != VK_SUCCESS) {
		VK_ERROR(vk, "vk_create_image_from_dmabuf: vkCreateImage(modifier 0x%016" PRIx64 "): %s",
		         desc->drm_modifier, vk_result_string(ret));
		goto fail;
	}

	PFN_vkGetMemoryFdPropertiesKHR get_fd_props = get_memory_fd_properties_fn(vk);
	VkMemoryFdPropertiesKHR fd_props = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
	};
	ret = get_fd_props(vk->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fd_props);
	if (ret != VK_SUCCESS) {
		VK_ERROR(vk, "vk_create_image_from_dmabuf: vkGetMemoryFdPropertiesKHR: %s", vk_result_string(ret));
		goto fail;
	}

	VkMemoryDedicatedRequirements dedicated_reqs = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
	};
	VkMemoryRequirements2 reqs = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
	    .pNext = &dedicated_reqs,
	};
	VkImageMemoryRequirementsInfo2 reqs_info = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
	    .image = image,
	};
	vk->vkGetImageMemoryRequirements2(vk->device, &reqs_info, &reqs);

	/*
	 * An allocationSize larger than the dma-buf is invalid usage the driver
	 * need not catch, and the descriptor is a client's word. A dma-buf reports
	 * its size through lseek(SEEK_END); where it does not (not a dma-buf), the
	 * driver's own import checks are all that is left.
	 */
	const off_t dmabuf_size = lseek(fd, 0, SEEK_END);
	if (dmabuf_size >= 0) {
		(void)lseek(fd, 0, SEEK_SET);
		if ((uint64_t)dmabuf_size < (uint64_t)reqs.memoryRequirements.size) {
			VK_ERROR(vk, "vk_create_image_from_dmabuf: dma-buf is %" PRIu64 " bytes, image needs %" PRIu64,
			         (uint64_t)dmabuf_size, (uint64_t)reqs.memoryRequirements.size);
			ret = VK_ERROR_INVALID_EXTERNAL_HANDLE;
			goto fail;
		}
	}

	uint32_t memory_type = 0;
	if (!vk_get_memory_type(vk, reqs.memoryRequirements.memoryTypeBits & fd_props.memoryTypeBits, 0,
	                        &memory_type)) {
		VK_ERROR(vk, "vk_create_image_from_dmabuf: no memory type in image bits 0x%x & fd bits 0x%x",
		         reqs.memoryRequirements.memoryTypeBits, fd_props.memoryTypeBits);
		ret = VK_ERROR_OUT_OF_DEVICE_MEMORY;
		goto fail;
	}

	VkMemoryDedicatedAllocateInfo dedicated_info = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
	    .image = image,
	};
	VkImportMemoryFdInfoKHR import_info = {
	    .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
	    .pNext = &dedicated_info,
	    .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	    .fd = fd,
	};
	VkMemoryAllocateInfo alloc_info = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .pNext = &import_info,
	    .allocationSize = reqs.memoryRequirements.size,
	    .memoryTypeIndex = memory_type,
	};
	ret = vk->vkAllocateMemory(vk->device, &alloc_info, NULL, &memory);
	if (ret != VK_SUCCESS) {
		VK_ERROR(vk, "vk_create_image_from_dmabuf: vkAllocateMemory(import DMA_BUF): %s",
		         vk_result_string(ret));
		goto fail; // the import failed, so the fd is still ours: closed below
	}
	fd = -1; // Owned by `memory` now.

	ret = vk->vkBindImageMemory(vk->device, image, memory, 0);
	if (ret != VK_SUCCESS) {
		VK_ERROR(vk, "vk_create_image_from_dmabuf: vkBindImageMemory: %s", vk_result_string(ret));
		goto fail;
	}

	*out_image = image;
	*out_memory = memory;
	if (out_format != NULL) {
		*out_format = format;
	}
	return VK_SUCCESS;

fail:
	if (memory != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, memory, NULL); // releases the imported fd
	}
	if (image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, image, NULL);
	}
	close_fd(&fd);
	return ret;
}

VkResult
vk_create_exportable_dmabuf_image(struct vk_bundle *vk,
                                  uint32_t width,
                                  uint32_t height,
                                  VkFormat format,
                                  VkImageUsageFlags usage,
                                  const uint64_t *modifiers,
                                  uint32_t modifier_count,
                                  struct xrt_weave_dmabuf_output_desc *out_desc,
                                  VkImage *out_image,
                                  VkDeviceMemory *out_memory)
{
	static const uint64_t linear_only[] = {DRM_FORMAT_MOD_LINEAR};
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	uint64_t *candidates = NULL;
	VkResult ret;

	U_ZERO(out_desc);
	out_desc->fd = -1;
	*out_image = VK_NULL_HANDLE;
	*out_memory = VK_NULL_HANDLE;

	if (!vk_dmabuf_supported(vk)) {
		VK_ERROR(vk, "vk_create_exportable_dmabuf_image: dma-buf + DRM format modifier not enabled on device");
		return VK_ERROR_EXTENSION_NOT_PRESENT;
	}
	const uint32_t fourcc = vk_dmabuf_vk_format_to_fourcc(format, true);
	if (fourcc == 0) {
		VK_ERROR(vk, "vk_create_exportable_dmabuf_image: VkFormat %d has no DRM fourcc mapping", format);
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	}
	if (width == 0 || height == 0) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	if (modifiers == NULL || modifier_count == 0) {
		modifiers = linear_only;
		modifier_count = ARRAY_SIZE(linear_only);
	}

	/*
	 * Every entry of VkImageDrmFormatModifierListCreateInfoEXT must be
	 * compatible with the create info (VUID-...-pDrmFormatModifiers-02263), so
	 * filter: drop INVALID (a Wayland "implicit modifier" marker, not a Vulkan
	 * modifier) and anything the device cannot export at this size + usage.
	 */
	candidates = U_TYPED_ARRAY_CALLOC(uint64_t, modifier_count);
	if (candidates == NULL) {
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}
	uint32_t candidate_count = 0;
	for (uint32_t i = 0; i < modifier_count; i++) {
		if (modifiers[i] == DRM_FORMAT_MOD_INVALID) {
			continue;
		}
		VkExternalMemoryFeatureFlags ext_features = 0;
		VkExtent3D max_extent = {0};
		if (modifier_image_support(vk, format, modifiers[i], usage, &ext_features, &max_extent) != VK_SUCCESS ||
		    (ext_features & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) == 0 || width > max_extent.width ||
		    height > max_extent.height) {
			continue;
		}
		candidates[candidate_count++] = modifiers[i];
	}
	if (candidate_count == 0) {
		VK_ERROR(vk,
		         "vk_create_exportable_dmabuf_image: none of the %u modifiers can export VkFormat %d "
		         "%ux%u usage 0x%x",
		         modifier_count, format, width, height, usage);
		free(candidates);
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	}

	VkImageDrmFormatModifierListCreateInfoEXT list_info = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
	    .drmFormatModifierCount = candidate_count,
	    .pDrmFormatModifiers = candidates,
	};
	VkExternalMemoryImageCreateInfo external_info = {
	    .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
	    .pNext = &list_info,
	    .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	};
	VkImageCreateInfo image_info = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .pNext = &external_info,
	    .imageType = VK_IMAGE_TYPE_2D,
	    .format = format,
	    .extent = {.width = width, .height = height, .depth = 1},
	    .mipLevels = 1,
	    .arrayLayers = 1,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
	    .usage = usage,
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	ret = vk->vkCreateImage(vk->device, &image_info, NULL, &image);
	free(candidates); // Only read during vkCreateImage.
	candidates = NULL;
	if (ret != VK_SUCCESS) {
		VK_ERROR(vk, "vk_create_exportable_dmabuf_image: vkCreateImage: %s", vk_result_string(ret));
		goto fail;
	}

	VkMemoryRequirements2 reqs = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
	};
	VkImageMemoryRequirementsInfo2 reqs_info = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
	    .image = image,
	};
	vk->vkGetImageMemoryRequirements2(vk->device, &reqs_info, &reqs);

	uint32_t memory_type = 0;
	if (!vk_get_memory_type(vk, reqs.memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
	                        &memory_type) &&
	    !vk_get_memory_type(vk, reqs.memoryRequirements.memoryTypeBits, 0, &memory_type)) {
		VK_ERROR(vk, "vk_create_exportable_dmabuf_image: no memory type in bits 0x%x",
		         reqs.memoryRequirements.memoryTypeBits);
		ret = VK_ERROR_OUT_OF_DEVICE_MEMORY;
		goto fail;
	}

	VkMemoryDedicatedAllocateInfo dedicated_info = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
	    .image = image,
	};
	VkExportMemoryAllocateInfo export_info = {
	    .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
	    .pNext = &dedicated_info,
	    .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	};
	VkMemoryAllocateInfo alloc_info = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .pNext = &export_info,
	    .allocationSize = reqs.memoryRequirements.size,
	    .memoryTypeIndex = memory_type,
	};
	ret = vk->vkAllocateMemory(vk->device, &alloc_info, NULL, &memory);
	if (ret != VK_SUCCESS) {
		VK_ERROR(vk, "vk_create_exportable_dmabuf_image: vkAllocateMemory(export DMA_BUF): %s",
		         vk_result_string(ret));
		goto fail;
	}
	ret = vk->vkBindImageMemory(vk->device, image, memory, 0);
	if (ret != VK_SUCCESS) {
		VK_ERROR(vk, "vk_create_exportable_dmabuf_image: vkBindImageMemory: %s", vk_result_string(ret));
		goto fail;
	}

	VkImageDrmFormatModifierPropertiesEXT modifier_props = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT,
	};
	ret = vk->vkGetImageDrmFormatModifierPropertiesEXT(vk->device, image, &modifier_props);
	if (ret != VK_SUCCESS) {
		VK_ERROR(vk, "vk_create_exportable_dmabuf_image: vkGetImageDrmFormatModifierPropertiesEXT: %s",
		         vk_result_string(ret));
		goto fail;
	}

	const uint32_t planes = get_modifier_plane_count(vk, format, modifier_props.drmFormatModifier);
	if (planes == 0 || planes > XRT_WEAVE_DMABUF_MAX_PLANES) {
		VK_ERROR(vk, "vk_create_exportable_dmabuf_image: chosen modifier 0x%016" PRIx64 " has %u planes",
		         modifier_props.drmFormatModifier, planes);
		ret = VK_ERROR_FORMAT_NOT_SUPPORTED;
		goto fail;
	}
	for (uint32_t i = 0; i < planes; i++) {
		VkImageSubresource subresource = {.aspectMask = memory_plane_aspects[i]};
		VkSubresourceLayout layout = {0};
		vk->vkGetImageSubresourceLayout(vk->device, image, &subresource, &layout);
		if (layout.offset > UINT32_MAX || layout.rowPitch > UINT32_MAX) {
			VK_ERROR(vk, "vk_create_exportable_dmabuf_image: plane %u layout does not fit the descriptor",
			         i);
			ret = VK_ERROR_FORMAT_NOT_SUPPORTED;
			goto fail;
		}
		out_desc->offsets[i] = (uint32_t)layout.offset;
		out_desc->strides[i] = (uint32_t)layout.rowPitch;
	}

	VkMemoryGetFdInfoKHR get_fd_info = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
	    .memory = memory,
	    .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	};
	int fd = -1;
	ret = vk->vkGetMemoryFdKHR(vk->device, &get_fd_info, &fd);
	if (ret != VK_SUCCESS || fd < 0) {
		VK_ERROR(vk, "vk_create_exportable_dmabuf_image: vkGetMemoryFdKHR(DMA_BUF): %s", vk_result_string(ret));
		if (ret == VK_SUCCESS) {
			ret = VK_ERROR_INVALID_EXTERNAL_HANDLE;
		}
		goto fail;
	}

	out_desc->fd = fd;
	out_desc->width = width;
	out_desc->height = height;
	out_desc->drm_fourcc = fourcc;
	out_desc->drm_modifier = modifier_props.drmFormatModifier;
	out_desc->plane_count = planes;
	out_desc->size = (uint64_t)reqs.memoryRequirements.size;

	*out_image = image;
	*out_memory = memory;
	return VK_SUCCESS;

fail:
	if (memory != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, memory, NULL);
	}
	if (image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, image, NULL);
	}
	U_ZERO(out_desc);
	out_desc->fd = -1;
	return ret;
}


/*
 *
 * sync_file (SYNC_FD) semaphores.
 *
 */

VkResult
vk_semaphore_import_sync_fd(struct vk_bundle *vk, VkSemaphore semaphore, int fd)
{
	if (!vk_dmabuf_sync_fd_supported(vk)) {
		close_fd(&fd);
		return VK_ERROR_FEATURE_NOT_PRESENT;
	}

	VkImportSemaphoreFdInfoKHR import_info = {
	    .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
	    .semaphore = semaphore,
	    .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
	    .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
	    .fd = fd,
	};
	VkResult ret = vk->vkImportSemaphoreFdKHR(vk->device, &import_info);
	if (ret != VK_SUCCESS) {
		// A failed import does not transfer ownership: the fd is still ours.
		VK_ERROR(vk, "vk_semaphore_import_sync_fd: vkImportSemaphoreFdKHR(SYNC_FD): %s", vk_result_string(ret));
		close_fd(&fd);
	}
	return ret;
}

VkResult
vk_create_exportable_sync_fd_semaphore(struct vk_bundle *vk, VkSemaphore *out_semaphore)
{
	*out_semaphore = VK_NULL_HANDLE;
	if (!vk_dmabuf_sync_fd_supported(vk)) {
		return VK_ERROR_FEATURE_NOT_PRESENT;
	}

	VkExportSemaphoreCreateInfo export_info = {
	    .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
	    .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
	};
	VkSemaphoreCreateInfo create_info = {
	    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
	    .pNext = &export_info,
	};
	VkResult ret = vk->vkCreateSemaphore(vk->device, &create_info, NULL, out_semaphore);
	if (ret != VK_SUCCESS) {
		VK_ERROR(vk, "vk_create_exportable_sync_fd_semaphore: vkCreateSemaphore: %s", vk_result_string(ret));
		*out_semaphore = VK_NULL_HANDLE;
	}
	return ret;
}

VkResult
vk_semaphore_export_sync_fd(struct vk_bundle *vk, VkSemaphore semaphore, int *out_fd)
{
	*out_fd = -1;
	if (!vk_dmabuf_sync_fd_supported(vk)) {
		return VK_ERROR_FEATURE_NOT_PRESENT;
	}

	VkSemaphoreGetFdInfoKHR get_info = {
	    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
	    .semaphore = semaphore,
	    .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
	};
	int fd = -1;
	VkResult ret = vk->vkGetSemaphoreFdKHR(vk->device, &get_info, &fd);
	if (ret != VK_SUCCESS) {
		VK_ERROR(vk, "vk_semaphore_export_sync_fd: vkGetSemaphoreFdKHR(SYNC_FD): %s", vk_result_string(ret));
		return ret;
	}
	*out_fd = fd;
	return VK_SUCCESS;
}


/*
 *
 * Queue-family ownership.
 *
 */

static void
foreign_barrier(struct vk_bundle *vk,
                VkCommandBuffer cmd,
                VkImage image,
                bool acquire,
                VkImageLayout layout,
                VkPipelineStageFlags stage,
                VkAccessFlags access)
{
	const uint32_t own = vk->main_queue->family_index;

	VkImageMemoryBarrier barrier = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    // The foreign side's accesses are made visible by the semaphore, not by masks.
	    .srcAccessMask = acquire ? 0 : access,
	    .dstAccessMask = acquire ? access : 0,
	    .oldLayout = acquire ? VK_IMAGE_LAYOUT_GENERAL : layout,
	    .newLayout = acquire ? layout : VK_IMAGE_LAYOUT_GENERAL,
	    .srcQueueFamilyIndex = acquire ? VK_QUEUE_FAMILY_FOREIGN_EXT : own,
	    .dstQueueFamilyIndex = acquire ? own : VK_QUEUE_FAMILY_FOREIGN_EXT,
	    .image = image,
	    .subresourceRange =
	        {
	            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	            .baseMipLevel = 0,
	            .levelCount = 1,
	            .baseArrayLayer = 0,
	            .layerCount = 1,
	        },
	};

	vk->vkCmdPipelineBarrier(                                   //
	    cmd,                                                    // commandBuffer
	    acquire ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : stage,    // srcStageMask
	    acquire ? stage : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, // dstStageMask
	    0,                                                      // dependencyFlags
	    0, NULL,                                                // memory barriers
	    0, NULL,                                                // buffer barriers
	    1, &barrier);                                           // image barriers
}

void
vk_dmabuf_cmd_acquire_foreign(struct vk_bundle *vk,
                              VkCommandBuffer cmd,
                              VkImage image,
                              VkImageLayout dst_layout,
                              VkPipelineStageFlags dst_stage,
                              VkAccessFlags dst_access)
{
	foreign_barrier(vk, cmd, image, true, dst_layout, dst_stage, dst_access);
}

void
vk_dmabuf_cmd_release_foreign(struct vk_bundle *vk,
                              VkCommandBuffer cmd,
                              VkImage image,
                              VkImageLayout src_layout,
                              VkPipelineStageFlags src_stage,
                              VkAccessFlags src_access)
{
	foreign_barrier(vk, cmd, image, false, src_layout, src_stage, src_access);
}

#endif // XRT_OS_LINUX_DESKTOP
