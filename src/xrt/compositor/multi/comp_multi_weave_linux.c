// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_weave on the desktop-Linux service path (#1699 piece R2) — the
 *         Linux sibling of comp_multi_weave_macos.c (#759) and
 *         comp_multi_weave_android.c (#1036).
 * @ingroup comp_multi
 *
 * A window-bound weave service for present-owners on desktop Linux: the caller
 * hands the runtime pre-weave side-by-side (or v6 N-view) pixels plus
 * window-relative rect(s), and gets back a woven buffer it composites into its
 * own window. The caller NEVER weaves (ADR-007 / ADR-019) — the interlace is the
 * vendor DP's calibrated shader, behind the plug-in.
 *
 * Service topology: the desktop-Linux service runs null_compositor + comp_multi
 * (never vk_native). The engine takes the service's vk_bundle through
 * comp_target_service_get_vk (#1702) and builds ITS OWN display processor from
 * the active plug-in's Vulkan factory (`info.dp_factory_vk`), exactly like the
 * macOS / Android engines. The snap and the phase feed therefore go through
 * this engine's DP, not through any compositor's.
 *
 * Linux platform mapping (vs the Android original):
 *
 *  - Handles are fds (XRT_GRAPHICS_BUFFER_HANDLE_IS_FD,
 *    CONSUMED_BY_VULKAN_IMPORT). An fd is NOT an identity — every SCM_RIGHTS
 *    receive installs a new number — and carries no dims, format or tiling.
 *  - STAGE A (this file today): the plain-handle submit carries an OPAQUE_FD
 *    exported by the SAME driver on the SAME physical device. It is imported
 *    with the tree helper vk_create_image_from_native. The image parameters are
 *    a fixed contract (see WEAVE_STAGE_A_BITS / WEAVE_VK_FORMAT below); the
 *    extent is inferred from the submit (see weave_stage_a_input_dims), and the
 *    allocation size is probed with lseek(fd, 0, SEEK_END).
 *  - STAGE B (R4 wire + R5 aux_vk helpers): a dma-buf with a DRM format
 *    modifier plus sync_file acquire / release fences, through
 *    comp_multi_weave_submit_dmabuf / comp_multi_weave_export_output_dmabuf.
 *    Stubs today — look for the `R5:` / `R4:` markers.
 *  - fd ownership (the handler hands the engine every received fd):
 *      1. every received fd is closed by the engine exactly once, in the submit
 *         epilogue — on a cache hit, a miss, a refusal and an error alike;
 *      2. an import never consumes the received fd: it is handed a dup(), which
 *         a successful import consumes (the driver owns it) and a failed import
 *         does not (the engine closes it — Vulkan never takes ownership on
 *         failure);
 *      3. the output is exported ONCE per allocation (vkGetMemoryFdKHR mints a
 *         new fd per call and the IPC send dups without closing), cached, and
 *         closed on release; export_output hands the cached number out.
 *  - Queue-family ownership: the imported input / overlay are acquired from the
 *    producer's queue family and released back every frame (never the macOS
 *    GENERAL-only pattern). For a same-driver OPAQUE_FD that family is
 *    VK_QUEUE_FAMILY_EXTERNAL (core since 1.1, no extension); for a stage-B
 *    dma-buf it is VK_QUEUE_FAMILY_FOREIGN_EXT, which needs
 *    VK_EXT_queue_family_foreign enabled on the service device first (R5). The
 *    layout contract across the boundary is GENERAL on both sides, with
 *    oldLayout == newLayout == GENERAL in the ownership-transfer barriers.
 *  - Completion: SYNCHRONOUS in stage A — a bounded (1 s) fence wait under the
 *    queue lock before the IPC reply, so xrWeaveSubmitDXR returning IS the
 *    completion signal. export_fence reports none; fenceValue is a monotonic
 *    counter.
 *  - Window geometry: the caller publishes it (spec v7 XrWeaveWindowGeometryDXR,
 *    device pixels, desktop-absolute). A Linux service cannot derive it — on
 *    Wayland a client is never told its position, and the window owner is not
 *    necessarily the runtime client. It is fed to the DP every submit as a
 *    panel-relative present origin (`set_present_origin`, the slot the Linux
 *    vendor DP honours in-process), and serves window metrics (#1116).
 *
 * The weave itself is ONE process_atlas per submit — the same one-weave-per-frame
 * batch strategy as every other platform.
 */

#include "xrt/xrt_compositor.h"
#include "xrt/xrt_display_processor.h"
#include "xrt/xrt_display_processor_vk.h"
#include "xrt/xrt_display_metrics.h"
#include "xrt/xrt_handles.h"
#include "xrt/xrt_weave_dmabuf.h"

#include "util/u_misc.h"
#include "util/u_logging.h"
#include "util/u_handles.h"
#include "util/u_debug.h"

#include "vk/vk_helpers.h"
#include "vk/vk_image_allocator.h"
#include "vk/vk_local2d_composite.h"

#include "comp_multi_private.h"

#ifdef XRT_OS_LINUX_DESKTOP

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Kill-switch for the self-submitting-DP split submission (default ON) — the
// Android #1036 ordering fix, which applies to any DP that submits its own
// command buffer during process_atlas.
DEBUG_GET_ONCE_BOOL_OPTION(dxr_linux_weave_split, "DXR_LINUX_WEAVE_SPLIT", true)

/*
 *
 * Stage-A image contract.
 *
 */

/*!
 * BGRA8 UNORM end to end: the DP factory's target format, the output and the
 * stage-A input. The DP builds its render pass once, against this format.
 * (Stage B maps each dma-buf's DRM fourcc to its own VkFormat for the INPUT;
 * the output / DP format stays this.)
 */
#define WEAVE_VK_FORMAT VK_FORMAT_B8G8R8A8_UNORM

/*!
 * Usage every stage-A image is created with — on BOTH sides. A same-driver
 * OPAQUE_FD import of a dedicated allocation must be created with parameters
 * identical to the exporter's, so this set is part of the wire contract, not a
 * local choice: a producer renders into its input (COLOR) or uploads it
 * (TRANSFER_DST); the engine blits or samples it (TRANSFER_SRC / SAMPLED). The
 * output uses the same set, so the caller imports it with the same parameters.
 */
#define WEAVE_STAGE_A_BITS                                                                                             \
	((enum xrt_swapchain_usage_bits)(XRT_SWAPCHAIN_USAGE_COLOR | XRT_SWAPCHAIN_USAGE_SAMPLED |                     \
	                                 XRT_SWAPCHAIN_USAGE_TRANSFER_SRC | XRT_SWAPCHAIN_USAGE_TRANSFER_DST))

//! Bounded completion wait (Android #967b style): a wedged weaver must not hang
//! the caller's present thread forever behind a synchronous IPC call.
#define WEAVE_FENCE_TIMEOUT_NS (1000ULL * 1000ULL * 1000ULL)


/*
 *
 * Helpers.
 *
 */

static struct vk_bundle *
weave_get_vk(struct multi_compositor *mc)
{
	if (mc == NULL || mc->msc == NULL || mc->msc->target_service == NULL) {
		return NULL;
	}
	return comp_target_service_get_vk(mc->msc->target_service);
}

/*!
 * Serializes the lazy creation of every client's weave engine lock — a
 * file-static LEAF mutex (the Android #1387 d1 fix), never held while any other
 * lock is taken, so weave_ensure_mutex is safe under any caller's lock.
 */
static pthread_mutex_t g_weave_mutex_init_lock = PTHREAD_MUTEX_INITIALIZER;

//! Lazily create the per-client engine lock + the fd fields' -1 sentinels
//! (multi_compositor is zero-alloced, and 0 is a valid fd).
static void
weave_ensure_mutex(struct multi_compositor *mc)
{
	pthread_mutex_lock(&g_weave_mutex_init_lock);
	if (!mc->weave.mutex_initialized) {
		os_mutex_init(&mc->weave.mutex);
		mc->weave.out_fd = -1;
		mc->weave.mutex_initialized = true;
	}
	pthread_mutex_unlock(&g_weave_mutex_init_lock);
}

static void
weave_close_fd(int *fd)
{
	if (*fd >= 0) {
		close(*fd);
		*fd = -1;
	}
}

static void
weave_queue_wait_idle(struct vk_bundle *vk)
{
	vk_queue_lock(vk->main_queue);
	vk->vkQueueWaitIdle(vk->main_queue->queue);
	vk_queue_unlock(vk->main_queue);
}

//! The swapchain-create-info shape the tree's image helpers speak, for one 2D image.
static struct xrt_swapchain_create_info
weave_sci(uint32_t w, uint32_t h, VkFormat format, enum xrt_swapchain_usage_bits bits)
{
	struct xrt_swapchain_create_info info = {
	    .create = 0,
	    .bits = bits,
	    .format = (uint32_t)format,
	    .sample_count = 1,
	    .width = w,
	    .height = h,
	    .face_count = 1,
	    .array_size = 1,
	    .mip_count = 1,
	};
	return info;
}

/*!
 * Does @p fd still name the file (@p dev, @p ino)? Used after a failed import to
 * decide whether the dup it was handed is still ours to close: Vulkan never takes
 * ownership on a failed import, but a bind failure AFTER a successful allocation
 * (vk_alloc_and_bind_image_memory frees the memory) did consume it, and the
 * number may since have been reused by another thread. Closing only when the
 * number still names the same file keeps both cases correct.
 */
static bool
weave_fd_is_file(int fd, uint64_t dev, uint64_t ino)
{
	struct stat st;
	if (fd < 0 || fstat(fd, &st) != 0) {
		return false;
	}
	return (uint64_t)st.st_dev == dev && (uint64_t)st.st_ino == ino;
}

/*!
 * Stage A: import a same-driver OPAQUE_FD into a VkImage (+ full-image view).
 *
 * @p fd is BORROWED — it stays the caller's (the submit epilogue closes it). The
 * import is handed a dup(), per the ownership rules in the file header.
 */
static bool
weave_import_opaque_fd(struct vk_bundle *vk,
                       struct multi_compositor *mc,
                       int fd,
                       uint64_t dev,
                       uint64_t ino,
                       uint32_t w,
                       uint32_t h,
                       VkImage *out_image,
                       VkDeviceMemory *out_memory,
                       VkImageView *out_view,
                       const char *what)
{
	if (w == 0 || h == 0) {
		U_LOG_E(
		    "weave(#1699): %s import has zero dimensions — stage A infers them from the submit "
		    "(bind geometry for a batch submit, the packed grid for v6)",
		    what);
		return false;
	}

	/*
	 * Allocation size. vk_create_image_from_native rejects an import whose
	 * requirements exceed image_native->size. The fd carries its size only as
	 * a file size: a dma-buf (which is what Mesa hands out for OPAQUE_FD)
	 * answers lseek(SEEK_END); an fd that cannot be sized gets "unknown" (the
	 * check is then skipped and the driver's own requirement is used, which is
	 * exactly the allocation size for an identical same-driver image). The
	 * offset is shared with the producer's open file description, so it is
	 * put back.
	 */
	uint64_t size = UINT64_MAX;
	off_t end = lseek(fd, 0, SEEK_END);
	if (end > 0) {
		size = (uint64_t)end;
		(void)lseek(fd, 0, SEEK_SET);
	} else if (!mc->weave.size_probe_warned) {
		mc->weave.size_probe_warned = true;
		U_LOG_W(
		    "weave(#1699): lseek cannot size the %s fd (%s) — trusting the same-driver import's "
		    "own memory requirements",
		    what, end < 0 ? strerror(errno) : "size 0");
	}

	int import_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
	if (import_fd < 0) {
		U_LOG_E("weave(#1699): dup(%s fd) failed: %s", what, strerror(errno));
		return false;
	}

	struct xrt_swapchain_create_info info = weave_sci(w, h, WEAVE_VK_FORMAT, WEAVE_STAGE_A_BITS);
	struct xrt_image_native image_native = {
	    .handle = import_fd,
	    .size = size,
	    .use_dedicated_allocation = true,
	};

	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkResult ret = vk_create_image_from_native(vk, &info, &image_native, &image, &memory);
	if (ret != VK_SUCCESS) {
		// A failed import never takes ownership (Vulkan spec) — unless the
		// failure was a bind after a successful allocation; see the helper.
		if (weave_fd_is_file(import_fd, dev, ino)) {
			close(import_fd);
		}
		U_LOG_E("weave(#1699): vk_create_image_from_native(%s, OPAQUE_FD %ux%u) failed: %s", what, w, h,
		        vk_result_string(ret));
		return false;
	}
	// Success: the driver owns import_fd now.

	VkImageSubresourceRange range = {
	    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	    .levelCount = 1,
	    .layerCount = 1,
	};
	VkImageView view = VK_NULL_HANDLE;
	ret = vk_create_view(vk, image, VK_IMAGE_VIEW_TYPE_2D, WEAVE_VK_FORMAT, range, &view);
	if (ret != VK_SUCCESS) {
		U_LOG_E("weave(#1699): vk_create_view(%s) failed: %s", what, vk_result_string(ret));
		vk->vkDestroyImage(vk->device, image, NULL);
		vk->vkFreeMemory(vk->device, memory, NULL);
		return false;
	}

	*out_image = image;
	*out_memory = memory;
	*out_view = view;
	return true;
}

static void
weave_slot_release(struct vk_bundle *vk, struct comp_multi_weave_linux_slot *slot)
{
	if (slot->view != VK_NULL_HANDLE) {
		vk->vkDestroyImageView(vk->device, slot->view, NULL);
	}
	if (slot->image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, slot->image, NULL);
	}
	if (slot->memory != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, slot->memory, NULL);
	}
	U_ZERO(slot);
}

static void
weave_slots_release_all(struct vk_bundle *vk, struct comp_multi_weave_linux_slot *slots)
{
	for (uint32_t i = 0; i < COMP_MULTI_WEAVE_LINUX_SLOTS; i++) {
		if (slots[i].used) {
			weave_slot_release(vk, &slots[i]);
		}
	}
}

/*!
 * Look @p fd up in @p slots, importing it on a miss. Returns the slot to read
 * from, or NULL. @p fd stays BORROWED either way (closed by the submit
 * epilogue): a hit needs nothing from it, a miss hands the import a dup.
 *
 * Key: the caller's @p buffer_id when non-zero (stage B), else the fd's file
 * (st_dev, st_ino) — every SCM_RIGHTS receive of one buffer is the same open
 * file description, so the same inode. Our cached import keeps the underlying
 * memory alive, so its inode cannot be recycled for another buffer while the
 * slot exists. A hit with different dims (the producer reallocated at the same
 * identity) re-imports into that slot.
 */
static struct comp_multi_weave_linux_slot *
weave_cache_acquire(struct vk_bundle *vk,
                    struct multi_compositor *mc,
                    struct comp_multi_weave_linux_slot *slots,
                    int fd,
                    uint64_t buffer_id,
                    uint32_t w,
                    uint32_t h,
                    const char *what)
{
	struct stat st;
	if (fstat(fd, &st) != 0) {
		U_LOG_E("weave(#1699): fstat(%s fd %d) failed: %s", what, fd, strerror(errno));
		return NULL;
	}
	const uint64_t dev = (uint64_t)st.st_dev;
	const uint64_t ino = (uint64_t)st.st_ino;
	const uint64_t tick = ++mc->weave.slot_clock;

	struct comp_multi_weave_linux_slot *victim = NULL;
	for (uint32_t i = 0; i < COMP_MULTI_WEAVE_LINUX_SLOTS; i++) {
		struct comp_multi_weave_linux_slot *s = &slots[i];
		if (!s->used) {
			if (victim == NULL || victim->used) {
				victim = s; // prefer an empty slot
			}
			continue;
		}
		const bool same = buffer_id != 0 ? s->buffer_id == buffer_id : (s->dev == dev && s->ino == ino);
		if (same) {
			if (s->w == w && s->h == h && s->format == WEAVE_VK_FORMAT) {
				s->last_used = tick;
				return s; // hit
			}
			victim = s; // same buffer identity, new extent: re-import in place
			break;
		}
		if (victim == NULL || (victim->used && s->last_used < victim->last_used)) {
			victim = s; // LRU
		}
	}

	if (victim->used) {
		// The slot may still be referenced by work another path left in
		// flight (a timed-out fence). Rare (misses only) — never yank it.
		weave_queue_wait_idle(vk);
		weave_slot_release(vk, victim);
	}

	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
	// R5: a stage-B dma-buf descriptor takes vk_create_image_from_dmabuf here
	// (explicit modifier, DMA_BUF handle type; handed a dup, which it always
	// consumes) with ext_queue_family = VK_QUEUE_FAMILY_FOREIGN_EXT, and its
	// barriers become vk_dmabuf_cmd_acquire_foreign / _release_foreign — once
	// the service device enables VK_EXT_external_memory_dma_buf /
	// _image_drm_format_modifier / _queue_family_foreign (vk_dmabuf_supported).
	if (!weave_import_opaque_fd(vk, mc, fd, dev, ino, w, h, &image, &memory, &view, what)) {
		return NULL;
	}

	victim->used = true;
	victim->buffer_id = buffer_id;
	victim->dev = dev;
	victim->ino = ino;
	victim->image = image;
	victim->memory = memory;
	victim->view = view;
	victim->w = w;
	victim->h = h;
	victim->format = WEAVE_VK_FORMAT;
	victim->ext_queue_family = VK_QUEUE_FAMILY_EXTERNAL;
	victim->first_use = true;
	victim->last_used = tick;
	U_LOG_W("weave(#1699): %s import cached (OPAQUE_FD %ux%u, inode %" PRIu64 ")", what, w, h, ino);
	return victim;
}

/*!
 * Stage A: the extent of the OPAQUE_FD input. An fd carries no dims and the
 * plain-handle wire carries none either, so they are inferred from the
 * submit's layout contract:
 *
 *  - v6 N-view: the packed grid (tile_columns x content_view_w by tile_rows x
 *    content_view_h) — i.e. stage A supports the zero-copy case only, where
 *    the atlas the caller allocated is exactly the packed region. A
 *    worst-case-sized atlas needs its extent on the wire (stage B descriptor).
 *  - batch (rect_count > 0): the input is window-client-sized, so the bound
 *    geometry's client size. Without geometry, the rects' bounding box.
 *  - legacy single rect: the geometry's client size, else rect offset+extent.
 */
static void
weave_stage_a_input_dims(struct multi_compositor *mc,
                         int32_t rect_x,
                         int32_t rect_y,
                         uint32_t rect_w,
                         uint32_t rect_h,
                         uint32_t rect_count,
                         const struct xrt_rect *rects,
                         const struct xrt_weave_atlas_layout *layout,
                         uint32_t *out_w,
                         uint32_t *out_h)
{
	*out_w = 0;
	*out_h = 0;
	if (layout != NULL && layout->view_count > 0) {
		*out_w = layout->tile_columns * layout->content_view_w;
		*out_h = layout->tile_rows * layout->content_view_h;
		return;
	}
	if (mc->weave.have_geometry) {
		*out_w = mc->weave.win_w;
		*out_h = mc->weave.win_h;
		return;
	}
	if (rect_count > 0 && rects != NULL) {
		int64_t max_x = 0, max_y = 0;
		for (uint32_t i = 0; i < rect_count; i++) {
			int64_t rx = (int64_t)rects[i].offset.w + rects[i].extent.w; // offset.w/h hold x/y
			int64_t ry = (int64_t)rects[i].offset.h + rects[i].extent.h;
			max_x = rx > max_x ? rx : max_x;
			max_y = ry > max_y ? ry : max_y;
		}
		*out_w = max_x > 0 ? (uint32_t)max_x : 0;
		*out_h = max_y > 0 ? (uint32_t)max_y : 0;
		return;
	}
	if (rect_x >= 0 && rect_y >= 0) {
		*out_w = (uint32_t)rect_x + rect_w;
		*out_h = (uint32_t)rect_y + rect_h;
	}
}

//! Plain device-local image + view (the SBS scratch atlas / the v6 crop staging).
static bool
weave_create_local(
    struct vk_bundle *vk, uint32_t w, uint32_t h, VkImage *out_image, VkDeviceMemory *out_memory, VkImageView *out_view)
{
	VkExtent2D extent = {.width = w, .height = h};
	VkResult ret =
	    vk_create_image_simple(vk, extent, WEAVE_VK_FORMAT,
	                           VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, out_memory, out_image);
	if (ret != VK_SUCCESS) {
		U_LOG_E("weave(#1699): vk_create_image_simple(%ux%u) failed: %s", w, h, vk_result_string(ret));
		return false;
	}

	VkImageSubresourceRange range = {
	    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	    .levelCount = 1,
	    .layerCount = 1,
	};
	ret = vk_create_view(vk, *out_image, VK_IMAGE_VIEW_TYPE_2D, WEAVE_VK_FORMAT, range, out_view);
	if (ret != VK_SUCCESS) {
		U_LOG_E("weave(#1699): vk_create_view(local) failed: %s", vk_result_string(ret));
		return false;
	}
	return true;
}

static void
weave_release_local(struct vk_bundle *vk, VkImage *image, VkDeviceMemory *memory, VkImageView *view)
{
	if (*view != VK_NULL_HANDLE) {
		vk->vkDestroyImageView(vk->device, *view, NULL);
		*view = VK_NULL_HANDLE;
	}
	if (*image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, *image, NULL);
		*image = VK_NULL_HANDLE;
	}
	if (*memory != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, *memory, NULL);
		*memory = VK_NULL_HANDLE;
	}
}

