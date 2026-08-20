// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Runtime-supplied 2D backdrop for compose-under transparency (#1073 T0).
 * @author David Fattal
 * @ingroup comp_multi
 *
 * See comp_multi_bg2d.h for the why. This file is the whole producer: parse a
 * config string, rasterise a tiny RGBA8 image on the CPU, upload it once.
 */

#include "comp_multi_bg2d.h"
#include "comp_multi_private.h"

#include "util/u_logging.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef XRT_OS_ANDROID
#include <sys/system_properties.h>
#endif

/*!
 * Backdrop resolution. The DP samples it with tile-local UV (0..1 across each
 * atlas tile), so the image only has to carry the gradient — 4x256 with a
 * linear sampler is indistinguishable from a full-canvas one and costs 4 KB.
 */
#define BG2D_W 4u
#define BG2D_H 256u


/*
 *
 * Config.
 *
 */

struct bg2d_config
{
	bool enabled;
	uint8_t top[3];    //!< RGB at v = 0.
	uint8_t bottom[3]; //!< RGB at v = 1 (== top for a solid fill).
};

//! Parse "RRGGBB" (with or without a leading '#') into @p out. Returns false on
//! any malformed input so a typo falls back to the default rather than to black.
static bool
parse_hex_rgb(const char *s, uint8_t out[3])
{
	if (s == NULL) {
		return false;
	}
	if (s[0] == '#') {
		s++;
	}
	if (strlen(s) < 6) {
		return false;
	}
	for (int i = 0; i < 6; i++) {
		char c = s[i];
		bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
		if (!ok) {
			return false;
		}
	}
	unsigned int r = 0, g = 0, b = 0;
	if (sscanf(s, "%2x%2x%2x", &r, &g, &b) != 3) {
		return false;
	}
	out[0] = (uint8_t)r;
	out[1] = (uint8_t)g;
	out[2] = (uint8_t)b;
	return true;
}

//! Read the raw config string: `debug.dxr.bg2d` on Android, `DXR_BG2D` elsewhere
//! (so the path is reachable for a desktop dry-run without inventing a second
//! vocabulary). Returns NULL when unset.
static const char *
bg2d_config_string(char *buf, size_t buf_size)
{
#ifdef XRT_OS_ANDROID
	char prop[PROP_VALUE_MAX] = {0};
	if (__system_property_get("debug.dxr.bg2d", prop) > 0 && prop[0] != '\0') {
		snprintf(buf, buf_size, "%s", prop);
		return buf;
	}
#else
	(void)buf;
	(void)buf_size;
#endif
	const char *env = getenv("DXR_BG2D");
	if (env != NULL && env[0] != '\0') {
		return env;
	}
	return NULL;
}

static const struct bg2d_config *
bg2d_config_get(void)
{
	static struct bg2d_config cfg;
	static bool parsed = false;
	if (parsed) {
		return &cfg;
	}
	parsed = true;
	memset(&cfg, 0, sizeof(cfg));

	char buf[128] = {0};
	const char *s = bg2d_config_string(buf, sizeof(buf));
	if (s == NULL || strcmp(s, "0") == 0 || strcmp(s, "off") == 0 || strcmp(s, "false") == 0) {
		return &cfg; // disabled — the shipping default
	}

	// Default gradient: a dark slate that reads as "behind the app" without
	// pretending to be a real background. Deliberately not black — a black
	// backdrop is indistinguishable from the pre-#1073 de-occlusion fringe, so
	// an A/B would prove nothing.
	static const uint8_t kDefTop[3] = {0x1a, 0x1f, 0x2b};
	static const uint8_t kDefBottom[3] = {0x05, 0x06, 0x0a};

	cfg.enabled = true;
	memcpy(cfg.top, kDefTop, 3);
	memcpy(cfg.bottom, kDefBottom, 3);

	if (strcmp(s, "1") == 0 || strcmp(s, "on") == 0 || strcmp(s, "grad") == 0) {
		// keep the default gradient
	} else if (strncmp(s, "grad:", 5) == 0) {
		const char *rest = s + 5;
		const char *comma = strchr(rest, ',');
		uint8_t a[3], b[3];
		char first[16] = {0};
		if (comma != NULL && (size_t)(comma - rest) < sizeof(first)) {
			memcpy(first, rest, (size_t)(comma - rest));
			if (parse_hex_rgb(first, a) && parse_hex_rgb(comma + 1, b)) {
				memcpy(cfg.top, a, 3);
				memcpy(cfg.bottom, b, 3);
			}
		}
	} else {
		// "solid:RRGGBB" or a bare "RRGGBB" / "#RRGGBB".
		const char *hex = (strncmp(s, "solid:", 6) == 0) ? s + 6 : s;
		uint8_t c[3];
		if (parse_hex_rgb(hex, c)) {
			memcpy(cfg.top, c, 3);
			memcpy(cfg.bottom, c, 3);
		}
	}

	U_LOG_W("bg2d(#1073 T0): backdrop enabled '%s' → top #%02x%02x%02x bottom #%02x%02x%02x", s, cfg.top[0],
	        cfg.top[1], cfg.top[2], cfg.bottom[0], cfg.bottom[1], cfg.bottom[2]);
	return &cfg;
}

