// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The conversion core's Vulkan GPU glue — see vk_conv_gpu.h.
 *
 * Every glue image is kept in VK_IMAGE_LAYOUT_GENERAL from its creation on
 * (cleared to zero and transitioned in one submit before it is handed out), so
 * a provider that imported it, the S5 stage, the uploads and the readbacks
 * all agree on the layout without per-access transitions; ordering between
 * accesses is a global memory barrier at the start of each glue submit. A
 * DMABUF image is additionally released to VK_QUEUE_FAMILY_FOREIGN_EXT after
 * creation and after every glue access, and acquired back before every glue
 * access (the vk_dmabuf.h protocol).
 *
 * @ingroup comp_vk_conv
 */

#include "vk_conv_gpu.h"
#include "vk_conv_viewsyn.h"

#include "xrt/xrt_config_os.h"
#include "xrt/xrt_handles.h"

#include "vk/vk_cmd_pool.h"
#include "vk/vk_helpers.h"
#ifdef XRT_OS_LINUX_DESKTOP
#include "vk/vk_dmabuf.h"
#endif

#include "os/os_threading.h"
#include "os/os_time.h"
#include "util/u_lift_autoconv.h"
#include "util/u_logging.h"
#include "util/u_misc.h"

#if defined(XRT_GRAPHICS_BUFFER_HANDLE_IS_METAL)
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurface.h>
#endif

#if defined(XRT_GRAPHICS_BUFFER_HANDLE_IS_FD) || defined(XRT_GRAPHICS_SYNC_HANDLE_IS_FD)
#include <unistd.h> // close, dup
#endif

#include <math.h>
#include <stdlib.h>
#include <string.h>


/*
 *
 * Structs.
 *
 */

struct vkc_img
{
	struct vkc_img *next;
	uint64_t key;
	uint64_t stream_id;
	uint32_t conv_fmt;
	uint32_t w, h, layers;
	uint32_t kind; //!< XRT_CONV_MEM_* of the export, 0 = device-private
	VkFormat format;
	VkFormat view_format; //!< the UNORM alias the shaders sample
	VkImage image;
	VkDeviceMemory mem;
	VkImageView view; //!< 2D, layer 0, view_format
	uint64_t handle;  //!< fd / IOSurfaceRef / VK_CONV_GPU_NO_HANDLE (owned by the glue)
	uint64_t byte_size;
	uint64_t drm_modifier;
	uint32_t row_stride; //!< DMABUF plane 0 stride
	bool foreign;        //!< DMABUF: owned by VK_QUEUE_FAMILY_FOREIGN_EXT between glue accesses
};

//! Per-stream glue state: scratch for CPU images the S5 stage reads / writes, and S6.
struct vkc_stream
{
	struct vkc_stream *next;
	uint64_t id;

	struct vkc_img *s_color, *s_depth, *s_target;

	VkBuffer rb_buf;
	VkDeviceMemory rb_mem;
	void *rb_map;
	VkDeviceSize rb_size;
	VkFence rb_fence;
	VkCommandBuffer rb_cmd;
	bool rb_pending;
	uint64_t rb_issued_ns;
	uint64_t rb_src_key;
	uint32_t rb_w, rb_h;
	uint32_t rb_fmt; //!< XRT_CONV_FMT_*
	float *samples;
	size_t samples_cap;
};

//! An imported timeline semaphore (XRT_CONV_SYNC_VK_SEMAPHORE_FD), by import_key.
struct vkc_sem
{
	struct vkc_sem *next;
	uint64_t key;
	VkSemaphore sem;
};

struct vk_conv_gpu
{
	struct vk_bundle *vk;
	struct vk_conv_gpu_caps caps;
	uint32_t provider_kinds;

	//! Gate: destroy closes it and waits for in-flight calls.
	struct os_mutex gate_mtx;
	struct os_cond gate_cond;
	uint32_t calls;
	bool closed;

	//! Leaf: the tables below.
	struct os_mutex tab_mtx;
	struct vkc_img *imgs;
	struct vkc_stream *streams;
	uint64_t next_key;
	uint32_t img_count;

	//! One GPU job at a time: recording, submit, wait; the S5 stage, staging, semaphores.
	struct os_mutex work_mtx;
	struct vk_cmd_pool pool;
	VkFence fence;
	VkBuffer stage_buf;
	VkDeviceMemory stage_mem;
	void *stage_map;
	VkDeviceSize stage_size;
	struct vkc_sem *sems;
	struct vk_conv_viewsyn *viewsyn;
	bool viewsyn_tried;
};

//! Waits a submit carries (imported sync of the images it reads).
#define VKC_MAX_WAITS 4u
struct vkc_waits
{
	uint32_t count;
	VkSemaphore sem[VKC_MAX_WAITS];
	uint64_t value[VKC_MAX_WAITS];
	bool timeline[VKC_MAX_WAITS];
	bool temp[VKC_MAX_WAITS]; //!< destroy after the submit (SYNC_FD imports)
};


/*
 *
 * Formats.
 *
 */

static VkFormat
conv_to_vk(uint32_t f)
{
	switch (f) {
	case XRT_CONV_FMT_RGBA8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
	case XRT_CONV_FMT_RGBA8_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
	case XRT_CONV_FMT_BGRA8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
	case XRT_CONV_FMT_BGRA8_SRGB: return VK_FORMAT_B8G8R8A8_SRGB;
	case XRT_CONV_FMT_R8_UNORM: return VK_FORMAT_R8_UNORM;
	case XRT_CONV_FMT_R16_FLOAT: return VK_FORMAT_R16_SFLOAT;
	case XRT_CONV_FMT_R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
	case XRT_CONV_FMT_RGBA16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
	case XRT_CONV_FMT_R16_UNORM: return VK_FORMAT_R16_UNORM;
	default: return VK_FORMAT_UNDEFINED; // NV12 and unknown: the CPU lane
	}
}

//! The UNORM alias the shaders sample (no sRGB decode, D3D11 / browser parity).
static VkFormat
unorm_alias(VkFormat f)
{
	switch (f) {
	case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
	case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
	default: return f;
	}
}

static uint32_t
conv_fmt_bytes(uint32_t fmt)
{
	switch (fmt) {
	case XRT_CONV_FMT_R8_UNORM: return 1;
	case XRT_CONV_FMT_R16_FLOAT:
	case XRT_CONV_FMT_R16_UNORM: return 2;
	case XRT_CONV_FMT_RGBA16_FLOAT: return 8;
	default: return 4;
	}
}

//! IEEE half -> float (the depth may be R16_FLOAT).
static float
half_to_float(uint16_t h)
{
	const uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
	uint32_t exp = (h >> 10) & 0x1fu;
	uint32_t man = h & 0x3ffu;
	uint32_t bits;
	if (exp == 0) {
		if (man == 0) {
			bits = sign;
		} else {
			exp = 127 - 15 + 1;
			while ((man & 0x400u) == 0) {
				man <<= 1;
				exp--;
			}
			man &= 0x3ffu;
			bits = sign | (exp << 23) | (man << 13);
		}
	} else if (exp == 0x1f) {
		bits = sign | 0x7f800000u | (man << 13);
	} else {
		bits = sign | ((exp + 127 - 15) << 23) | (man << 13);
	}
	float f;
	memcpy(&f, &bits, sizeof(f));
	return f;
}


/*
 *
 * Gate.
 *
 */

static bool
gate_enter(struct vk_conv_gpu *g)
{
	os_mutex_lock(&g->gate_mtx);
	const bool ok = !g->closed;
	if (ok) {
		g->calls++;
	}
	os_mutex_unlock(&g->gate_mtx);
	return ok;
}

static void
gate_leave(struct vk_conv_gpu *g)
{
	os_mutex_lock(&g->gate_mtx);
	g->calls--;
	os_cond_broadcast(&g->gate_cond);
	os_mutex_unlock(&g->gate_mtx);
}


/*
 *
 * Command submission (work_mtx held).
 *
 */

static bool
cmd_begin(struct vk_conv_gpu *g, VkCommandBuffer *out)
{
	struct vk_bundle *vk = g->vk;
	vk_cmd_pool_lock(&g->pool);
	VkResult r = vk_cmd_pool_create_and_begin_cmd_buffer_locked(vk, &g->pool, 0, out);
	vk_cmd_pool_unlock(&g->pool);
	return r == VK_SUCCESS;
}

