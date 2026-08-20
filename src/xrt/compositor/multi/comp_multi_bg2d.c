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
#include "comp_multi_bg2d_capture.h"
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

	//! T2: take the backdrop from an external capture producer instead of
	//! drawing it. When set, `top`/`bottom` are unused and the image is
	//! rebuilt whenever a new frame lands. See comp_multi_bg2d_capture.h.
	bool capture;
	char capture_sock[64];
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

	if (strcmp(s, "capture") == 0 || strncmp(s, "capture:", 8) == 0) {
		// T2 — real screen-behind-the-window pixels from a privileged
		// producer. The gradient stays parsed as the fallback the DP gets
		// while no producer is connected: nothing, i.e. today's path.
		cfg.capture = true;
		const char *sock = (s[7] == ':') ? s + 8 : "";
		snprintf(cfg.capture_sock, sizeof(cfg.capture_sock), "%s", sock);
		U_LOG_W("bg2d(#1073 T2): external capture producer selected (socket @%s)",
		        cfg.capture_sock[0] != '\0' ? cfg.capture_sock : COMP_MULTI_BG2D_CAPTURE_SOCKET);
		return &cfg;
	} else if (strcmp(s, "1") == 0 || strcmp(s, "on") == 0 || strcmp(s, "grad") == 0) {
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
	// #174 crop scratch — plain host memory, so it goes regardless of `vk`.
	free(mc->session_render.bg2d_crop_scratch);
	mc->session_render.bg2d_crop_scratch = NULL;
	mc->session_render.bg2d_crop_capacity = 0;
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
	mc->session_render.bg2d_uploaded_once = false;
	mc->session_render.bg2d_seq = 0;
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

//! Allocate the backdrop image, its view and a host-visible staging buffer at
//! @p w x @p h. Only ever called when nothing is allocated yet, or after a
//! teardown forced by a size change.
static bool
bg2d_create(struct multi_compositor *mc, struct vk_bundle *vk, uint32_t w, uint32_t h)
{
	const VkDeviceSize pixel_size = (VkDeviceSize)w * h * 4;
	VkResult ret;

	// --- image (device-local, TRANSFER_DST | SAMPLED) ---
	VkImageCreateInfo image_info = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .imageType = VK_IMAGE_TYPE_2D,
	    .format = VK_FORMAT_R8G8B8A8_UNORM,
	    .extent = {w, h, 1},
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
		goto fail_create;
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
		goto fail_create;
	}
	ret = vk->vkAllocateMemory(vk->device, &img_alloc, NULL, &mc->session_render.bg2d_memory);
	if (ret != VK_SUCCESS) {
		U_LOG_E("bg2d: image memory alloc failed: %s", vk_result_string(ret));
		goto fail_create;
	}
	vk->vkBindImageMemory(vk->device, mc->session_render.bg2d_image, mc->session_render.bg2d_memory, 0);

	VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	ret = vk_create_view(vk, mc->session_render.bg2d_image, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, range,
	                     &mc->session_render.bg2d_view);
	if (ret != VK_SUCCESS) {
		U_LOG_E("bg2d: view create failed: %s", vk_result_string(ret));
		goto fail_create;
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
		goto fail_create;
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
		goto fail_create;
	}
	ret = vk->vkAllocateMemory(vk->device, &buf_alloc, NULL, &mc->session_render.bg2d_staging_memory);
	if (ret != VK_SUCCESS) {
		U_LOG_E("bg2d: staging memory alloc failed: %s", vk_result_string(ret));
		goto fail_create;
	}
	vk->vkBindBufferMemory(vk->device, mc->session_render.bg2d_staging_buffer,
	                       mc->session_render.bg2d_staging_memory, 0);

	mc->session_render.bg2d_w = w;
	mc->session_render.bg2d_h = h;
	return true;

fail_create:
	comp_multi_bg2d_teardown(mc, vk);
	mc->session_render.bg2d_failed = true;
	return false;
}