bool
comp_multi_bg2d_enabled(void)
{
	return bg2d_config_get()->enabled;
}


/*
 *
 * GPU resources.
 *
 */

void
comp_multi_bg2d_teardown(struct multi_compositor *mc, struct vk_bundle *vk)
{
	if (mc == NULL) {
		return;
	}
	if (vk != NULL) {
		if (mc->session_render.bg2d_view != VK_NULL_HANDLE) {
			vk->vkDestroyImageView(vk->device, mc->session_render.bg2d_view, NULL);
		}
		if (mc->session_render.bg2d_image != VK_NULL_HANDLE) {
			vk->vkDestroyImage(vk->device, mc->session_render.bg2d_image, NULL);
		}
		if (mc->session_render.bg2d_memory != VK_NULL_HANDLE) {
			vk->vkFreeMemory(vk->device, mc->session_render.bg2d_memory, NULL);
		}
		if (mc->session_render.bg2d_staging_buffer != VK_NULL_HANDLE) {
			vk->vkDestroyBuffer(vk->device, mc->session_render.bg2d_staging_buffer, NULL);
		}
		if (mc->session_render.bg2d_staging_memory != VK_NULL_HANDLE) {
			vk->vkFreeMemory(vk->device, mc->session_render.bg2d_staging_memory, NULL);
		}
	}
	mc->session_render.bg2d_view = VK_NULL_HANDLE;
	mc->session_render.bg2d_image = VK_NULL_HANDLE;
	mc->session_render.bg2d_memory = VK_NULL_HANDLE;
	mc->session_render.bg2d_staging_buffer = VK_NULL_HANDLE;
	mc->session_render.bg2d_staging_memory = VK_NULL_HANDLE;
	mc->session_render.bg2d_w = 0;
	mc->session_render.bg2d_h = 0;
	mc->session_render.bg2d_initialized = false;
	mc->session_render.bg2d_failed = false;
}

//! Rasterise the configured backdrop into @p pixels (RGBA8, BG2D_W x BG2D_H).
static void
bg2d_rasterize(const struct bg2d_config *cfg, uint8_t *pixels)
{
	for (uint32_t y = 0; y < BG2D_H; y++) {
		// Nearest-texel-centre interpolation so a 1-row image would still be
		// well defined; with BG2D_H = 256 this is just y/255.
		float t = (BG2D_H > 1) ? ((float)y / (float)(BG2D_H - 1)) : 0.0f;
		uint8_t r = (uint8_t)(cfg->top[0] + (cfg->bottom[0] - cfg->top[0]) * t);
		uint8_t g = (uint8_t)(cfg->top[1] + (cfg->bottom[1] - cfg->top[1]) * t);
		uint8_t b = (uint8_t)(cfg->top[2] + (cfg->bottom[2] - cfg->top[2]) * t);
		for (uint32_t x = 0; x < BG2D_W; x++) {
			uint8_t *p = pixels + (y * BG2D_W + x) * 4;
			p[0] = r;
			p[1] = g;
			p[2] = b;
			p[3] = 0xff; // opaque ⟹ premultiplied == straight
		}
	}
}