static void
cmd_free(struct vk_conv_gpu *g, VkCommandBuffer cmd)
{
	vk_cmd_pool_lock(&g->pool);
	g->vk->vkFreeCommandBuffers(g->vk->device, g->pool.pool, 1, &cmd);
	vk_cmd_pool_unlock(&g->pool);
}

static void
waits_release(struct vk_conv_gpu *g, struct vkc_waits *w)
{
	for (uint32_t i = 0; i < w->count; i++) {
		if (w->temp[i]) {
			g->vk->vkDestroySemaphore(g->vk->device, w->sem[i], NULL);
		}
	}
	w->count = 0;
}

/*!
 * End @p cmd, submit it on the main queue waiting @p waits (may be NULL) and
 * signalling @p fence, without waiting. The command buffer is NOT freed.
 */
static bool
cmd_submit(struct vk_conv_gpu *g, VkCommandBuffer cmd, const struct vkc_waits *waits, VkFence fence)
{
	struct vk_bundle *vk = g->vk;
	if (vk->vkEndCommandBuffer(cmd) != VK_SUCCESS) {
		return false;
	}
	VkPipelineStageFlags stages[VKC_MAX_WAITS];
	uint64_t values[VKC_MAX_WAITS];
	bool any_timeline = false;
	const uint32_t n = waits != NULL ? waits->count : 0;
	for (uint32_t i = 0; i < n; i++) {
		stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		values[i] = waits->timeline[i] ? waits->value[i] : 0;
		any_timeline = any_timeline || waits->timeline[i];
	}
#ifdef VK_KHR_timeline_semaphore
	VkTimelineSemaphoreSubmitInfoKHR tl = {
	    .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO_KHR,
	    .waitSemaphoreValueCount = n,
	    .pWaitSemaphoreValues = values,
	};
#endif
	VkSubmitInfo si = {
	    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	    .waitSemaphoreCount = n,
	    .pWaitSemaphores = n > 0 ? waits->sem : NULL,
	    .pWaitDstStageMask = n > 0 ? stages : NULL,
	    .commandBufferCount = 1,
	    .pCommandBuffers = &cmd,
	};
#ifdef VK_KHR_timeline_semaphore
	if (any_timeline) {
		si.pNext = &tl;
	}
#else
	(void)any_timeline;
#endif
	vk_queue_lock(vk->main_queue);
	VkResult r = vk->vkQueueSubmit(vk->main_queue->queue, 1, &si, fence);
	vk_queue_unlock(vk->main_queue);
	return r == VK_SUCCESS;
}

/*!
 * Submit, wait, free. Always frees @p cmd and releases @p waits.
 *
 * THE HAND-OVER POINT FOR RESULTS (XRT_CONV_SYNC_NONE, "producer finished
 * before hand-over"). Every GPU write of a result image goes through here and
 * returns only after a HOST-side vkWaitForFences on that submit: the S5 views
 * (glue_viewsyn_render) and the #2030 mirror upload (glue_upload ->
 * vk_conv_gpu_write -> write_locked). conv_stream publishes the result (S8,
 * u_lift_mailbox_publish_output) only after those calls returned, so a
 * consumer on another API -- the macOS Metal weave backend sampling the
 * result's IOSurface -- never reads unfinished Vulkan work and needs no
 * semaphore. (No POSIX provider writes a GPU result itself: the provider mask
 * defaults to 0.) The reader keeps the result pinned until ITS GPU work is done
 * (Metal: vk_conv_weave_complete runs after waitUntilCompleted).
 */
static bool
cmd_submit_wait(struct vk_conv_gpu *g, VkCommandBuffer cmd, struct vkc_waits *waits)
{
	struct vk_bundle *vk = g->vk;
	bool ok = vk->vkResetFences(vk->device, 1, &g->fence) == VK_SUCCESS;
	const bool submitted = ok && cmd_submit(g, cmd, waits, g->fence);
	ok = submitted && vk->vkWaitForFences(vk->device, 1, &g->fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
	if (submitted && !ok) {
		// Never free what the GPU may still read.
		U_LOG_E("[conv] Vulkan glue: fence wait failed; leaking one command buffer");
		return false;
	}
	cmd_free(g, cmd);
	if (waits != NULL) {
		waits_release(g, waits);
	}
	return ok;
}

//! Order this submit after every earlier queue operation on the glue's images.
static void
cmd_global_barrier(struct vk_bundle *vk, VkCommandBuffer cmd)
{
	VkMemoryBarrier mb = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
	    .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
	};
	vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb,
	                         0, NULL, 0, NULL);
}

static void
img_acquire_foreign(struct vk_conv_gpu *g, VkCommandBuffer cmd, struct vkc_img *img)
{
#ifdef XRT_OS_LINUX_DESKTOP
	if (img->foreign) {
		vk_dmabuf_cmd_acquire_foreign(g->vk, cmd, img->image, VK_IMAGE_LAYOUT_GENERAL,
		                              VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		                              VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
	}
#else
	(void)g;
	(void)cmd;
	(void)img;
#endif
}

static void
img_release_foreign(struct vk_conv_gpu *g, VkCommandBuffer cmd, struct vkc_img *img)
{
#ifdef XRT_OS_LINUX_DESKTOP
	if (img->foreign) {
		vk_dmabuf_cmd_release_foreign(g->vk, cmd, img->image, VK_IMAGE_LAYOUT_GENERAL,
		                              VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT);
	}
#else
	(void)g;
	(void)cmd;
	(void)img;
#endif
}


/*
 *
 * Staging (work_mtx held).
 *
 */

static void
buffer_free(struct vk_bundle *vk, VkBuffer *buf, VkDeviceMemory *mem, void **map)
{
	if (*map != NULL) {
		vk->vkUnmapMemory(vk->device, *mem);
		*map = NULL;
	}
	if (*buf != VK_NULL_HANDLE) {
		vk->vkDestroyBuffer(vk->device, *buf, NULL);
		*buf = VK_NULL_HANDLE;
	}
	if (*mem != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, *mem, NULL);
		*mem = VK_NULL_HANDLE;
	}
}

//! A host-visible transfer buffer of @p size, mapped. Prefers HOST_CACHED (readbacks).
static bool
buffer_make(struct vk_bundle *vk, VkDeviceSize size, VkBuffer *out_buf, VkDeviceMemory *out_mem, void **out_map)
{
	VkBufferCreateInfo bci = {
	    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
	    .size = size,
	    .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	if (vk->vkCreateBuffer(vk->device, &bci, NULL, out_buf) != VK_SUCCESS) {
		*out_buf = VK_NULL_HANDLE;
		return false;
	}
	VkMemoryRequirements req;
	vk->vkGetBufferMemoryRequirements(vk->device, *out_buf, &req);
	// Coherent memory only (every desktop driver has it): no flush / invalidate.
	uint32_t type = 0;
	if (!vk_get_memory_type(vk, req.memoryTypeBits,
	                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
	                            VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
	                        &type) &&
	    !vk_get_memory_type(vk, req.memoryTypeBits,
	                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &type)) {
		vk->vkDestroyBuffer(vk->device, *out_buf, NULL);
		*out_buf = VK_NULL_HANDLE;
		return false;
	}
	VkMemoryAllocateInfo ai = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .allocationSize = req.size,
	    .memoryTypeIndex = type,
	};
	*out_map = NULL;
	if (vk->vkAllocateMemory(vk->device, &ai, NULL, out_mem) != VK_SUCCESS) {
		*out_mem = VK_NULL_HANDLE;
		buffer_free(vk, out_buf, out_mem, out_map);
		return false;
	}
	if (vk->vkBindBufferMemory(vk->device, *out_buf, *out_mem, 0) != VK_SUCCESS ||
	    vk->vkMapMemory(vk->device, *out_mem, 0, VK_WHOLE_SIZE, 0, out_map) != VK_SUCCESS) {
		*out_map = NULL;
		buffer_free(vk, out_buf, out_mem, out_map);
		return false;
	}
	return true;
}

static bool
stage_ensure(struct vk_conv_gpu *g, VkDeviceSize size)
{
	if (g->stage_buf != VK_NULL_HANDLE && g->stage_size >= size) {
		return true;
	}
	buffer_free(g->vk, &g->stage_buf, &g->stage_mem, &g->stage_map);
	g->stage_size = 0;
	if (!buffer_make(g->vk, size, &g->stage_buf, &g->stage_mem, &g->stage_map)) {
		return false;
	}
	g->stage_size = size;
	return true;
}

