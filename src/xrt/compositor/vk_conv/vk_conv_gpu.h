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
#include "xrt/xrt_vulkan_includes.h"

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
	/*!
	 * macOS: S5 renders into glue images are handed over asynchronously,
	 * signalled on an MTLSharedEvent (vk_conv_gpu_image_ref::mtl_shared_event)
	 * instead of a host fence wait before publish. Vulkan readers on this
	 * bundle's main queue need nothing (submission order); another API waits
	 * on the event. DXR_CONV_VK_ASYNC_S5=0 turns it off.
	 */
	bool async_s5;
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

/*!
 * One glue image as another recorder on the same device sees it (the weave
 * engines' lifted rects, vk_conv_weave): the image is in
 * VK_IMAGE_LAYOUT_GENERAL between glue accesses, and a @ref foreign one
 * (DMABUF) is owned by VK_QUEUE_FAMILY_FOREIGN_EXT between them -- bracket the
 * access with vk_conv_gpu_cmd_acquire / vk_conv_gpu_cmd_release.
 */
struct vk_conv_gpu_image_ref
{
	VkImage image;
	VkFormat format; //!< the creation format (an _SRGB one is copied, not blitted, to stay encoded)
	uint32_t width, height, layers;
	bool foreign;
	/*!
	 * macOS, caps.async_s5: the image's last S5 write completes when this
	 * id<MTLSharedEvent> (the glue's; valid while the glue lives) reaches
	 * @ref event_value. NULL = already complete when published (SYNC_NONE).
	 * A Vulkan reader on the glue's queue ignores it (submission order).
	 */
	void *mtl_shared_event;
	uint64_t event_value;
};

/*!
 * Look up @p x, an image this glue allocated (false for a CPU image or one it
 * does not own). The image lives as long as the core holds it: the caller
 * holds a pin or the slot it belongs to while the GPU uses it.
 */
bool
vk_conv_gpu_lookup(struct vk_conv_gpu *g, const struct xrt_conv_image *x, struct vk_conv_gpu_image_ref *out);

//! Record the acquire of a foreign @p ref into @p cmd (GENERAL; no-op for a non-foreign image).
void
vk_conv_gpu_cmd_acquire(struct vk_conv_gpu *g, VkCommandBuffer cmd, const struct vk_conv_gpu_image_ref *ref);

//! Record the release of a foreign @p ref back to VK_QUEUE_FAMILY_FOREIGN_EXT (no-op otherwise).
void
vk_conv_gpu_cmd_release(struct vk_conv_gpu *g, VkCommandBuffer cmd, const struct vk_conv_gpu_image_ref *ref);

/*
 *
 * The client transport (XR_DXR_lift spec 4 GPU kinds, ADR-053 P5 step E).
 *
 */

/*!
 * A client's image, as the IPC transport delivered it: an IOSurface (macOS,
 * global: the transport carries its ID), a Vulkan OPAQUE_FD allocation or a
 * single-plane dma-buf (Linux). Handles are BORROWED: the glue duplicates what
 * it imports.
 *
 * OPAQUE_FD has no self-describing layout, so the exporter and the glue must
 * create the image identically (the transport contract, XR_DXR_lift §4.5): 2D,
 * 1 mip, 1 layer, VK_IMAGE_TILING_OPTIMAL, usage SAMPLED | TRANSFER_SRC |
 * TRANSFER_DST, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT for an _SRGB format (else no
 * flags), a dedicated allocation, @ref width x @ref height x @ref format.
 */
struct vk_conv_gpu_external
{
	uint32_t memory_kind;   //!< XRT_CONV_MEM_IOSURFACE / _VK_OPAQUE_FD / _DMABUF
	uint64_t handle;        //!< IOSurfaceRef / fd (borrowed)
	uint32_t width, height; //!< the whole image
	uint32_t format;        //!< XRT_CONV_FMT_* (RGBA8 / BGRA8, UNORM or SRGB)
	uint32_t row_stride;    //!< DMABUF: plane 0 stride (plane 0 offset is 0)
	uint64_t drm_modifier;  //!< DMABUF
	//! What to wait for before reading: NONE (writes complete before the call),
	//! SYNC_FD (a sync_file) or VK_SEMAPHORE_FD (a timeline + value), Linux.
	//! The handle is borrowed; imported temporarily, never cached.
	struct xrt_conv_sync sync;
};

/*!
 * Copy region @p x, @p y, @p w x @p h of client image @p src into @p dst, the
 * stream's input slot image:
 *  - a glue image: one GPU blit (scaled to @p dst's extent; channel order
 *    converted), no host memory touched -- the zero-copy path;
 *  - a CPU image (a provider on the CPU lane): the blit into a scratch image of
 *    @p dst's extent, then one GPU -> host readback into dst->cpu_ptr.
 * Synchronous (waits its own submit, so the slot is complete on return).
 * @p src's sync is waited for on the GPU in the same submit.
 */
