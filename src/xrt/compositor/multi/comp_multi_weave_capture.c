// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  File-trigger capture for the service-side weave engines (#2207, #2470, #2501).
 * @ingroup comp_multi
 *
 * See comp_multi_weave_capture.h for the trigger names and the three files.
 */

#include "multi/comp_multi_weave_capture.h"

#include "util/u_logging.h"
#include "util/u_image_capture.h"
#include "vk/vk_helpers.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// STB_IMAGE_WRITE_STATIC scopes stbi_write_* to this TU (every PNG-writing
// compositor carries its own static copy, so none clash at link time).
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"


static const char *const stage_infix[COMP_MULTI_WEAVE_CAPTURE_STAGE_COUNT] = {
    ".input",   // the caller's buffer
    "",         // the DP input keeps the vk_native name: displayxr_atlas[.projection].png
    ".woven",   // post-weave
    ".overlay", // the v4 2D overlay layer (#2501)
};

static const char *const stage_name[COMP_MULTI_WEAVE_CAPTURE_STAGE_COUNT] = {"input", "atlas", "woven", "overlay"};

static bool
format_swap_rb(VkFormat format, bool *out_swap)
{
	switch (format) {
	case VK_FORMAT_B8G8R8A8_UNORM:
	case VK_FORMAT_B8G8R8A8_SRGB: *out_swap = true; return true;
	case VK_FORMAT_R8G8B8A8_UNORM:
	case VK_FORMAT_R8G8B8A8_SRGB: *out_swap = false; return true;
	default: return false;
	}
}

static void
target_free_vk(struct comp_multi_weave_capture_target *t, struct vk_bundle *vk)
{
	if (vk == NULL) {
		return;
	}
	if (t->buffer != VK_NULL_HANDLE) {
		vk->vkDestroyBuffer(vk->device, t->buffer, NULL);
		t->buffer = VK_NULL_HANDLE;
	}
	if (t->memory != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, t->memory, NULL);
		t->memory = VK_NULL_HANDLE;
	}
}

void
comp_multi_weave_capture_release(struct comp_multi_weave_capture *cap, struct vk_bundle *vk)
{
	if (cap == NULL) {
		return;
	}
	for (uint32_t i = 0; i < COMP_MULTI_WEAVE_CAPTURE_STAGE_COUNT; i++) {
		struct comp_multi_weave_capture_target *t = &cap->target[i];
		target_free_vk(t, vk);
		free(t->rgba);
		memset(t, 0, sizeof(*t));
	}
}

bool
comp_multi_weave_capture_begin(struct comp_multi_weave_capture *cap, struct vk_bundle *vk)
{
	// A capture whose submit failed before finishing: that submit's fence has
	// since been waited (the caller's contract), so its buffers are idle.
	if (cap->intent.pending) {
		U_LOG_W("weave capture (#2207): the submit that consumed %s failed before completing — nothing written",
		        cap->intent.path[0] != '\0' ? cap->intent.path : "the trigger");
		comp_multi_weave_capture_release(cap, vk);
	}

	// The client's own tag first (it names THIS client), then the
	// service-process per-exe and the global pair (first poller wins among
	// several weave clients).
	if (!u_capture_intent_poll_tag(&cap->intent, cap->tag)) {
		u_capture_intent_poll(&cap->intent, NULL);
	}
	return cap->intent.pending;
}

bool
comp_multi_weave_capture_wants(const struct comp_multi_weave_capture *cap, enum comp_multi_weave_capture_stage stage)
{
	if (cap == NULL || !cap->intent.pending || (uint32_t)stage >= COMP_MULTI_WEAVE_CAPTURE_STAGE_COUNT) {
		return false;
	}
	if (cap->intent.mode == MCP_CAPTURE_MODE_PROJECTION_ONLY) {
		return stage == COMP_MULTI_WEAVE_CAPTURE_ATLAS;
	}
	return true;
}

/*
 * The readback lands in memory the CPU then reads every byte of. Plain
 * HOST_VISIBLE | HOST_COHERENT is write-combined (uncached) on most GPUs,
 * where CPU reads run at a few MB/s: resolving three 1280x720 stages took
 * 1.2 s on an Intel iGPU, and seconds at 2560x1600 (#2536). Prefer a
 * HOST_CACHED type and fall back to the uncached one where a device has no
 * cached + coherent type.
 */
