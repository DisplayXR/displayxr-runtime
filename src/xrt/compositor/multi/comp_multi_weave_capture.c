// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  File-trigger capture for the service-side Vulkan weave engine (#2207).
 * @ingroup comp_multi
 *
 * See comp_multi_weave_capture.h for the trigger names and the three files.
 */

#include "multi/comp_multi_weave_capture.h"

#include "util/u_logging.h"
#include "util/u_image_capture.h"
#include "vk/vk_helpers.h"

#include <stdlib.h>
#include <string.h>

// STB_IMAGE_WRITE_STATIC scopes stbi_write_* to this TU (every PNG-writing
// compositor carries its own static copy, so none clash at link time).
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"


static const char *const stage_infix[COMP_MULTI_WEAVE_CAPTURE_STAGE_COUNT] = {
    ".input", // the caller's buffer
    "",       // the DP input keeps the vk_native name: displayxr_atlas[.projection].png
    ".woven", // post-weave
};

static const char *const stage_name[COMP_MULTI_WEAVE_CAPTURE_STAGE_COUNT] = {"input", "atlas", "woven"};

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

void
comp_multi_weave_capture_release(struct comp_multi_weave_capture *cap, struct vk_bundle *vk)
{
	if (cap == NULL || vk == NULL) {
		return;
	}
	for (uint32_t i = 0; i < COMP_MULTI_WEAVE_CAPTURE_STAGE_COUNT; i++) {
		struct comp_multi_weave_capture_target *t = &cap->target[i];
		if (t->buffer != VK_NULL_HANDLE) {
			vk->vkDestroyBuffer(vk->device, t->buffer, NULL);
		}
		if (t->memory != VK_NULL_HANDLE) {
			vk->vkFreeMemory(vk->device, t->memory, NULL);
		}
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
	if (!vk_buffer_init(vk, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &t->buffer,
	                    &t->memory)) {
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

void
comp_multi_weave_capture_finish(struct comp_multi_weave_capture *cap, struct vk_bundle *vk, const char *what)
{
	if (cap == NULL || !cap->intent.pending) {
		return;
	}
	const bool keep_alpha = u_image_capture_raw_alpha();
	char written[COMP_MULTI_WEAVE_CAPTURE_STAGE_COUNT][MCP_CAPTURE_PATH_MAX + 16];
	uint32_t dims[COMP_MULTI_WEAVE_CAPTURE_STAGE_COUNT][2] = {{0}};
	bool ok_any = false;
	bool failed_any = false;

	for (uint32_t i = 0; i < COMP_MULTI_WEAVE_CAPTURE_STAGE_COUNT; i++) {
		written[i][0] = '\0';
		struct comp_multi_weave_capture_target *t = &cap->target[i];
		if (!t->recorded) {
			continue;
		}
		char path[MCP_CAPTURE_PATH_MAX + 16];
		u_capture_intent_sibling_path(cap->intent.path, stage_infix[i], path, sizeof(path));

		bool ok = false;
		bool swap = false;
		(void)format_swap_rb(t->format, &swap);
		const size_t bytes = (size_t)t->w * t->h * 4;
		void *mapped = NULL;
		uint8_t *rgba = malloc(bytes);
		if (rgba != NULL &&
		    vk->vkMapMemory(vk->device, t->memory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS) {
			u_image_capture_to_rgba8(rgba, (const uint8_t *)mapped, t->w, t->h, (size_t)t->w * 4, swap,
			                         keep_alpha);
			vk->vkUnmapMemory(vk->device, t->memory);
			ok = stbi_write_png(path, (int)t->w, (int)t->h, 4, rgba, (int)t->w * 4) != 0;
		}
		free(rgba);
		if (ok) {
			snprintf(written[i], sizeof(written[i]), "%s", path);
			dims[i][0] = t->w;
			dims[i][1] = t->h;
			ok_any = true;
		} else {
			failed_any = true;
			U_LOG_W("weave capture (#2207): writing the %s stage to %s FAILED", stage_name[i], path);
		}
	}
	comp_multi_weave_capture_release(cap, vk);

	if (ok_any) {
		U_LOG_W(
		    "weave capture (#2207) %s (mode=%s, raw_alpha=%d): input %ux%u %s | atlas %ux%u %s | woven %ux%u "
		    "%s",
		    what != NULL ? what : "",
		    cap->intent.mode == MCP_CAPTURE_MODE_PROJECTION_ONLY ? "projection" : "post",
		    (int)keep_alpha,                                                  //
		    dims[0][0], dims[0][1], written[0][0] != '\0' ? written[0] : "-", //
		    dims[1][0], dims[1][1], written[1][0] != '\0' ? written[1] : "-", //
		    dims[2][0], dims[2][1], written[2][0] != '\0' ? written[2] : "-");
	} else if (!failed_any) {
		U_LOG_W("weave capture (#2207): trigger consumed for %s but this submit recorded no stage",
		        cap->intent.path);
	}
	u_capture_intent_complete(&cap->intent, NULL, ok_any);
}