bool
vk_conv_gpu_import_copy(struct vk_conv_gpu *g,
                        const struct vk_conv_gpu_external *src,
                        uint32_t x,
                        uint32_t y,
                        uint32_t w,
                        uint32_t h,
                        const struct xrt_conv_image *dst);

/*!
 * vk_conv_gpu_import_copy for a spec-4 input as the service's I/O table sees
 * it (a CONV_INPUT_IOSURFACE / _OPAQUE_FD / _DMABUF conv_input_import; its
 * handles stay the caller's): the region in->x, y, w, h into @p dst.
 */
bool
vk_conv_gpu_import_input(struct vk_conv_gpu *g, const struct conv_input_import *in, const struct xrt_conv_image *dst);

/*!
 * The XRT_CONV_MEM_* kinds vk_conv_gpu_import_copy takes on this device
 * (CPU excluded): IOSURFACE (macOS); VK_OPAQUE_FD, DMABUF (Linux).
 */
uint32_t
vk_conv_gpu_import_kinds(const struct vk_conv_gpu *g);

/*!
 * The XRT_CONV_MEM_* kinds vk_conv_gpu_export_alloc makes on this device:
 * IOSURFACE (macOS, a GLOBAL IOSurface the glue creates and imports, so a peer
 * process can look its ID up -- the ones MoltenVK creates are not global) and
 * VK_OPAQUE_FD (Linux). DMABUF is not offered: XrLiftResultExportDXR cannot
 * describe a modifier / stride to the app.
 */
uint32_t
vk_conv_gpu_export_kinds(const struct vk_conv_gpu *g);

/*!
 * A per-stream result export image of @p kind (one of vk_conv_gpu_export_kinds)
 * -- a glue image like any other (free it with the alloc ops' free). Its
 * handle is the glue's; the transport duplicates it into the client.
 */
bool
vk_conv_gpu_export_alloc(struct vk_conv_gpu *g,
                         uint32_t kind,
                         uint32_t conv_fmt,
                         uint32_t w,
                         uint32_t h,
                         uint64_t stream_id,
                         struct xrt_conv_image *out);

/*!
 * Copy @p src into glue image @p dst (same extent and byte layout; one layer):
 * a glue image is copied on the GPU, a CPU image is uploaded. Synchronous.
 */
bool
vk_conv_gpu_copy(struct vk_conv_gpu *g, const struct xrt_conv_image *src, const struct xrt_conv_image *dst);

/*!
 * Copy region @p x, @p y, @p w x @p h of @p src -- a VkImage of another
 * recorder on this device, in @p src_layout, not in use by the GPU any more
 * (the weave engines' snapshot chain after their fence) -- into @p dst (a
 * glue image: GPU only; a CPU image: through a host readback). Synchronous.
 */
bool
vk_conv_gpu_copy_from_vk(struct vk_conv_gpu *g,
                         VkImage src,
                         VkImageLayout src_layout,
                         VkFormat src_format,
                         uint32_t x,
                         uint32_t y,
                         uint32_t w,
                         uint32_t h,
                         const struct xrt_conv_image *dst);

/*!
 * The memory kind the glue would give a stream's INPUT image whose binding
 * imports @p import_memory_kinds / @p import_sync_kinds (glue_alloc's rule):
 * a GPU XRT_CONV_MEM_* kind, or XRT_CONV_MEM_CPU when the CPU allocator would
 * serve it (the default: providers on the CPU lane).
 */
uint32_t
vk_conv_gpu_input_memory_kind(const struct vk_conv_gpu *g, uint32_t import_memory_kinds, uint32_t import_sync_kinds);

//! Diagnostics: copies the glue made, by direction (monotonic counters).
struct vk_conv_gpu_stats
{
	uint64_t host_writes; //!< CPU -> GPU image uploads (staged)
	uint64_t host_reads;  //!< GPU image -> CPU readbacks (staged; S6 samples not counted)
	uint64_t gpu_copies;  //!< GPU -> GPU copies / blits (client imports, exports, weave snapshots)
	uint64_t imports;     //!< client images imported (vk_conv_gpu_import_copy)
	uint64_t async_s5;    //!< S5 renders published before their GPU work finished (shared event)
};

void
vk_conv_gpu_get_stats(struct vk_conv_gpu *g, struct vk_conv_gpu_stats *out);

//! Diagnostics: how many times the S5 stage ran its passes 1-6 (0 before the first S5).
uint64_t
vk_conv_gpu_viewsyn_prepass_count(struct vk_conv_gpu *g);

//! Diagnostics: GPU images currently owned.
uint32_t
vk_conv_gpu_image_count(struct vk_conv_gpu *g);

#ifdef __cplusplus
}
#endif
