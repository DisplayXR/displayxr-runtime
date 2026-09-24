// Copyright 2026, DisplayXR contributors.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Desktop-Linux dma-buf + DRM-format-modifier + sync_file helpers (#1699 R5).
 *
 * Import a dma-buf another process rendered (explicit modifier, explicit plane
 * layouts), allocate an exportable dma-buf image the driver tiles from a
 * modifier list, move binary semaphores in and out of SYNC_FD sync_files, and
 * record the VK_QUEUE_FAMILY_FOREIGN_EXT ownership transfers that make a
 * cross-process, cross-API dma-buf coherent on every driver (GENERAL +
 * VK_QUEUE_FAMILY_IGNORED only happens to work on some Mesa drivers).
 *
 * Every function here is a free function over an already-initialised
 * @ref vk_bundle. None of them adds state to the bundle: @ref vk_bundle crosses
 * the plug-in ABI, so the one entry point these helpers need that the bundle
 * does not carry (vkGetMemoryFdPropertiesKHR) is resolved on demand through
 * vk->vkGetDeviceProcAddr.
 *
 * ## fd ownership, summarised
 *
 * | function                               | fd in                         | fd out                    |
 * |----------------------------------------|-------------------------------|---------------------------|
 * | vk_create_image_from_dmabuf            | always consumed (see below)   | —                         |
 * | vk_create_exportable_dmabuf_image      | —                             | caller owns out_desc->fd  |
 * | vk_semaphore_import_sync_fd            | always consumed (see below)   | —                         |
 * | vk_semaphore_export_sync_fd            | —                             | caller owns *out_fd       |
 *
 * "Always consumed": on success Vulkan owns the fd (the spec transfers
 * ownership on a successful import, and the application must not touch it
 * again); on any failure the helper closes it. Either way the caller never
 * closes an fd it passed in.
 *
 * Built only on desktop Linux (XRT_OS_LINUX_DESKTOP); the header is empty
 * elsewhere.
 *
 * @ingroup aux_vk
 */

#pragma once

#include "xrt/xrt_config_os.h"

#ifdef XRT_OS_LINUX_DESKTOP

#include "vk/vk_helpers.h"
#include "xrt/xrt_weave_dmabuf.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * The DRM fourcc and modifier constants come from the kernel uapi header
 * (linux-libc-dev, present on every supported Ubuntu release); the fallback
 * defines cover a toolchain without it. Values are ABI and never change.
 */
#if defined(__has_include)
#if __has_include(<drm/drm_fourcc.h>)
#include <drm/drm_fourcc.h>
#endif
#endif

