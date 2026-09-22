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

#include <stdlib.h>
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

	//! #1559: the images really are the app-requested `*_SRGB` format, so the
	//! compose blit must not read them directly (it would sRGB-decode).
	bool true_srgb;

	//! The format the runtime READS these images in — the UNORM sibling for an
	//! sRGB swapchain (raw passthrough, Model A), else the image format itself.
	VkFormat raw_format;

	//! @name #1559 compose scratch
	//! Lazily created UNORM staging image for the compose blit's source rect.
	//! Only allocated for a `true_srgb` swapchain, at the swapchain's full
	//! extent on first use, never resized (see vk_swapchain_ensure_scratch),
	//! and freed with the swapchain.
	//! @{
	VkImage scratch_image;
	VkDeviceMemory scratch_memory;
	uint32_t scratch_w, scratch_h;
	bool scratch_failed;
	//! @}
};

/*!
 * #1559 kill switch. Default ON: `xrEnumerateSwapchainImages` hands back
 * `VkImage`s in the format the app asked for. `DXR_VK_SWAPCHAIN_TRUE_FORMAT=0`
 * restores the pre-#1559 substitution (UNORM image behind an sRGB-capable
 * format list) for a one-binary A/B.
 */
static bool
vk_swapchain_true_format_enabled(void)
{
	static bool cached = false;
	static bool value = true;
	if (!cached) {
		const char *env = getenv("DXR_VK_SWAPCHAIN_TRUE_FORMAT");
		if (env != NULL && (env[0] == '0' || env[0] == 'n' || env[0] == 'N' || env[0] == 'f' || env[0] == 'F' ||
		                    (env[0] == 'o' && (env[1] == 'f' || env[1] == 'F')))) {
			value = false;
		}
		cached = true;
	}
	return value;
}

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

	// #1559 compose scratch.
	if (sc->scratch_image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, sc->scratch_image, NULL);
	}
	if (sc->scratch_memory != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, sc->scratch_memory, NULL);
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

	// Filled in below once the format decision is made (#1559); the destroy
	// path runs on every early return, so keep them defined from the start.
	sc->true_srgb = false;
	sc->raw_format = VK_FORMAT_UNDEFINED;

	VkFormat vk_format = xrt_format_to_vk(info->format);
	bool depth = is_depth_format(vk_format);

	/*
	 * sRGB passthrough, and who gets to see which format (#1559).
	 *
	 * The compose vkCmdBlitImage reads its source in the IMAGE's format, so
	 * blitting an sRGB image into the UNORM atlas auto-decodes sRGB->linear
	 * with no re-encode and the DP (which wants display-referred bytes, Model
	 * A / ADR-021) gets ~2.2x-too-dark content. The runtime therefore has to
	 * READ these images as UNORM.
	 *
	 * It does NOT follow that the app must be handed a UNORM image, and
	 * handing it one is a deviation from XR_KHR_vulkan_enable[2] (the returned
	 * VkImage is specified to have XrSwapchainCreateInfo::format) that the CTS
	 * conformance layer flags on every sRGB swapchain — 285 warnings a run.
	 * So: create the image in the format the app asked for, keep
	 * MUTABLE_FORMAT + a format list carrying both siblings, and give every
	 * runtime-internal reader the UNORM sibling instead:
	 *   - sampling paths use sc->views[], created below in `raw_format`;
	 *   - the one format-sensitive reader, the compose blit, goes through
	 *     comp_vk_native_swapchain_stage_unorm_copy() — a raw vkCmdCopyImage
	 *     (size-compatible formats, no conversion anywhere) into an UNORM
	 *     scratch, which is then the blit source;
	 *   - zero-copy is disabled for these swapchains, so no DP is ever handed
	 *     an sRGB VkImage (same shape as the #918 guard: a placement fact
	 *     applied to u_tiling_can_zero_copy()'s RESULT, not a second gate).
	 */
	VkFormat unorm_sibling = VK_FORMAT_UNDEFINED;
	if (!depth) {
		switch (vk_format) {
		case VK_FORMAT_R8G8B8A8_SRGB: unorm_sibling = VK_FORMAT_R8G8B8A8_UNORM; break;
		case VK_FORMAT_B8G8R8A8_SRGB: unorm_sibling = VK_FORMAT_B8G8R8A8_UNORM; break;
		default: break;
		}
	}
	const bool srgb_requested = (unorm_sibling != VK_FORMAT_UNDEFINED);
	bool true_srgb = srgb_requested && vk_swapchain_true_format_enabled();

	// What the app gets, and what the runtime reads.
	VkFormat image_format = true_srgb ? vk_format : (srgb_requested ? unorm_sibling : vk_format);
	VkFormat raw_format = srgb_requested ? unorm_sibling : vk_format;
	const bool mutable_srgb = srgb_requested;

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
	 * Query against image_format, not vk_format: the app's bits have to be
	 * satisfiable by the image we actually create.
	 *
	 * #1559 corollary: sRGB formats never advertise the storage-image feature,
	 * so an app asking for UNORDERED_ACCESS on an sRGB swapchain can only be
	 * served by the UNORM substitution. Rather than regress that app from
	 * "works" to XR_ERROR_FEATURE_UNSUPPORTED for the sake of a truthful
	 * format, fall back to the substitution for that one swapchain.
	 *
	 * MUTABLE_FORMAT is deliberately not the helper's job (it is an image
	 * create flag, not a usage flag) and is handled by mutable_srgb below.
	 */
	const enum xrt_swapchain_usage_bits mappable_bits = info->bits & ~XRT_SWAPCHAIN_USAGE_MUTABLE_FORMAT;
	VkImageUsageFlags usage = 0;
	if (mappable_bits != 0) {
		usage = vk_csci_get_image_usage_flags(vk, image_format, mappable_bits);
		if (usage == 0 && true_srgb) {
			VkImageUsageFlags as_unorm = vk_csci_get_image_usage_flags(vk, raw_format, mappable_bits);
			if (as_unorm != 0) {
				static bool usage_fallback_reported = false;
				if (!usage_fallback_reported) {
					usage_fallback_reported = true;
					U_LOG_W(
					    "#1559: %s cannot carry every requested usage bit (0x%08x) — keeping "
					    "the UNORM substitution for this swapchain (the returned VkImage will "
					    "not match the requested format).",
					    vk_format_string(image_format), (unsigned)mappable_bits);
				}
				true_srgb = false;
				image_format = raw_format;
				usage = as_unorm;
			}
		}
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

	// The usage fallback above can still turn true_srgb off, so latch after it.
	sc->true_srgb = true_srgb;
	sc->raw_format = raw_format;

	// Both siblings, whichever way round the image itself was created — the app
	// may make a view in the format it asked for, the runtime makes UNORM ones.
	VkFormat view_format_list[2] = {vk_format, unorm_sibling};
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
		// sample-time decode; see the image-format note above). Legal on a
		// truthfully-sRGB image because of MUTABLE_FORMAT + the format list.
		VkImageViewCreateInfo view_ci = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		    .image = sc->images[i],
		    .viewType = view_type,
		    .format = raw_format,
		    .subresourceRange =
		        {
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

	// One-off (never per frame, never per swapchain): say once per process that
	// the truthful-format path is live and what it costs.
	if (sc->true_srgb) {
		static bool true_srgb_reported = false;
		if (!true_srgb_reported) {
			true_srgb_reported = true;
			U_LOG_W(
			    "#1559: sRGB swapchain: truthful format, compose via UNORM scratch copy; "
			    "zero-copy disabled (DXR_VK_SWAPCHAIN_TRUE_FORMAT=0 restores the substitution)");
		}
	}

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

bool
comp_vk_native_swapchain_is_true_srgb(struct xrt_swapchain *xsc)
{
	struct comp_vk_native_swapchain *sc = vk_sc(xsc);
	return sc->true_srgb;
}

/*!
 * Create the #1559 compose scratch on first use, at the swapchain's FULL
 * extent, and never resize it. Returns false once it has failed, so the caller
 * degrades to a direct blit instead of retrying every frame.
 *
 * Never grow it. The scratch is staged into once per VIEW, and every view of a
 * frame is recorded into the SAME command buffer before one submit. It used to
 * be sized to the rect in hand and grown on demand, so on a tiled atlas view 0
 * (the left tile) created it tile-wide and view 1 (the next tile over) then
 * destroyed it to make a wider one — while view 0's already-recorded copy and
 * blit still referenced it. Destroying an image a command buffer references
 * invalidates that command buffer; submitting it anyway was VK_ERROR_DEVICE_LOST
 * on the first two-tile frame and every frame after (a transparent window with
 * no content, for every app on the blit compose path with an sRGB swapchain).
 * The vkDeviceWaitIdle that guarded the destroy could not help: nothing had
 * been submitted yet. A swapchain's extent is fixed for its lifetime, so the
 * full-size scratch is the only size it ever needs.
 */
static bool
vk_swapchain_ensure_scratch(struct comp_vk_native_swapchain *sc)
{
	struct vk_bundle *vk = sc->vk;

	if (sc->scratch_failed) {
		return false;
	}
	if (sc->scratch_image != VK_NULL_HANDLE) {
		return true;
	}

	uint32_t w = sc->info.width;
	uint32_t h = sc->info.height;

	VkImageCreateInfo image_ci = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .imageType = VK_IMAGE_TYPE_2D,
	    .format = sc->raw_format,
	    .extent = {w, h, 1},
	    .mipLevels = 1,
	    .arrayLayers = 1,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .tiling = VK_IMAGE_TILING_OPTIMAL,
	    .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};

	VkResult res = vk->vkCreateImage(vk->device, &image_ci, NULL, &sc->scratch_image);
	if (res != VK_SUCCESS) {
		sc->scratch_image = VK_NULL_HANDLE;
		goto failed;
	}

	VkMemoryRequirements mem_reqs;
	vk->vkGetImageMemoryRequirements(vk->device, sc->scratch_image, &mem_reqs);

	uint32_t mem_type_index = 0;
	VkPhysicalDeviceMemoryProperties mem_props;
	vk->vkGetPhysicalDeviceMemoryProperties(vk->physical_device, &mem_props);
	for (uint32_t j = 0; j < mem_props.memoryTypeCount; j++) {
		if ((mem_reqs.memoryTypeBits & (1u << j)) &&
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
	res = vk->vkAllocateMemory(vk->device, &alloc_info, NULL, &sc->scratch_memory);
	if (res != VK_SUCCESS) {
		sc->scratch_memory = VK_NULL_HANDLE;
		goto failed;
	}

	res = vk->vkBindImageMemory(vk->device, sc->scratch_image, sc->scratch_memory, 0);
	if (res != VK_SUCCESS) {
		goto failed;
	}

	sc->scratch_w = w;
	sc->scratch_h = h;
	return true;

failed:
	if (sc->scratch_image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, sc->scratch_image, NULL);
		sc->scratch_image = VK_NULL_HANDLE;
	}
	if (sc->scratch_memory != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, sc->scratch_memory, NULL);
		sc->scratch_memory = VK_NULL_HANDLE;
	}
	sc->scratch_w = 0;
	sc->scratch_h = 0;
	sc->scratch_failed = true;
	U_LOG_W(
	    "#1559: could not create the %ux%u UNORM compose scratch (%d) — blitting the sRGB image "
	    "directly, which sRGB-decodes it. Set DXR_VK_SWAPCHAIN_TRUE_FORMAT=0 to go back to the "
	    "UNORM substitution.",
	    w, h, res);
	return false;
}

uint64_t
comp_vk_native_swapchain_stage_unorm_copy(struct xrt_swapchain *xsc,
                                          void *cmd_ptr,
                                          uint32_t index,
                                          int32_t src_x,
                                          int32_t src_y,
                                          uint32_t src_w,
                                          uint32_t src_h,
                                          uint32_t array_layer)
{
	struct comp_vk_native_swapchain *sc = vk_sc(xsc);
	struct vk_bundle *vk = sc->vk;
	VkCommandBuffer cmd = (VkCommandBuffer)cmd_ptr;

	// Not a truthful-sRGB swapchain: the image already IS the runtime's raw
	// format, so the caller blits from it directly (zero added work).
	if (!sc->true_srgb || cmd == VK_NULL_HANDLE || index >= sc->image_count) {
		return 0;
	}
	if (sc->images[index] == VK_NULL_HANDLE || src_w == 0 || src_h == 0) {
		return 0;
	}

	// Clamp the source rect into the image; vkCmdCopyImage has no filtering
	// and no clamping, an out-of-bounds region is invalid usage.
	if (src_x < 0) {
		src_x = 0;
	}
	if (src_y < 0) {
		src_y = 0;
	}
	if ((uint32_t)src_x >= sc->info.width || (uint32_t)src_y >= sc->info.height) {
		return 0;
	}
	if ((uint32_t)src_x + src_w > sc->info.width) {
		src_w = sc->info.width - (uint32_t)src_x;
	}
	if ((uint32_t)src_y + src_h > sc->info.height) {
		src_h = sc->info.height - (uint32_t)src_y;
	}

	// The copy lands at the SAME offsets as in the app image, so the caller's
	// srcOffsets need no adjusting — only the array layer collapses to 0.
	if (!vk_swapchain_ensure_scratch(sc)) {
		return 0;
	}

	VkImageMemoryBarrier to_dst = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = 0,
	    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .image = sc->scratch_image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
	                         NULL, 1, &to_dst);

	// Raw copy: R8G8B8A8_SRGB -> R8G8B8A8_UNORM (and the BGRA pair) are
	// size-compatible, and vkCmdCopyImage never converts — the app's stored
	// bytes arrive in the scratch untouched, which is exactly what Model A's
	// passthrough needs. The caller's scaling blit then reads UNORM, as before.
	VkImageCopy region = {
	    .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, array_layer, 1},
	    .srcOffset = {src_x, src_y, 0},
	    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	    .dstOffset = {src_x, src_y, 0},
	    .extent = {src_w, src_h, 1},
	};
	vk->vkCmdCopyImage(cmd, sc->images[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sc->scratch_image,
	                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

	VkImageMemoryBarrier to_src = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .image = sc->scratch_image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
	                         NULL, 1, &to_src);

	return (uint64_t)(uintptr_t)sc->scratch_image;
}