static void
weave_release_scratch(struct vk_bundle *vk, struct multi_compositor *mc)
{
	weave_release_local(vk, &mc->weave.sbs_image, &mc->weave.sbs_memory, &mc->weave.sbs_view);
	mc->weave.sbs_w = 0;
	mc->weave.sbs_h = 0;
}

static void
weave_release_crop(struct vk_bundle *vk, struct multi_compositor *mc)
{
	weave_release_local(vk, &mc->weave.crop_image, &mc->weave.crop_memory, &mc->weave.crop_view);
	mc->weave.crop_w = 0;
	mc->weave.crop_h = 0;
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
	// The caller's copies (sent over SCM_RIGHTS) are theirs; ours goes here.
	weave_close_fd(&mc->weave.out_fd);
	mc->weave.out_size = 0;
	mc->weave.out_w = 0;
	mc->weave.out_h = 0;
	mc->weave.out_layout = VK_IMAGE_LAYOUT_UNDEFINED; // #1387: a fresh allocation starts undefined
}

/*!
 * Exportable output image + view + framebuffer, and its ONE exported fd.
 *
 * Stage A: an OPAQUE_FD allocation from the tree's image allocator (dedicated,
 * TILING_OPTIMAL, WEAVE_STAGE_A_BITS) — readable by a same-driver Vulkan / GL
 * consumer on the same device. R5: stage B allocates it with
 * vk_create_exportable_dmabuf_image (DMA_BUF handle type, LINEAR unless the
 * caller advertises modifiers) and records modifier + plane layout for
 * comp_multi_weave_export_output_dmabuf.
 */
