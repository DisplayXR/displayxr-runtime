// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_lift (ADR-042) on the Android service — see
 *         comp_multi_lift_android.h. The Android twin of
 *         d3d11_service/d3d11_lift.cpp: same mailbox / ring / scheduler
 *         (util/u_lift_mailbox.h, unchanged), Vulkan + AHardwareBuffer in place
 *         of D3D11 shared textures + keyed mutexes, CPU-drained sync in place
 *         of fences.
 * @ingroup comp_multi
 *
 * Deliberate v1 differences from the D3D11 service (all documented in the spec's
 * § Android and docs/roadmap/android-lift.md):
 *  - the letterbox crop is OFF (its profile reduction is an HLSL pixel shader;
 *    the GLSL port is a follow-up) — weave-rect snapshots take the whole rect;
 *  - the capped snapshot is ONE linear vkCmdBlitImage (the D3D11 path is a
 *    4-tap box filter) — fine up to a 2x reduction, aliases a little beyond;
 *  - no dedicated lift queue (the service device has one graphics queue), so
 *    lift submissions are short copies on the main queue under its lock;
 *  - no fence is exported: every acquire completes on the GPU before its IPC
 *    reply (the Android weave contract), so XrLiftResultDXR::fence is NULL and
 *    fenceValue is a plain monotonic counter.
 */

#include "comp_multi_lift_android.h"

#ifdef XRT_OS_ANDROID

#include "xrt/xrt_display_processor_vk.h"
#include "xrt/xrt_compositor.h"

#include "util/u_lift_mailbox.h"
#include "util/u_logging.h"
#include "util/u_handles.h"
#include "util/u_misc.h"
#include "os/os_time.h"

#include "android/android_globals.h"
#include "android/android_main_thread.h"

#include "comp_multi_private.h"

#include <android/hardware_buffer.h>
#include <jni.h>
#include <sys/system_properties.h>

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

//! Streams the service holds at once (the module's own max_streams is lower).
#define LIFT_MAX_STREAMS 16
//! Floats in the explicit-viewpoint scratch (3 per viewpoint).
#define LIFT_VP_FLOATS (3 * XRT_DP_LIFT_MAX_EXPLICIT_VIEWPOINTS)
//! Tracked eyes older than this are not handed to the module.
#define LIFT_EYES_MAX_AGE_NS (500ull * 1000ull * 1000ull)
//! Bound on every CPU wait for a lift GPU copy (a wedged queue must not hang an IPC thread).
#define LIFT_FENCE_TIMEOUT_NS (1000ull * 1000ull * 1000ull)


/*
 *
 * Settings: Android system property first (getenv does not reach the service
 * process), then the environment (desktop-style launches, tests).
 *
 */

static const char *
lift_setting(const char *env_name, const char *prop_name, char *buf, size_t cap)
{
	char v[PROP_VALUE_MAX] = {0};
	if (prop_name != NULL && __system_property_get(prop_name, v) > 0 && v[0] != '\0') {
		snprintf(buf, cap, "%s", v);
		return buf;
	}
	const char *e = env_name != NULL ? getenv(env_name) : NULL;
	if (e != NULL && e[0] != '\0') {
		snprintf(buf, cap, "%s", e);
		return buf;
	}
	return NULL;
}


/*
 *
 * AHardwareBuffer-backed images.
 *
 */

//! One VkImage bound to an AHardwareBuffer we hold a reference on.
struct lift_img
{
	AHardwareBuffer *ahb;
	VkImage image;
	VkDeviceMemory memory;
	uint32_t w, h;
	uint32_t ahb_format; //!< AHARDWAREBUFFER_FORMAT_*
	VkFormat vk_format;
	bool general; //!< Transitioned out of UNDEFINED (it stays GENERAL from then on).
};

static VkFormat
ahb_to_vk_format(uint32_t f)
{
	switch (f) {
	case AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM:
	case AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
	case AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
	case AHARDWAREBUFFER_FORMAT_R10G10B10A2_UNORM: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
	case AHARDWAREBUFFER_FORMAT_R8_UNORM: return VK_FORMAT_R8_UNORM;
	default: return VK_FORMAT_UNDEFINED;
	}
}

static const enum xrt_swapchain_usage_bits LIFT_IMG_BITS =
    XRT_SWAPCHAIN_USAGE_SAMPLED | XRT_SWAPCHAIN_USAGE_TRANSFER_SRC | XRT_SWAPCHAIN_USAGE_TRANSFER_DST;

static void
lift_img_destroy(struct vk_bundle *vk, struct lift_img *img)
{
	if (img->image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, img->image, NULL);
	}
	if (img->memory != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, img->memory, NULL);
	}
	if (img->ahb != NULL) {
		AHardwareBuffer_release(img->ahb);
	}
	U_ZERO(img);
}

/*!
 * Import @p ahb (BORROWED — we take our own reference) into a VkImage. The
 * Vulkan helper consumes the handle it is given, so it gets a reference of its
 * own, exactly like the weave engine's import (weave_import_ahb).
 */
static bool
lift_img_import(struct vk_bundle *vk, AHardwareBuffer *ahb, struct lift_img *img, const char *what)
{
	U_ZERO(img);
	AHardwareBuffer_Desc desc = {0};
	AHardwareBuffer_describe(ahb, &desc);
	const VkFormat fmt = ahb_to_vk_format(desc.format);
	if (desc.width == 0 || desc.height == 0 || fmt == VK_FORMAT_UNDEFINED) {
		U_LOG_E("[lift] %s AHardwareBuffer %ux%u fmt=0x%x not importable", what, desc.width, desc.height,
		        desc.format);
		return false;
	}
	struct xrt_swapchain_create_info info = {
	    .create = 0,
	    .bits = LIFT_IMG_BITS,
	    .format = fmt,
	    .sample_count = 1,
	    .width = desc.width,
	    .height = desc.height,
	    .face_count = 1,
	    .array_size = 1,
	    .mip_count = 1,
	};
	xrt_graphics_buffer_handle_t consumed = u_graphics_buffer_ref((xrt_graphics_buffer_handle_t)ahb);
	if (!xrt_graphics_buffer_is_valid(consumed)) {
		return false;
	}
	struct xrt_image_native native = {
	    .handle = consumed,
	    .size = 0,
	    .use_dedicated_allocation = true,
	};
	VkResult ret = vk_create_image_from_native(vk, &info, &native, &img->image, &img->memory);
	u_graphics_buffer_unref(&native.handle);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[lift] %s import (%ux%u fmt=0x%x) failed: %s", what, desc.width, desc.height, desc.format,
		        vk_result_string(ret));
		U_ZERO(img);
		return false;
	}
	AHardwareBuffer_acquire(ahb);
	img->ahb = ahb;
	img->w = desc.width;
	img->h = desc.height;
	img->ahb_format = desc.format;
	img->vk_format = fmt;
	img->general = false;
	return true;
}

//! Allocate a runtime-owned AHardwareBuffer and import it.
static bool
lift_img_alloc(struct vk_bundle *vk,
               uint32_t w,
               uint32_t h,
               uint32_t ahb_format,
               bool cpu_read,
               struct lift_img *img,
               const char *what)
{
	AHardwareBuffer_Desc desc = {0};
	desc.width = w;
	desc.height = h;
	desc.layers = 1;
	desc.format = ahb_format;
	desc.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
	if (ahb_format == AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM) {
		desc.usage |= AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT;
	}
	if (cpu_read) {
		desc.usage |= AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;
	}
	AHardwareBuffer *ahb = NULL;
	if (AHardwareBuffer_allocate(&desc, &ahb) != 0 || ahb == NULL) {
		U_LOG_E("[lift] %s AHardwareBuffer_allocate(%ux%u fmt=0x%x) failed", what, w, h, ahb_format);
		return false;
	}
	bool ok = lift_img_import(vk, ahb, img, what);
	AHardwareBuffer_release(ahb); // the import holds its own reference
	return ok;
}


/*
 *
 * One-shot GPU context: its own pool (pools are externally synchronized), one
 * command buffer, one fence; submitted to the main queue under its lock and
 * CPU-waited.
 *
 */

struct lift_gpu
{
	VkCommandPool pool;
	VkCommandBuffer cmd;
	VkFence fence;
	bool ready;
};

static bool
lift_gpu_init(struct vk_bundle *vk, struct lift_gpu *g)
{
	if (g->ready) {
		return true;
	}
	VkCommandPoolCreateInfo pool_info = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
	    .queueFamilyIndex = vk->main_queue->family_index,
	    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
	};
	if (vk->vkCreateCommandPool(vk->device, &pool_info, NULL, &g->pool) != VK_SUCCESS) {
		return false;
	}
	VkCommandBufferAllocateInfo cb_info = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	    .commandPool = g->pool,
	    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
	    .commandBufferCount = 1,
	};
	VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	if (vk->vkAllocateCommandBuffers(vk->device, &cb_info, &g->cmd) != VK_SUCCESS ||
	    vk->vkCreateFence(vk->device, &fence_info, NULL, &g->fence) != VK_SUCCESS) {
		return false;
	}
	g->ready = true;
	return true;
}