/*!
 * Fill the backdrop image, creating it first if needed.
 *
 * @param src Tightly packed RGBA8 `w * h * 4`, or NULL to rasterise the
 *            configured gradient (T0). The upload path is identical either
 *            way — that is the point of the seam: T0 and T2 differ only in
 *            where the bytes came from.
 */
static bool
bg2d_build(struct multi_compositor *mc, struct vk_bundle *vk, uint32_t w, uint32_t h, const uint8_t *src)
{
	if (!mc->session_render.bg2d_initialized && !bg2d_create(mc, vk, w, h)) {
		return false;
	}

	const VkDeviceSize pixel_size = (VkDeviceSize)w * h * 4;
	VkResult ret;
	VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

	void *mapped = NULL;
	ret = vk->vkMapMemory(vk->device, mc->session_render.bg2d_staging_memory, 0, pixel_size, 0, &mapped);
	if (ret != VK_SUCCESS || mapped == NULL) {
		U_LOG_E("bg2d: staging map failed: %s", vk_result_string(ret));
		goto fail;
	}
	if (src != NULL) {
		memcpy(mapped, src, (size_t)pixel_size);
	} else {
		bg2d_rasterize(bg2d_config_get(), (uint8_t *)mapped);
	}
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

	// A refresh (T2 delivers a new frame) finds the image in
	// SHADER_READ_ONLY_OPTIMAL from the previous upload; the very first one
	// finds it UNDEFINED. Getting this wrong is a validation error and, on a
	// tiler, a real corruption.
	VkImageMemoryBarrier to_dst = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = 0,
	    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	    .oldLayout = mc->session_render.bg2d_uploaded_once ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
	                                                       : VK_IMAGE_LAYOUT_UNDEFINED,
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
	    .imageExtent = {w, h, 1},
	};
	vk->vkCmdCopyBufferToImage(cmd, mc->session_render.bg2d_staging_buffer, mc->session_render.bg2d_image,
	                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

	// The slot's contract: the view is left in SHADER_READ_ONLY_OPTIMAL and
	// outlives the process_atlas call. That is also the layout the next T2
	// refresh transitions *from*, which is why bg2d_uploaded_once exists.
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

	mc->session_render.bg2d_initialized = true;
	mc->session_render.bg2d_uploaded_once = true;
	if (!mc->session_render.bg2d_logged) {
		mc->session_render.bg2d_logged = true;
		U_LOG_W("bg2d(#1073): backdrop %ux%u uploaded (%s) — handing it to the DP via set_background_2d", w, h,
		        src != NULL ? "captured" : "runtime-drawn");
	}
	return true;

fail:
	comp_multi_bg2d_teardown(mc, vk);
	// Latch the failure so a broken environment costs one attempt, not one per
	// frame. Teardown clears the flag, so set it after.
	mc->session_render.bg2d_failed = true;
	return false;
}


/*!
 * Where the canvas lands inside a T2 capture frame (#174).
 *
 * A capture producer sends the whole **panel**; slot 16 promises the DP the
 * **canvas** (see comp_multi_bg2d.h). Both are expressed here in panel pixels
 * and the frame is a uniformly downscaled copy of the panel — SurfaceFlinger
 * does the scaling via `DisplayCaptureArgs.setSize` — so the mapping is one
 * ratio per axis, taken from the frame's own dims against the panel's.
 *
 * Only computes the rect; the repack is separate so the caller can settle the
 * upload dimensions (and therefore whether the image must be rebuilt) *before*
 * touching the scratch buffer that teardown owns.
 *
 * @return false for any reason not to crop — no panel dims yet, a degenerate or
 *         fully off-panel canvas, or a canvas that already is the whole frame.
 *         An uncropped backdrop is merely mis-scaled; a wrongly cropped one can
 *         be empty, so "don't crop" is always the safe answer.
 */