//! Tight CPU rows <-> one layer-major staging blob.
static void
copy_rows(uint8_t *dst, uint32_t dst_pitch, const uint8_t *src, uint32_t src_pitch, uint32_t row_bytes, uint32_t rows)
{
	for (uint32_t y = 0; y < rows; y++) {
		memcpy(dst + (size_t)y * dst_pitch, src + (size_t)y * src_pitch, row_bytes);
	}
}

static void
record_buffer_image(struct vk_bundle *vk, VkCommandBuffer cmd, VkBuffer buf, struct vkc_img *img, bool to_image)
{
	VkBufferImageCopy region = {
	    .bufferOffset = 0,
	    .bufferRowLength = 0, // tight
	    .bufferImageHeight = 0,
	    .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, img->layers},
	    .imageOffset = {0, 0, 0},
	    .imageExtent = {img->w, img->h, 1},
	};
	if (to_image) {
		vk->vkCmdCopyBufferToImage(cmd, buf, img->image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
	} else {
		vk->vkCmdCopyImageToBuffer(cmd, img->image, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &region);
	}
}


/*
 *
 * Images.
 *
 */

#if defined(XRT_GRAPHICS_BUFFER_HANDLE_IS_FD)
static bool
format_exportable(struct vk_bundle *vk,
                  VkFormat format,
                  VkImageCreateFlags flags,
                  VkImageUsageFlags usage,
                  uint32_t layers,
                  VkExternalMemoryHandleTypeFlagBits type)
{
	VkPhysicalDeviceExternalImageFormatInfo ext = {
	    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
	    .handleType = type,
	};
	VkPhysicalDeviceImageFormatInfo2 info = {
	    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
	    .pNext = &ext,
	    .format = format,
	    .type = VK_IMAGE_TYPE_2D,
	    .tiling = VK_IMAGE_TILING_OPTIMAL,
	    .usage = usage,
	    .flags = flags,
	};
	VkExternalImageFormatProperties ext_props = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
	VkImageFormatProperties2 props = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &ext_props};
	if (vk->vkGetPhysicalDeviceImageFormatProperties2(vk->physical_device, &info, &props) != VK_SUCCESS) {
		return false;
	}
	if (props.imageFormatProperties.maxArrayLayers < layers) {
		return false;
	}
	return (ext_props.externalMemoryProperties.externalMemoryFeatures &
	        VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) != 0;
}

#endif

static void
img_destroy(struct vk_conv_gpu *g, struct vkc_img *img)
{
	struct vk_bundle *vk = g->vk;
	if (img == NULL) {
		return;
	}
	if (img->view != VK_NULL_HANDLE) {
		vk->vkDestroyImageView(vk->device, img->view, NULL);
	}
	if (img->image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, img->image, NULL);
	}
	if (img->mem != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, img->mem, NULL);
	}
	if (img->handle != VK_CONV_GPU_NO_HANDLE) {
#if defined(XRT_GRAPHICS_BUFFER_HANDLE_IS_METAL)
		if (img->kind == XRT_CONV_MEM_IOSURFACE && img->handle != 0) {
			CFRelease((IOSurfaceRef)(uintptr_t)img->handle);
		}
#endif
#if defined(XRT_GRAPHICS_BUFFER_HANDLE_IS_FD)
		if (img->kind == XRT_CONV_MEM_VK_OPAQUE_FD || img->kind == XRT_CONV_MEM_DMABUF) {
			close((int)img->handle);
		}
#endif
	}
	free(img);
}

//! Dedicated device-local memory for @p image, @p pnext chained on the allocation.
static bool
img_bind_memory(struct vk_bundle *vk, VkImage image, const void *pnext, VkDeviceMemory *out_mem, uint64_t *out_size)
{
	VkMemoryRequirements req;
	vk->vkGetImageMemoryRequirements(vk->device, image, &req);
	uint32_t type = 0;
	if (!vk_get_memory_type(vk, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type)) {
		return false;
	}
	VkMemoryDedicatedAllocateInfoKHR dedicated = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO_KHR,
	    .pNext = pnext,
	    .image = image,
	};
	VkMemoryAllocateInfo ai = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .pNext = &dedicated,
	    .allocationSize = req.size,
	    .memoryTypeIndex = type,
	};
	if (vk->vkAllocateMemory(vk->device, &ai, NULL, out_mem) != VK_SUCCESS) {
		*out_mem = VK_NULL_HANDLE;
		return false;
	}
	if (vk->vkBindImageMemory(vk->device, image, *out_mem, 0) != VK_SUCCESS) {
		vk->vkFreeMemory(vk->device, *out_mem, NULL);
		*out_mem = VK_NULL_HANDLE;
		return false;
	}
	*out_size = req.size;
	return true;
}

/*!
 * Create an image of @p conv_fmt with export @p kind (0 = private). Leaves it
 * in VK_IMAGE_LAYOUT_UNDEFINED with no view (img_finish adds them).
 */
static struct vkc_img *
img_create(struct vk_conv_gpu *g, uint32_t conv_fmt, uint32_t w, uint32_t h, uint32_t layers, uint32_t kind)
{
	struct vk_bundle *vk = g->vk;
	const VkFormat format = conv_to_vk(conv_fmt);
	if (format == VK_FORMAT_UNDEFINED || w == 0 || h == 0 || layers == 0) {
		return NULL;
	}
	struct vkc_img *img = U_TYPED_CALLOC(struct vkc_img);
	img->conv_fmt = conv_fmt;
	img->w = w;
	img->h = h;
	img->layers = layers;
	img->kind = kind;
	img->format = format;
	img->view_format = unorm_alias(format);
	img->handle = VK_CONV_GPU_NO_HANDLE;

	const VkImageUsageFlags usage =
	    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	const VkImageCreateFlags flags = img->view_format != format ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;

#ifdef XRT_OS_LINUX_DESKTOP
	if (kind == XRT_CONV_MEM_DMABUF) {
		// Single-plane modifiers only: xrt_conv_image carries one stride and
		// no plane-0 offset (a CCS modifier's aux plane has nowhere to go).
		struct vk_dmabuf_modifier_info mods[32];
		uint32_t count = 0;
		uint64_t list[32];
		uint32_t n = 0;
		(void)vk_dmabuf_query_modifiers(vk, format, usage, mods, 32, &count);
		for (uint32_t i = 0; i < count && i < 32; i++) {
			if (mods[i].exportable && mods[i].plane_count == 1) {
				list[n++] = mods[i].modifier;
			}
		}
		struct xrt_weave_dmabuf_output_desc desc;
		memset(&desc, 0, sizeof(desc));
		desc.fd = -1;
		if (n == 0 || flags != 0 ||
		    vk_create_exportable_dmabuf_image(vk, w, h, format, usage, list, n, &desc, &img->image,
		                                      &img->mem) != VK_SUCCESS) {
			free(img);
			return NULL;
		}
		if (desc.plane_count != 1 || desc.offsets[0] != 0) {
			close(desc.fd);
			img->handle = VK_CONV_GPU_NO_HANDLE;
			img_destroy(g, img);
			return NULL;
		}
		img->handle = (uint64_t)(uint32_t)desc.fd;
		img->drm_modifier = desc.drm_modifier;
		img->row_stride = desc.strides[0];
		img->byte_size = desc.size;
		img->foreign = true; // released at img_finish
		return img;
	}
#endif

	VkImageCreateInfo ici = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .flags = flags,
	    .imageType = VK_IMAGE_TYPE_2D,
	    .format = format,
	    .extent = {w, h, 1},
	    .mipLevels = 1,
	    .arrayLayers = layers,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .tiling = VK_IMAGE_TILING_OPTIMAL,
	    .usage = usage,
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	const void *alloc_next = NULL;

#if defined(XRT_GRAPHICS_BUFFER_HANDLE_IS_FD)
	VkExternalMemoryImageCreateInfo ext_ici = {
	    .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
	    .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
	};
	VkExportMemoryAllocateInfo export_ai = {
	    .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
	    .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
	};
	if (kind == XRT_CONV_MEM_VK_OPAQUE_FD) {
		ici.pNext = &ext_ici;
		alloc_next = &export_ai;
	}
#endif
#if defined(XRT_GRAPHICS_BUFFER_HANDLE_IS_METAL) && defined(VK_EXT_metal_objects)
	VkExportMetalObjectCreateInfoEXT export_metal = {
	    .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT,
	    .exportObjectType = VK_EXPORT_METAL_OBJECT_TYPE_METAL_IOSURFACE_BIT_EXT,
	};
	if (kind == XRT_CONV_MEM_IOSURFACE) {
		ici.pNext = &export_metal;
	}
#endif

	if (vk->vkCreateImage(vk->device, &ici, NULL, &img->image) != VK_SUCCESS) {
		img->image = VK_NULL_HANDLE;
		img_destroy(g, img);
		return NULL;
	}
	if (!img_bind_memory(vk, img->image, alloc_next, &img->mem, &img->byte_size)) {
		img_destroy(g, img);
		return NULL;
	}

#if defined(XRT_GRAPHICS_BUFFER_HANDLE_IS_FD)
	if (kind == XRT_CONV_MEM_VK_OPAQUE_FD) {
		int fd = -1;
		VkMemoryGetFdInfoKHR gi = {
		    .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
		    .memory = img->mem,
		    .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
		};
		if (vk->vkGetMemoryFdKHR == NULL || vk->vkGetMemoryFdKHR(vk->device, &gi, &fd) != VK_SUCCESS ||
		    fd < 0) {
			img_destroy(g, img);
			return NULL;
		}
		img->handle = (uint64_t)(uint32_t)fd;
	}
#endif
#if defined(XRT_GRAPHICS_BUFFER_HANDLE_IS_METAL) && defined(VK_EXT_metal_objects)
	if (kind == XRT_CONV_MEM_IOSURFACE) {
		VkExportMetalIOSurfaceInfoEXT es = {
		    .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_IO_SURFACE_INFO_EXT,
		    .image = img->image,
		};
		VkExportMetalObjectsInfoEXT ei = {
		    .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
		    .pNext = &es,
		};
		vk->vkExportMetalObjectsEXT(vk->device, &ei);
		if (es.ioSurface == NULL) {
			img_destroy(g, img);
			return NULL;
		}
		CFRetain(es.ioSurface);
		img->handle = (uint64_t)(uintptr_t)es.ioSurface;
	}
#endif
	return img;
}