static bool
weave_create_output(struct vk_bundle *vk, struct multi_compositor *mc, uint32_t w, uint32_t h)
{
	struct xrt_swapchain_create_info info = weave_sci(w, h, WEAVE_VK_FORMAT, WEAVE_STAGE_A_BITS);
	struct vk_image_collection vkic;
	U_ZERO(&vkic);
	VkResult ret = vk_ic_allocate(vk, &info, 1, &vkic);
	if (ret != VK_SUCCESS) {
		U_LOG_E("weave(#1699): output allocation (%ux%u, OPAQUE_FD) failed: %s", w, h, vk_result_string(ret));
		return false;
	}
	mc->weave.out_image = vkic.images[0].handle;
	mc->weave.out_memory = vkic.images[0].memory;
	mc->weave.out_size = vkic.images[0].size;
	mc->weave.out_w = w;
	mc->weave.out_h = h;

	xrt_graphics_buffer_handle_t fd = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
	ret = vk_get_native_handle_from_device_memory(vk, mc->weave.out_memory, &fd);
	if (ret != VK_SUCCESS || fd < 0) {
		U_LOG_E("weave(#1699): output vkGetMemoryFdKHR failed: %s", vk_result_string(ret));
		return false;
	}
	mc->weave.out_fd = fd;

	VkImageSubresourceRange range = {
	    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	    .levelCount = 1,
	    .layerCount = 1,
	};
	ret =
	    vk_create_view(vk, mc->weave.out_image, VK_IMAGE_VIEW_TYPE_2D, WEAVE_VK_FORMAT, range, &mc->weave.out_view);
	if (ret != VK_SUCCESS) {
		U_LOG_E("weave(#1699): vk_create_view(output) failed: %s", vk_result_string(ret));
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
	if (vk->vkCreateFramebuffer(vk->device, &fb_ci, NULL, &mc->weave.out_fb) != VK_SUCCESS) {
		U_LOG_E("weave(#1699): vkCreateFramebuffer(output) failed");
		return false;
	}
	return true;
}

/*!
 * One-time engine bring-up: command pool + two buffers, fence, render pass
 * (compatible with the DP's own — one BGRA8 color attachment), and the DP
 * instance from the plug-in's Vulkan factory.
 *
 * No main-thread hop: unlike the Android vendor SDK (Looper affinity), nothing
 * in the Linux DP contract ties its creation to a thread, so the factory runs on
 * the IPC handler thread.
 */
static bool
weave_ensure_engine(struct vk_bundle *vk, struct multi_compositor *mc)
{
	if (mc->weave.engine_initialized) {
		return true;
	}
	if (mc->weave.engine_failed) {
		return false; // one-shot: don't re-run a hopeless bring-up every frame
	}
	mc->weave.engine_failed = true; // cleared on success below

	VkCommandPoolCreateInfo pool_info = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
	    .queueFamilyIndex = vk->main_queue->family_index,
	    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
	};
	if (vk->vkCreateCommandPool(vk->device, &pool_info, NULL, &mc->weave.cmd_pool) != VK_SUCCESS) {
		U_LOG_E("weave(#1699): vkCreateCommandPool failed");
		return false;
	}
	VkCommandBufferAllocateInfo cb_info = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	    .commandPool = mc->weave.cmd_pool,
	    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
	    .commandBufferCount = 1,
	};
	if (vk->vkAllocateCommandBuffers(vk->device, &cb_info, &mc->weave.cmd) != VK_SUCCESS ||
	    vk->vkAllocateCommandBuffers(vk->device, &cb_info, &mc->weave.cmd_post) != VK_SUCCESS) {
		U_LOG_E("weave(#1699): vkAllocateCommandBuffers failed");
		return false;
	}
	VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	if (vk->vkCreateFence(vk->device, &fence_info, NULL, &mc->weave.fence) != VK_SUCCESS) {
		U_LOG_E("weave(#1699): vkCreateFence failed");
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
		U_LOG_E("weave(#1699): vkCreateRenderPass failed");
		return false;
	}

	// The DP that weaves: this engine's own instance from the active plug-in's
	// Vulkan factory — the service has no other DP to borrow (null_compositor
	// + comp_multi; vk_native is in-process only). Pure offscreen: no window.
	xrt_dp_factory_vk_fn_t factory = (xrt_dp_factory_vk_fn_t)mc->msc->base.info.dp_factory_vk;
	if (factory == NULL) {
		U_LOG_E("weave(#1699): the active plug-in has no Vulkan DP factory — cannot weave");
		return false;
	}
	xrt_result_t xret = factory(vk, (void *)(uintptr_t)mc->weave.cmd_pool, NULL /* window_handle */,
	                            (int32_t)WEAVE_VK_FORMAT, &mc->weave.dp);
	if (xret != XRT_SUCCESS || mc->weave.dp == NULL) {
		U_LOG_E("weave(#1699): Vulkan DP factory failed: %d", xret);
		return false;
	}

	// R5: stage B creates acquire_sem (plain binary; vk_semaphore_import_sync_fd
	// per frame) and release_sem (vk_create_exportable_sync_fd_semaphore) here,
	// gated on vk_dmabuf_sync_fd_supported. vkGetMemoryFdPropertiesKHR is
	// resolved inside R5's helpers via vk->vkGetDeviceProcAddr — never a
	// vk_bundle member (plug-in ABI).

	mc->weave.engine_initialized = true;
	mc->weave.engine_failed = false;
	U_LOG_W(
	    "weave(#1699): desktop-Linux weave engine initialized (BGRA8, OPAQUE_FD stage A, synchronous; "
	    "DP self-submitting=%d)",
	    (int)xrt_display_processor_is_self_submitting(mc->weave.dp));
	return true;
}

