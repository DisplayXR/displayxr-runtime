// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The conversion core's Vulkan GPU glue (ADR-053 P5): the POSIX twin
 *         of d3d11_service/d3d11_conv_gpu.{h,cpp}, on a borrowed vk_bundle.
 *
 * What it provides to the conversion core (conv_pipeline.h / conv_stream.h):
 *
 *  - conv_image_alloc_ops: runtime image allocation as VkImages with
 *    exportable memory where the device can export it, plus the #2030 CPU ->
 *    GPU mirror upload;
 *  - conv_viewsyn_ops: the S5 reference view synthesis (vk_conv_viewsyn), on
 *    GPU images it allocated AND on CPU-memory images (a CPU provider's depth,
 *    the CPU input slot): those are uploaded into per-stream scratch images
 *    first, and a CPU target is read back after the synthesis;
 *  - conv_depth_readback_ops: S6 GPU depth (R32F / R16F) -> CPU samples, with
 *    the D3D11 glue's cadence (one copy in flight per stream, issued at most
 *    every U_LIFT_AUTOCONV_SAMPLE_PERIOD_NS, never waited on).
 *
 * ## Memory kinds (what is real where)
 *
 * | Kind                      | Where                                      | How                                   |
 * |---------------------------|--------------------------------------------|---------------------------------------|
 * | XRT_CONV_MEM_CPU          | everywhere (the v1 lane)                   | the service's CPU allocator; this glue
 * declines and the trampoline serves it | | XRT_CONV_MEM_VK_OPAQUE_FD | Linux (VK_KHR_external_memory_fd)          |
 * dedicated VkExportMemoryAllocateInfo; vkGetMemoryFdKHR | | XRT_CONV_MEM_DMABUF       | Linux with
 * VK_EXT_external_memory_dma_buf + VK_EXT_image_drm_format_modifier | vk_create_exportable_dmabuf_image, single-plane
 * modifiers only, 1 layer | | XRT_CONV_MEM_IOSURFACE    | macOS (VK_EXT_metal_objects + VK_EXT_external_memory_metal) |
 * VkExportMetalObjectCreateInfoEXT(IOSURFACE); 1 layer |
 *
 * MoltenVK exposes no VK_KHR_external_memory_fd and no external semaphore fd,
 * so OPAQUE_FD and the fd sync kinds are Linux only; on macOS IOSurface is the
 * one GPU transport.
 *
 * ## Who gets GPU memory (the allocation rule)
 *
 *  - RENDER_TARGET (the composed views) and MIRROR images: the runtime is
 *    their only consumer, so they are always GPU images: exportable when the
 *    platform can export the format / layer count, else device-private
 *    (`handle` = VK_CONV_GPU_NO_HANDLE; conv_stream never registers either
 *    usage with a provider).
 *  - INPUT and PROVIDER_OUT images: a provider imports them. They are GPU
 *    images only when the bound capability's import_memory_kinds names a kind
 *    in vk_conv_gpu_set_provider_kinds()'s mask; that mask is 0 by default,
 *    because no POSIX conversion-host lane exists yet (conv_host_proxy refuses
 *    a GPU image for a satellite without one), so every provider is served on
 *    the CPU lane. A composed binding reports its depth stage's import
 *    kinds (conv_stream's bound_import_kinds), so the same rule applies.
 *
 * ## Sync
 *
 * Every image this glue hands out carries XRT_CONV_SYNC_NONE: each glue call
 * submits and waits for its own GPU work before returning (the discovery
 * spec's "SYNC_NONE meaning the producer finished before hand-over"). An
 * image the glue READS whose sync is XRT_CONV_SYNC_VK_SEMAPHORE_FD (timeline,
 * opaque fd, imported once per import_key) or XRT_CONV_SYNC_SYNC_FD (binary
 * sync_file, temporary import) is waited for on the GPU in the same submit;
 * those kinds are offered only when the device can import them (Linux).
 *
 * Threads: producers, the core's dispatchers and the service's attach call
 * in; one internal work mutex serialises recording + submit + wait on the
 * bundle's main queue (the queue mutex is taken only around vkQueueSubmit).
 * Never takes a compositor lock. Never logs per frame.
 *
 * @ingroup comp_vk_conv
 */

#pragma once

#include "xrt/xrt_conversion_plugin.h"

#include "conversion/conv_pipeline.h"
#include "conversion/conv_stream.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct vk_bundle;
struct vk_conv_gpu;

//! xrt_conv_image::handle of a device-private (not shareable) GPU image.
#define VK_CONV_GPU_NO_HANDLE UINT64_MAX

//! What this device can do (filled once at create).
struct vk_conv_gpu_caps
{
	//! XRT_CONV_MEM_* the glue can allocate as exportable images (CPU always set).
	uint32_t memory_kinds;
	//! 1u << XRT_CONV_SYNC_* honoured on images the glue reads (NONE always set).
	uint32_t sync_kinds;
	//! Timeline semaphores are enabled on the bundle.
	bool timeline_semaphore;
};

/*!
 * Create the glue on @p vk (borrowed; must outlive the glue). The view
 * synthesis stage is created lazily on the first S5 call.
 */
bool
vk_conv_gpu_create(struct vk_bundle *vk, struct vk_conv_gpu **out);

/*!
 * Close the glue (waits for in-flight calls), then free every image it still
 * owns and every Vulkan object. Call after the conversion core let go of the
 * ops (ipc_server_conv_install_gpu(NULL)).
 */
void
vk_conv_gpu_destroy(struct vk_conv_gpu **g_ptr);

void
vk_conv_gpu_get_caps(const struct vk_conv_gpu *g, struct vk_conv_gpu_caps *out);

/*!
 * The XRT_CONV_MEM_* GPU kinds a PROVIDER may be handed for its INPUT /
 * PROVIDER_OUT images (masked with the device's caps). 0 (the default) = the
 * CPU lane for every provider. Raise it only for providers that import in this
 * process or through a host lane that can carry the kind.
 */
void
vk_conv_gpu_set_provider_kinds(struct vk_conv_gpu *g, uint32_t memory_kinds);

//! Runtime image allocation (conv_image_alloc_ops: alloc / free / upload).
void
vk_conv_gpu_alloc_ops(struct vk_conv_gpu *g, struct conv_image_alloc_ops *out);

//! S5 / S6 stages.
void
vk_conv_gpu_stage_ops(struct vk_conv_gpu *g,
                      struct conv_viewsyn_ops *viewsyn,
                      struct conv_depth_readback_ops *readback);

/*!
 * Copy tightly packed or @p row_pitch-strided CPU pixels into GPU image @p dst
 * (one this glue allocated): every layer, back to back. Synchronous.
 * @p row_pitch 0 = tight.
 */
bool
vk_conv_gpu_write(struct vk_conv_gpu *g, const struct xrt_conv_image *dst, const void *pixels, uint32_t row_pitch);

/*!
 * Read GPU image @p src (one this glue allocated) back into CPU memory: every
 * layer, back to back, rows @p row_pitch apart (0 = tight). Synchronous.
 */
bool
vk_conv_gpu_read(struct vk_conv_gpu *g, const struct xrt_conv_image *src, void *pixels, uint32_t row_pitch);

//! Diagnostics: how many times the S5 stage ran its passes 1-6 (0 before the first S5).
uint64_t
vk_conv_gpu_viewsyn_prepass_count(struct vk_conv_gpu *g);

//! Diagnostics: GPU images currently owned.
uint32_t
vk_conv_gpu_image_count(struct vk_conv_gpu *g);

#ifdef __cplusplus
}
#endif