/*!
 * The sampled view, then zero + GENERAL (+ foreign release) in one submit.
 * work_mtx held. Destroys @p img on failure.
 */
static bool
img_finish(struct vk_conv_gpu *g, struct vkc_img *img)
{
	struct vk_bundle *vk = g->vk;
	VkImageSubresourceRange vrange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	if (vk_create_view(vk, img->image, VK_IMAGE_VIEW_TYPE_2D, img->view_format, vrange, &img->view) != VK_SUCCESS) {
		img->view = VK_NULL_HANDLE;
		img_destroy(g, img);
		return false;
	}
	VkCommandBuffer cmd;
	if (!cmd_begin(g, &cmd)) {
		img_destroy(g, img);
		return false;
	}
	VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, img->layers};
	vk_cmd_image_barrier_locked(vk, cmd, img->image, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
	                            VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	                            VK_PIPELINE_STAGE_TRANSFER_BIT, range);
	VkClearColorValue zero;
	memset(&zero, 0, sizeof(zero));
	vk->vkCmdClearColorImage(cmd, img->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
	cmd_global_barrier(vk, cmd);
	img_release_foreign(g, cmd, img);
	if (!cmd_submit_wait(g, cmd, NULL)) {
		img_destroy(g, img);
		return false;
	}
	return true;
}

//! A glue image by key; the caller holds a gate entry (images die only via free / destroy).
static struct vkc_img *
img_find(struct vk_conv_gpu *g, uint64_t key)
{
	os_mutex_lock(&g->tab_mtx);
	struct vkc_img *it = g->imgs;
	while (it != NULL && it->key != key) {
		it = it->next;
	}
	os_mutex_unlock(&g->tab_mtx);
	return it;
}

static bool
is_glue_image(const struct xrt_conv_image *x)
{
	return x != NULL && x->memory_kind != 0 && x->memory_kind != XRT_CONV_MEM_CPU && (x->import_key >> 63) == 0;
}

static bool
is_cpu_image(const struct xrt_conv_image *x)
{
	return x != NULL && x->memory_kind == XRT_CONV_MEM_CPU && x->cpu_ptr != NULL;
}

//! Per-stream state (created on demand). tab_mtx held.
static struct vkc_stream *
stream_get_locked(struct vk_conv_gpu *g, uint64_t id, bool create)
{
	struct vkc_stream *s = g->streams;
	while (s != NULL && s->id != id) {
		s = s->next;
	}
	if (s == NULL && create) {
		s = U_TYPED_CALLOC(struct vkc_stream);
		s->id = id;
		s->next = g->streams;
		g->streams = s;
	}
	return s;
}

//! Free a stream's state; waits its pending readback. work_mtx held, unlinked.
static void
stream_free(struct vk_conv_gpu *g, struct vkc_stream *s)
{
	struct vk_bundle *vk = g->vk;
	if (s->rb_pending) {
		(void)vk->vkWaitForFences(vk->device, 1, &s->rb_fence, VK_TRUE, UINT64_MAX);
		s->rb_pending = false;
	}
	if (s->rb_cmd != VK_NULL_HANDLE) {
		cmd_free(g, s->rb_cmd);
	}
	if (s->rb_fence != VK_NULL_HANDLE) {
		vk->vkDestroyFence(vk->device, s->rb_fence, NULL);
	}
	buffer_free(vk, &s->rb_buf, &s->rb_mem, &s->rb_map);
	img_destroy(g, s->s_color);
	img_destroy(g, s->s_depth);
	img_destroy(g, s->s_target);
	free(s->samples);
	free(s);
}


/*
 *
 * conv_image_alloc_ops.
 *
 */

//! The export kind for one request, or false = decline (the CPU allocator serves it).
static bool
pick_kind(struct vk_conv_gpu *g,
          const struct conv_image_request *req,
          uint32_t layers,
          uint32_t *out_kind,
          bool *out_private_ok)
{
	const bool runtime_only = (req->usage & (CONV_IMAGE_USAGE_RENDER_TARGET | CONV_IMAGE_USAGE_MIRROR)) != 0;
	uint32_t allowed;
	if (runtime_only) {
		allowed = g->caps.memory_kinds & ~XRT_CONV_MEM_CPU;
	} else {
		// A provider imports it: only kinds both it and the policy take.
		if (req->import_memory_kinds == 0) {
			return false;
		}
		allowed = req->import_memory_kinds & g->provider_kinds & g->caps.memory_kinds & ~XRT_CONV_MEM_CPU;
		if (allowed == 0) {
			return false;
		}
		if (req->import_sync_kinds != 0 && (req->import_sync_kinds & (1u << XRT_CONV_SYNC_NONE)) == 0) {
			return false; // every glue image is SYNC_NONE
		}
	}
	*out_private_ok = runtime_only;
	// Preference: OPAQUE_FD (any layer count) > DMABUF (1 layer) > IOSURFACE (1 layer).
	if ((allowed & XRT_CONV_MEM_VK_OPAQUE_FD) != 0) {
		*out_kind = XRT_CONV_MEM_VK_OPAQUE_FD;
	} else if ((allowed & XRT_CONV_MEM_DMABUF) != 0 && layers == 1) {
		*out_kind = XRT_CONV_MEM_DMABUF;
	} else if ((allowed & XRT_CONV_MEM_IOSURFACE) != 0 && layers == 1) {
		*out_kind = XRT_CONV_MEM_IOSURFACE;
	} else {
		*out_kind = 0;
		if (!runtime_only) {
			return false;
		}
	}
	return true;
}