static bool
staging_buffer_init(struct vk_bundle *vk, VkDeviceSize bytes, VkBuffer *out_buffer, VkDeviceMemory *out_memory)
{
	VkBufferCreateInfo bci = {
	    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
	    .size = bytes,
	    .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	};
	if (vk->vkCreateBuffer(vk->device, &bci, NULL, out_buffer) != VK_SUCCESS) {
		return false;
	}
	VkMemoryRequirements req;
	vk->vkGetBufferMemoryRequirements(vk->device, *out_buffer, &req);

	const VkMemoryPropertyFlags uncached =
	    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	uint32_t type = UINT32_MAX;
	if (!vk_get_memory_type(vk, req.memoryTypeBits, uncached | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, &type) &&
	    !vk_get_memory_type(vk, req.memoryTypeBits, uncached, &type)) {
		vk->vkDestroyBuffer(vk->device, *out_buffer, NULL);
		return false;
	}
	VkMemoryAllocateInfo mai = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .allocationSize = req.size,
	    .memoryTypeIndex = type,
	};
	if (vk->vkAllocateMemory(vk->device, &mai, NULL, out_memory) != VK_SUCCESS) {
		vk->vkDestroyBuffer(vk->device, *out_buffer, NULL);
		return false;
	}
	if (vk->vkBindBufferMemory(vk->device, *out_buffer, *out_memory, 0) != VK_SUCCESS) {
		vk->vkFreeMemory(vk->device, *out_memory, NULL);
		vk->vkDestroyBuffer(vk->device, *out_buffer, NULL);
		return false;
	}
	return true;
}

bool
comp_multi_weave_capture_record(struct comp_multi_weave_capture *cap,
                                struct vk_bundle *vk,
                                VkCommandBuffer cmd,
                                enum comp_multi_weave_capture_stage stage,
                                VkImage image,
                                uint32_t w,
                                uint32_t h,
                                VkFormat format,
                                VkImageLayout layout,
                                VkPipelineStageFlags last_stage,
                                VkAccessFlags last_access)
{
	if (!comp_multi_weave_capture_wants(cap, stage) || vk == NULL || cmd == VK_NULL_HANDLE ||
	    image == VK_NULL_HANDLE || w == 0 || h == 0) {
		return false;
	}
	struct comp_multi_weave_capture_target *t = &cap->target[stage];
	if (t->recorded) {
		return false; // one readback per stage per capture
	}
	bool swap = false;
	if (!format_swap_rb(format, &swap)) {
		U_LOG_W("weave capture (#2207): %s stage format %d is not RGBA8/BGRA8 — skipped", stage_name[stage],
		        (int)format);
		return false;
	}

	const VkDeviceSize bytes = (VkDeviceSize)w * (VkDeviceSize)h * 4;
	if (!staging_buffer_init(vk, bytes, &t->buffer, &t->memory)) {
		U_LOG_W("weave capture (#2207): %s staging buffer (%ux%u) allocation failed", stage_name[stage], w, h);
		t->buffer = VK_NULL_HANDLE;
		t->memory = VK_NULL_HANDLE;
		return false;
	}

	const VkImageLayout copy_layout =
	    layout == VK_IMAGE_LAYOUT_GENERAL ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	VkImageMemoryBarrier b = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = last_access,
	    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
	    .oldLayout = layout,
	    .newLayout = copy_layout,
	    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .image = image,
	    .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1},
	};
	vk->vkCmdPipelineBarrier(cmd, last_stage, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);

	VkBufferImageCopy region = {
	    .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
	    .imageExtent = {w, h, 1},
	};
	vk->vkCmdCopyImageToBuffer(cmd, image, copy_layout, t->buffer, 1, &region);

	// Back to where the frame expects it: the image's next use re-reads (or
	// re-writes) it at last_stage / last_access.
	b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	b.dstAccessMask = last_access;
	b.oldLayout = copy_layout;
	b.newLayout = layout;
	vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, last_stage, 0, 0, NULL, 0, NULL, 1, &b);

	// The copy lands in host memory: make it visible to the CPU map once the
	// submit's fence has signalled.
	VkMemoryBarrier hb = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	    .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
	};
	vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0,
	                         NULL);

	t->w = w;
	t->h = h;
	t->format = format;
	t->recorded = true;
	return true;
}