VkImageView
comp_multi_bg2d_ensure(struct multi_compositor *mc, struct vk_bundle *vk, uint32_t *out_w, uint32_t *out_h)
{
	if (mc == NULL || vk == NULL) {
		return VK_NULL_HANDLE;
	}
	const struct bg2d_config *cfg = bg2d_config_get();
	if (!cfg->enabled || mc->session_render.bg2d_failed) {
		return VK_NULL_HANDLE;
	}
	if (mc->session_render.bg2d_initialized) {
		if (out_w != NULL) {
			*out_w = mc->session_render.bg2d_w;
		}
		if (out_h != NULL) {
			*out_h = mc->session_render.bg2d_h;
		}
		return mc->session_render.bg2d_view;
	}

	const VkDeviceSize pixel_size = (VkDeviceSize)BG2D_W * BG2D_H * 4;
	VkResult ret;

	// --- image (device-local, TRANSFER_DST | SAMPLED) ---
	VkImageCreateInfo image_info = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .imageType = VK_IMAGE_TYPE_2D,
	    .format = VK_FORMAT_R8G8B8A8_UNORM,
	    .extent = {BG2D_W, BG2D_H, 1},
	    .mipLevels = 1,
	    .arrayLayers = 1,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .tiling = VK_IMAGE_TILING_OPTIMAL,
	    .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	ret = vk->vkCreateImage(vk->device, &image_info, NULL, &mc->session_render.bg2d_image);
	if (ret != VK_SUCCESS) {
		U_LOG_E("bg2d: image create failed: %s", vk_result_string(ret));
		goto fail;
	}

	VkMemoryRequirements img_reqs;
	vk->vkGetImageMemoryRequirements(vk->device, mc->session_render.bg2d_image, &img_reqs);
	VkMemoryAllocateInfo img_alloc = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .allocationSize = img_reqs.size,
	};
	if (!vk_get_memory_type(vk, img_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
	                        &img_alloc.memoryTypeIndex)) {
		U_LOG_E("bg2d: no device-local memory type");
		goto fail;
	}
	ret = vk->vkAllocateMemory(vk->device, &img_alloc, NULL, &mc->session_render.bg2d_memory);
	if (ret != VK_SUCCESS) {
		U_LOG_E("bg2d: image memory alloc failed: %s", vk_result_string(ret));
		goto fail;
	}
	vk->vkBindImageMemory(vk->device, mc->session_render.bg2d_image, mc->session_render.bg2d_memory, 0);

	VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	ret = vk_create_view(vk, mc->session_render.bg2d_image, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, range,
	                     &mc->session_render.bg2d_view);
	if (ret != VK_SUCCESS) {
		U_LOG_E("bg2d: view create failed: %s", vk_result_string(ret));
		goto fail;
	}

	// --- staging buffer, filled with the rasterised gradient ---
	VkBufferCreateInfo buf_info = {
	    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
	    .size = pixel_size,
	    .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	};
	ret = vk->vkCreateBuffer(vk->device, &buf_info, NULL, &mc->session_render.bg2d_staging_buffer);
	if (ret != VK_SUCCESS) {
		U_LOG_E("bg2d: staging buffer create failed: %s", vk_result_string(ret));
		goto fail;
	}
	VkMemoryRequirements buf_reqs;
	vk->vkGetBufferMemoryRequirements(vk->device, mc->session_render.bg2d_staging_buffer, &buf_reqs);
	VkMemoryAllocateInfo buf_alloc = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .allocationSize = buf_reqs.size,
	};
	if (!vk_get_memory_type(vk, buf_reqs.memoryTypeBits,
	                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	                        &buf_alloc.memoryTypeIndex)) {
		U_LOG_E("bg2d: no host-visible memory type");
		goto fail;
	}
	ret = vk->vkAllocateMemory(vk->device, &buf_alloc, NULL, &mc->session_render.bg2d_staging_memory);
	if (ret != VK_SUCCESS) {
		U_LOG_E("bg2d: staging memory alloc failed: %s", vk_result_string(ret));
		goto fail;
	}
	vk->vkBindBufferMemory(vk->device, mc->session_render.bg2d_staging_buffer,
	                       mc->session_render.bg2d_staging_memory, 0);

	void *mapped = NULL;
	ret = vk->vkMapMemory(vk->device, mc->session_render.bg2d_staging_memory, 0, pixel_size, 0, &mapped);
	if (ret != VK_SUCCESS || mapped == NULL) {
		U_LOG_E("bg2d: staging map failed: %s", vk_result_string(ret));
		goto fail;
	}
	bg2d_rasterize(cfg, (uint8_t *)mapped);
	vk->vkUnmapMemory(vk->device, mc->session_render.bg2d_staging_memory);

	// --- one-shot upload ---
	//
	// Deliberately NOT recorded into the frame command buffer: the Android
	// vendor DP is self-submitting, so `cmd` is still unsubmitted when the DP's
	// compose pass samples this image. A synchronous upload at first use is both
	// simpler and correct, and it happens exactly once per session.
	VkCommandBufferAllocateInfo cbai = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	    .commandPool = mc->session_render.cmd_pool,
	    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
	    .commandBufferCount = 1,
	};
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	ret = vk->vkAllocateCommandBuffers(vk->device, &cbai, &cmd);
	if (ret != VK_SUCCESS) {
		U_LOG_E("bg2d: upload cmd alloc failed: %s", vk_result_string(ret));
		goto fail;
	}
	VkCommandBufferBeginInfo bi = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	vk->vkBeginCommandBuffer(cmd, &bi);

	VkImageMemoryBarrier to_dst = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = 0,
	    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .image = mc->session_render.bg2d_image,
	    .subresourceRange = range,
	};
	vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
	                         NULL, 1, &to_dst);

	VkBufferImageCopy region = {
	    .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	    .imageExtent = {BG2D_W, BG2D_H, 1},
	};
	vk->vkCmdCopyBufferToImage(cmd, mc->session_render.bg2d_staging_buffer, mc->session_render.bg2d_image,
	                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

	// The slot's contract: the view is left in SHADER_READ_ONLY_OPTIMAL and
	// outlives the process_atlas call. Nothing ever writes it again, so this is
	// the image's final layout for the session.
	VkImageMemoryBarrier to_read = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .image = mc->session_render.bg2d_image,
	    .subresourceRange = range,
	};
	vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL,
	                         0, NULL, 1, &to_read);

	vk->vkEndCommandBuffer(cmd);

	VkSubmitInfo si = {
	    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	    .commandBufferCount = 1,
	    .pCommandBuffers = &cmd,
	};
	ret = vk->vkQueueSubmit(vk->main_queue->queue, 1, &si, VK_NULL_HANDLE);
	if (ret == VK_SUCCESS) {
		vk->vkQueueWaitIdle(vk->main_queue->queue);
	} else {
		U_LOG_E("bg2d: upload submit failed: %s", vk_result_string(ret));
	}
	vk->vkFreeCommandBuffers(vk->device, mc->session_render.cmd_pool, 1, &cmd);
	if (ret != VK_SUCCESS) {
		goto fail;
	}

	mc->session_render.bg2d_w = BG2D_W;
	mc->session_render.bg2d_h = BG2D_H;
	mc->session_render.bg2d_initialized = true;
	U_LOG_W("bg2d(#1073 T0): backdrop %ux%u uploaded — handing it to the DP via set_background_2d", BG2D_W,
	        BG2D_H);

	if (out_w != NULL) {
		*out_w = BG2D_W;
	}
	if (out_h != NULL) {
		*out_h = BG2D_H;
	}
	return mc->session_render.bg2d_view;

fail:
	comp_multi_bg2d_teardown(mc, vk);
	// Latch the failure so a broken environment costs one attempt, not one per
	// frame. Teardown clears the flag, so set it after.
	mc->session_render.bg2d_failed = true;
	return VK_NULL_HANDLE;
}