static bool
glue_alloc(void *ud, const struct conv_image_request *req, struct xrt_conv_image *out)
{
	struct vk_conv_gpu *g = (struct vk_conv_gpu *)ud;
	const uint32_t layers = req->array_layers > 0 ? req->array_layers : 1;
	uint32_t kind = 0;
	bool private_ok = false;
	if (conv_to_vk(req->format) == VK_FORMAT_UNDEFINED || req->width == 0 || req->height == 0 ||
	    !pick_kind(g, req, layers, &kind, &private_ok) || !gate_enter(g)) {
		return false;
	}
	os_mutex_lock(&g->work_mtx);
	struct vkc_img *img = kind != 0 ? img_create(g, req->format, req->width, req->height, layers, kind) : NULL;
	if (img == NULL && private_ok) {
		img = img_create(g, req->format, req->width, req->height, layers, 0);
	}
	const bool ok = img != NULL && img_finish(g, img);
	os_mutex_unlock(&g->work_mtx);
	if (!ok) {
		gate_leave(g);
		return false;
	}
	img->stream_id = req->stream_id;
	os_mutex_lock(&g->tab_mtx);
	img->key = ++g->next_key; // bit 63 clear: disjoint from the service CPU allocator's keys
	img->next = g->imgs;
	g->imgs = img;
	g->img_count++;
	os_mutex_unlock(&g->tab_mtx);

	memset(out, 0, sizeof(*out));
	out->memory_kind = img->kind != 0                                         ? img->kind
	                   : (g->caps.memory_kinds & XRT_CONV_MEM_IOSURFACE) != 0 ? XRT_CONV_MEM_IOSURFACE
	                                                                          : XRT_CONV_MEM_VK_OPAQUE_FD;
	out->handle = img->handle;
	out->import_key = img->key;
	out->byte_size = img->byte_size;
	out->width = img->w;
	out->height = img->h;
	out->format = req->format;
	out->color_space = (req->role == 0 || req->role == XRT_CONV_OUT_VIEWS) ? XRT_CONV_CS_SRGB : XRT_CONV_CS_DATA;
	out->row_stride = img->row_stride;
	out->drm_modifier = img->drm_modifier;
	out->adapter_luid = 0;
	out->sync.kind = XRT_CONV_SYNC_NONE;
	out->sync.handle = 0;
	out->sync.import_key = img->key;
	gate_leave(g);
	return true;
}

static void
glue_free(void *ud, struct xrt_conv_image *img)
{
	struct vk_conv_gpu *g = (struct vk_conv_gpu *)ud;
	if (!is_glue_image(img) || !gate_enter(g)) {
		return;
	}
	os_mutex_lock(&g->work_mtx);
	os_mutex_lock(&g->tab_mtx);
	struct vkc_img **pp = &g->imgs;
	while (*pp != NULL && (*pp)->key != img->import_key) {
		pp = &(*pp)->next;
	}
	struct vkc_img *found = *pp;
	struct vkc_stream *drop = NULL;
	struct vkc_stream *s = NULL;
	if (found != NULL) {
		*pp = found->next;
		g->img_count--;
		bool any = false;
		for (struct vkc_img *it = g->imgs; it != NULL; it = it->next) {
			any = any || it->stream_id == found->stream_id;
		}
		s = stream_get_locked(g, found->stream_id, false);
		if (!any && s != NULL) {
			// The stream's last glue image: its scratch and readback go too.
			struct vkc_stream **sp = &g->streams;
			while (*sp != s) {
				sp = &(*sp)->next;
			}
			*sp = s->next;
			drop = s;
		}
	}
	os_mutex_unlock(&g->tab_mtx);
	if (found != NULL) {
		// A readback copy still reading this image finishes first.
		if (drop == NULL && s != NULL && s->rb_pending && s->rb_src_key == found->key) {
			(void)g->vk->vkWaitForFences(g->vk->device, 1, &s->rb_fence, VK_TRUE, UINT64_MAX);
		}
		if (drop != NULL) {
			stream_free(g, drop);
		}
		// A later view could reuse the destroyed handle values.
		vk_conv_viewsyn_invalidate(g->viewsyn);
		img_destroy(g, found);
	}
	os_mutex_unlock(&g->work_mtx);
	gate_leave(g);
}

/*!
 * #2030: a CPU-memory provider result into its GPU mirror (dispatcher thread).
 * CPU ARRAY views hold their layers back to back.
 */
static bool
glue_upload(void *ud, const struct xrt_conv_image *src, const struct xrt_conv_image *dst)
{
	struct vk_conv_gpu *g = (struct vk_conv_gpu *)ud;
	if (!is_cpu_image(src) || !is_glue_image(dst)) {
		return false;
	}
	const uint32_t pitch = src->row_stride != 0 ? src->row_stride : src->width * conv_fmt_bytes(src->format);
	if (!gate_enter(g)) {
		return false;
	}
	struct vkc_img *d = img_find(g, dst->import_key);
	const bool fits = d != NULL && d->w == src->width && d->h == src->height && d->conv_fmt == src->format &&
	                  src->byte_size >= (uint64_t)pitch * src->height * d->layers;
	gate_leave(g);
	return fits && vk_conv_gpu_write(g, dst, src->cpu_ptr, pitch);
}

void
vk_conv_gpu_alloc_ops(struct vk_conv_gpu *g, struct conv_image_alloc_ops *out)
{
	memset(out, 0, sizeof(*out));
	if (g == NULL) {
		return;
	}
	out->alloc = glue_alloc;
	out->free = glue_free;
	out->upload = glue_upload;
	out->ud = g;
}


/*
 *
 * CPU <-> image.
 *
 */

//! Record a staged upload of @p pixels into @p img (work_mtx held, stage sized).
static bool
record_upload(struct vk_conv_gpu *g, VkCommandBuffer cmd, struct vkc_img *img, const void *pixels, uint32_t row_pitch)
{
	const uint32_t row_bytes = img->w * conv_fmt_bytes(img->conv_fmt);
	const uint32_t pitch = row_pitch != 0 ? row_pitch : row_bytes;
	const VkDeviceSize layer_bytes = (VkDeviceSize)row_bytes * img->h;
	if (!stage_ensure(g, layer_bytes * img->layers)) {
		return false;
	}
	for (uint32_t l = 0; l < img->layers; l++) {
		copy_rows((uint8_t *)g->stage_map + layer_bytes * l, row_bytes,
		          (const uint8_t *)pixels + (size_t)pitch * img->h * l, pitch, row_bytes, img->h);
	}
	img_acquire_foreign(g, cmd, img);
	record_buffer_image(g->vk, cmd, g->stage_buf, img, true);
	cmd_global_barrier(g->vk, cmd);
	img_release_foreign(g, cmd, img);
	return true;
}

static bool
write_locked(struct vk_conv_gpu *g, struct vkc_img *img, const void *pixels, uint32_t row_pitch)
{
	VkCommandBuffer cmd;
	if (!cmd_begin(g, &cmd)) {
		return false;
	}
	cmd_global_barrier(g->vk, cmd);
	if (!record_upload(g, cmd, img, pixels, row_pitch)) {
		(void)cmd_submit_wait(g, cmd, NULL);
		return false;
	}
	return cmd_submit_wait(g, cmd, NULL);
}

//! Read @p img into the staging buffer and out to @p pixels (work_mtx held).
static bool
read_locked(struct vk_conv_gpu *g, struct vkc_img *img, void *pixels, uint32_t row_pitch)
{
	const uint32_t row_bytes = img->w * conv_fmt_bytes(img->conv_fmt);
	const uint32_t pitch = row_pitch != 0 ? row_pitch : row_bytes;
	const VkDeviceSize layer_bytes = (VkDeviceSize)row_bytes * img->h;
	VkCommandBuffer cmd;
	if (!stage_ensure(g, layer_bytes * img->layers) || !cmd_begin(g, &cmd)) {
		return false;
	}
	cmd_global_barrier(g->vk, cmd);
	img_acquire_foreign(g, cmd, img);
	record_buffer_image(g->vk, cmd, g->stage_buf, img, false);
	cmd_global_barrier(g->vk, cmd);
	img_release_foreign(g, cmd, img);
	if (!cmd_submit_wait(g, cmd, NULL)) {
		return false;
	}
	for (uint32_t l = 0; l < img->layers; l++) {
		copy_rows((uint8_t *)pixels + (size_t)pitch * img->h * l, pitch,
		          (const uint8_t *)g->stage_map + layer_bytes * l, row_bytes, row_bytes, img->h);
	}
	return true;
}

bool
vk_conv_gpu_write(struct vk_conv_gpu *g, const struct xrt_conv_image *dst, const void *pixels, uint32_t row_pitch)
{
	if (g == NULL || !is_glue_image(dst) || pixels == NULL || !gate_enter(g)) {
		return false;
	}
	bool ok = false;
	os_mutex_lock(&g->work_mtx);
	struct vkc_img *img = img_find(g, dst->import_key);
	if (img != NULL) {
		ok = write_locked(g, img, pixels, row_pitch);
	}
	os_mutex_unlock(&g->work_mtx);
	gate_leave(g);
	return ok;
}

