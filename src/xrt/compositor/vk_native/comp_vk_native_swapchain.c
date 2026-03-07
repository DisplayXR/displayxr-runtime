// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Vulkan native swapchain implementation.
 * @author David Fattal
 * @ingroup comp_vk_native
 */

#include "comp_vk_native_swapchain.h"
#include "comp_vk_native_compositor.h"

#include "xrt/xrt_handles.h"
#include "xrt/xrt_vulkan_includes.h"
#include "vk/vk_helpers.h"

#include "util/u_logging.h"
#include "util/u_misc.h"

#include <string.h>

/*!
 * Maximum number of images in a swapchain.
 */
#define MAX_SWAPCHAIN_IMAGES 8

/*!
 * Vulkan swapchain structure.
 */
struct comp_vk_native_swapchain
{
	//! Base type - must be first!
	struct xrt_swapchain_native base;

	//! Vulkan bundle (borrowed from compositor).
	struct vk_bundle *vk;

	//! VkImages.
	VkImage images[MAX_SWAPCHAIN_IMAGES];

	//! VkDeviceMemory for each image.
	VkDeviceMemory memories[MAX_SWAPCHAIN_IMAGES];

	//! VkImageViews for sampling.
	VkImageView views[MAX_SWAPCHAIN_IMAGES];

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

static inline struct comp_vk_native_swapchain *
vk_sc(struct xrt_swapchain *xsc)
{
	return (struct comp_vk_native_swapchain *)xsc;
}

/*!
 * Convert xrt format (int64_t) to VkFormat.
 */
static VkFormat
xrt_format_to_vk(int64_t format)
{
	// OpenXR Vulkan apps pass VkFormat values directly
	switch (format) {
	case VK_FORMAT_R8G8B8A8_UNORM:
	case VK_FORMAT_R8G8B8A8_SRGB:
	case VK_FORMAT_B8G8R8A8_UNORM:
	case VK_FORMAT_B8G8R8A8_SRGB:
	case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
	case VK_FORMAT_R16G16B16A16_SFLOAT:
	case VK_FORMAT_R16G16B16A16_UNORM:
	case VK_FORMAT_D16_UNORM:
	case VK_FORMAT_D32_SFLOAT:
	case VK_FORMAT_D24_UNORM_S8_UINT:
	case VK_FORMAT_D32_SFLOAT_S8_UINT:
	case VK_FORMAT_R32_SFLOAT:
		return (VkFormat)format;

	default:
		U_LOG_W("Unknown format %" PRId64 ", using R8G8B8A8_UNORM", format);
		return VK_FORMAT_R8G8B8A8_UNORM;
	}
}

static bool
is_depth_format(VkFormat format)
{
	return format == VK_FORMAT_D16_UNORM ||
	       format == VK_FORMAT_D32_SFLOAT ||
	       format == VK_FORMAT_D24_UNORM_S8_UINT ||
	       format == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

/*
 *
 * xrt_swapchain member functions
 *
 */

static xrt_result_t
vk_swapchain_acquire_image(struct xrt_swapchain *xsc, uint32_t *out_index)
{
	struct comp_vk_native_swapchain *sc = vk_sc(xsc);

	if (sc->acquired_index >= 0) {
		U_LOG_E("Image already acquired");
		return XRT_ERROR_IPC_FAILURE;
	}

	uint32_t index = (sc->last_released_index + 1) % sc->image_count;
	sc->acquired_index = (int32_t)index;
	*out_index = index;

	return XRT_SUCCESS;
}

static xrt_result_t
vk_swapchain_wait_image(struct xrt_swapchain *xsc, int64_t timeout_ns, uint32_t index)
{
	struct comp_vk_native_swapchain *sc = vk_sc(xsc);
	(void)timeout_ns;

	if (sc->acquired_index < 0) {
		U_LOG_E("No image acquired");
		return XRT_ERROR_IPC_FAILURE;
	}

	if ((uint32_t)sc->acquired_index != index) {
		U_LOG_E("Wait index %u doesn't match acquired index %d", index, sc->acquired_index);
		return XRT_ERROR_IPC_FAILURE;
	}

	sc->waited_index = sc->acquired_index;
	sc->acquired_index = -1;

	return XRT_SUCCESS;
}

static xrt_result_t
vk_swapchain_barrier_image(struct xrt_swapchain *xsc, enum xrt_barrier_direction direction, uint32_t index)
{
	(void)xsc;
	(void)direction;
	(void)index;
	return XRT_SUCCESS;
}

static xrt_result_t
vk_swapchain_release_image(struct xrt_swapchain *xsc, uint32_t index)
{
	struct comp_vk_native_swapchain *sc = vk_sc(xsc);

	if (sc->waited_index < 0) {
		U_LOG_E("No image to release");
		return XRT_ERROR_IPC_FAILURE;
	}

	if ((uint32_t)sc->waited_index != index) {
		U_LOG_E("Release index %u doesn't match waited index %d", index, sc->waited_index);
		return XRT_ERROR_IPC_FAILURE;
	}

	sc->last_released_index = index;
	sc->waited_index = -1;

	return XRT_SUCCESS;
}

static void
vk_swapchain_destroy(struct xrt_swapchain *xsc)
{
	struct comp_vk_native_swapchain *sc = vk_sc(xsc);
	struct vk_bundle *vk = sc->vk;

	for (uint32_t i = 0; i < sc->image_count; i++) {
		if (sc->views[i] != VK_NULL_HANDLE) {
			vk->vkDestroyImageView(vk->device, sc->views[i], NULL);
		}
		if (sc->images[i] != VK_NULL_HANDLE) {
			vk->vkDestroyImage(vk->device, sc->images[i], NULL);
		}
		if (sc->memories[i] != VK_NULL_HANDLE) {
			vk->vkFreeMemory(vk->device, sc->memories[i], NULL);
		}
	}

	free(sc);
}

/*
 *
 * Exported functions
 *
 */

xrt_result_t
comp_vk_native_swapchain_create(struct comp_vk_native_compositor *c,
                                const struct xrt_swapchain_create_info *info,
                                struct xrt_swapchain **out_xsc)
{
	struct vk_bundle *vk = comp_vk_native_compositor_get_vk(c);

	uint32_t image_count = 3; // Triple buffering
	if (image_count > MAX_SWAPCHAIN_IMAGES) {
		image_count = MAX_SWAPCHAIN_IMAGES;
	}

	struct comp_vk_native_swapchain *sc = U_TYPED_CALLOC(struct comp_vk_native_swapchain);
	if (sc == NULL) {
		return XRT_ERROR_ALLOCATION;
	}

	sc->vk = vk;
	sc->info = *info;
	sc->image_count = image_count;
	sc->acquired_index = -1;
	sc->waited_index = -1;
	sc->last_released_index = image_count - 1;

	VkFormat vk_format = xrt_format_to_vk(info->format);
	bool depth = is_depth_format(vk_format);

	// Determine usage flags
	VkImageUsageFlags usage = 0;
	if (info->bits & XRT_SWAPCHAIN_USAGE_COLOR) {
		usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	}
	if (info->bits & XRT_SWAPCHAIN_USAGE_DEPTH_STENCIL) {
		usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	}
	if (info->bits & XRT_SWAPCHAIN_USAGE_SAMPLED) {
		usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
	}
	if (info->bits & XRT_SWAPCHAIN_USAGE_TRANSFER_SRC) {
		usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	}
	if (info->bits & XRT_SWAPCHAIN_USAGE_TRANSFER_DST) {
		usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	}
	// Always allow sampling for color textures
	if (!depth) {
		usage |= VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	}

	VkImageCreateInfo image_ci = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .imageType = VK_IMAGE_TYPE_2D,
	    .format = vk_format,
	    .extent = {info->width, info->height, 1},
	    .mipLevels = info->mip_count > 0 ? info->mip_count : 1,
	    .arrayLayers = info->array_size > 0 ? info->array_size : 1,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .tiling = VK_IMAGE_TILING_OPTIMAL,
	    .usage = usage,
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};

	VkImageViewType view_type = (image_ci.arrayLayers > 1) ?
	    VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
	VkImageAspectFlags aspect = depth ?
	    VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;

	for (uint32_t i = 0; i < image_count; i++) {
		VkResult res = vk->vkCreateImage(vk->device, &image_ci, NULL, &sc->images[i]);
		if (res != VK_SUCCESS) {
			U_LOG_E("Failed to create swapchain image %u: %d", i, res);
			vk_swapchain_destroy(&sc->base.base);
			return XRT_ERROR_VULKAN;
		}

		// Allocate memory
		VkMemoryRequirements mem_reqs;
		vk->vkGetImageMemoryRequirements(vk->device, sc->images[i], &mem_reqs);

		// Find suitable memory type
		uint32_t mem_type_index = 0;
		VkPhysicalDeviceMemoryProperties mem_props;
		vk->vkGetPhysicalDeviceMemoryProperties(vk->physical_device, &mem_props);
		for (uint32_t j = 0; j < mem_props.memoryTypeCount; j++) {
			if ((mem_reqs.memoryTypeBits & (1 << j)) &&
			    (mem_props.memoryTypes[j].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
				mem_type_index = j;
				break;
			}
		}

		VkMemoryAllocateInfo alloc_info = {
		    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		    .allocationSize = mem_reqs.size,
		    .memoryTypeIndex = mem_type_index,
		};

		res = vk->vkAllocateMemory(vk->device, &alloc_info, NULL, &sc->memories[i]);
		if (res != VK_SUCCESS) {
			U_LOG_E("Failed to allocate swapchain memory %u: %d", i, res);
			vk_swapchain_destroy(&sc->base.base);
			return XRT_ERROR_VULKAN;
		}

		res = vk->vkBindImageMemory(vk->device, sc->images[i], sc->memories[i], 0);
		if (res != VK_SUCCESS) {
			U_LOG_E("Failed to bind swapchain memory %u: %d", i, res);
			vk_swapchain_destroy(&sc->base.base);
			return XRT_ERROR_VULKAN;
		}

		// Create image view
		VkImageViewCreateInfo view_ci = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		    .image = sc->images[i],
		    .viewType = view_type,
		    .format = vk_format,
		    .subresourceRange = {
		        .aspectMask = aspect,
		        .baseMipLevel = 0,
		        .levelCount = image_ci.mipLevels,
		        .baseArrayLayer = 0,
		        .layerCount = image_ci.arrayLayers,
		    },
		};

		res = vk->vkCreateImageView(vk->device, &view_ci, NULL, &sc->views[i]);
		if (res != VK_SUCCESS) {
			U_LOG_W("Failed to create image view for swapchain %u: %d", i, res);
		}

		// Set up native image handle
		sc->base.images[i].handle = (xrt_graphics_buffer_handle_t)(uintptr_t)sc->images[i];
		sc->base.images[i].size = 0;
		sc->base.images[i].use_dedicated_allocation = false;
		sc->base.images[i].is_dxgi_handle = false;
	}

	// Set up swapchain interface
	sc->base.base.image_count = image_count;
	sc->base.base.wait_image = vk_swapchain_wait_image;
	sc->base.base.acquire_image = vk_swapchain_acquire_image;
	sc->base.base.barrier_image = vk_swapchain_barrier_image;
	sc->base.base.release_image = vk_swapchain_release_image;
	sc->base.base.destroy = vk_swapchain_destroy;
	sc->base.base.reference.count = 1;

	*out_xsc = &sc->base.base;

	U_LOG_I("Created VK native swapchain: %ux%u, %u images, format %d",
	        info->width, info->height, image_count, (int)vk_format);

	return XRT_SUCCESS;
}

uint64_t
comp_vk_native_swapchain_get_image_view(struct xrt_swapchain *xsc, uint32_t index)
{
	struct comp_vk_native_swapchain *sc = vk_sc(xsc);
	if (index >= sc->image_count) {
		return 0;
	}
	return (uint64_t)(uintptr_t)sc->views[index];
}

uint64_t
comp_vk_native_swapchain_get_image(struct xrt_swapchain *xsc, uint32_t index)
{
	struct comp_vk_native_swapchain *sc = vk_sc(xsc);
	if (index >= sc->image_count) {
		return 0;
	}
	return (uint64_t)(uintptr_t)sc->images[index];
}