bool
comp_multi_weave_capture_store(struct comp_multi_weave_capture *cap,
                               enum comp_multi_weave_capture_stage stage,
                               const uint8_t *src,
                               uint32_t w,
                               uint32_t h,
                               size_t stride,
                               bool swap_rb)
{
	if (!comp_multi_weave_capture_wants(cap, stage) || src == NULL || w == 0 || h == 0 || stride < (size_t)w * 4) {
		return false;
	}
	struct comp_multi_weave_capture_target *t = &cap->target[stage];
	if (t->recorded) {
		return false; // one readback per stage per capture
	}
	t->rgba = malloc((size_t)w * h * 4);
	if (t->rgba == NULL) {
		U_LOG_W("weave capture (#2207): %s stage (%ux%u) allocation failed", stage_name[stage], w, h);
		return false;
	}
	u_image_capture_to_rgba8(t->rgba, src, w, h, stride, swap_rb, u_image_capture_raw_alpha());
	t->w = w;
	t->h = h;
	t->format = swap_rb ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
	t->recorded = true;
	return true;
}

void
comp_multi_weave_capture_resolve(struct comp_multi_weave_capture *cap, struct vk_bundle *vk, bool gpu_ok)
{
	if (cap == NULL || vk == NULL) {
		return;
	}
	const bool keep_alpha = u_image_capture_raw_alpha();
	for (uint32_t i = 0; i < COMP_MULTI_WEAVE_CAPTURE_STAGE_COUNT; i++) {
		struct comp_multi_weave_capture_target *t = &cap->target[i];
		if (t->buffer == VK_NULL_HANDLE) {
			continue; // never recorded, or already CPU pixels
		}
		bool swap = false;
		(void)format_swap_rb(t->format, &swap);
		void *mapped = NULL;
		if (gpu_ok && t->rgba == NULL) {
			t->rgba = malloc((size_t)t->w * t->h * 4);
		}
		if (gpu_ok && t->rgba != NULL &&
		    vk->vkMapMemory(vk->device, t->memory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS) {
			u_image_capture_to_rgba8(t->rgba, (const uint8_t *)mapped, t->w, t->h, (size_t)t->w * 4, swap,
			                         keep_alpha);
			vk->vkUnmapMemory(vk->device, t->memory);
		} else {
			// Nothing readable: the stage is dropped (finish reports what it wrote).
			if (gpu_ok) {
				U_LOG_W("weave capture (#2207): mapping the %s stage readback FAILED", stage_name[i]);
			}
			free(t->rgba);
			t->rgba = NULL;
			t->recorded = false;
		}
		target_free_vk(t, vk);
	}
}

/*
 * The PNG encode runs on a detached worker, not on the weave path: stbi
 * encodes a 2560x1600 stage in about two seconds, so three stages written
 * inline held one xrWeaveSubmitDXR for 5-7 s and the caller's frames went
 * late or flat for that long (#2536). The worker owns the pixels and the
 * paths; nothing in it touches the capture or Vulkan. Each file is written
 * to "<path>.tmp" and renamed, so a reader polling for it never sees a
 * partial PNG, and the one WARN is emitted after the last rename (a script
 * can wait on the WARN, or on the files). Jobs are serialised so two
 * back-to-back captures never write the same path at once.
 */
struct capture_write_job
{
	char path[COMP_MULTI_WEAVE_CAPTURE_STAGE_COUNT][MCP_CAPTURE_PATH_MAX + 16]; //!< "" = stage not filled
	uint8_t *rgba[COMP_MULTI_WEAVE_CAPTURE_STAGE_COUNT];
	uint32_t w[COMP_MULTI_WEAVE_CAPTURE_STAGE_COUNT], h[COMP_MULTI_WEAVE_CAPTURE_STAGE_COUNT];
	char what[96];
	bool projection;
	bool keep_alpha;
};

static pthread_mutex_t g_capture_write_lock = PTHREAD_MUTEX_INITIALIZER;

static bool
write_png_atomic(const char *path, uint32_t w, uint32_t h, const uint8_t *rgba)
{
	char tmp[MCP_CAPTURE_PATH_MAX + 32];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	if (stbi_write_png(tmp, (int)w, (int)h, 4, rgba, (int)w * 4) == 0) {
		remove(tmp);
		return false;
	}
	if (rename(tmp, path) != 0) {
		remove(tmp);
		return false;
	}
	return true;
}

static void
capture_write_job_run(struct capture_write_job *job)
{
	pthread_mutex_lock(&g_capture_write_lock);
	uint32_t dims[COMP_MULTI_WEAVE_CAPTURE_STAGE_COUNT][2] = {{0}};
	bool ok_any = false;
	for (uint32_t i = 0; i < COMP_MULTI_WEAVE_CAPTURE_STAGE_COUNT; i++) {
		if (job->path[i][0] == '\0') {
			continue;
		}
		if (job->rgba[i] != NULL && write_png_atomic(job->path[i], job->w[i], job->h[i], job->rgba[i])) {
			dims[i][0] = job->w[i];
			dims[i][1] = job->h[i];
			ok_any = true;
		} else {
			U_LOG_W("weave capture (#2207): writing the %s stage to %s FAILED", stage_name[i],
			        job->path[i]);
			job->path[i][0] = '\0';
		}
		free(job->rgba[i]);
		job->rgba[i] = NULL;
	}
	pthread_mutex_unlock(&g_capture_write_lock);

	if (ok_any) {
		U_LOG_W(
		    "weave capture (#2207) %s (mode=%s, raw_alpha=%d): input %ux%u %s | atlas %ux%u %s | woven %ux%u "
		    "%s | overlay %ux%u %s",
		    job->what, job->projection ? "projection" : "post", (int)job->keep_alpha, //
		    dims[0][0], dims[0][1], job->path[0][0] != '\0' ? job->path[0] : "-",     //
		    dims[1][0], dims[1][1], job->path[1][0] != '\0' ? job->path[1] : "-",     //
		    dims[2][0], dims[2][1], job->path[2][0] != '\0' ? job->path[2] : "-",     //
		    dims[3][0], dims[3][1], job->path[3][0] != '\0' ? job->path[3] : "-");
	}
	free(job);
}

static void *
capture_write_thread(void *arg)
{
	capture_write_job_run((struct capture_write_job *)arg);
	return NULL;
}

void
comp_multi_weave_capture_finish(struct comp_multi_weave_capture *cap, struct vk_bundle *vk, const char *what)
{
	if (cap == NULL || !cap->intent.pending) {
		return;
	}
	comp_multi_weave_capture_resolve(cap, vk, true);

	struct capture_write_job *job = calloc(1, sizeof(*job));
	bool any = false;
	for (uint32_t i = 0; i < COMP_MULTI_WEAVE_CAPTURE_STAGE_COUNT; i++) {
		struct comp_multi_weave_capture_target *t = &cap->target[i];
		if (job == NULL || !t->recorded) {
			continue;
		}
		u_capture_intent_sibling_path(cap->intent.path, stage_infix[i], job->path[i], sizeof(job->path[i]));
		job->rgba[i] = t->rgba; // the job owns the pixels from here
		job->w[i] = t->w;
		job->h[i] = t->h;
		t->rgba = NULL;
		any = true;
	}
	comp_multi_weave_capture_release(cap, vk);

	if (job == NULL) {
		U_LOG_W("weave capture (#2207): out of memory for %s — nothing written", cap->intent.path);
	} else if (!any) {
		U_LOG_W("weave capture (#2207): trigger consumed for %s but this submit recorded no stage",
		        cap->intent.path);
		free(job);
	} else {
		snprintf(job->what, sizeof(job->what), "%s", what != NULL ? what : "");
		job->projection = cap->intent.mode == MCP_CAPTURE_MODE_PROJECTION_ONLY;
		job->keep_alpha = u_image_capture_raw_alpha();
		pthread_t thread;
		if (pthread_create(&thread, NULL, capture_write_thread, job) == 0) {
			pthread_detach(thread);
		} else {
			capture_write_job_run(job); // no thread: write inline, as before
		}
	}
	// Consumed now (the pixels are already copied out); the trigger path never
	// carries an MCP waiter on this route, so there is no status to hold back.
	u_capture_intent_complete(&cap->intent, NULL, any);
}