bool
vk_conv_gpu_read(struct vk_conv_gpu *g, const struct xrt_conv_image *src, void *pixels, uint32_t row_pitch)
{
	if (g == NULL || !is_glue_image(src) || pixels == NULL || !gate_enter(g)) {
		return false;
	}
	bool ok = false;
	os_mutex_lock(&g->work_mtx);
	struct vkc_img *img = img_find(g, src->import_key);
	if (img != NULL) {
		ok = read_locked(g, img, pixels, row_pitch);
	}
	os_mutex_unlock(&g->work_mtx);
	gate_leave(g);
	return ok;
}


/*
 *
 * Sync of images the glue reads (work_mtx held).
 *
 */

/*!
 * Add @p x's sync to @p w. False = a sync kind this device cannot wait for
 * (the caller refuses the call rather than racing the producer).
 */
static bool
waits_add(struct vk_conv_gpu *g, struct vkc_waits *w, const struct xrt_conv_image *x)
{
	if (x->sync.kind == XRT_CONV_SYNC_NONE) {
		return true;
	}
	if ((g->caps.sync_kinds & (1u << x->sync.kind)) == 0 || w->count >= VKC_MAX_WAITS) {
		return false;
	}
#if defined(XRT_GRAPHICS_SYNC_HANDLE_IS_FD)
	struct vk_bundle *vk = g->vk;
	if (x->sync.kind == XRT_CONV_SYNC_VK_SEMAPHORE_FD) {
		struct vkc_sem *s = g->sems;
		while (s != NULL && s->key != x->sync.import_key) {
			s = s->next;
		}
		if (s == NULL) {
			// The handle is borrowed: import a duplicate (OPAQUE_FD imports take ownership).
			int fd = dup((int)x->sync.handle);
			VkSemaphore sem = VK_NULL_HANDLE;
			if (fd < 0 || vk_create_timeline_semaphore_from_native(vk, fd, &sem) != VK_SUCCESS) {
				if (fd >= 0) {
					close(fd);
				}
				return false;
			}
			s = U_TYPED_CALLOC(struct vkc_sem);
			s->key = x->sync.import_key;
			s->sem = sem;
			s->next = g->sems;
			g->sems = s;
		}
		w->sem[w->count] = s->sem;
		w->value[w->count] = x->sync.acquire_value;
		w->timeline[w->count] = true;
		w->temp[w->count] = false;
		w->count++;
		return true;
	}
#ifdef XRT_OS_LINUX_DESKTOP
	if (x->sync.kind == XRT_CONV_SYNC_SYNC_FD) {
		VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		VkSemaphore sem = VK_NULL_HANDLE;
		if (vk->vkCreateSemaphore(vk->device, &sci, NULL, &sem) != VK_SUCCESS) {
			return false;
		}
		const int src = (int)(int64_t)x->sync.handle;
		const int fd = src >= 0 ? dup(src) : -1; // -1 = already signalled
		if (vk_semaphore_import_sync_fd(vk, sem, fd) != VK_SUCCESS) {
			vk->vkDestroySemaphore(vk->device, sem, NULL);
			return false;
		}
		w->sem[w->count] = sem;
		w->value[w->count] = 0;
		w->timeline[w->count] = false;
		w->temp[w->count] = true;
		w->count++;
		return true;
	}
#endif
#endif
	return false;
}


/*
 *
 * S5: the reference view synthesis.
 *
 */

//! A per-stream scratch image matching @p conv_fmt / size (work_mtx held).
static struct vkc_img *
scratch_ensure(struct vk_conv_gpu *g, struct vkc_img **slot, uint32_t conv_fmt, uint32_t w, uint32_t h, uint32_t layers)
{
	struct vkc_img *s = *slot;
	if (s != NULL && s->conv_fmt == conv_fmt && s->w == w && s->h == h && s->layers == layers) {
		return s;
	}
	vk_conv_viewsyn_invalidate(g->viewsyn);
	img_destroy(g, s);
	*slot = NULL;
	s = img_create(g, conv_fmt, w, h, layers, 0);
	if (s == NULL || !img_finish(g, s)) {
		return NULL;
	}
	*slot = s;
	return s;
}

static bool
glue_viewsyn_render(void *ud, const struct conv_viewsyn_request *req)
{
	struct vk_conv_gpu *g = (struct vk_conv_gpu *)ud;
	if (req == NULL || req->color == NULL || req->depth == NULL || req->target == NULL || !gate_enter(g)) {
		return false;
	}
	struct vk_bundle *vk = g->vk;
	os_mutex_lock(&g->work_mtx);
	if (g->viewsyn == NULL && !g->viewsyn_tried) {
		g->viewsyn_tried = true;
		g->viewsyn = vk_conv_viewsyn_create(vk);
	}
	bool ok = g->viewsyn != NULL;

	// Resolve the three images: glue images directly, CPU images via scratch.
	struct vkc_img *c = NULL, *d = NULL, *t = NULL;
	const struct xrt_conv_image *imgs[3] = {req->color, req->depth, req->target};
	struct vkc_img **res[3] = {&c, &d, &t};
	struct vkc_stream *st = NULL;
	for (int i = 0; ok && i < 3; i++) {
		const struct xrt_conv_image *x = imgs[i];
		if (is_glue_image(x)) {
			*res[i] = img_find(g, x->import_key);
			ok = *res[i] != NULL;
			if (ok && st == NULL) {
				os_mutex_lock(&g->tab_mtx);
				st = stream_get_locked(g, (*res[i])->stream_id, true);
				os_mutex_unlock(&g->tab_mtx);
			}
		} else if (!is_cpu_image(x)) {
			ok = false;
		}
	}
	// A request with no glue image at all has no stream to hang scratch on.
	ok = ok && st != NULL;
	const uint32_t tlayers = req->layout == XRT_CONV_VIEWS_ARRAY ? (req->view_count > 0 ? req->view_count : 1) : 1;
	if (ok && c == NULL) {
		c = scratch_ensure(g, &st->s_color, req->color->format, req->color->width, req->color->height, 1);
		ok = c != NULL;
	}
	if (ok && d == NULL) {
		d = scratch_ensure(g, &st->s_depth, req->depth->format, req->depth->width, req->depth->height, 1);
		ok = d != NULL;
	}
	if (ok && t == NULL) {
		t = scratch_ensure(g, &st->s_target, req->target->format, req->target->width, req->target->height,
		                   tlayers);
		ok = t != NULL;
	}

	struct vkc_waits waits;
	memset(&waits, 0, sizeof(waits));
	ok = ok && waits_add(g, &waits, req->color) && waits_add(g, &waits, req->depth);

	// CPU inputs: staged uploads into the scratch, each its own submit (one
	// staging buffer), before the synthesis is recorded.
	if (ok && is_cpu_image(req->color)) {
		ok = req->color->byte_size >= (uint64_t)(req->color->row_stride != 0
		                                             ? req->color->row_stride
		                                             : c->w * conv_fmt_bytes(c->conv_fmt)) *
		                                  c->h &&
		     write_locked(g, c, req->color->cpu_ptr, req->color->row_stride);
	}
	if (ok && is_cpu_image(req->depth)) {
		ok = req->depth->byte_size >= (uint64_t)(req->depth->row_stride != 0
		                                             ? req->depth->row_stride
		                                             : d->w * conv_fmt_bytes(d->conv_fmt)) *
		                                  d->h &&
		     write_locked(g, d, req->depth->cpu_ptr, req->depth->row_stride);
	}

	VkCommandBuffer cmd = VK_NULL_HANDLE;
	ok = ok && cmd_begin(g, &cmd);
	bool recorded = false;
	if (ok) {
		struct u_conv_viewsyn_params p;
		u_conv_viewsyn_params_defaults(&p);
		p.view_count = req->view_count;
		for (uint32_t i = 0; i < req->view_count && i < U_CONV_VIEWSYN_MAX_VIEWS; i++) {
			p.viewpoints_m[i][0] = req->viewpoints_m[i][0];
			p.viewpoints_m[i][1] = req->viewpoints_m[i][1];
			p.viewpoints_m[i][2] = req->viewpoints_m[i][2];
		}
		p.rect_width_m = req->rect_width_m;
		p.rect_height_m = req->rect_height_m;
		if (req->depth_info != NULL) {
			p.depth = *req->depth_info;
		}
		p.range_near = req->range_near;
		p.range_far = req->range_far;
		p.convergence = req->convergence;
		p.budget = req->budget;
		p.rear_cap_m = req->rear_cap_m;

		struct vk_conv_viewsyn_inputs in;
		memset(&in, 0, sizeof(in));
		in.color = c->view;
		in.color_width = c->w;
		in.color_height = c->h;
		in.depth = d->view;
		in.depth_width = d->w;
		in.depth_height = d->h;
		in.input_layout = VK_IMAGE_LAYOUT_GENERAL;
		in.new_content = req->new_content;
		struct vk_conv_viewsyn_target tg;
		memset(&tg, 0, sizeof(tg));
		tg.image = t->image;
		tg.format = t->format;
		tg.width = t->w;
		tg.height = t->h;
		tg.array_layers = t->layers;
		tg.layout = VK_IMAGE_LAYOUT_GENERAL;
		tg.layout_kind = req->layout;
		img_acquire_foreign(g, cmd, c);
		img_acquire_foreign(g, cmd, d);
		img_acquire_foreign(g, cmd, t);
		recorded = vk_conv_viewsyn_record(g->viewsyn, cmd, &in, &p, &tg);
		img_release_foreign(g, cmd, c);
		img_release_foreign(g, cmd, d);
		img_release_foreign(g, cmd, t);
	}
	if (cmd != VK_NULL_HANDLE) {
		// Submit even a failed recording: its uploads / barriers are harmless,
		// and the command buffer must go back to the pool.
		const bool submitted = cmd_submit_wait(g, cmd, &waits);
		ok = ok && submitted;
		if (!submitted) {
			vk_conv_viewsyn_invalidate(g->viewsyn);
		}
	} else {
		waits_release(g, &waits);
	}
	ok = ok && recorded;
	// A CPU target: read the views back into it.
	if (ok && is_cpu_image(req->target)) {
		const uint32_t row_bytes = t->w * conv_fmt_bytes(t->conv_fmt);
		const uint32_t pitch = req->target->row_stride != 0 ? req->target->row_stride : row_bytes;
		ok = req->target->byte_size >= (uint64_t)pitch * t->h * t->layers &&
		     read_locked(g, t, req->target->cpu_ptr, pitch);
	}
	os_mutex_unlock(&g->work_mtx);
	gate_leave(g);
	return ok;
}