static void
lift_gpu_fini(struct vk_bundle *vk, struct lift_gpu *g)
{
	if (g->fence != VK_NULL_HANDLE) {
		vk->vkDestroyFence(vk->device, g->fence, NULL);
	}
	if (g->pool != VK_NULL_HANDLE) {
		vk->vkDestroyCommandPool(vk->device, g->pool, NULL);
	}
	U_ZERO(g);
}

static bool
lift_gpu_begin(struct vk_bundle *vk, struct lift_gpu *g)
{
	vk->vkResetCommandBuffer(g->cmd, 0);
	VkCommandBufferBeginInfo begin = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	return vk->vkBeginCommandBuffer(g->cmd, &begin) == VK_SUCCESS;
}

static bool
lift_gpu_submit_wait(struct vk_bundle *vk, struct lift_gpu *g, const char *what)
{
	if (vk->vkEndCommandBuffer(g->cmd) != VK_SUCCESS) {
		return false;
	}
	VkSubmitInfo submit = {
	    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	    .commandBufferCount = 1,
	    .pCommandBuffers = &g->cmd,
	};
	vk_queue_lock(vk->main_queue);
	VkResult ret = vk->vkQueueSubmit(vk->main_queue->queue, 1, &submit, g->fence);
	vk_queue_unlock(vk->main_queue);
	VK_WARN_QUEUE_RACE_ONCE("[lift] lift_gpu_submit_wait", ret);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[lift] %s: vkQueueSubmit failed: %s", what, vk_result_string(ret));
		return false;
	}
	ret = vk->vkWaitForFences(vk->device, 1, &g->fence, VK_TRUE, LIFT_FENCE_TIMEOUT_NS);
	vk->vkResetFences(vk->device, 1, &g->fence);
	if (ret != VK_SUCCESS) {
		U_LOG_E("[lift] %s: GPU copy did not complete within 1 s: %s", what, vk_result_string(ret));
		return false;
	}
	return true;
}

