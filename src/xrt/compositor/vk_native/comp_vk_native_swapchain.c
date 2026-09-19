// Copyright 2025, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Vulkan native swapchain implementation.
 * @author David Fattal
 * @ingroup comp_vk_native
 */

#include "comp_vk_native_swapchain.h"
#include "comp_vk_native_compositor.h"

#include "util/comp_swapchain_ring.h"

#include "xrt/xrt_compositor.h"
#include "xrt/xrt_vulkan_includes.h"
#include "vk/vk_helpers.h"

#include "util/u_logging.h"
#include "util/u_misc.h"

#include <string.h>

/*!
 * Maximum number of images in a swapchain.
 */
#define MAX_SWAPCHAIN_IMAGES COMP_SWAPCHAIN_MAX_IMAGES

/*!
 * Vulkan swapchain structure.
 */
struct comp_vk_native_swapchain
{
	//! Base type - must be first!
	//! Uses xrt_swapchain_vk (not xrt_swapchain_native) because vk_enumerate_images
	//! casts to xrt_swapchain_vk and reads base.images[] as VkImage pointers.
	//! Since there's no client compositor wrapper in the VK native path, the
	//! swapchain struct must match what the OpenXR state tracker expects.
	struct xrt_swapchain_vk base;

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

	//! Per-image acquire/wait/release state. See util/comp_swapchain_ring.h.
	struct comp_swapchain_ring ring;
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

	// OpenXR permits up to image_count concurrently acquired images, and the
	// Vulkan state-tracker path waits inside xrAcquireSwapchainImage, so the
	// ring must be able to hand out every image before any is released (#1504).
	uint32_t index = 0;
	xrt_result_t xret = comp_swapchain_ring_acquire(&sc->ring, &index);
	if (xret != XRT_SUCCESS) {
		U_LOG_E("No free swapchain image: all %u are already acquired", sc->image_count);
		return xret;
	}

	*out_index = index;

	return XRT_SUCCESS;
}