static bool
bg2d_canvas_crop_rect(const struct xrt_rect *canvas_on_panel,
                      uint32_t panel_w,
                      uint32_t panel_h,
                      uint32_t frame_w,
                      uint32_t frame_h,
                      uint32_t *out_x,
                      uint32_t *out_y,
                      uint32_t *out_w,
                      uint32_t *out_h)
{
	if (canvas_on_panel == NULL || panel_w == 0 || panel_h == 0 || frame_w == 0 || frame_h == 0) {
		return false;
	}
	if (canvas_on_panel->extent.w <= 0 || canvas_on_panel->extent.h <= 0) {
		return false;
	}

	// Round each EDGE from its panel coordinate rather than adding a rounded
	// extent to a rounded origin, so abutting canvases stay abutting.
	const double sx = (double)frame_w / (double)panel_w;
	const double sy = (double)frame_h / (double)panel_h;
	int64_t x0 = (int64_t)((double)canvas_on_panel->offset.w * sx + 0.5);
	int64_t y0 = (int64_t)((double)canvas_on_panel->offset.h * sy + 0.5);
	int64_t x1 = (int64_t)(((double)canvas_on_panel->offset.w + canvas_on_panel->extent.w) * sx + 0.5);
	int64_t y1 = (int64_t)(((double)canvas_on_panel->offset.h + canvas_on_panel->extent.h) * sy + 0.5);

	// Clamp to the frame; a canvas partly off-panel crops to what exists.
	x0 = x0 < 0 ? 0 : (x0 > (int64_t)frame_w ? (int64_t)frame_w : x0);
	y0 = y0 < 0 ? 0 : (y0 > (int64_t)frame_h ? (int64_t)frame_h : y0);
	x1 = x1 < 0 ? 0 : (x1 > (int64_t)frame_w ? (int64_t)frame_w : x1);
	y1 = y1 < 0 ? 0 : (y1 > (int64_t)frame_h ? (int64_t)frame_h : y1);
	if (x1 <= x0 || y1 <= y0) {
		return false;
	}
	if ((uint32_t)(x1 - x0) == frame_w && (uint32_t)(y1 - y0) == frame_h) {
		return false; // already exactly the canvas — nothing to do
	}

	*out_x = (uint32_t)x0;
	*out_y = (uint32_t)y0;
	*out_w = (uint32_t)(x1 - x0);
	*out_h = (uint32_t)(y1 - y0);
	return true;
}

//! Repack a sub-rect of @p src into @p scratch (grown as needed). NULL on OOM.
static const uint8_t *
bg2d_repack_crop(const uint8_t *src,
                 uint32_t frame_w,
                 uint32_t x,
                 uint32_t y,
                 uint32_t w,
                 uint32_t h,
                 uint8_t **scratch,
                 size_t *scratch_capacity)
{
	const size_t need = (size_t)w * h * 4;
	if (*scratch_capacity < need) {
		uint8_t *grown = realloc(*scratch, need);
		if (grown == NULL) {
			return NULL;
		}
		*scratch = grown;
		*scratch_capacity = need;
	}
	for (uint32_t row = 0; row < h; row++) {
		memcpy(*scratch + (size_t)row * w * 4, src + ((size_t)(y + row) * frame_w + x) * 4, (size_t)w * 4);
	}
	return *scratch;
}