/*
 *
 * S6: depth readback. Never stalls: one copy in flight per stream, issued at
 * most every U_LIFT_AUTOCONV_SAMPLE_PERIOD_NS and polled (vkGetFenceStatus) on
 * a later conversion; the samples returned are of an earlier conversion of
 * the same stream (the D3D11 glue's cadence).
 *
 */

static bool
glue_rb_read(void *ud,
             const struct xrt_conv_image *depth,
             const float **out_samples,
             uint32_t *out_w,
             uint32_t *out_h,
             uint32_t *out_stride_floats)
{
	struct vk_conv_gpu *g = (struct vk_conv_gpu *)ud;
	if (!is_glue_image(depth) ||
	    (depth->format != XRT_CONV_FMT_R32_FLOAT && depth->format != XRT_CONV_FMT_R16_FLOAT) || !gate_enter(g)) {
		return false;
	}
	struct vk_bundle *vk = g->vk;
	os_mutex_lock(&g->work_mtx);
	struct vkc_img *img = img_find(g, depth->import_key);
	struct vkc_stream *s = NULL;
	if (img != NULL) {
		os_mutex_lock(&g->tab_mtx);
		s = stream_get_locked(g, img->stream_id, true);
		os_mutex_unlock(&g->tab_mtx);
	}
	bool fresh = false;
	if (s != NULL && s->rb_pending && vk->vkGetFenceStatus(vk->device, s->rb_fence) == VK_SUCCESS) {
		s->rb_pending = false;
		cmd_free(g, s->rb_cmd);
		s->rb_cmd = VK_NULL_HANDLE;
		const size_t n = (size_t)s->rb_w * s->rb_h;
		if (s->samples_cap < n) {
			free(s->samples);
			s->samples = U_TYPED_ARRAY_CALLOC(float, n);
			s->samples_cap = s->samples != NULL ? n : 0;
		}
		if (s->samples != NULL) {
			if (s->rb_fmt == XRT_CONV_FMT_R32_FLOAT) {
				memcpy(s->samples, s->rb_map, n * sizeof(float));
			} else {
				const uint16_t *h = (const uint16_t *)s->rb_map;
				for (size_t i = 0; i < n; i++) {
					s->samples[i] = half_to_float(h[i]);
				}
			}
			fresh = true;
		}
	}
	const uint64_t now = os_monotonic_get_ns();
	if (s != NULL && img != NULL && !s->rb_pending &&
	    (s->rb_issued_ns == 0 || now - s->rb_issued_ns >= U_LIFT_AUTOCONV_SAMPLE_PERIOD_NS)) {
		const VkDeviceSize bytes = (VkDeviceSize)img->w * img->h * conv_fmt_bytes(img->conv_fmt);
		bool ready = true;
		if (s->rb_buf == VK_NULL_HANDLE || s->rb_size < bytes) {
			buffer_free(vk, &s->rb_buf, &s->rb_mem, &s->rb_map);
			ready = buffer_make(vk, bytes, &s->rb_buf, &s->rb_mem, &s->rb_map);
			s->rb_size = ready ? bytes : 0;
		}
		if (ready && s->rb_fence == VK_NULL_HANDLE) {
			VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
			ready = vk->vkCreateFence(vk->device, &fci, NULL, &s->rb_fence) == VK_SUCCESS;
		}
		VkCommandBuffer cmd = VK_NULL_HANDLE;
		ready = ready && vk->vkResetFences(vk->device, 1, &s->rb_fence) == VK_SUCCESS && cmd_begin(g, &cmd);
		if (ready) {
			// Only layer 0 (depth is one layer); the buffer is tight.
			cmd_global_barrier(vk, cmd);
			img_acquire_foreign(g, cmd, img);
			VkBufferImageCopy region = {
			    .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
			    .imageExtent = {img->w, img->h, 1},
			};
			vk->vkCmdCopyImageToBuffer(cmd, img->image, VK_IMAGE_LAYOUT_GENERAL, s->rb_buf, 1, &region);
			VkMemoryBarrier to_host = {
			    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
			    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			    .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
			};
			vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
			                         &to_host, 0, NULL, 0, NULL);
			img_release_foreign(g, cmd, img);
			if (cmd_submit(g, cmd, NULL, s->rb_fence)) {
				s->rb_cmd = cmd;
				s->rb_pending = true;
				s->rb_issued_ns = now;
				s->rb_src_key = img->key;
				s->rb_w = img->w;
				s->rb_h = img->h;
				s->rb_fmt = img->conv_fmt;
			} else {
				cmd_free(g, cmd);
			}
		}
	}
	if (fresh) {
		*out_samples = s->samples;
		*out_w = s->rb_w;
		*out_h = s->rb_h;
		*out_stride_floats = s->rb_w;
	}
	os_mutex_unlock(&g->work_mtx);
	gate_leave(g);
	return fresh;
}

static void
glue_rb_release(void *ud, const float *samples)
{
	(void)ud;
	(void)samples; // owned by the stream's readback state until its next read
}

void
vk_conv_gpu_stage_ops(struct vk_conv_gpu *g, struct conv_viewsyn_ops *viewsyn, struct conv_depth_readback_ops *readback)
{
	if (viewsyn != NULL) {
		memset(viewsyn, 0, sizeof(*viewsyn));
		if (g != NULL) {
			viewsyn->render = glue_viewsyn_render;
			viewsyn->ud = g;
		}
	}
	if (readback != NULL) {
		memset(readback, 0, sizeof(*readback));
		if (g != NULL) {
			readback->read = glue_rb_read;
			readback->release = glue_rb_release;
			readback->ud = g;
		}
	}
}


/*
 *
 * Lifecycle.
 *
 */