#ifndef DRM_FORMAT_ARGB8888
#define VK_DMABUF_FOURCC_CODE(a, b, c, d)                                                                              \
	((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define DRM_FORMAT_ARGB8888 VK_DMABUF_FOURCC_CODE('A', 'R', '2', '4')
#define DRM_FORMAT_XRGB8888 VK_DMABUF_FOURCC_CODE('X', 'R', '2', '4')
#define DRM_FORMAT_ABGR8888 VK_DMABUF_FOURCC_CODE('A', 'B', '2', '4')
#define DRM_FORMAT_XBGR8888 VK_DMABUF_FOURCC_CODE('X', 'B', '2', '4')
#endif
#ifndef DRM_FORMAT_MOD_LINEAR
#define DRM_FORMAT_MOD_LINEAR 0ULL
#endif
#ifndef DRM_FORMAT_MOD_INVALID
#define DRM_FORMAT_MOD_INVALID 0x00ffffffffffffffULL
#endif

#ifdef __cplusplus
extern "C" {
#endif


/*
 *
 * Capability.
 *
 */

/*!
 * Can this device import and export dma-bufs with explicit DRM format modifiers?
 *
 * True when VK_EXT_external_memory_dma_buf and VK_EXT_image_drm_format_modifier
 * are flagged on the bundle, the physical device offers
 * VK_EXT_queue_family_foreign (the barrier helpers below need it; the
 * service's own device enables it whenever it is offered), and the
 * entry points (vkGetMemoryFdKHR, vkGetImageDrmFormatModifierPropertiesEXT, and
 * the locally resolved vkGetMemoryFdPropertiesKHR) are all non-NULL.
 *
 * Every other function here returns VK_ERROR_EXTENSION_NOT_PRESENT (and still
 * honours its fd contract) when this is false.
 *
 * @ingroup aux_vk
 */
bool
vk_dmabuf_supported(struct vk_bundle *vk);

/*!
 * Can this device move binary semaphores through SYNC_FD sync_files, in both
 * directions? (vkImportSemaphoreFdKHR + vkGetSemaphoreFdKHR loaded, and
 * VkExternalSemaphoreProperties reports SYNC_FD importable AND exportable.)
 *
 * Independent of @ref vk_dmabuf_supported: a caller without it falls back to a
 * CPU poll() on the fence fd.
 *
 * @ingroup aux_vk
 */
bool
vk_dmabuf_sync_fd_supported(struct vk_bundle *vk);


/*
 *
 * Formats and modifiers.
 *
 */

/*!
 * DRM fourcc -> VkFormat with the same byte order in memory.
 *
 * DRM fourccs name a little-endian 32-bit word, so the channel order in memory
 * is the name reversed:
 * - DRM_FORMAT_ARGB8888 / XRGB8888 -> VK_FORMAT_B8G8R8A8_UNORM (bytes B,G,R,A)
 * - DRM_FORMAT_ABGR8888 / XBGR8888 -> VK_FORMAT_R8G8B8A8_UNORM (bytes R,G,B,A)
 *
 * The X variants share the A variant's format; @p out_has_alpha is false for
 * them and the caller must sample alpha as 1 (swizzle a = ONE), never read the
 * undefined byte. Returns VK_FORMAT_UNDEFINED for anything else.
 *
 * @param      fourcc        DRM fourcc.
 * @param[out] out_has_alpha Optional.
 *
 * @ingroup aux_vk
 */
VkFormat
vk_dmabuf_fourcc_to_vk_format(uint32_t fourcc, bool *out_has_alpha);

/*!
 * VkFormat -> DRM fourcc, the inverse of @ref vk_dmabuf_fourcc_to_vk_format.
 * Returns 0 for a format with no mapping.
 *
 * @ingroup aux_vk
 */
uint32_t
vk_dmabuf_vk_format_to_fourcc(VkFormat format, bool has_alpha);

/*!
 * One DRM format modifier the device offers for a format.
 *
 * @ingroup aux_vk
 */
struct vk_dmabuf_modifier_info
{
	uint64_t modifier;
	//! Memory planes (drmFormatModifierPlaneCount); an import must pass exactly this many.
	uint32_t plane_count;
	//! drmFormatModifierTilingFeatures.
	VkFormatFeatureFlags features;
	//! A dma-buf of this modifier + usage can be imported.
	bool importable;
	//! An image of this modifier + usage can be exported as a dma-buf.
	bool exportable;
};

/*!
 * Enumerate the modifiers the device offers for @p format that pass
 * vkGetPhysicalDeviceImageFormatProperties2 for a 2D, single-mip, single-layer
 * DRM_FORMAT_MODIFIER_EXT image with @p usage and a DMA_BUF external handle.
 * Modifiers that fail that check are left out (format features alone are not
 * the full answer).
 *
 * @param      vk        Bundle; @ref vk_dmabuf_supported must be true.
 * @param      format    Image format.
 * @param      usage     Usage the image will be created with.
 * @param[out] out_list  Array of at least @p max entries (may be NULL when max == 0).
 * @param      max       Capacity of @p out_list.
 * @param[out] out_count Number of supported modifiers. May exceed @p max, in
 *                       which case only the first @p max were written and
 *                       VK_INCOMPLETE is returned.
 *
 * @ingroup aux_vk
 */
VkResult
vk_dmabuf_query_modifiers(struct vk_bundle *vk,
                          VkFormat format,
                          VkImageUsageFlags usage,
                          struct vk_dmabuf_modifier_info *out_list,
                          uint32_t max,
                          uint32_t *out_count);


/*
 *
 * Images.
 *
 */

/*!
 * Import a dma-buf as a VkImage with an explicit DRM format modifier.
 *
 * Tiling is VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT with
 * VkImageDrmFormatModifierExplicitCreateInfoEXT built from the descriptor's
 * plane offsets/strides; DRM_FORMAT_MOD_LINEAR goes through the same path (no
 * TILING_LINEAR special case). Memory is a dedicated
 * VkImportMemoryFdInfoKHR{DMA_BUF} allocation of a type from
 * vkGetMemoryFdPropertiesKHR.
 *
 * The descriptor is untrusted (it arrives from a client over IPC), so before
 * anything touches the driver the helper checks: the fourcc maps, the modifier
 * is not DRM_FORMAT_MOD_INVALID and is offered for the format with this usage
 * and DMA_BUF import, @p desc->plane_count equals the modifier's plane count,
 * the size is within the device limits, and (when the kernel reports it) the
 * dma-buf is at least as large as the image needs.
 *
 * The image's contents are the producer's and it is owned by
 * VK_QUEUE_FAMILY_FOREIGN_EXT: acquire it with
 * @ref vk_dmabuf_cmd_acquire_foreign (GENERAL -> your layout) before every
 * use and release it with @ref vk_dmabuf_cmd_release_foreign after.
 *
 * **fd ownership:** @p desc->fd is always consumed. On VK_SUCCESS it belongs
 * to the returned VkDeviceMemory (freeing the memory releases it); never close
 * it. On any failure the helper has closed it. The descriptor itself is not
 * modified.
 *
 * @param      vk         Bundle.
 * @param      desc       Descriptor; desc->fd is consumed.
 * @param      usage      Image usage (e.g. SAMPLED, TRANSFER_SRC).
 * @param[out] out_image  The image.
 * @param[out] out_memory Its dedicated, bound memory.
 * @param[out] out_format Optional: the VkFormat used (from the fourcc).
 *
 * @ingroup aux_vk
 */
VkResult
vk_create_image_from_dmabuf(struct vk_bundle *vk,
                            const struct xrt_weave_dmabuf_desc *desc,
                            VkImageUsageFlags usage,
                            VkImage *out_image,
                            VkDeviceMemory *out_memory,
                            VkFormat *out_format);

/*!
 * Allocate a VkImage whose memory can be exported as a dma-buf, letting the
 * driver pick the modifier from a list, and describe it.
 *
 * The image uses VkImageDrmFormatModifierListCreateInfoEXT built from
 * @p modifiers after dropping DRM_FORMAT_MOD_INVALID and every entry the
 * device cannot export with this format + usage (listing an incompatible
 * modifier is invalid usage). @p modifiers == NULL means {LINEAR}, the only
 * layout every importer (EGL, another GPU, the CPU) can read. Memory is a
 * dedicated VkExportMemoryAllocateInfo{DMA_BUF} allocation.
 *
 * On success @p out_desc holds the fourcc (from @p format, with alpha), the
 * modifier the driver chose (vkGetImageDrmFormatModifierPropertiesEXT), the
 * per-memory-plane offsets/strides (vkGetImageSubresourceLayout on
 * MEMORY_PLANE_i), the allocation size, and ONE fd from vkGetMemoryFdKHR.
 *
 * The image is left in VK_IMAGE_LAYOUT_UNDEFINED and owned by the device's
 * queue family (not foreign yet): transition it with an ordinary barrier
 * before the first write, and hand it over with
 * @ref vk_dmabuf_cmd_release_foreign.
 *
 * **fd ownership:** @p out_desc->fd is a new fd the CALLER owns and must close
 * (it is independent of the VkDeviceMemory; closing it does not free the
 * memory, and freeing the memory does not invalidate it: the dma-buf lives
 * while either holds it). Call vkGetMemoryFdKHR again for more. On failure
 * @p out_desc->fd is -1 and nothing is allocated.
 *
 * @ingroup aux_vk
 */
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
                                  VkDeviceMemory *out_memory);


/*
 *
 * sync_file (SYNC_FD) semaphores.
 *
 */

/*!
 * Import a sync_file into a binary semaphore, temporarily
 * (VK_SEMAPHORE_IMPORT_TEMPORARY_BIT, the only permanence SYNC_FD allows):
 * the next wait on @p semaphore waits the fence, then the semaphore reverts to
 * its own payload.
 *
 * @p fd == -1 is legal for SYNC_FD and means "already signalled"; nothing is
 * closed in that case.
 *
 * @p semaphore must be a binary semaphore with no pending signal or wait.
 *
 * **fd ownership:** always consumed. On VK_SUCCESS Vulkan owns it; on any
 * failure the helper has closed it.
 *
 * @ingroup aux_vk
 */
VkResult
vk_semaphore_import_sync_fd(struct vk_bundle *vk, VkSemaphore semaphore, int fd);

/*!
 * Create a binary semaphore that can be exported as a SYNC_FD
 * (VkExportSemaphoreCreateInfo{SYNC_FD}).
 *
 * Returns VK_ERROR_FEATURE_NOT_PRESENT when @ref vk_dmabuf_sync_fd_supported
 * is false.
 *
 * @ingroup aux_vk
 */
VkResult
vk_create_exportable_sync_fd_semaphore(struct vk_bundle *vk, VkSemaphore *out_semaphore);

/*!
 * Export a semaphore created by @ref vk_create_exportable_sync_fd_semaphore as
 * a sync_file.
 *
 * The semaphore must be signalled, or have a signal operation submitted (the
 * usual use: export right after the vkQueueSubmit that signals it). SYNC_FD
 * export has copy transference and unsignals the semaphore, so it is reusable
 * for the next frame's submit without re-creation.
 *
 * **fd ownership:** on VK_SUCCESS the caller owns @p *out_fd and must close
 * it. It may be -1, which the spec allows when the payload has already
 * signalled; -1 is a valid "already signalled" sync_file everywhere a SYNC_FD
 * is accepted (including @ref vk_semaphore_import_sync_fd). On failure
 * @p *out_fd is -1.
 *
 * @ingroup aux_vk
 */
VkResult
vk_semaphore_export_sync_fd(struct vk_bundle *vk, VkSemaphore semaphore, int *out_fd);


/*
 *
 * Queue-family ownership.
 *
 */

/*!
 * Record the ACQUIRE half of a VK_QUEUE_FAMILY_FOREIGN_EXT ->
 * vk->main_queue->family_index ownership transfer for a single-mip,
 * single-layer colour image, transitioning GENERAL -> @p dst_layout (GENERAL,
 * because that is the layout a foreign producer leaves the bits in; the
 * content is preserved).
 *
 * Put it first in the command buffer that consumes the image, in a submit
 * that waits the producer's acquire semaphore.
 *
 * @ingroup aux_vk
 */
void
vk_dmabuf_cmd_acquire_foreign(struct vk_bundle *vk,
                              VkCommandBuffer cmd,
                              VkImage image,
                              VkImageLayout dst_layout,
                              VkPipelineStageFlags dst_stage,
                              VkAccessFlags dst_access);

/*!
 * Record the RELEASE half: vk->main_queue->family_index ->
 * VK_QUEUE_FAMILY_FOREIGN_EXT, transitioning @p src_layout -> GENERAL, after
 * the last access (@p src_stage / @p src_access) in this command buffer.
 * Signal the release semaphore from the same submit and hand its sync_file to
 * the other side.
 *
 * @ingroup aux_vk
 */
void
vk_dmabuf_cmd_release_foreign(struct vk_bundle *vk,
                              VkCommandBuffer cmd,
                              VkImage image,
                              VkImageLayout src_layout,
                              VkPipelineStageFlags src_stage,
                              VkAccessFlags src_access);


#ifdef __cplusplus
}
#endif

#endif // XRT_OS_LINUX_DESKTOP