static void
lift_barrier(struct vk_bundle *vk,
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

//! Make @p img readable by TRANSFER (it was last written by another submission).
static void
lift_img_prepare_read(struct vk_bundle *vk, VkCommandBuffer cmd, struct lift_img *img)
{
	lift_barrier(vk, cmd, img->image, img->general ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
	             VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
	             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	img->general = true;
}

//! Make @p img writable by TRANSFER (its previous readers were CPU-drained).
static void
lift_img_prepare_write(struct vk_bundle *vk, VkCommandBuffer cmd, struct lift_img *img)
{
	lift_barrier(vk, cmd, img->image, img->general ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
	             VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
	             VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	img->general = true;
}

//! After a TRANSFER write: make it available to the next reader (GPU or host).
static void
lift_img_finish_write(struct vk_bundle *vk, VkCommandBuffer cmd, struct lift_img *img)
{
	lift_barrier(vk, cmd, img->image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
	             VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_HOST_READ_BIT,
	             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT);
}

/*!
 * Record the snapshot of (@p x, @p y, @p w, @p h) of @p src into the top-left
 * @p dw x @p dh of @p dst (which is exactly that size): 1:1 nearest when the
 * dims match, one linear blit when the cap shrank them.
 */
static void
lift_record_snapshot(struct vk_bundle *vk,
                     VkCommandBuffer cmd,
                     VkImage src,
                     VkImageLayout src_layout,
                     int32_t x,
                     int32_t y,
                     uint32_t w,
                     uint32_t h,
                     struct lift_img *dst,
                     uint32_t dw,
                     uint32_t dh)
{
	lift_img_prepare_write(vk, cmd, dst);
	VkImageBlit blit = {
	    .srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
	    .srcOffsets = {{x, y, 0}, {x + (int32_t)w, y + (int32_t)h, 1}},
	    .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
	    .dstOffsets = {{0, 0, 0}, {(int32_t)dw, (int32_t)dh, 1}},
	};
	const bool scaled = dw != w || dh != h;
	vk->vkCmdBlitImage(cmd, src, src_layout, dst->image, VK_IMAGE_LAYOUT_GENERAL, 1, &blit,
	                   scaled ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
	lift_img_finish_write(vk, cmd, dst);
}


/*
 *
 * Stream + module.
 *
 */

struct lift_in_slot
{
	struct lift_img img; //!< exactly the frame's size (CPU_READ_OFTEN)
	struct xrt_dp_lift_params params;
	float viewpoints[LIFT_VP_FLOATS];
	uint32_t viewpoint_floats;
};

struct lift_out_slot
{
	struct lift_img img;
	uint32_t view_count;
	uint8_t *blob; //!< GAUSSIANS
	size_t blob_size;
	uint32_t blob_format;
};

struct lift_stream
{
	uint64_t id;
	uint64_t owner;
	struct xrt_dp_lift_stream_info info;
	struct u_lift_mailbox mb;
	struct lift_in_slot in[U_LIFT_INPUT_SLOTS];
	struct lift_out_slot out[U_LIFT_RING_SIZE];
	struct xrt_dp_lift_params last_params;
	float last_viewpoints[LIFT_VP_FLOATS];
	uint32_t last_viewpoint_floats;

	uint32_t priority;  //!< XrLiftPriorityDXR
	bool dead;          //!< destroy requested; the lift thread reaps it
	bool converting;    //!< the lift thread is inside a conversion for it
	uint32_t producing; //!< producers between begin_submit and commit/abort (no reap)

	// Lift thread only.
	uint64_t dp_id;
	bool dp_created;
	bool dp_failed;
	struct lift_img dp_out; //!< import of the DP's output AHardwareBuffer (when it returns no VkImage)

	// Caller-input import cache (the connection's IPC thread only).
	struct lift_img imp;

	// Client export (IPC thread, under io_mtx).
	struct lift_img exp;
	uint64_t exp_fence_value;

	// Blob latch.
	uint8_t *blob_latched;
	size_t blob_latched_size;
	struct u_lift_frame_meta blob_latched_meta;
	uint32_t blob_latched_format;

	uint64_t last_stats_log_ns;
	uint32_t cap_logged_w, cap_logged_h;
};

struct comp_multi_lift
{
	struct vk_bundle *vk;
	xrt_dp_factory_vk_fn_t factory;

	pthread_mutex_t mtx;
	pthread_cond_t cv;
	bool stop;
	bool activate_requested;
	bool activated;
	struct xrt_dp_lift_caps caps;
	uint32_t logged_state;
	uint64_t next_stream_id;
	uint32_t live_streams;
	//! Ascending id order (ids only grow; removal keeps the order).
	struct lift_stream *streams[LIFT_MAX_STREAMS];
	uint32_t stream_count;
	struct u_lift_sched sched;

	pthread_t thread;
	bool thread_started;

	//! DXR_LIFT_MAX_INPUT_EDGE / debug.dxr.lift.max_input_edge (weave rects only).
	uint32_t max_input_edge;

	// Lift thread only.
	struct lift_gpu lift_gpu;
	struct xrt_display_processor_vk *dp;

	// IPC threads (explicit submit + acquire): one shared context.
	pthread_mutex_t io_mtx;
	struct lift_gpu io_gpu;

	// Tracked eyes, fed by the weave.
	pthread_mutex_t eyes_mtx;
	struct xrt_eye_positions eyes;
	uint64_t eyes_ns;
};

static const char *
state_str(uint32_t s)
{
	switch (s) {
	case XRT_DP_LIFT_STATE_READY: return "READY";
	case XRT_DP_LIFT_STATE_ACTIVATING: return "ACTIVATING";
	default: return "UNAVAILABLE";
	}
}

//! Caller holds mtx. WARN once per state CHANGE, never per poll.
static void
caps_set_state(struct comp_multi_lift *l, uint32_t state, const char *why)
{
	l->caps.state = state;
	if (l->logged_state != state) {
		l->logged_state = state;
		U_LOG_W("[lift] module state -> %s (modes=0x%x backend='%s' max_streams=%u max_views=%u)%s%s",
		        state_str(state), l->caps.modes, l->caps.backend, l->caps.max_streams, l->caps.max_views,
		        why != NULL ? " — " : "", why != NULL ? why : "");
	}
}

static struct lift_stream *
find_any(struct comp_multi_lift *l, uint64_t id)
{
	for (uint32_t i = 0; i < l->stream_count; i++) {
		if (l->streams[i]->id == id) {
			return l->streams[i];
		}
	}
	return NULL;
}

static struct lift_stream *
find_live(struct comp_multi_lift *l, uint64_t owner, uint64_t id)
{
	struct lift_stream *st = find_any(l, id);
	if (st == NULL || st->dead || st->owner != owner) {
		return NULL;
	}
	return st;
}

static void
stream_release_gpu(struct comp_multi_lift *l, struct lift_stream *st)
{
	struct vk_bundle *vk = l->vk;
	for (uint32_t i = 0; i < U_LIFT_INPUT_SLOTS; i++) {
		lift_img_destroy(vk, &st->in[i].img);
	}
	for (uint32_t i = 0; i < U_LIFT_RING_SIZE; i++) {
		lift_img_destroy(vk, &st->out[i].img);
		free(st->out[i].blob);
		st->out[i].blob = NULL;
	}
	lift_img_destroy(vk, &st->dp_out);
	lift_img_destroy(vk, &st->imp);
	lift_img_destroy(vk, &st->exp);
	free(st->blob_latched);
	st->blob_latched = NULL;
}

static void
lift_notify(struct comp_multi_lift *l)
{
	pthread_cond_broadcast(&l->cv);
}

//! pthread_cond_timedwait for @p ms milliseconds (CLOCK_REALTIME, the cond's clock).
static void
lift_wait_ms(struct comp_multi_lift *l, uint32_t ms)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_nsec += (long)(ms % 1000) * 1000000L;
	ts.tv_sec += (time_t)(ms / 1000) + ts.tv_nsec / 1000000000L;
	ts.tv_nsec %= 1000000000L;
	pthread_cond_timedwait(&l->cv, &l->mtx, &ts);
}


/*
 *
 * Lift thread.
 *
 */

struct lift_factory_call
{
	xrt_dp_factory_vk_fn_t factory;
	void *vk;
	void *pool;
	struct xrt_display_processor *xdp;
	xrt_result_t result;
};

static void
lift_run_factory_on_main_thread(void *data)
{
	struct lift_factory_call *c = (struct lift_factory_call *)data;
	c->result = c->factory(c->vk, c->pool, NULL /* window_handle */, (int32_t)VK_FORMAT_R8G8B8A8_UNORM, &c->xdp);
}

//! Bring up the lift GPU context + the vendor module's DP. Lift thread, no lock held.
static void
lift_activate(struct comp_multi_lift *l)
{
	char buf[64];
	const char *kill = lift_setting("DXR_LIFT", "debug.dxr.lift", buf, sizeof(buf));
	if (kill != NULL && strcmp(kill, "0") == 0) {
		pthread_mutex_lock(&l->mtx);
		l->activated = true;
		caps_set_state(l, XRT_DP_LIFT_STATE_UNAVAILABLE, "DXR_LIFT=0 / debug.dxr.lift=0 (kill switch)");
		pthread_mutex_unlock(&l->mtx);
		return;
	}
	if (l->factory == NULL) {
		pthread_mutex_lock(&l->mtx);
		l->activated = true;
		caps_set_state(l, XRT_DP_LIFT_STATE_UNAVAILABLE,
		               "no lift-only Vulkan DP factory (plug-in has no create_dp_vk_lift; "
		               "debug.dxr.lift.plugin picks another)");
		pthread_mutex_unlock(&l->mtx);
		return;
	}
	if (!lift_gpu_init(l->vk, &l->lift_gpu)) {
		pthread_mutex_lock(&l->mtx);
		l->activated = true;
		caps_set_state(l, XRT_DP_LIFT_STATE_UNAVAILABLE, "lift command pool / fence creation failed");
		pthread_mutex_unlock(&l->mtx);
		return;
	}

	// The factory runs on the service MAIN thread (Looper-bearing, JNI-attached),
	// like every Android DP factory: a vendor SDK may start async init there.
	struct lift_factory_call call = {
	    .factory = l->factory,
	    .vk = l->vk,
	    .pool = (void *)(uintptr_t)l->lift_gpu.pool,
	    .xdp = NULL,
	    .result = XRT_SUCCESS,
	};
	android_run_on_main_thread_blocking(lift_run_factory_on_main_thread, &call);
	struct xrt_display_processor_vk *dp = (struct xrt_display_processor_vk *)call.xdp;
	U_LOG_W("[lift] lift DP %s via the plug-in's lift-only Vulkan factory (service device, NULL window)",
	        call.result == XRT_SUCCESS && dp != NULL ? "created" : "REFUSED");

	struct xrt_dp_lift_caps caps;
	bool have = false;
	if (call.result == XRT_SUCCESS && dp != NULL && xrt_display_processor_vk_has_lift(dp)) {
		have = xrt_display_processor_vk_lift_get_caps(dp, &caps);
	} else {
		xrt_dp_lift_caps_init(&caps);
	}
	if (!have && dp != NULL) {
		struct xrt_display_processor *base = &dp->base;
		xrt_display_processor_destroy(&base);
		dp = NULL;
	}

	pthread_mutex_lock(&l->mtx);
	l->activated = true;
	l->dp = dp;
	l->caps = caps;
	if (!have) {
		caps_set_state(l, XRT_DP_LIFT_STATE_UNAVAILABLE,
		               call.result != XRT_SUCCESS ? "lift DP factory refused"
		                                          : "display processor ships no conversion module");
		pthread_mutex_unlock(&l->mtx);
		return;
	}
	if (l->caps.state > XRT_DP_LIFT_STATE_READY || l->caps.modes == 0) {
		l->caps.state = XRT_DP_LIFT_STATE_UNAVAILABLE;
	}
	l->logged_state = 0xffffffffu;
	caps_set_state(l, l->caps.state, NULL);
	pthread_mutex_unlock(&l->mtx);
}

//! Poll caps while not READY (a module warming up). Lift thread, no lock held.
static void
lift_poll_caps(struct comp_multi_lift *l)
{
	if (l->dp == NULL) {
		return;
	}
	struct xrt_dp_lift_caps caps;
	bool have = xrt_display_processor_vk_lift_get_caps(l->dp, &caps);
	pthread_mutex_lock(&l->mtx);
	if (!have) {
		caps_set_state(l, XRT_DP_LIFT_STATE_UNAVAILABLE, "module stopped answering");
	} else {
		const uint32_t st = caps.state > XRT_DP_LIFT_STATE_READY ? XRT_DP_LIFT_STATE_UNAVAILABLE : caps.state;
		l->caps = caps;
		l->caps.state = l->logged_state; // keep the logged value until caps_set_state compares
		caps_set_state(l, st, NULL);
	}
	pthread_mutex_unlock(&l->mtx);
}

static bool
stream_pinned(const struct lift_stream *st)
{
	for (uint32_t i = 0; i < U_LIFT_RING_SIZE; i++) {
		if (st->mb.out_pins[i] > 0) {
			return true;
		}
	}
	return false;
}

//! Destroy dead streams nobody still touches. Caller holds mtx; drops it around GPU / module work.
static void
lift_reap(struct comp_multi_lift *l)
{
	for (uint32_t i = 0; i < l->stream_count;) {
		struct lift_stream *st = l->streams[i];
		if (!st->dead || st->converting || st->producing > 0 || stream_pinned(st)) {
			i++;
			continue;
		}
		// Unlink (keep ascending order), then release outside the lock.
		for (uint32_t k = i; k + 1 < l->stream_count; k++) {
			l->streams[k] = l->streams[k + 1];
		}
		l->stream_count--;
		pthread_mutex_unlock(&l->mtx);
		if (st->dp_created && l->dp != NULL) {
			l->dp->lift_stream_destroy(l->dp, st->dp_id);
		}
		stream_release_gpu(l, st);
		U_LOG_I("[lift] stream %llu reaped (submitted=%llu converted=%llu dropped=%llu failed=%llu)",
		        (unsigned long long)st->id, (unsigned long long)st->mb.submitted,
		        (unsigned long long)st->mb.converted, (unsigned long long)st->mb.dropped,
		        (unsigned long long)st->mb.failed);
		free(st);
		pthread_mutex_lock(&l->mtx);
		i = 0; // the table may have changed while unlocked
	}
}

/*!
 * Convert the pending frame of @p st. Called with mtx HELD; drops it around
 * every GPU / module call and returns with it held.
 */
static void
lift_convert_one(struct comp_multi_lift *l, struct lift_stream *st)
{
	struct vk_bundle *vk = l->vk;
	int32_t in_slot = -1;
	struct u_lift_frame_meta meta = {0};
	if (!u_lift_mailbox_take_pending(&st->mb, os_monotonic_get_ns(), &in_slot, &meta)) {
		return;
	}
	st->converting = true;
	struct lift_in_slot *in = &st->in[in_slot];
	const struct xrt_dp_lift_params params = in->params;
	float vps[LIFT_VP_FLOATS];
	memcpy(vps, in->viewpoints, sizeof(vps));
	uint32_t vp_floats = in->viewpoint_floats;
	const uint32_t w = meta.width, h = meta.height;
	const uint32_t mode = st->info.mode;
	pthread_mutex_unlock(&l->mtx);

	// TRACKED viewpoints, resolved per conversion from the panel's latest
	// predicted eyes (fed by the weave): the lift DP has no tracker session.
	if (vp_floats == 0 && mode != XRT_DP_LIFT_MODE_GAUSSIANS && mode != XRT_DP_LIFT_MODE_DEPTH) {
		pthread_mutex_lock(&l->eyes_mtx);
		const struct xrt_eye_positions eyes = l->eyes;
		const uint64_t eyes_ns = l->eyes_ns;
		pthread_mutex_unlock(&l->eyes_mtx);
		if (eyes.valid && eyes.count >= 2 && os_monotonic_get_ns() - eyes_ns < LIFT_EYES_MAX_AGE_NS) {
			uint32_t n = eyes.count == params.view_count ? eyes.count : 2;
			n = n > XRT_DP_LIFT_MAX_EXPLICIT_VIEWPOINTS ? XRT_DP_LIFT_MAX_EXPLICIT_VIEWPOINTS : n;
			for (uint32_t i = 0; i < n; i++) {
				vps[3 * i + 0] = eyes.eyes[i].x;
				vps[3 * i + 1] = eyes.eyes[i].y;
				vps[3 * i + 2] = eyes.eyes[i].z;
			}
			vp_floats = 3 * n;
		}
	}

	// 1. The module's own stream, created lazily (lift thread only).
	if (!st->dp_created && !st->dp_failed) {
		if (l->dp->lift_stream_create(l->dp, &st->info, &st->dp_id)) {
			st->dp_created = true;
		} else {
			st->dp_failed = true;
			U_LOG_W("[lift] module refused stream %llu (mode=%u hint=%u) — its frames will never convert",
			        (unsigned long long)st->id, st->info.mode, st->info.content_hint);
		}
	}
	bool ok = st->dp_created && in->img.ahb != NULL && in->img.w == w && in->img.h == h;

	// 2. The conversion itself — synchronous, possibly long. The input slot is
	//    exactly w x h and its writes completed before the commit.
	const bool is_blob = mode == XRT_DP_LIFT_MODE_GAUSSIANS;
	void *out_ahb = NULL;
	VkImage_XDP out_image = (VkImage_XDP)0;
	uint32_t ow = 0, oh = 0, of = 0;
	const void *blob_bytes = NULL;
	size_t blob_size = 0;
	uint32_t blob_format = 0;
	if (ok) {
		if (is_blob) {
			ok = xrt_display_processor_vk_has_lift_blob(l->dp) &&
			     l->dp->lift_convert_blob(l->dp, st->dp_id, in->img.ahb, (VkImage_XDP)in->img.image, w, h,
			                              &params, &blob_format, &blob_bytes, &blob_size) &&
			     blob_bytes != NULL && blob_size > 0;
		} else {
			ok = l->dp->lift_convert(l->dp, st->dp_id, in->img.ahb, (VkImage_XDP)in->img.image, w, h,
			                         &params, vp_floats > 0 ? vps : NULL, vp_floats, &out_ahb, &out_image,
			                         &ow, &oh, &of) &&
			     (out_ahb != NULL || out_image != (VkImage_XDP)0) && ow > 0 && oh > 0;
		}
	}

	// 3. The DP's output (valid until its next call): a VkImage it handed us, or
	//    our own import of its AHardwareBuffer (cached by pointer identity).
	VkImage src_image = VK_NULL_HANDLE;
	bool src_ours = false;
	if (ok && !is_blob) {
		if (out_image != (VkImage_XDP)0) {
			src_image = (VkImage)out_image;
		} else {
			if (st->dp_out.ahb != (AHardwareBuffer *)out_ahb) {
				lift_img_destroy(vk, &st->dp_out);
				ok = lift_img_import(vk, (AHardwareBuffer *)out_ahb, &st->dp_out, "module output");
			}
			src_image = st->dp_out.image;
			src_ours = true;
		}
		if (ok && ahb_to_vk_format(of) == VK_FORMAT_UNDEFINED) {
			U_LOG_E("[lift] stream %llu: module output format 0x%x unsupported", (unsigned long long)st->id,
			        of);
			ok = false;
		}
	}
	uint8_t *blob_copy = NULL;
	if (ok && is_blob) {
		blob_copy = (uint8_t *)malloc(blob_size);
		ok = blob_copy != NULL;
		if (ok) {
			memcpy(blob_copy, blob_bytes, blob_size);
		}
	}

	pthread_mutex_lock(&l->mtx);
	u_lift_mailbox_finish_input(&st->mb, in_slot);
	int32_t out_slot = -1;
	if (ok) {
		// Wait while a consumer still pins the only writable slot (one CPU-waited copy).
		while (!u_lift_mailbox_begin_output(&st->mb, &out_slot)) {
			if (st->dead || l->stop) {
				ok = false;
				break;
			}
			lift_wait_ms(l, 5);
		}
	}
	pthread_mutex_unlock(&l->mtx);

	// 4. Copy the output into the ring (runtime-owned AHardwareBuffer image).
	if (ok && !is_blob) {
		struct lift_out_slot *o = &st->out[out_slot];
		const uint32_t ring_fmt =
		    of == AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM ? AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM : of;
		if (o->img.image == VK_NULL_HANDLE || o->img.w != ow || o->img.h != oh ||
		    o->img.ahb_format != ring_fmt) {
			lift_img_destroy(vk, &o->img);
			ok = lift_img_alloc(vk, ow, oh, ring_fmt, /*cpu_read*/ false, &o->img, "ring slot");
		}
		if (ok) {
			ok = lift_gpu_begin(vk, &l->lift_gpu);
		}
		if (ok) {
			VkCommandBuffer cmd = l->lift_gpu.cmd;
			if (src_ours) {
				lift_img_prepare_read(vk, cmd, &st->dp_out);
			} else {
				// The DP's image, GENERAL by contract; make its writes visible.
				lift_barrier(vk, cmd, src_image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
				             VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
				             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
			}
			lift_img_prepare_write(vk, cmd, &o->img);
			VkImageCopy copy = {
			    .srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
			    .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
			    .extent = {ow, oh, 1},
			};
			vk->vkCmdCopyImage(cmd, src_image, VK_IMAGE_LAYOUT_GENERAL, o->img.image,
			                   VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
			lift_img_finish_write(vk, cmd, &o->img);
			ok = lift_gpu_submit_wait(vk, &l->lift_gpu, "ring copy");
		}
		if (ok) {
			uint32_t views = 1;
			if (mode == XRT_DP_LIFT_MODE_SBS) {
				views = 2;
			} else if (mode == XRT_DP_LIFT_MODE_NVIEW) {
				views = params.view_count >= 1 ? params.view_count : 2;
			}
			o->view_count = views;
		}
	} else if (ok && is_blob) {
		struct lift_out_slot *o = &st->out[out_slot];
		free(o->blob);
		o->blob = blob_copy;
		o->blob_size = blob_size;
		o->blob_format = blob_format;
		blob_copy = NULL;
	}
	free(blob_copy);

	pthread_mutex_lock(&l->mtx);
	st->converting = false;
	if (out_slot >= 0) {
		if (ok) {
			u_lift_mailbox_publish_output(&st->mb, out_slot, &meta, os_monotonic_get_ns());
		} else {
			u_lift_mailbox_abort_output(&st->mb, out_slot);
		}
	} else if (!ok) {
		st->mb.failed++;
	}
	const uint64_t now = os_monotonic_get_ns();
	if (now - st->last_stats_log_ns > 5ull * 1000 * 1000 * 1000) {
		st->last_stats_log_ns = now;
		U_LOG_I(
		    "[lift] stream %llu: submitted=%llu converted=%llu dropped=%llu failed=%llu "
		    "latency last=%.1fms ema=%.1fms min=%.1fms max=%.1fms",
		    (unsigned long long)st->id, (unsigned long long)st->mb.submitted,
		    (unsigned long long)st->mb.converted, (unsigned long long)st->mb.dropped,
		    (unsigned long long)st->mb.failed, st->mb.lat_last_ns / 1e6, st->mb.lat_ema_ns / 1e6,
		    st->mb.lat_min_ns / 1e6, st->mb.lat_max_ns / 1e6);
	}
	lift_notify(l);
}

static bool
any_dead(struct comp_multi_lift *l)
{
	for (uint32_t i = 0; i < l->stream_count; i++) {
		if (l->streams[i]->dead) {
			return true;
		}
	}
	return false;
}

static bool
any_work(struct comp_multi_lift *l)
{
	for (uint32_t i = 0; i < l->stream_count; i++) {
		const struct lift_stream *st = l->streams[i];
		if (st->dead || (st->priority != U_LIFT_PRIORITY_PAUSED && u_lift_mailbox_has_pending(&st->mb))) {
			return true;
		}
	}
	return false;
}

static bool
lift_has_wake_reason(struct comp_multi_lift *l)
{
	return l->stop || (l->activate_requested && !l->activated) ||
	       (l->activated && (any_dead(l) || (l->caps.state == XRT_DP_LIFT_STATE_READY && any_work(l))));
}

static void *
lift_thread_main(void *data)
{
	struct comp_multi_lift *l = (struct comp_multi_lift *)data;

	// Attached for the thread's lifetime: a vendor module behind the lift slots
	// may call into Java (the Media SDK plan wraps a DexClassLoader-loaded module).
	struct _JavaVM *vm = android_globals_get_vm();
	bool attached = false;
	if (vm != NULL) {
		JNIEnv *env = NULL;
		attached = vm->functions->AttachCurrentThread(&vm->functions, &env, NULL) == JNI_OK;
	}

	uint64_t last_poll_ns = 0;
	pthread_mutex_lock(&l->mtx);
	for (;;) {
		if (!lift_has_wake_reason(l)) {
			lift_wait_ms(l, 250);
		}
		if (l->stop) {
			break;
		}
		if (l->activate_requested && !l->activated) {
			pthread_mutex_unlock(&l->mtx);
			lift_activate(l);
			pthread_mutex_lock(&l->mtx);
			continue;
		}
		if (!l->activated) {
			continue;
		}

		// A module warming up: poll ≤ 1 Hz until READY.
		const uint64_t now = os_monotonic_get_ns();
		if (l->dp != NULL && l->caps.state != XRT_DP_LIFT_STATE_READY && now - last_poll_ns > 1000000000ull) {
			last_poll_ns = now;
			pthread_mutex_unlock(&l->mtx);
			lift_poll_caps(l);
			pthread_mutex_lock(&l->mtx);
		}

		lift_reap(l);

		if (l->caps.state != XRT_DP_LIFT_STATE_READY || l->dp == NULL) {
			if (any_dead(l)) {
				lift_wait_ms(l, 5); // dead-but-pinned: don't spin on the wake predicate
			}
			continue; // frames wait (latest wins) until the module is READY
		}

		// A failed stream's frames can never convert: drain them.
		for (uint32_t i = 0; i < l->stream_count; i++) {
			struct lift_stream *st = l->streams[i];
			int32_t slot = -1;
			while (st->dp_failed && u_lift_mailbox_take_pending(&st->mb, now, &slot, NULL)) {
				u_lift_mailbox_finish_input(&st->mb, slot);
				st->mb.failed++;
			}
		}

		// One scheduling round (XrLiftPriorityDXR).
		struct u_lift_sched_entry entries[LIFT_MAX_STREAMS];
		uint32_t n_entries = 0;
		for (uint32_t i = 0; i < l->stream_count; i++) {
			const struct lift_stream *st = l->streams[i];
			if (st->dead || st->dp_failed) {
				continue;
			}
			entries[n_entries].id = st->id;
			entries[n_entries].priority = st->priority;
			entries[n_entries].pending = u_lift_mailbox_has_pending(&st->mb);
			n_entries++;
		}
		uint64_t plan_ids[LIFT_MAX_STREAMS];
		const uint32_t n = u_lift_sched_plan(&l->sched, entries, n_entries, plan_ids, LIFT_MAX_STREAMS);
		if (n == 0) {
			if (any_work(l)) {
				lift_wait_ms(l, 5); // only a not-this-round LOW / dead-but-pinned left
			}
			continue;
		}
		for (uint32_t i = 0; i < n && !l->stop; i++) {
			struct lift_stream *st = find_any(l, plan_ids[i]); // re-look-up: unlocked between conversions
			if (st == NULL || st->dead) {
				continue;
			}
			lift_convert_one(l, st);
		}
	}

	// Shutdown: every stream, then the DP, on this thread.
	for (uint32_t i = 0; i < l->stream_count; i++) {
		struct lift_stream *st = l->streams[i];
		if (st->dp_created && l->dp != NULL) {
			l->dp->lift_stream_destroy(l->dp, st->dp_id);
		}
		stream_release_gpu(l, st);
		free(st);
	}
	l->stream_count = 0;
	pthread_mutex_unlock(&l->mtx);
	if (l->dp != NULL) {
		struct xrt_display_processor *base = &l->dp->base;
		xrt_display_processor_destroy(&base);
		l->dp = NULL;
	}
	lift_gpu_fini(l->vk, &l->lift_gpu);

	if (attached) {
		vm->functions->DetachCurrentThread(&vm->functions);
	}
	return NULL;
}


/*
 *
 * Module lifetime.
 *
 */

static struct comp_multi_lift *
lift_create(struct vk_bundle *vk, void *factory)
{
	struct comp_multi_lift *l = U_TYPED_CALLOC(struct comp_multi_lift);
	if (l == NULL) {
		return NULL;
	}
	l->vk = vk;
	l->factory = (xrt_dp_factory_vk_fn_t)factory;
	pthread_mutex_init(&l->mtx, NULL);
	pthread_cond_init(&l->cv, NULL);
	pthread_mutex_init(&l->io_mtx, NULL);
	pthread_mutex_init(&l->eyes_mtx, NULL);
	l->logged_state = 0xffffffffu;
	u_lift_sched_init(&l->sched);
	xrt_dp_lift_caps_init(&l->caps);
	l->caps.state = XRT_DP_LIFT_STATE_ACTIVATING;

	char buf[32];
	l->max_input_edge = u_lift_max_input_edge_parse(
	    lift_setting("DXR_LIFT_MAX_INPUT_EDGE", "debug.dxr.lift.max_input_edge", buf, sizeof(buf)));
	if (l->max_input_edge != U_LIFT_MAX_INPUT_EDGE_DEFAULT) {
		U_LOG_W("[lift] max input edge %u%s", l->max_input_edge,
		        l->max_input_edge == 0 ? " (weave-rect snapshots uncapped)" : "");
	}
	// The letterbox crop is Windows-only in v1 (its profile pass is HLSL).
	if (lift_setting("DXR_LIFT_LETTERBOX", "debug.dxr.lift.letterbox", buf, sizeof(buf)) != NULL) {
		U_LOG_W(
		    "[lift] letterbox crop requested but not implemented on Android yet — snapshots take the whole "
		    "rect");
	}

	if (l->factory == NULL) {
		// No module on this plug-in: answer UNAVAILABLE from the first query
		// (the thread still runs, to reap streams created meanwhile — none can be).
		l->activated = true;
		l->activate_requested = true;
		caps_set_state(l, XRT_DP_LIFT_STATE_UNAVAILABLE,
		               "no lift-only Vulkan DP factory (the plug-in has no create_dp_vk_lift; "
		               "debug.dxr.lift.plugin=<id> takes another bundled plug-in's)");
	}

	if (pthread_create(&l->thread, NULL, lift_thread_main, l) != 0) {
		U_LOG_E("[lift] lift thread creation failed — lift UNAVAILABLE");
		l->activated = true;
		l->caps.state = XRT_DP_LIFT_STATE_UNAVAILABLE;
	} else {
		l->thread_started = true;
	}
	return l;
}

void
comp_multi_lift_destroy(struct comp_multi_lift **lift_ptr)
{
	if (lift_ptr == NULL || *lift_ptr == NULL) {
		return;
	}
	struct comp_multi_lift *l = *lift_ptr;
	pthread_mutex_lock(&l->mtx);
	l->stop = true;
	lift_notify(l);
	pthread_mutex_unlock(&l->mtx);
	if (l->thread_started) {
		pthread_join(l->thread, NULL);
	}
	// Streams are freed by the thread; anything left (thread never started) here.
	for (uint32_t i = 0; i < l->stream_count; i++) {
		stream_release_gpu(l, l->streams[i]);
		free(l->streams[i]);
	}
	lift_gpu_fini(l->vk, &l->io_gpu);
	pthread_mutex_destroy(&l->eyes_mtx);
	pthread_mutex_destroy(&l->io_mtx);
	pthread_cond_destroy(&l->cv);
	pthread_mutex_destroy(&l->mtx);
	free(l);
	*lift_ptr = NULL;
}

static pthread_mutex_t g_lift_create_lock = PTHREAD_MUTEX_INITIALIZER;

struct comp_multi_lift *
comp_multi_lift_peek_system(struct xrt_system_compositor *xsysc)
{
	if (xsysc == NULL) {
		return NULL;
	}
	struct multi_system_compositor *msc = multi_system_compositor(xsysc);
	pthread_mutex_lock(&g_lift_create_lock);
	struct comp_multi_lift *l = msc->lift;
	pthread_mutex_unlock(&g_lift_create_lock);
	return l;
}

struct comp_multi_lift *
comp_multi_lift_for_system(struct xrt_system_compositor *xsysc)
{
	if (xsysc == NULL) {
		return NULL;
	}
	struct multi_system_compositor *msc = multi_system_compositor(xsysc);
	pthread_mutex_lock(&g_lift_create_lock);
	if (msc->lift == NULL && msc->target_service != NULL) {
		struct vk_bundle *vk = comp_target_service_get_vk(msc->target_service);
		if (vk != NULL && vk->device != VK_NULL_HANDLE) {
			void *factory = msc->base.info.dp_factory_vk_lift;
			msc->lift = lift_create(vk, factory);
			U_LOG_W("[lift] lift module created (lift-only Vulkan factory %s)",
			        factory != NULL ? "present" : "absent");
		}
	}
	struct comp_multi_lift *l = msc->lift;
	pthread_mutex_unlock(&g_lift_create_lock);
	return l;
}


/*
 *
 * Public API.
 *
 */

void
comp_multi_lift_get_caps(struct comp_multi_lift *l, struct xrt_dp_lift_caps *out)
{
	xrt_dp_lift_caps_init(out);
	if (l == NULL) {
		return;
	}
	pthread_mutex_lock(&l->mtx);
	*out = l->caps;
	out->struct_size = (uint32_t)sizeof(*out);
	if (!l->activate_requested) {
		l->activate_requested = true;
		lift_notify(l);
	}
	pthread_mutex_unlock(&l->mtx);
}

xrt_result_t
comp_multi_lift_stream_create(struct comp_multi_lift *l,
                              uint64_t owner,
                              const struct xrt_dp_lift_stream_info *info,
                              uint64_t *out_id)
{
	if (l == NULL || info == NULL || out_id == NULL) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	const uint32_t m = info->mode;
	if (m != XRT_DP_LIFT_MODE_DEPTH && m != XRT_DP_LIFT_MODE_SBS && m != XRT_DP_LIFT_MODE_NVIEW &&
	    m != XRT_DP_LIFT_MODE_GAUSSIANS) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	pthread_mutex_lock(&l->mtx);
	l->activate_requested = true;
	xrt_result_t xret = XRT_SUCCESS;
	const uint32_t max_streams = l->caps.max_streams > 0 ? l->caps.max_streams : 4;
	if (l->caps.state == XRT_DP_LIFT_STATE_UNAVAILABLE && l->activated) {
		xret = XRT_ERROR_FEATURE_NOT_SUPPORTED;
	} else if (l->caps.state == XRT_DP_LIFT_STATE_READY && (l->caps.modes & m) == 0) {
		xret = XRT_ERROR_FEATURE_NOT_SUPPORTED;
	} else if (l->live_streams >= max_streams || l->stream_count >= LIFT_MAX_STREAMS) {
		xret = XRT_ERROR_CLIENT_LIMIT_REACHED;
	}
	struct lift_stream *st = NULL;
	if (xret == XRT_SUCCESS) {
		st = U_TYPED_CALLOC(struct lift_stream);
		if (st == NULL) {
			xret = XRT_ERROR_ALLOCATION;
		}
	}
	if (xret != XRT_SUCCESS) {
		pthread_mutex_unlock(&l->mtx);
		return xret;
	}
	st->id = ++l->next_stream_id;
	st->owner = owner;
	st->info = *info;
	st->info.struct_size = (uint32_t)sizeof(st->info);
	if (!(st->info.input_scale > 0.0f) || st->info.input_scale > 1.0f) {
		st->info.input_scale = 1.0f;
	}
	st->priority = U_LIFT_PRIORITY_NORMAL;
	u_lift_mailbox_init(&st->mb);
	st->last_params.struct_size = (uint32_t)sizeof(st->last_params);
	st->last_params.convergence = -1.0f;
	st->last_params.strength = 1.0f;
	st->last_params.inpaint = 1;
	st->last_params.view_count = m == XRT_DP_LIFT_MODE_NVIEW ? 4 : 2;
	*out_id = st->id;
	l->streams[l->stream_count++] = st;
	l->live_streams++;
	U_LOG_W("[lift] stream %llu created (mode=%u hint=%u scale=%.2f, owner=%llu)", (unsigned long long)st->id, m,
	        info->content_hint, st->info.input_scale, (unsigned long long)owner);
	lift_notify(l);
	pthread_mutex_unlock(&l->mtx);
	return XRT_SUCCESS;
}

void
comp_multi_lift_stream_destroy(struct comp_multi_lift *l, uint64_t owner, uint64_t id)
{
	if (l == NULL) {
		return;
	}
	pthread_mutex_lock(&l->mtx);
	struct lift_stream *st = find_live(l, owner, id);
	if (st != NULL) {
		st->dead = true;
		l->live_streams--;
		U_LOG_W("[lift] stream %llu destroyed", (unsigned long long)id);
		lift_notify(l);
	}
	pthread_mutex_unlock(&l->mtx);
}

void
comp_multi_lift_release_owner(struct comp_multi_lift *l, uint64_t owner)
{
	if (l == NULL || owner == 0) {
		return;
	}
	pthread_mutex_lock(&l->mtx);
	uint32_t n = 0;
	for (uint32_t i = 0; i < l->stream_count; i++) {
		struct lift_stream *st = l->streams[i];
		if (st->owner == owner && !st->dead) {
			st->dead = true;
			l->live_streams--;
			n++;
		}
	}
	if (n > 0) {
		U_LOG_W("[lift] client gone: %u stream(s) released", n);
		lift_notify(l);
	}
	pthread_mutex_unlock(&l->mtx);
}

uint32_t
comp_multi_lift_stream_mode(struct comp_multi_lift *l, uint64_t owner, uint64_t id)
{
	if (l == NULL) {
		return 0;
	}
	pthread_mutex_lock(&l->mtx);
	struct lift_stream *st = find_live(l, owner, id);
	const uint32_t mode = st != NULL ? st->info.mode : 0;
	pthread_mutex_unlock(&l->mtx);
	return mode;
}

/*!
 * Producer, step 1 (caller holds mtx): take an input slot for a @p dw x @p dh
 * frame and record its parameters. On true the stream is marked `producing`
 * (never reaped) and the slot is WRITING until step 2. @p viewpoints NULL =
 * keep the stream's last ones.
 */
static xrt_result_t
producer_begin_locked(struct comp_multi_lift *l,
                      struct lift_stream *st,
                      const struct xrt_dp_lift_params *params,
                      const float *viewpoints,
                      uint32_t viewpoint_floats,
                      float focal_scale,
                      int32_t *out_slot)
{
	if (!u_lift_mailbox_begin_submit(&st->mb, out_slot)) {
		return XRT_ERROR_WEAVE_REFUSED;
	}
	if (params != NULL) {
		st->last_params = *params;
		st->last_params.struct_size = (uint32_t)sizeof(st->last_params);
		if (st->last_params.convergence > 1.0f) {
			st->last_params.convergence = 1.0f; // [0,1]; negative = AUTO passes through
		}
		if (!(st->last_params.strength >= 0.0f)) {
			st->last_params.strength = 1.0f;
		}
		if (st->last_params.view_count == 0) {
			st->last_params.view_count = st->info.mode == XRT_DP_LIFT_MODE_NVIEW ? 4 : 2;
		}
		uint32_t n = viewpoint_floats > LIFT_VP_FLOATS ? LIFT_VP_FLOATS : viewpoint_floats;
		st->last_viewpoint_floats = viewpoints != NULL ? n - n % 3 : 0;
		if (st->last_viewpoint_floats > 0) {
			memcpy(st->last_viewpoints, viewpoints, st->last_viewpoint_floats * sizeof(float));
		}
	}
	struct lift_in_slot *in = &st->in[*out_slot];
	in->params = st->last_params;
	if (in->params.focal_px > 0.0f) {
		in->params.focal_px *= focal_scale; // focal is in INPUT pixels
	}
	in->viewpoint_floats = st->last_viewpoint_floats;
	memcpy(in->viewpoints, st->last_viewpoints, sizeof(in->viewpoints));
	st->producing++;
	return XRT_SUCCESS;
}

//! Make input slot @p in exactly @p w x @p h (RGBA8, CPU-readable). The slot is WRITING: ours alone.
static bool
in_slot_ensure(struct comp_multi_lift *l, struct lift_in_slot *in, uint32_t w, uint32_t h)
{
	if (in->img.image != VK_NULL_HANDLE && in->img.w == w && in->img.h == h) {
		return true;
	}
	lift_img_destroy(l->vk, &in->img);
	if (!lift_img_alloc(l->vk, w, h, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, /*cpu_read*/ true, &in->img,
	                    "input slot")) {
		return false;
	}
	U_LOG_I("[lift] input slot %ux%u ready", w, h);
	return true;
}

//! Producer, step 2: commit or abort, drop `producing`.
static uint64_t
producer_end(struct comp_multi_lift *l,
             struct lift_stream *st,
             int32_t slot,
             bool ok,
             int64_t source_time,
             uint32_t w,
             uint32_t h)
{
	uint64_t frame_id = 0;
	pthread_mutex_lock(&l->mtx);
	st->producing--;
	if (ok) {
		frame_id = u_lift_mailbox_commit_submit(&st->mb, slot, source_time, os_monotonic_get_ns(), w, h);
	} else {
		u_lift_mailbox_abort_submit(&st->mb, slot);
	}
	lift_notify(l);
	pthread_mutex_unlock(&l->mtx);
	return frame_id;
}

xrt_result_t
comp_multi_lift_submit_ahb(struct comp_multi_lift *l,
                           uint64_t owner,
                           uint64_t id,
                           xrt_graphics_buffer_handle_t ahb_handle,
                           uint32_t w,
                           uint32_t h,
                           int64_t source_time,
                           const struct xrt_dp_lift_params *params,
                           const float *viewpoints,
                           uint32_t viewpoint_floats,
                           uint64_t *out_frame_id)
{
	AHardwareBuffer *ahb = (AHardwareBuffer *)ahb_handle;
	*out_frame_id = 0;
	if (l == NULL || ahb == NULL) {
		if (ahb != NULL) {
			AHardwareBuffer_release(ahb);
		}
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	if (w == 0 || h == 0) {
		AHardwareBuffer_release(ahb);
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	}

	pthread_mutex_lock(&l->mtx);
	struct lift_stream *st = find_live(l, owner, id);
	if (st == NULL) {
		pthread_mutex_unlock(&l->mtx);
		AHardwareBuffer_release(ahb);
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE; // unknown stream: non-fatal
	}
	if (!l->activated) {
		l->activate_requested = true;
		lift_notify(l);
		pthread_mutex_unlock(&l->mtx);
		AHardwareBuffer_release(ahb);
		return XRT_SUCCESS; // frameId 0: not taken, module not up yet
	}
	int32_t slot = -1;
	xrt_result_t xret = producer_begin_locked(l, st, params, viewpoints, viewpoint_floats, 1.0f, &slot);
	pthread_mutex_unlock(&l->mtx);
	if (xret != XRT_SUCCESS) {
		AHardwareBuffer_release(ahb);
		return xret;
	}

	// The per-stream import cache: AHardwareBuffer pointers are stable while
	// referenced, and the cache holds a reference — pointer identity is a sound key.
	bool ok = true;
	if (st->imp.ahb != ahb) {
		lift_img_destroy(l->vk, &st->imp);
		ok = lift_img_import(l->vk, ahb, &st->imp, "caller input");
		if (ok) {
			U_LOG_I("[lift] stream %llu input import cached (%ux%u fmt=0x%x)", (unsigned long long)id,
			        st->imp.w, st->imp.h, st->imp.ahb_format);
		}
	}
	AHardwareBuffer_release(ahb); // the IPC receive's reference; the cache holds its own
	if (ok && (w > st->imp.w || h > st->imp.h)) {
		U_LOG_E("[lift] stream %llu: extent %ux%u exceeds the input %ux%u", (unsigned long long)id, w, h,
		        st->imp.w, st->imp.h);
		ok = false;
	}
	ok = ok && in_slot_ensure(l, &st->in[slot], w, h);
	if (ok) {
		pthread_mutex_lock(&l->io_mtx);
		ok = lift_gpu_init(l->vk, &l->io_gpu) && lift_gpu_begin(l->vk, &l->io_gpu);
		if (ok) {
			VkCommandBuffer cmd = l->io_gpu.cmd;
			// The caller's buffer: its writes are complete (the input contract).
			lift_img_prepare_read(l->vk, cmd, &st->imp);
			lift_record_snapshot(l->vk, cmd, st->imp.image, VK_IMAGE_LAYOUT_GENERAL, 0, 0, w, h,
			                     &st->in[slot].img, w, h);
			ok = lift_gpu_submit_wait(l->vk, &l->io_gpu, "submit snapshot");
		}
		pthread_mutex_unlock(&l->io_mtx);
	}
	*out_frame_id = producer_end(l, st, slot, ok, source_time, w, h);
	return ok ? XRT_SUCCESS : XRT_ERROR_WEAVE_REFUSED;
}

xrt_result_t
comp_multi_lift_acquire_result(
    struct comp_multi_lift *l, uint64_t owner, uint64_t id, bool *out_ready, struct xrt_lift_result *out)
{
	*out_ready = false;
	U_ZERO(out);
	if (l == NULL) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	int32_t slot = -1;
	struct u_lift_frame_meta meta = {0};
	pthread_mutex_lock(&l->mtx);
	struct lift_stream *st = find_live(l, owner, id);
	if (st == NULL) {
		pthread_mutex_unlock(&l->mtx);
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	}
	if (st->info.mode == XRT_DP_LIFT_MODE_GAUSSIANS) {
		pthread_mutex_unlock(&l->mtx);
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	if (!u_lift_mailbox_pin_latest(&st->mb, true, &slot, &meta)) {
		pthread_mutex_unlock(&l->mtx);
		return XRT_SUCCESS; // nothing newer: NOT READY
	}
	pthread_mutex_unlock(&l->mtx);

	struct lift_out_slot *o = &st->out[slot];
	xrt_result_t xret = XRT_SUCCESS;
	bool realloc = false;
	pthread_mutex_lock(&l->io_mtx);
	if (st->exp.image == VK_NULL_HANDLE || st->exp.w != o->img.w || st->exp.h != o->img.h ||
	    st->exp.ahb_format != o->img.ahb_format) {
		lift_img_destroy(l->vk, &st->exp);
		// DEPTH results are CPU-readable: a client (the browser's GPU process)
		// reads depth values on the CPU — AHardwareBuffer_lock + the stride from
		// AHardwareBuffer_describe — without a Vulkan readback of its own.
		// SBS / NVIEW exports stay GPU-only: they are only ever sampled, and a
		// CPU-read usage can push gralloc to a linear / uncached layout that
		// costs GPU sampling bandwidth on every weave.
		const bool cpu_read = st->info.mode == XRT_DP_LIFT_MODE_DEPTH;
		if (!lift_img_alloc(l->vk, o->img.w, o->img.h, o->img.ahb_format, cpu_read, &st->exp, "export")) {
			xret = XRT_ERROR_WEAVE_REFUSED;
		} else {
			realloc = true;
			U_LOG_W("[lift] stream %llu export AHardwareBuffer %ux%u fmt=0x%x ready%s",
			        (unsigned long long)id, st->exp.w, st->exp.h, st->exp.ahb_format,
			        cpu_read ? " (CPU_READ_OFTEN: DEPTH)" : "");
		}
	}
	if (xret == XRT_SUCCESS) {
		bool ok = lift_gpu_init(l->vk, &l->io_gpu) && lift_gpu_begin(l->vk, &l->io_gpu);
		if (ok) {
			VkCommandBuffer cmd = l->io_gpu.cmd;
			lift_img_prepare_read(l->vk, cmd, &o->img);
			lift_img_prepare_write(l->vk, cmd, &st->exp);
			VkImageCopy copy = {
			    .srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
			    .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
			    .extent = {o->img.w, o->img.h, 1},
			};
			l->vk->vkCmdCopyImage(cmd, o->img.image, VK_IMAGE_LAYOUT_GENERAL, st->exp.image,
			                      VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
			lift_img_finish_write(l->vk, cmd, &st->exp);
			ok = lift_gpu_submit_wait(l->vk, &l->io_gpu, "export copy");
		}
		if (ok) {
			st->exp_fence_value++;
			out->frame_id = meta.frame_id;
			out->source_time = meta.source_time;
			out->fence_value = st->exp_fence_value;
			out->latency_ns = meta.done_ns >= meta.submit_ns ? meta.done_ns - meta.submit_ns : 0;
			out->width = o->img.w;
			out->height = o->img.h;
			out->format = o->img.ahb_format; // AHARDWAREBUFFER_FORMAT_* on Android
			out->view_count = o->view_count;
			out->output_realloc = realloc;
			*out_ready = true;
		} else {
			xret = XRT_ERROR_WEAVE_REFUSED;
		}
	}
	pthread_mutex_unlock(&l->io_mtx);

	pthread_mutex_lock(&l->mtx);
	u_lift_mailbox_unpin(&st->mb, slot); // the stream is not reaped while pinned
	lift_notify(l);
	pthread_mutex_unlock(&l->mtx);
	return xret;
}

bool
comp_multi_lift_export_output(struct comp_multi_lift *l,
                              uint64_t owner,
                              uint64_t id,
                              xrt_graphics_buffer_handle_t *out_handle,
                              uint32_t *out_w,
                              uint32_t *out_h,
                              uint32_t *out_format)
{
	if (l == NULL) {
		return false;
	}
	bool ok = false;
	pthread_mutex_lock(&l->mtx);
	struct lift_stream *st = find_live(l, owner, id);
	pthread_mutex_unlock(&l->mtx);
	if (st == NULL) {
		return false;
	}
	pthread_mutex_lock(&l->io_mtx);
	if (st->exp.ahb != NULL) {
		*out_handle = (xrt_graphics_buffer_handle_t)st->exp.ahb; // borrowed; see the header
		*out_w = st->exp.w;
		*out_h = st->exp.h;
		*out_format = st->exp.ahb_format;
		ok = true;
	}
	pthread_mutex_unlock(&l->io_mtx);
	return ok;
}

xrt_result_t
comp_multi_lift_acquire_blob(struct comp_multi_lift *l,
                             uint64_t owner,
                             uint64_t id,
                             uint64_t capacity,
                             bool *out_ready,
                             struct xrt_lift_blob_info *info,
                             uint8_t **out_bytes)
{
	*out_ready = false;
	U_ZERO(info);
	*out_bytes = NULL;
	if (l == NULL) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	xrt_result_t xret = XRT_SUCCESS;
	pthread_mutex_lock(&l->mtx);
	struct lift_stream *st = find_live(l, owner, id);
	if (st == NULL) {
		xret = XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	} else if (st->info.mode != XRT_DP_LIFT_MODE_GAUSSIANS) {
		xret = XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	if (xret != XRT_SUCCESS) {
		pthread_mutex_unlock(&l->mtx);
		return xret;
	}
	if (st->blob_latched == NULL) {
		int32_t slot = -1;
		struct u_lift_frame_meta meta = {0};
		if (!u_lift_mailbox_pin_latest(&st->mb, true, &slot, &meta)) {
			pthread_mutex_unlock(&l->mtx);
			return XRT_SUCCESS; // nothing newer
		}
		const struct lift_out_slot *o = &st->out[slot];
		if (o->blob != NULL && o->blob_size > 0) {
			st->blob_latched = (uint8_t *)malloc(o->blob_size);
			if (st->blob_latched != NULL) {
				memcpy(st->blob_latched, o->blob, o->blob_size);
				st->blob_latched_size = o->blob_size;
				st->blob_latched_meta = meta;
				st->blob_latched_format = o->blob_format;
			}
		}
		u_lift_mailbox_unpin(&st->mb, slot); // the latch holds its own copy
		if (st->blob_latched == NULL) {
			pthread_mutex_unlock(&l->mtx);
			return XRT_SUCCESS;
		}
	}
	info->frame_id = st->blob_latched_meta.frame_id;
	info->source_time = st->blob_latched_meta.source_time;
	info->format = st->blob_latched_format;
	info->byte_count = st->blob_latched_size;
	*out_ready = true;
	if (capacity >= st->blob_latched_size) {
		// Delivered: hand the latched copy over; the latch is consumed.
		*out_bytes = st->blob_latched;
		st->blob_latched = NULL;
		st->blob_latched_size = 0;
	}
	pthread_mutex_unlock(&l->mtx);
	return XRT_SUCCESS;
}

xrt_result_t
comp_multi_lift_set_priority(struct comp_multi_lift *l, uint64_t owner, uint64_t id, uint32_t priority)
{
	if (l == NULL) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	if (priority > U_LIFT_PRIORITY_HIGH) {
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	}
	pthread_mutex_lock(&l->mtx);
	struct lift_stream *st = find_live(l, owner, id);
	if (st == NULL) {
		pthread_mutex_unlock(&l->mtx);
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	}
	if (st->priority != priority) {
		U_LOG_I("[lift] stream %llu priority %u -> %u", (unsigned long long)id, st->priority, priority);
		st->priority = priority;
		lift_notify(l); // an un-paused stream may have a frame waiting
	}
	pthread_mutex_unlock(&l->mtx);
	return XRT_SUCCESS;
}

xrt_result_t
comp_multi_lift_get_stats(struct comp_multi_lift *l, uint64_t owner, uint64_t id, struct xrt_lift_stream_stats *out)
{
	U_ZERO(out);
	if (l == NULL) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	pthread_mutex_lock(&l->mtx);
	struct lift_stream *st = find_live(l, owner, id);
	if (st == NULL) {
		pthread_mutex_unlock(&l->mtx);
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	}
	out->priority = st->priority;
	out->submitted = st->mb.submitted;
	out->converted = st->mb.converted;
	out->dropped = st->mb.dropped;
	out->failed = st->mb.failed;
	out->latency_last_ns = st->mb.lat_last_ns;
	out->latency_avg_ns = st->mb.lat_ema_ns;
	out->latency_min_ns = st->mb.lat_min_ns;
	out->latency_max_ns = st->mb.lat_max_ns;
	out->rate_hz = u_lift_mailbox_rate_hz(&st->mb);
	pthread_mutex_unlock(&l->mtx);
	return XRT_SUCCESS;
}

void
comp_multi_lift_note_eyes(struct comp_multi_lift *l, const struct xrt_eye_positions *eyes)
{
	if (l == NULL || eyes == NULL || !eyes->valid) {
		return;
	}
	pthread_mutex_lock(&l->eyes_mtx);
	l->eyes = *eyes;
	l->eyes_ns = os_monotonic_get_ns();
	pthread_mutex_unlock(&l->eyes_mtx);
}


/*
 *
 * The weave path.
 *
 */

bool
comp_multi_lift_weave_snapshot(struct comp_multi_lift *l,
                               uint64_t owner,
                               uint64_t id,
                               VkCommandBuffer cmd,
                               VkImage src,
                               VkImageLayout src_layout,
                               int32_t x,
                               int32_t y,
                               uint32_t w,
                               uint32_t h,
                               const struct xrt_dp_lift_params *params,
                               struct comp_multi_lift_ticket *out_ticket)
{
	U_ZERO(out_ticket);
	if (l == NULL || w == 0 || h == 0) {
		return false;
	}
	uint32_t dw = w, dh = h;
	const bool capped = u_lift_cap_dims(w, h, l->max_input_edge, &dw, &dh);

	pthread_mutex_lock(&l->mtx);
	struct lift_stream *st = find_live(l, owner, id);
	if (st == NULL) {
		pthread_mutex_unlock(&l->mtx);
		return false;
	}
	if (!l->activated) {
		l->activate_requested = true;
		lift_notify(l);
		pthread_mutex_unlock(&l->mtx);
		return false; // not taken: the module is not up yet
	}
	int32_t slot = -1;
	const float focal_scale = (float)dw / (float)w;
	xrt_result_t xret = producer_begin_locked(l, st, params, NULL, 0, focal_scale, &slot);
	if (xret == XRT_SUCCESS && capped && (st->cap_logged_w != dw || st->cap_logged_h != dh)) {
		st->cap_logged_w = dw;
		st->cap_logged_h = dh;
		U_LOG_W("[lift] stream %llu: snapshot %ux%u capped to %ux%u (max input edge %u)",
		        (unsigned long long)id, w, h, dw, dh, l->max_input_edge);
	}
	pthread_mutex_unlock(&l->mtx);
	if (xret != XRT_SUCCESS) {
		return false;
	}

	if (!in_slot_ensure(l, &st->in[slot], dw, dh)) {
		(void)producer_end(l, st, slot, false, 0, 0, 0);
		return false;
	}
	lift_record_snapshot(l->vk, cmd, src, src_layout, x, y, w, h, &st->in[slot].img, dw, dh);

	out_ticket->lift = l;
	out_ticket->stream_id = id;
	out_ticket->slot = slot;
	out_ticket->w = dw;
	out_ticket->h = dh;
	out_ticket->active = true;
	return true;
}

void
comp_multi_lift_weave_snapshot_done(struct comp_multi_lift_ticket *t, bool gpu_ok)
{
	if (t == NULL || !t->active || t->lift == NULL) {
		return;
	}
	struct comp_multi_lift *l = t->lift;
	pthread_mutex_lock(&l->mtx);
	struct lift_stream *st = find_any(l, t->stream_id); // `producing` kept it mapped
	pthread_mutex_unlock(&l->mtx);
	if (st != NULL) {
		// Stamped with the runtime clock: a weave rect has no caller timestamp.
		(void)producer_end(l, st, t->slot, gpu_ok, (int64_t)os_monotonic_get_ns(), t->w, t->h);
	}
	t->active = false;
}

bool
comp_multi_lift_pin_latest(struct comp_multi_lift *l, uint64_t owner, uint64_t id, struct comp_multi_lift_pin *out)
{
	U_ZERO(out);
	if (l == NULL) {
		return false;
	}
	pthread_mutex_lock(&l->mtx);
	struct lift_stream *st = find_live(l, owner, id);
	int32_t slot = -1;
	if (st == NULL || st->info.mode == XRT_DP_LIFT_MODE_GAUSSIANS ||
	    !u_lift_mailbox_pin_latest(&st->mb, false, &slot, NULL)) {
		pthread_mutex_unlock(&l->mtx);
		return false;
	}
	const struct lift_out_slot *o = &st->out[slot];
	if (o->img.image == VK_NULL_HANDLE || o->view_count == 0) {
		u_lift_mailbox_unpin(&st->mb, slot);
		pthread_mutex_unlock(&l->mtx);
		return false;
	}
	out->lift = l;
	out->stream_id = id;
	out->slot = slot;
	out->image = o->img.image;
	out->width = o->img.w;
	out->height = o->img.h;
	out->view_count = o->view_count;
	out->valid = true;
	pthread_mutex_unlock(&l->mtx);
	return true;
}

void
comp_multi_lift_unpin(struct comp_multi_lift_pin *pin)
{
	if (pin == NULL || !pin->valid || pin->lift == NULL) {
		return;
	}
	struct comp_multi_lift *l = pin->lift;
	pthread_mutex_lock(&l->mtx);
	struct lift_stream *st = find_any(l, pin->stream_id); // dead streams stay mapped until unpinned
	if (st != NULL) {
		u_lift_mailbox_unpin(&st->mb, pin->slot);
	}
	lift_notify(l);
	pthread_mutex_unlock(&l->mtx);
	U_ZERO(pin);
}

bool
comp_multi_lift_set_weave_rects(struct xrt_compositor *xc,
                                uint64_t owner,
                                uint32_t count,
                                const struct xrt_lift_weave_rect *rects)
{
	struct multi_compositor *mc = multi_compositor(xc);
	if (mc == NULL || mc->msc == NULL || count > XRT_LIFT_WEAVE_RECTS_MAX || (count > 0 && rects == NULL)) {
		return false;
	}
	if (count > 0) {
		// Create the module HERE, outside every weave lock.
		struct comp_multi_lift *l = comp_multi_lift_for_system(&mc->msc->base);
		if (l == NULL) {
			return false;
		}
		for (uint32_t i = 0; i < count; i++) {
			const uint32_t mode = comp_multi_lift_stream_mode(l, owner, rects[i].stream_id);
			if (mode != XRT_DP_LIFT_MODE_SBS && mode != XRT_DP_LIFT_MODE_NVIEW) {
				return false;
			}
		}
	}
	comp_multi_weave_android_set_lift_rects(mc, owner, count, rects);
	return true;
}

#endif // XRT_OS_ANDROID