VkImageView
comp_multi_bg2d_ensure(struct multi_compositor *mc,
                       struct vk_bundle *vk,
                       const struct xrt_rect *canvas_on_panel,
                       uint32_t *out_w,
                       uint32_t *out_h)
{
	if (mc == NULL || vk == NULL) {
		return VK_NULL_HANDLE;
	}
	const struct bg2d_config *cfg = bg2d_config_get();
	if (!cfg->enabled || mc->session_render.bg2d_failed) {
		return VK_NULL_HANDLE;
	}

	if (cfg->capture) {
		// T2 — the producer is out of process (it must be: no Android
		// permission tier lets an app capture the screen behind its own
		// layer, see comp_multi_bg2d_capture.h). Start the listener once,
		// then re-upload only when a genuinely new frame has landed; a
		// static screen therefore costs one upload, not one per frame.
		comp_multi_bg2d_capture_start(cfg->capture_sock);

		struct comp_multi_bg2d_capture_frame f = {0};
		if (comp_multi_bg2d_capture_acquire(&f, mc->session_render.bg2d_seq)) {
			// #174 — the producer sent PANEL pixels; slot 16 promises the
			// CANVAS. Crop before the upload so the DP's (0,0)-(1,1) tile
			// mapping lands the backdrop exactly where the atlas depicts.
			uint32_t cx = 0, cy = 0, up_w = f.width, up_h = f.height;
			const bool crop = bg2d_canvas_crop_rect(
			    canvas_on_panel, mc->session_render.window_screen_disp_w,
			    mc->session_render.window_screen_disp_h, f.width, f.height, &cx, &cy, &up_w, &up_h);

			// Settle the upload dims BEFORE any teardown: the producer may
			// re-negotiate its output size (a rotation, a different capture
			// crop) and the canvas rect itself can move. Rebuild rather than
			// scale — it happens ~never and correctness is free. Teardown also
			// frees the crop scratch, so it must run before the repack writes
			// into it.
			if (mc->session_render.bg2d_initialized &&
			    (mc->session_render.bg2d_w != up_w || mc->session_render.bg2d_h != up_h)) {
				comp_multi_bg2d_teardown(mc, vk);
			}

			const uint8_t *px = f.pixels;
			if (crop) {
				// Session-owned scratch, not a per-frame malloc: a producer
				// at 10 Hz repacks once per delivery, not once per frame.
				px = bg2d_repack_crop(f.pixels, f.width, cx, cy, up_w, up_h,
				                      &mc->session_render.bg2d_crop_scratch,
				                      &mc->session_render.bg2d_crop_capacity);
				if (px == NULL) { // OOM — a mis-scaled backdrop beats none
					px = f.pixels;
					up_w = f.width;
					up_h = f.height;
				} else if (!mc->session_render.bg2d_logged_crop) {
					mc->session_render.bg2d_logged_crop = true;
					U_LOG_W("bg2d(#1073 T2): cropped the %ux%u panel capture to the canvas"
					        " %d,%d %dx%d on a %ux%u panel -> %ux%u at (%u,%u) (#174)",
					        f.width, f.height, canvas_on_panel->offset.w,
					        canvas_on_panel->offset.h, canvas_on_panel->extent.w,
					        canvas_on_panel->extent.h, mc->session_render.window_screen_disp_w,
					        mc->session_render.window_screen_disp_h, up_w, up_h, cx, cy);
				}
			}

			uint32_t seq = f.seq;
			bool ok = bg2d_build(mc, vk, up_w, up_h, px);
			comp_multi_bg2d_capture_release();
			if (ok) {
				mc->session_render.bg2d_seq = seq;
			}
		}

		// No producer yet (or ever): no background, which is exactly the
		// pre-#1073 path. Never latch bg2d_failed for this — the producer
		// is allowed to show up an hour into the session.
		if (!mc->session_render.bg2d_initialized) {
			return VK_NULL_HANDLE;
		}
	} else if (!mc->session_render.bg2d_initialized) {
		// T0's backdrop is runtime-drawn, so it is canvas-space by
		// construction and never cropped — canvas_on_panel is unused here.
		if (!bg2d_build(mc, vk, BG2D_W, BG2D_H, NULL)) {
			return VK_NULL_HANDLE;
		}
	}

	if (out_w != NULL) {
		*out_w = mc->session_render.bg2d_w;
	}
	if (out_h != NULL) {
		*out_h = mc->session_render.bg2d_h;
	}
	return mc->session_render.bg2d_view;
}