/*!
 * Queue-family ownership transfer of a caller-owned image, in GENERAL on both
 * sides (the stage-A contract: the producer releases with oldLayout ==
 * newLayout == GENERAL, and so do we).
 *
 * @p acquire true = producer family -> ours (before the first read this frame);
 * false = ours -> producer family (after the last read).
 */
static void
weave_ownership_barrier(struct vk_bundle *vk,
                        VkCommandBuffer cmd,
                        VkImage image,
                        uint32_t ext_family,
                        bool acquire,
                        VkPipelineStageFlags our_stage,
                        VkAccessFlags our_access)
{
	const uint32_t ours = vk->main_queue->family_index;
	VkImageMemoryBarrier b = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = acquire ? 0 : our_access,
	    .dstAccessMask = acquire ? our_access : 0,
	    .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
	    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
	    .srcQueueFamilyIndex = acquire ? ext_family : ours,
	    .dstQueueFamilyIndex = acquire ? ours : ext_family,
	    .image = image,
	    .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1},
	};
	vk->vkCmdPipelineBarrier(cmd, acquire ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : our_stage,
	                         acquire ? our_stage : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1,
	                         &b);
}

//! Plain same-family layout transition (no ownership change).
static void
weave_layout_barrier(struct vk_bundle *vk,
                     VkCommandBuffer cmd,
                     VkImage image,
                     VkImageLayout old_layout,
                     VkImageLayout new_layout,
                     VkAccessFlags src_access,
                     VkAccessFlags dst_access,
                     VkPipelineStageFlags src_stage,
                     VkPipelineStageFlags dst_stage)
{
	VkImageMemoryBarrier b = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = src_access,
	    .dstAccessMask = dst_access,
	    .oldLayout = old_layout,
	    .newLayout = new_layout,
	    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .image = image,
	    .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1},
	};
	vk->vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