static void
probe_caps(struct vk_conv_gpu *g)
{
	struct vk_bundle *vk = g->vk;
	struct vk_conv_gpu_caps *c = &g->caps;
	memset(c, 0, sizeof(*c));
	c->memory_kinds = XRT_CONV_MEM_CPU;
	c->sync_kinds = 1u << XRT_CONV_SYNC_NONE;
	c->timeline_semaphore = vk->features.timeline_semaphore;
	const VkImageUsageFlags usage =
	    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	(void)usage;

#if defined(XRT_GRAPHICS_BUFFER_HANDLE_IS_FD)
	if (vk->vkGetMemoryFdKHR != NULL && format_exportable(vk, VK_FORMAT_R8G8B8A8_UNORM, 0, usage, 1,
	                                                      VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT)) {
		c->memory_kinds |= XRT_CONV_MEM_VK_OPAQUE_FD;
	}
#endif
#ifdef XRT_OS_LINUX_DESKTOP
	if (vk_dmabuf_supported(vk)) {
		c->memory_kinds |= XRT_CONV_MEM_DMABUF;
	}
	if (vk_dmabuf_sync_fd_supported(vk)) {
		c->sync_kinds |= 1u << XRT_CONV_SYNC_SYNC_FD;
	}
#endif
#if defined(XRT_GRAPHICS_BUFFER_HANDLE_IS_METAL)
	if (vk->has_EXT_metal_objects && vk->has_EXT_external_memory_metal) {
		c->memory_kinds |= XRT_CONV_MEM_IOSURFACE;
	}
#endif
#if defined(XRT_GRAPHICS_SYNC_HANDLE_IS_FD) && defined(VK_KHR_timeline_semaphore)
	if (c->timeline_semaphore && vk->has_KHR_external_semaphore_fd &&
	    vk->vkGetPhysicalDeviceExternalSemaphorePropertiesKHR != NULL) {
		VkSemaphoreTypeCreateInfoKHR type = {
		    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO_KHR,
		    .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE_KHR,
		};
		VkPhysicalDeviceExternalSemaphoreInfo info = {
		    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
		    .pNext = &type,
		    .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,
		};
		VkExternalSemaphoreProperties props = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
		vk->vkGetPhysicalDeviceExternalSemaphorePropertiesKHR(vk->physical_device, &info, &props);
		if ((props.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT) != 0) {
			c->sync_kinds |= 1u << XRT_CONV_SYNC_VK_SEMAPHORE_FD;
		}
	}
#endif
}

bool
vk_conv_gpu_create(struct vk_bundle *vk, struct vk_conv_gpu **out)
{
	if (vk == NULL || vk->device == VK_NULL_HANDLE || vk->main_queue == NULL || out == NULL) {
		return false;
	}
	struct vk_conv_gpu *g = U_TYPED_CALLOC(struct vk_conv_gpu);
	g->vk = vk;
	if (os_mutex_init(&g->gate_mtx) != 0 || os_cond_init(&g->gate_cond) != 0 || os_mutex_init(&g->tab_mtx) != 0 ||
	    os_mutex_init(&g->work_mtx) != 0) {
		free(g);
		return false;
	}
	VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	if (vk_cmd_pool_init(vk, &g->pool, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT) != VK_SUCCESS) {
		os_mutex_destroy(&g->work_mtx);
		os_mutex_destroy(&g->tab_mtx);
		os_cond_destroy(&g->gate_cond);
		os_mutex_destroy(&g->gate_mtx);
		free(g);
		return false;
	}
	if (vk->vkCreateFence(vk->device, &fci, NULL, &g->fence) != VK_SUCCESS) {
		vk_cmd_pool_destroy(vk, &g->pool);
		os_mutex_destroy(&g->work_mtx);
		os_mutex_destroy(&g->tab_mtx);
		os_cond_destroy(&g->gate_cond);
		os_mutex_destroy(&g->gate_mtx);
		free(g);
		return false;
	}
	probe_caps(g);
	*out = g;
	return true;
}

void
vk_conv_gpu_destroy(struct vk_conv_gpu **g_ptr)
{
	if (g_ptr == NULL || *g_ptr == NULL) {
		return;
	}
	struct vk_conv_gpu *g = *g_ptr;
	struct vk_bundle *vk = g->vk;
	os_mutex_lock(&g->gate_mtx);
	g->closed = true;
	while (g->calls > 0) {
		os_cond_wait(&g->gate_cond, &g->gate_mtx);
	}
	os_mutex_unlock(&g->gate_mtx);

	os_mutex_lock(&g->work_mtx);
	while (g->streams != NULL) {
		struct vkc_stream *s = g->streams;
		g->streams = s->next;
		stream_free(g, s);
	}
	while (g->imgs != NULL) {
		struct vkc_img *i = g->imgs;
		g->imgs = i->next;
		img_destroy(g, i);
	}
	while (g->sems != NULL) {
		struct vkc_sem *s = g->sems;
		g->sems = s->next;
		vk->vkDestroySemaphore(vk->device, s->sem, NULL);
		free(s);
	}
	vk_conv_viewsyn_destroy(&g->viewsyn);
	buffer_free(vk, &g->stage_buf, &g->stage_mem, &g->stage_map);
	vk->vkDestroyFence(vk->device, g->fence, NULL);
	vk_cmd_pool_destroy(vk, &g->pool);
	os_mutex_unlock(&g->work_mtx);

	os_mutex_destroy(&g->work_mtx);
	os_mutex_destroy(&g->tab_mtx);
	os_cond_destroy(&g->gate_cond);
	os_mutex_destroy(&g->gate_mtx);
	free(g);
	*g_ptr = NULL;
}

bool
vk_conv_gpu_lookup(struct vk_conv_gpu *g, const struct xrt_conv_image *x, struct vk_conv_gpu_image_ref *out)
{
	memset(out, 0, sizeof(*out));
	if (g == NULL || !is_glue_image(x) || !gate_enter(g)) {
		return false;
	}
	struct vkc_img *img = img_find(g, x->import_key);
	if (img != NULL) {
		out->image = img->image;
		out->format = img->format;
		out->width = img->w;
		out->height = img->h;
		out->layers = img->layers;
		out->foreign = img->foreign;
	}
	gate_leave(g);
	return img != NULL;
}

void
vk_conv_gpu_cmd_acquire(struct vk_conv_gpu *g, VkCommandBuffer cmd, const struct vk_conv_gpu_image_ref *ref)
{
#ifdef XRT_OS_LINUX_DESKTOP
	if (g != NULL && ref != NULL && ref->foreign) {
		vk_dmabuf_cmd_acquire_foreign(g->vk, cmd, ref->image, VK_IMAGE_LAYOUT_GENERAL,
		                              VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		                              VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
	}
#else
	(void)g;
	(void)cmd;
	(void)ref;
#endif
}

void
vk_conv_gpu_cmd_release(struct vk_conv_gpu *g, VkCommandBuffer cmd, const struct vk_conv_gpu_image_ref *ref)
{
#ifdef XRT_OS_LINUX_DESKTOP
	if (g != NULL && ref != NULL && ref->foreign) {
		vk_dmabuf_cmd_release_foreign(g->vk, cmd, ref->image, VK_IMAGE_LAYOUT_GENERAL,
		                              VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT);
	}
#else
	(void)g;
	(void)cmd;
	(void)ref;
#endif
}

void
vk_conv_gpu_get_caps(const struct vk_conv_gpu *g, struct vk_conv_gpu_caps *out)
{
	if (g == NULL) {
		memset(out, 0, sizeof(*out));
		return;
	}
	*out = g->caps;
}

void
vk_conv_gpu_set_provider_kinds(struct vk_conv_gpu *g, uint32_t memory_kinds)
{
	if (g != NULL) {
		g->provider_kinds = memory_kinds & g->caps.memory_kinds & ~XRT_CONV_MEM_CPU;
	}
}

uint64_t
vk_conv_gpu_viewsyn_prepass_count(struct vk_conv_gpu *g)
{
	if (g == NULL) {
		return 0;
	}
	os_mutex_lock(&g->work_mtx);
	const uint64_t n = vk_conv_viewsyn_prepass_count(g->viewsyn);
	os_mutex_unlock(&g->work_mtx);
	return n;
}

uint32_t
vk_conv_gpu_image_count(struct vk_conv_gpu *g)
{
	if (g == NULL) {
		return 0;
	}
	os_mutex_lock(&g->tab_mtx);
	const uint32_t n = g->img_count;
	os_mutex_unlock(&g->tab_mtx);
	return n;
}