static xrt_result_t
vk_swapchain_wait_image(struct xrt_swapchain *xsc, int64_t timeout_ns, uint32_t index)
{
	struct comp_vk_native_swapchain *sc = vk_sc(xsc);
	(void)timeout_ns;

	// The app owns these images; there is no runtime-side GPU work to wait on
	// (the compositor reads them at layer_commit, after release). The state
	// tracker enforces the FIFO acquire->wait->release order, so this only has
	// to move the named image on and reject an index that is not acquired.
	xrt_result_t xret = comp_swapchain_ring_wait(&sc->ring, index);
	if (xret != XRT_SUCCESS) {
		U_LOG_E("Wait on non-acquired swapchain image index %u (image_count=%u)", index, sc->image_count);
		return xret;
	}

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

	xrt_result_t xret = comp_swapchain_ring_release(&sc->ring, index);
	if (xret != XRT_SUCCESS) {
		U_LOG_E("Release of non-waited swapchain image index %u (image_count=%u)", index, sc->image_count);
		return xret;
	}

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

	// One image for a static swapchain, triple buffering otherwise (#1504).
	// Same helper the compositor's get_swapchain_create_properties uses, so the
	// advertised count and the allocated one cannot drift.
	uint32_t image_count = comp_swapchain_image_count(info->create, 3);

	struct comp_vk_native_swapchain *sc = U_TYPED_CALLOC(struct comp_vk_native_swapchain);
	if (sc == NULL) {
		return XRT_ERROR_ALLOCATION;
	}

	sc->vk = vk;
	sc->info = *info;
	sc->image_count = image_count;
	comp_swapchain_ring_init(&sc->ring, image_count);

	VkFormat vk_format = xrt_format_to_vk(info->format);
	bool depth = is_depth_format(vk_format);

	// sRGB passthrough (mirrors the GL/D3D11/D3D12 fixes): the compose
	// vkCmdBlitImage reads the source in the IMAGE's format, so an sRGB image
	// would auto-decode sRGB->linear with no re-encode into the UNORM atlas,
	// and the DP (which wants display-referred bytes) would get ~2.2x-too-dark
	// content. There is no view to retag for a blit, so create the color image
	// in the UNORM sibling — the blit then passes the app's stored bytes through
	// unchanged. Expose the requested sRGB format via MUTABLE_FORMAT + a format
	// list so the app can still create an sRGB view to render with encode.
	VkFormat image_format = vk_format;
	VkFormat srgb_view_format = VK_FORMAT_UNDEFINED;
	if (!depth) {
		switch (vk_format) {
		case VK_FORMAT_R8G8B8A8_SRGB:
			image_format = VK_FORMAT_R8G8B8A8_UNORM;
			srgb_view_format = vk_format;
			break;
		case VK_FORMAT_B8G8R8A8_SRGB:
			image_format = VK_FORMAT_B8G8R8A8_UNORM;
			srgb_view_format = vk_format;
			break;
		default: break;
		}
	}
	const bool mutable_srgb = (srgb_view_format != VK_FORMAT_UNDEFINED);

	/*
	 * Usage flags. Go through vk_csci_get_image_usage_flags() rather than
	 * hand-rolling the map here (#1558): the shared helper covers every
	 * xrt_swapchain_usage_bits value — including UNORDERED_ACCESS ->
	 * VK_IMAGE_USAGE_STORAGE_BIT and INPUT_ATTACHMENT, which the hand-rolled
	 * version silently dropped — and it validates each bit against the
	 * format's optimalTilingFeatures instead of trusting the app.
	 *
	 * Dropping a requested usage bit is undefined behaviour, not a harmless
	 * optimisation: a driver that allocates strictly per-usage (Mesa lavapipe
	 * creates a storage image handle only when VK_IMAGE_USAGE_STORAGE_BIT was
	 * set at vkCreateImage) hands the app a NULL descriptor and the shader
	 * faults. Real GPU drivers tolerate it, which is why this hid for so long.
	 *
	 * Query against image_format, not vk_format: for the sRGB substitution
	 * above the app's bits have to be satisfiable by the image we actually
	 * create, and sRGB formats never advertise the storage-image feature.
	 *
	 * MUTABLE_FORMAT is deliberately not the helper's job (it is an image
	 * create flag, not a usage flag) and is handled by mutable_srgb below.
	 */
	const enum xrt_swapchain_usage_bits mappable_bits = info->bits & ~XRT_SWAPCHAIN_USAGE_MUTABLE_FORMAT;
	VkImageUsageFlags usage = 0;
	if (mappable_bits != 0) {
		usage = vk_csci_get_image_usage_flags(vk, image_format, mappable_bits);
		if (usage == 0) {
			// One-shot: a mis-matched app would otherwise log per swapchain.
			static bool reported = false;
			if (!reported) {
				reported = true;
				U_LOG_W(
				    "#1558: xrCreateSwapchain: %s does not support every requested usage bit "
				    "(0x%08x) — see the preceding vk_csci_get_image_usage_flags error for the "
				    "offending bit. Failing with XR_ERROR_FEATURE_UNSUPPORTED rather than handing "
				    "back an image that ignores the request.",
				    vk_format_string(image_format), (unsigned)mappable_bits);
			}
			vk_swapchain_destroy(&sc->base.base);
			return XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED;
		}
	}

	/*
	 * Colour swapchains additionally need SAMPLED + TRANSFER_SRC for the
	 * compositor's own compose/blit, whether or not the app asked. These are
	 * runtime-internal, so a format that cannot do them must NOT fail the
	 * app's swapchain — but they still get feature-checked through the same
	 * helper rather than assumed. (Every colour format the runtime advertises
	 * has both as mandatory optimal-tiling features, so in practice this never
	 * degrades; the check is there so an added format cannot regress it.)
	 */
	if (!depth) {
		static const enum xrt_swapchain_usage_bits extra_bits[] = {
		    XRT_SWAPCHAIN_USAGE_SAMPLED,
		    XRT_SWAPCHAIN_USAGE_TRANSFER_SRC,
		};
		for (size_t i = 0; i < ARRAY_SIZE(extra_bits); i++) {
			usage |= vk_csci_get_image_usage_flags(vk, image_format, extra_bits[i]);
		}
	}

	VkFormat view_format_list[2] = {image_format, srgb_view_format};
	VkImageFormatListCreateInfo format_list_ci = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
	    .viewFormatCount = 2,
	    .pViewFormats = view_format_list,
	};

	VkImageCreateInfo image_ci = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .pNext = mutable_srgb ? &format_list_ci : NULL,
	    .flags = mutable_srgb ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0,
	    .imageType = VK_IMAGE_TYPE_2D,
	    .format = image_format,
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

		// Create image view (UNORM base for sRGB swapchains — passthrough, no
		// sample-time decode; see the image-format note above).
		VkImageViewCreateInfo view_ci = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		    .image = sc->images[i],
		    .viewType = view_type,
		    .format = image_format,
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

		// Populate xrt_swapchain_vk.images[] so vk_enumerate_images can return them
		sc->base.images[i] = sc->images[i];
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

void
comp_vk_native_swapchain_get_dimensions(struct xrt_swapchain *xsc, uint32_t *out_w, uint32_t *out_h)
{
	struct comp_vk_native_swapchain *sc = vk_sc(xsc);
	*out_w = sc->info.width;
	*out_h = sc->info.height;
}

uint32_t
comp_vk_native_swapchain_get_array_size(struct xrt_swapchain *xsc)
{
	struct comp_vk_native_swapchain *sc = vk_sc(xsc);
	return sc->info.array_size;
}