/*!
 * Per-window weave phase: hand the DP the present-owner's client origin on the
 * panel (`set_present_origin`, panel-relative; (0,0) = display-scoped) — the
 * slot the Linux vendor DP honours, and the same value the in-process vk_native
 * compositor feeds (window − display screen origin). Re-asserted every submit so
 * a moved window keeps its phase; no-op for a DP without the slot (sim_display).
 */
static void
weave_feed_dp_geometry(struct multi_compositor *mc)
{
	if (!mc->weave.have_geometry || mc->weave.dp == NULL) {
		return;
	}
	const struct xrt_system_compositor_info *info = &mc->msc->base.info;
	const int32_t ox = mc->weave.win_x - info->display_screen_left;
	const int32_t oy = mc->weave.win_y - info->display_screen_top;
	const bool fed =
	    xrt_display_processor_vk_set_present_origin((struct xrt_display_processor_vk *)mc->weave.dp, ox, oy);
	if (mc->weave.geometry_dirty) {
		mc->weave.geometry_dirty = false;
		U_LOG_W("weave(#1699): present origin (%d,%d) panel-relative %s the DP phase slot", ox, oy,
		        fed ? "fed to" : "NOT accepted by (no set_present_origin slot — display-scoped weave)");
	}
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
	// Recorded only: an XID (X11) or 0 (Wayland, where a client cannot name
	// its window to another process). The service creates no window and never
	// derives geometry from it — that arrives through set_window_geometry.
	mc->weave.window_id = window_id;
	os_mutex_unlock(&mc->weave.mutex);
	U_LOG_W("weave(#1699): bound present-owner window id 0x%" PRIx64, window_id);
	return true;
}

bool
comp_multi_weave_set_window_geometry(struct xrt_compositor *xc,
                                     int32_t origin_x,
                                     int32_t origin_y,
                                     uint32_t client_w,
                                     uint32_t client_h,
                                     int32_t display_id)
{
	struct multi_compositor *mc = multi_compositor(xc);
	if (mc == NULL || mc->msc == NULL || client_w == 0 || client_h == 0) {
		return false;
	}
	weave_ensure_mutex(mc);
	os_mutex_lock(&mc->weave.mutex);
	const bool changed = !mc->weave.have_geometry || mc->weave.win_x != origin_x || mc->weave.win_y != origin_y ||
	                     mc->weave.win_w != client_w || mc->weave.win_h != client_h ||
	                     mc->weave.win_display_id != display_id;
	mc->weave.have_geometry = true;
	mc->weave.win_x = origin_x;
	mc->weave.win_y = origin_y;
	mc->weave.win_w = client_w;
	mc->weave.win_h = client_h;
	mc->weave.win_display_id = display_id;
	mc->weave.geometry_dirty = mc->weave.geometry_dirty || changed;
	os_mutex_unlock(&mc->weave.mutex);
	if (changed) {
		// Lifecycle event (the window moved / resized); the caller may
		// re-publish every frame, so log only on change.
		U_LOG_W("weave(#1699): present-owner window %d,%d %ux%u on display %d", origin_x, origin_y, client_w,
		        client_h, display_id);
	}
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
                        const struct xrt_weave_atlas_layout *layout,
                        uint32_t flat_rect_count,
                        const struct xrt_rect *flat_rects,
                        uint32_t *out_width,
                        uint32_t *out_height,
                        uint64_t *out_fence_value,
                        struct xrt_eye_positions *out_eyes)
{
	// v8 (browser#88): accepted and ignored, as on macOS / Android — the
	// per-region hardware wish has no desktop-Linux channel yet. Conformant:
	// the wish is advisory and hardware-only (ADR-027 D6), pixels are unaffected.
	(void)flat_rect_count;
	(void)flat_rects;

	// Every received fd is ours from here and is closed exactly once, at the
	// bottom — hit, miss, refusal or error (file header, rule 1).
	int in_fd = in_handle;
	int ov_fd = overlay_handle;

	struct multi_compositor *mc = multi_compositor(xc);
	struct vk_bundle *vk = weave_get_vk(mc);
	if (mc == NULL || mc->msc == NULL || vk == NULL || in_fd < 0) {
		weave_close_fd(&in_fd);
		weave_close_fd(&ov_fd);
		return false;
	}

	weave_ensure_mutex(mc);
	os_mutex_lock(&mc->weave.mutex);

	bool ok = false;
	do {
		if (!weave_ensure_engine(vk, mc)) {
			break;
		}

		uint32_t in_w = 0, in_h = 0;
		weave_stage_a_input_dims(mc, rect_x, rect_y, rect_w, rect_h, rect_count, rects, layout, &in_w, &in_h);
		struct comp_multi_weave_linux_slot *in =
		    weave_cache_acquire(vk, mc, mc->weave.in_slots, in_fd, 0, in_w, in_h, "input");
		if (in == NULL) {
			break;
		}

		// v4 overlay: window-sized premul atlas, same import cache shape. A
		// failed overlay import only drops the overlay (as on macOS / Android).
		struct comp_multi_weave_linux_slot *ov = NULL;
		if (ov_fd >= 0) {
			uint32_t ov_w = mc->weave.have_geometry ? mc->weave.win_w : in_w;
			uint32_t ov_h = mc->weave.have_geometry ? mc->weave.win_h : in_h;
			ov = weave_cache_acquire(vk, mc, mc->weave.ov_slots, ov_fd, 0, ov_w, ov_h, "overlay");
		}

		// Spec-v6 N-view atlas (#774): tiles contiguous from the top-left at
		// (content_view_w, content_view_h); crop the packed region if the atlas
		// is bigger (ADR-030 crop-before-DP) and weave once.
		const bool nview = (layout != NULL && layout->view_count > 0);
		uint32_t cvw = 0, cvh = 0, packed_w = 0, packed_h = 0;
		if (nview) {
			cvw = layout->content_view_w;
			cvh = layout->content_view_h;
			packed_w = layout->tile_columns * cvw;
			packed_h = layout->tile_rows * cvh;
			if (packed_w > in->w || packed_h > in->h) {
				U_LOG_E(
				    "weave(#1699) v6: packed region %ux%u exceeds input atlas %ux%u "
				    "(views=%u grid=%ux%u content=%ux%u)",
				    packed_w, packed_h, in->w, in->h, layout->view_count, layout->tile_columns,
				    layout->tile_rows, cvw, cvh);
				break;
			}
		}

		// Output dims: v6 = one content view; batch = the (window-client-sized)
		// input; legacy = rect offset+extent.
		uint32_t want_w = 0, want_h = 0;
		if (nview) {
			want_w = cvw;
			want_h = cvh;
		} else if (rect_count > 0) {
			want_w = in->w;
			want_h = in->h;
		} else {
			want_w = (uint32_t)rect_x + rect_w;
			want_h = (uint32_t)rect_y + rect_h;
		}
		if (want_w == 0 || want_h == 0) {
			break;
		}

		// (Re)allocate output (+ SBS scratch on the non-v6 paths) on resize.
		if (mc->weave.out_image == VK_NULL_HANDLE || mc->weave.out_w != want_w || mc->weave.out_h != want_h) {
			// Never yank resources out from under in-flight GPU work.
			weave_queue_wait_idle(vk);
			weave_release_output(vk, mc);
			weave_release_scratch(vk, mc);
			if (!weave_create_output(vk, mc, want_w, want_h)) {
				weave_release_output(vk, mc);
				break;
			}
			if (!nview) {
				if (!weave_create_local(vk, want_w * 2, want_h, &mc->weave.sbs_image,
				                        &mc->weave.sbs_memory, &mc->weave.sbs_view)) {
					break;
				}
				mc->weave.sbs_w = want_w * 2;
				mc->weave.sbs_h = want_h;
				mc->weave.sbs_first_use = true;
			}
			U_LOG_W("weave(#1699): output %ux%u (%s layout), exported OPAQUE_FD %" PRIu64 " bytes", want_w,
			        want_h, nview ? "v6 N-view" : (rect_count > 0 ? "v3 batch" : "legacy"),
			        (uint64_t)mc->weave.out_size);
		}

		// v6 crop staging: (re)create when the packed region is smaller than the
		// input. Zero-copy (packed == input) samples the input directly.
		const bool v6_zero_copy = nview && (packed_w == in->w && packed_h == in->h);
		if (nview && !v6_zero_copy &&
		    (mc->weave.crop_image == VK_NULL_HANDLE || mc->weave.crop_w != packed_w ||
		     mc->weave.crop_h != packed_h)) {
			weave_queue_wait_idle(vk);
			weave_release_crop(vk, mc);
			if (!weave_create_local(vk, packed_w, packed_h, &mc->weave.crop_image, &mc->weave.crop_memory,
			                        &mc->weave.crop_view)) {
				break;
			}
			mc->weave.crop_w = packed_w;
			mc->weave.crop_h = packed_h;
			mc->weave.crop_first_use = true;
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

		// Take the input from the producer's queue family (GENERAL both sides).
		// Mandatory for external memory: without the acquire, the producer's
		// writes are not guaranteed visible (and a compressed / aux-surface
		// image may not be resolved) on this queue.
		const VkPipelineStageFlags in_stage =
		    v6_zero_copy ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT;
		const VkAccessFlags in_access = v6_zero_copy ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_TRANSFER_READ_BIT;
		weave_ownership_barrier(vk, cmd, in->image, in->ext_queue_family, true, in_stage, in_access);
		in->first_use = false;

		VkImage dp_src_image = mc->weave.sbs_image;
		VkImageView dp_src_view = mc->weave.sbs_view;
		uint32_t atlas_view_w = mc->weave.out_w;
		uint32_t atlas_view_h = mc->weave.out_h;
		uint32_t grid_cols = 2, grid_rows = 1;
		VkImageLayout in_layout = VK_IMAGE_LAYOUT_GENERAL; // where the input sits after the reads

		if (nview) {
			if (v6_zero_copy) {
				// The packed atlas fills the input exactly — sample it directly.
				weave_layout_barrier(vk, cmd, in->image, VK_IMAGE_LAYOUT_GENERAL,
				                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0,
				                     VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
				                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
				in_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				dp_src_image = in->image;
				dp_src_view = in->view;
			} else {
				// Crop the top-left packed region: ONE box copy.
				weave_layout_barrier(vk, cmd, in->image, VK_IMAGE_LAYOUT_GENERAL,
				                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0,
				                     VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
				                     VK_PIPELINE_STAGE_TRANSFER_BIT);
				in_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
				weave_layout_barrier(
				    vk, cmd, mc->weave.crop_image,
				    mc->weave.crop_first_use ? VK_IMAGE_LAYOUT_UNDEFINED
				                             : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
				    VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
				    VK_PIPELINE_STAGE_TRANSFER_BIT);
				mc->weave.crop_first_use = false;

				VkImageCopy copy = {
				    .srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
				    .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
				    .extent = {packed_w, packed_h, 1},
				};
				vk->vkCmdCopyImage(cmd, in->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				                   mc->weave.crop_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
				                   &copy);

				weave_layout_barrier(
				    vk, cmd, mc->weave.crop_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
				    VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
				    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
				dp_src_image = mc->weave.crop_image;
				dp_src_view = mc->weave.crop_view;
			}
			atlas_view_w = cvw;
			atlas_view_h = cvh;
			grid_cols = layout->tile_columns;
			grid_rows = layout->tile_rows;
		} else {
			// Batch / legacy: the input stays in GENERAL (the acquire above
			// already made the producer's writes visible to TRANSFER reads).

			// Scratch -> TRANSFER_DST (persists across frames: stale regions
			// from closed elements re-weave harmlessly; the caller composites
			// back only its current rects).
			weave_layout_barrier(vk, cmd, mc->weave.sbs_image,
			                     mc->weave.sbs_first_use ? VK_IMAGE_LAYOUT_UNDEFINED
			                                             : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
			                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			                     VK_PIPELINE_STAGE_TRANSFER_BIT);
			mc->weave.sbs_first_use = false;

			// v5 firstChunk (browser#22): clear the SBS scratch to premultiplied
			// transparent on the first submit of a frame.
			if (weave_frame_first) {
				VkClearColorValue sbs_transparent = {.float32 = {0.0f, 0.0f, 0.0f, 0.0f}};
				vk->vkCmdClearColorImage(cmd, mc->weave.sbs_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				                         &sbs_transparent, 1, &range);
				// Order the whole-image clear before the per-rect blits.
				weave_layout_barrier(vk, cmd, mc->weave.sbs_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
				                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
				                     VK_PIPELINE_STAGE_TRANSFER_BIT);
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
				if (rx < 0 || ry < 0 || (uint32_t)(rx + rw) > in->w || (uint32_t)(ry + rh) > in->h) {
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
				vk->vkCmdBlitImage(cmd, in->image, VK_IMAGE_LAYOUT_GENERAL, mc->weave.sbs_image,
				                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, blits, VK_FILTER_LINEAR);
			}

			// Scratch -> SHADER_READ for the DP sample.
			weave_layout_barrier(vk, cmd, mc->weave.sbs_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
			                     VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
		}

		// Output -> COLOR_ATTACHMENT. Fully re-rendered every submit, so the
		// discard from UNDEFINED is fine — and needs no acquire from the
		// caller's family: contents that are not preserved need no transfer.
		weave_layout_barrier(vk, cmd, mc->weave.out_image, VK_IMAGE_LAYOUT_UNDEFINED,
		                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

		weave_feed_dp_geometry(mc);

		// SELF-SUBMITTING DP ORDERING (Android #1036's one-frame trail fix): a
		// DP that submits its own batch during process_atlas would otherwise
		// execute BEFORE this frame's blits (still unsubmitted in cmd). Flush
		// the pre-weave batch first; same-queue submission order then puts the
		// DP's batch after it. The final submit + fence below retire both.
		const bool self_submits = xrt_display_processor_is_self_submitting(mc->weave.dp);
		if (self_submits && debug_get_bool_option_dxr_linux_weave_split()) {
			if (vk->vkEndCommandBuffer(cmd) != VK_SUCCESS) {
				U_LOG_E("weave(#1699): vkEndCommandBuffer (pre-weave) failed");
				break;
			}
			VkSubmitInfo pre_submit = {
			    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			    .commandBufferCount = 1,
			    .pCommandBuffers = &cmd,
			};
			vk_queue_lock(vk->main_queue);
			VkResult pre_ret = vk->vkQueueSubmit(vk->main_queue->queue, 1, &pre_submit, VK_NULL_HANDLE);
			vk_queue_unlock(vk->main_queue);
			if (pre_ret != VK_SUCCESS) {
				U_LOG_E("weave(#1699): pre-weave vkQueueSubmit failed: %s", vk_result_string(pre_ret));
				break;
			}
			cmd = mc->weave.cmd_post;
			vk->vkResetCommandBuffer(cmd, 0);
			if (vk->vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS) {
				U_LOG_E("weave(#1699): vkBeginCommandBuffer (post-weave) failed");
				break;
			}
		}

		// ONE process_atlas per submit. Legacy/batch: 2x1 SBS, per-eye dims =
		// the window. v6: the caller's grid at content_view dims. A
		// self-submitting DP gets VK_NULL_HANDLE (xrt_display_processor
		// contract; it records into its own buffer).
		xrt_display_processor_set_target_color_view(mc->weave.dp, mc->weave.out_view);
		xrt_display_processor_process_atlas(mc->weave.dp, self_submits ? VK_NULL_HANDLE : cmd, //
		                                    (VkImage_XDP)dp_src_image, dp_src_view,            //
		                                    atlas_view_w, atlas_view_h,                        //
		                                    grid_cols, grid_rows,                              //
		                                    (VkFormat_XDP)WEAVE_VK_FORMAT,                     //
		                                    mc->weave.out_fb,                                  //
		                                    (VkImage_XDP)mc->weave.out_image,                  //
		                                    mc->weave.out_w, mc->weave.out_h,                  //
		                                    (VkFormat_XDP)WEAVE_VK_FORMAT,                     //
		                                    0, 0, 0, 0);

		// Input back to GENERAL (if a v6 path moved it) and released to the
		// producer's family, so its next writes land in a defined state.
		if (in_layout != VK_IMAGE_LAYOUT_GENERAL) {
			weave_layout_barrier(vk, cmd, in->image, in_layout, VK_IMAGE_LAYOUT_GENERAL, in_access, 0,
			                     in_stage, in_stage);
		}
		weave_ownership_barrier(vk, cmd, in->image, in->ext_queue_family, false, in_stage, in_access);

		// v4 overlay atlas (browser#18): composite the caller's window-sized
		// premultiplied 2D atlas OVER the woven output — not woven, drawn after
		// process_atlas onto the same attachment.
		if (ov != NULL) {
			bool blend_ready = mc->weave.overlay_blend_initialized;
			if (!blend_ready) {
				blend_ready = vk_local2d_composite_init(&mc->weave.overlay_blend, vk, WEAVE_VK_FORMAT,
				                                        WEAVE_VK_FORMAT);
				mc->weave.overlay_blend_initialized = blend_ready;
				if (blend_ready) {
					U_LOG_W("weave(#1699) v4: premul-over blend pipeline ready");
				} else {
					U_LOG_E("weave(#1699) v4: premul-over blend init failed");
				}
			}
			if (blend_ready) {
				weave_ownership_barrier(vk, cmd, ov->image, ov->ext_queue_family, true,
				                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
				                        VK_ACCESS_SHADER_READ_BIT);
				weave_layout_barrier(vk, cmd, ov->image, VK_IMAGE_LAYOUT_GENERAL,
				                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0,
				                     VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
				                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
				ov->first_use = false;

				weave_layout_barrier(
				    vk, cmd, mc->weave.out_image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
				    VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
				    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
				    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

				vk_local2d_composite_begin_frame(&mc->weave.overlay_blend, vk);
				vk_local2d_composite_flatten_draw(&mc->weave.overlay_blend, vk, cmd, mc->weave.out_fb,
				                                  mc->weave.out_w, mc->weave.out_h,
				                                  ov->view, //
				                                  0, 0, mc->weave.out_w,
				                                  mc->weave.out_h,        // dst = full window
				                                  0.0f, 0.0f, 1.0f, 1.0f, // src = whole atlas
				                                  /*unpremultiplied*/ false);

				weave_layout_barrier(vk, cmd, ov->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				                     VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT, 0,
				                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
				                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
				weave_ownership_barrier(vk, cmd, ov->image, ov->ext_queue_family, false,
				                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
				                        VK_ACCESS_SHADER_READ_BIT);
			}
		}

		// Output -> GENERAL, then released to the caller's family (GENERAL both
		// sides) for its cross-process read.
		weave_layout_barrier(vk, cmd, mc->weave.out_image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		                     VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		                     VK_ACCESS_MEMORY_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
		weave_ownership_barrier(vk, cmd, mc->weave.out_image, VK_QUEUE_FAMILY_EXTERNAL, false,
		                        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT);
		mc->weave.out_layout = VK_IMAGE_LAYOUT_GENERAL; // #1387 d3: the one authority

		if (vk->vkEndCommandBuffer(cmd) != VK_SUCCESS) {
			break;
		}

		// ---- Submit + synchronous completion (stage A) ----
		// R4/R5: stage B waits the imported acquire semaphore here
		// (pWaitDstStageMask = TRANSFER | FRAGMENT_SHADER), signals
		// release_sem and exports it (vk_semaphore_export_sync_fd) as the
		// per-frame *out_release_fence_fd — which the IPC handler owns, sends
		// and closes after the reply (#1712) — and replaces the CPU wait below
		// with a wait on the PREVIOUS frame's fence at the top of the next
		// submit (command buffers / scratch reuse).
		VkSubmitInfo submit = {
		    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		    .commandBufferCount = 1,
		    .pCommandBuffers = &cmd,
		};
		vk_queue_lock(vk->main_queue);
		VkResult ret = vk->vkQueueSubmit(vk->main_queue->queue, 1, &submit, mc->weave.fence);
		vk_queue_unlock(vk->main_queue);
		if (ret != VK_SUCCESS) {
			U_LOG_E("weave(#1699): vkQueueSubmit failed: %s", vk_result_string(ret));
			break;
		}
		ret = vk->vkWaitForFences(vk->device, 1, &mc->weave.fence, VK_TRUE, WEAVE_FENCE_TIMEOUT_NS);
		vk->vkResetFences(vk->device, 1, &mc->weave.fence);
		if (ret != VK_SUCCESS) {
			U_LOG_E("weave(#1699): weave did not complete within 1s: %s", vk_result_string(ret));
			break;
		}

		mc->weave.fence_value++;

		*out_width = mc->weave.out_w;
		*out_height = mc->weave.out_h;
		*out_fence_value = mc->weave.fence_value;

		// Eyes flow OUT for the caller's next off-axis frame; the weave itself
		// reads the tracker DP-internally.
		U_ZERO(out_eyes);
		if (!xrt_display_processor_get_predicted_eye_positions(mc->weave.dp, out_eyes)) {
			U_ZERO(out_eyes);
		}

		ok = true;
	} while (false);

	os_mutex_unlock(&mc->weave.mutex);

	// Rule 1: the received fds, whatever happened above. An import that needed
	// one was handed its own dup.
	weave_close_fd(&in_fd);
	weave_close_fd(&ov_fd);
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
	if (mc->weave.out_fd >= 0 && mc->weave.out_w != 0) {
		// Hand out the CACHED fd without a dup: the plain weave_get_output
		// handler sends it (generated out_handles → ipc_send_fds, SCM_RIGHTS
		// installs the peer's own copy) and never closes it — unchanged by the
		// v10 wire (#1712), whose defer-close parking covers only the dma-buf
		// calls. A dup here would leak one fd per (re)allocation. Ownership
		// stays with the engine, which closes it on reallocation / teardown.
		// (comp_multi_weave_export_output_dmabuf is the opposite contract: its
		// handler parks and closes what it is given, so stage B hands out one
		// dup per call.)
		*out_handle = mc->weave.out_fd;
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
	// Stage A is synchronous: xrWeaveSubmitDXR returning is the completion
	// signal, so there is no fence to export. R4: stage B's per-frame release
	// sync_file rides the submit reply (comp_multi_weave_submit_dmabuf), not
	// this latched once-per-allocation call.
	(void)xc;
	(void)out_handle;
	return false;
}

bool
comp_multi_weave_submit_dmabuf(struct xrt_compositor *xc,
                               const struct xrt_weave_dmabuf_desc *in,
                               const struct xrt_weave_dmabuf_desc *overlay,
                               int acquire_fence_fd,
                               int32_t rect_x,
                               int32_t rect_y,
                               uint32_t rect_w,
                               uint32_t rect_h,
                               uint32_t rect_count,
                               const struct xrt_rect *rects,
                               bool weave_frame_first,
                               const struct xrt_weave_atlas_layout *layout,
                               uint32_t flat_rect_count,
                               const struct xrt_rect *flat_rects,
                               int *out_release_fence_fd,
                               uint32_t *out_width,
                               uint32_t *out_height,
                               uint64_t *out_fence_value,
                               struct xrt_eye_positions *out_eyes)
{
	// R5: STUB until aux_vk grows vk_create_image_from_dmabuf /
	// vk_semaphore_import_sync_fd / vk_semaphore_export_sync_fd and the
	// FOREIGN_EXT barrier helpers. The fd plumbing honours the contract
	// already (xrt_weave_dmabuf.h): every fd handed in is ours, so a refusal
	// closes them all and returns no release fence.
	(void)xc;
	(void)rect_x;
	(void)rect_y;
	(void)rect_w;
	(void)rect_h;
	(void)rect_count;
	(void)rects;
	(void)weave_frame_first;
	(void)layout;
	(void)flat_rect_count;
	(void)flat_rects;
	(void)out_width;
	(void)out_height;
	(void)out_fence_value;
	(void)out_eyes;

	static bool warned = false;
	if (!warned) {
		warned = true;
		U_LOG_W(
		    "weave(#1699): dma-buf submit is not implemented yet (stage B, pending the R5 aux_vk "
		    "dma-buf helpers) — refusing; use the plain OPAQUE_FD submit");
	}
	if (in != NULL && in->fd >= 0) {
		close(in->fd);
	}
	if (overlay != NULL && overlay->fd >= 0) {
		close(overlay->fd);
	}
	if (acquire_fence_fd >= 0) {
		close(acquire_fence_fd);
	}
	if (out_release_fence_fd != NULL) {
		*out_release_fence_fd = -1;
	}
	return false;
}

bool
comp_multi_weave_export_output_dmabuf(struct xrt_compositor *xc, struct xrt_weave_dmabuf_output_desc *out)
{
	// R5: STUB — stage B exports the LINEAR/modifier output allocated by
	// vk_create_exportable_dmabuf_image (one dup per call, per the contract).
	(void)xc;
	static bool warned = false;
	if (!warned) {
		warned = true;
		U_LOG_W("weave(#1699): dma-buf output export is not implemented yet (stage B, pending R5)");
	}
	if (out != NULL) {
		out->fd = -1;
	}
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
	// The real snap, through THIS engine's DP (the service has no other one):
	// its xrt_display_processor_vk snap_window_rect slot, the same slot the
	// in-process vk_native compositor uses (#1588). Before the first submit
	// brought the DP up — or for a DP without the slot — the wrapper writes the
	// target through and returns false: identity, as spec §5c defines it.
	struct multi_compositor *mc = multi_compositor(xc);
	if (out_snapped_x == NULL || out_snapped_y == NULL) {
		return false;
	}
	*out_snapped_x = target_x;
	*out_snapped_y = target_y;
	if (mc == NULL || mc->msc == NULL) {
		return false;
	}
	weave_ensure_mutex(mc);
	os_mutex_lock(&mc->weave.mutex);
	bool snapped =
	    xrt_display_processor_vk_snap_window_rect((struct xrt_display_processor_vk *)mc->weave.dp, origin_x,
	                                              origin_y, target_x, target_y, out_snapped_x, out_snapped_y);
	os_mutex_unlock(&mc->weave.mutex);
	return snapped;
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
			// Submits are synchronous, but a timed-out fence can leave work in
			// flight — idle before tearing anything down.
			weave_queue_wait_idle(vk);
		}
		weave_slots_release_all(vk, mc->weave.in_slots);
		weave_slots_release_all(vk, mc->weave.ov_slots);
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
	}
	// fds are process state, not Vulkan state: close them even without a vk.
	weave_close_fd(&mc->weave.out_fd);
	mc->weave.engine_initialized = false;
	os_mutex_unlock(&mc->weave.mutex);
	os_mutex_destroy(&mc->weave.mutex);
	mc->weave.mutex_initialized = false;
}

#endif // XRT_OS_LINUX_DESKTOP
