// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Leia CNSDK display processor (Android).
 *
 * Wraps leia_cnsdk as an xrt_display_processor. Implements the real
 * Vulkan weave by:
 *   1. Lazily creating one VkImage + VkImageView per view (left/right)
 *      of the atlas tile dimensions.
 *   2. Blitting each tile out of the SBS atlas into its per-view image
 *      via vkCmdBlitImage (CNSDK's `set_view_for_texture_array` takes
 *      one VkImageView per view, which we cannot derive from a single
 *      SBS atlas image — sub-rectangle image views aren't a thing in
 *      Vulkan).
 *   3. Calling leia_cnsdk_weave, which internally records and submits
 *      its own VkCommandBuffer via leia_interlacer_vulkan_do_post_process.
 *
 * The DP advertises `is_self_submitting = true`, which tells the
 * vk_native compositor to:
 *   - flush its pre-DP cmd buffer (window-space composite, atlas crop,
 *     target-image layout transition) before calling process_atlas, and
 *   - skip its own post-process_atlas submit, since CNSDK already did it.
 *
 * Eye positions are hardcoded IPD-only for the POC (CNSDK face tracking
 * wiring TBD).
 *
 * @author David Fattal
 * @ingroup drv_leia
 */

#include "leia_display_processor_cnsdk.h"
#include "leia_cnsdk.h"

#include "xrt/xrt_display_metrics.h"
#include "vk/vk_helpers.h"
#include "util/u_logging.h"

#include <stdlib.h>

namespace {

// Lume Pad 2-class defaults. Used until CNSDK reports the real device
// metrics through leia_core (CNSDK exposes those via leia_core_get_*
// once the async init completes — wiring TBD).
constexpr float kDefaultDisplayWidthM  = 0.1934f;  // ~12.4" diagonal, 16:10
constexpr float kDefaultDisplayHeightM = 0.1209f;
constexpr uint32_t kDefaultDisplayPixelW = 2560;
constexpr uint32_t kDefaultDisplayPixelH = 1600;

// Hardcoded IPD-only eye positions. Origin = display center; z is
// toward the user. 65 mm IPD, ~50 cm viewing distance.
constexpr float kIpdHalfM       = 0.0325f;
constexpr float kEyeViewerDistM = 0.5f;

// CNSDK's leia_interlacer_vulkan_initialize is called in leia_cnsdk.cpp
// with VK_FORMAT_B8G8R8A8_SRGB as the views format, so per-view images
// must match.
constexpr VkFormat kPerViewFormat = VK_FORMAT_B8G8R8A8_SRGB;

struct leia_dp_cnsdk
{
	struct xrt_display_processor base;
	struct leia_cnsdk *cnsdk;          //!< Owned.
	struct vk_bundle *vk;              //!< Borrowed from compositor.
	VkCommandPool cmd_pool;            //!< Borrowed from compositor.

	// Per-view images. Lazily created; resized if view dims change.
	VkImage view_img[2];
	VkDeviceMemory view_mem[2];
	VkImageView view_iv[2];
	uint32_t cached_view_w;
	uint32_t cached_view_h;

	// Cached per-frame blit resources. Created once at factory time, reused
	// across frames to avoid per-frame Vulkan object churn.
	//
	//   blit_cmd     — one-shot atlas → per-view-image cmd buffer, reset
	//                  + re-recorded each frame.
	//   blit_done    — binary semaphore signaled by the blit submit and
	//                  waited on by CNSDK's weave submit (chains the two
	//                  submits on the GPU side; no host stall between).
	//   blit_fence   — fence signaled by the blit submit; we wait on it
	//                  the next time we want to reset the cmd buffer so we
	//                  don't overwrite work the GPU is still consuming.
	//   blit_in_flight — true once blit_fence has been submitted at least
	//                    once; gates the wait-then-reset on subsequent
	//                    frames so frame 0 doesn't wait on an unsignaled
	//                    fence.
	VkCommandBuffer blit_cmd;
	VkSemaphore blit_done;
	VkFence blit_fence;
	bool blit_in_flight;
};

inline leia_dp_cnsdk *
as_impl(struct xrt_display_processor *xdp)
{
	return reinterpret_cast<leia_dp_cnsdk *>(xdp);
}

void
destroy_per_view_images(leia_dp_cnsdk *impl)
{
	if (impl->vk == nullptr) {
		return;
	}
	struct vk_bundle *vk = impl->vk;
	for (int i = 0; i < 2; ++i) {
		if (impl->view_iv[i] != VK_NULL_HANDLE) {
			vk->vkDestroyImageView(vk->device, impl->view_iv[i], nullptr);
			impl->view_iv[i] = VK_NULL_HANDLE;
		}
		if (impl->view_img[i] != VK_NULL_HANDLE) {
			vk->vkDestroyImage(vk->device, impl->view_img[i], nullptr);
			impl->view_img[i] = VK_NULL_HANDLE;
		}
		if (impl->view_mem[i] != VK_NULL_HANDLE) {
			vk->vkFreeMemory(vk->device, impl->view_mem[i], nullptr);
			impl->view_mem[i] = VK_NULL_HANDLE;
		}
	}
	impl->cached_view_w = 0;
	impl->cached_view_h = 0;
}

bool
create_per_view_images(leia_dp_cnsdk *impl, uint32_t w, uint32_t h)
{
	struct vk_bundle *vk = impl->vk;

	VkImageCreateInfo ici = {};
	ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = kPerViewFormat;
	ici.extent = {w, h, 1};
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	for (int i = 0; i < 2; ++i) {
		VkResult res = vk->vkCreateImage(vk->device, &ici, nullptr, &impl->view_img[i]);
		if (res != VK_SUCCESS) {
			U_LOG_E("CNSDK DP: vkCreateImage view %d failed: %d", i, res);
			return false;
		}

		VkMemoryRequirements reqs;
		vk->vkGetImageMemoryRequirements(vk->device, impl->view_img[i], &reqs);

		res = vk_alloc_and_bind_image_memory(
		    vk, impl->view_img[i], &reqs, nullptr,
		    "leia_dp_cnsdk per-view", &impl->view_mem[i]);
		if (res != VK_SUCCESS) {
			U_LOG_E("CNSDK DP: vk_alloc_and_bind_image_memory view %d failed: %d", i, res);
			return false;
		}

		VkImageViewCreateInfo ivci = {};
		ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		ivci.image = impl->view_img[i];
		ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
		ivci.format = kPerViewFormat;
		ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		ivci.subresourceRange.baseMipLevel = 0;
		ivci.subresourceRange.levelCount = 1;
		ivci.subresourceRange.baseArrayLayer = 0;
		ivci.subresourceRange.layerCount = 1;

		res = vk->vkCreateImageView(vk->device, &ivci, nullptr, &impl->view_iv[i]);
		if (res != VK_SUCCESS) {
			U_LOG_E("CNSDK DP: vkCreateImageView view %d failed: %d", i, res);
			return false;
		}
	}

	impl->cached_view_w = w;
	impl->cached_view_h = h;
	return true;
}

// Records and submits the cached blit cmd buffer that copies the two atlas
// tiles into the per-view images, signaling impl->blit_done so CNSDK's
// weave submit can wait for the blit on the GPU side instead of stalling
// the host. Per-view images are left in SHADER_READ_ONLY_OPTIMAL so the
// interlacer can sample them.
bool
blit_atlas_to_per_view(leia_dp_cnsdk *impl,
                       VkImage atlas_image,
                       uint32_t view_w,
                       uint32_t view_h)
{
	struct vk_bundle *vk = impl->vk;
	VkCommandBuffer cmd = impl->blit_cmd;

	// Wait for the previous frame's blit to finish before we overwrite the
	// cmd buffer. Frame 0 has nothing in flight.
	if (impl->blit_in_flight) {
		vk->vkWaitForFences(vk->device, 1, &impl->blit_fence, VK_TRUE, UINT64_MAX);
		vk->vkResetFences(vk->device, 1, &impl->blit_fence);
		impl->blit_in_flight = false;
	}

	vk->vkResetCommandBuffer(cmd, 0);

	VkCommandBufferBeginInfo bi = {};
	bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vk->vkBeginCommandBuffer(cmd, &bi);

	// UNDEFINED → TRANSFER_DST_OPTIMAL on both view images.
	VkImageMemoryBarrier to_dst[2] = {};
	for (int i = 0; i < 2; ++i) {
		to_dst[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		to_dst[i].srcAccessMask = 0;
		to_dst[i].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		to_dst[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		to_dst[i].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		to_dst[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		to_dst[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		to_dst[i].image = impl->view_img[i];
		to_dst[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	}
	vk->vkCmdPipelineBarrier(cmd,
	    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	    VK_PIPELINE_STAGE_TRANSFER_BIT,
	    0, 0, nullptr, 0, nullptr, 2, to_dst);

	// Per-tile blit. Atlas layout is SBS: tile 0 = left half (x:0..w),
	// tile 1 = right half (x:w..2w). Both fill the full atlas height.
	for (int i = 0; i < 2; ++i) {
		VkImageBlit blit = {};
		blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		blit.srcOffsets[0] = {(int32_t)(i * view_w), 0, 0};
		blit.srcOffsets[1] = {(int32_t)((i + 1) * view_w), (int32_t)view_h, 1};
		blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		blit.dstOffsets[0] = {0, 0, 0};
		blit.dstOffsets[1] = {(int32_t)view_w, (int32_t)view_h, 1};

		vk->vkCmdBlitImage(cmd,
		    atlas_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		    impl->view_img[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		    1, &blit, VK_FILTER_LINEAR);
	}

	// TRANSFER_DST_OPTIMAL → SHADER_READ_ONLY_OPTIMAL so CNSDK can sample.
	VkImageMemoryBarrier to_shr[2] = {};
	for (int i = 0; i < 2; ++i) {
		to_shr[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		to_shr[i].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		to_shr[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		to_shr[i].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		to_shr[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		to_shr[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		to_shr[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		to_shr[i].image = impl->view_img[i];
		to_shr[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	}
	vk->vkCmdPipelineBarrier(cmd,
	    VK_PIPELINE_STAGE_TRANSFER_BIT,
	    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	    0, 0, nullptr, 0, nullptr, 2, to_shr);

	vk->vkEndCommandBuffer(cmd);

	VkSubmitInfo si = {};
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd;
	si.signalSemaphoreCount = 1;
	si.pSignalSemaphores = &impl->blit_done;
	VkResult res = vk->vkQueueSubmit(vk->main_queue->queue, 1, &si, impl->blit_fence);
	if (res != VK_SUCCESS) {
		return false;
	}
	impl->blit_in_flight = true;
	return true;
}

void
process_atlas_weave(struct xrt_display_processor *xdp,
                    VkCommandBuffer cmd_buffer,
                    VkImage_XDP atlas_image,
                    VkImageView atlas_view,
                    uint32_t view_width,
                    uint32_t view_height,
                    uint32_t tile_columns,
                    uint32_t tile_rows,
                    VkFormat_XDP view_format,
                    VkFramebuffer target_fb,
                    VkImage_XDP target_image,
                    uint32_t target_width,
                    uint32_t target_height,
                    VkFormat_XDP target_format,
                    int32_t canvas_offset_x,
                    int32_t canvas_offset_y,
                    uint32_t canvas_width,
                    uint32_t canvas_height)
{
	(void)cmd_buffer;     // self-submitting: compositor passes VK_NULL_HANDLE
	(void)atlas_view;     // CNSDK uses per-view image views we own
	(void)view_format;
	(void)canvas_offset_x; (void)canvas_offset_y;
	(void)canvas_width; (void)canvas_height;

	leia_dp_cnsdk *impl = as_impl(xdp);

	if (tile_columns != 2 || tile_rows != 1) {
		static bool warned = false;
		if (!warned) {
			U_LOG_W("CNSDK DP expects 2x1 SBS atlas, got %ux%u; skipping weave",
			        tile_columns, tile_rows);
			warned = true;
		}
		return;
	}

	if (impl->vk == nullptr || impl->cmd_pool == VK_NULL_HANDLE) {
		return;
	}

	// (Re)allocate per-view images if dims changed.
	if (impl->cached_view_w != view_width || impl->cached_view_h != view_height) {
		destroy_per_view_images(impl);
		if (!create_per_view_images(impl, view_width, view_height)) {
			destroy_per_view_images(impl);
			return;
		}
	}

	if (!blit_atlas_to_per_view(impl, (VkImage)(uintptr_t)atlas_image,
	                             view_width, view_height)) {
		return;
	}

	leia_cnsdk_weave(impl->cnsdk,
	                 impl->vk->device,
	                 impl->vk->physical_device,
	                 impl->view_iv[0],
	                 impl->view_iv[1],
	                 (VkFormat)target_format,
	                 target_width,
	                 target_height,
	                 target_fb,
	                 (VkImage)(uintptr_t)target_image,
	                 impl->blit_done);
}

bool
is_self_submitting_true(struct xrt_display_processor *xdp)
{
	(void)xdp;
	return true;
}

bool
get_predicted_eye_positions_ipd(struct xrt_display_processor *xdp,
                                 struct xrt_eye_positions *out_eye_pos)
{
	(void)xdp;
	out_eye_pos->eyes[0].x = -kIpdHalfM;
	out_eye_pos->eyes[0].y = 0.0f;
	out_eye_pos->eyes[0].z = kEyeViewerDistM;
	out_eye_pos->eyes[1].x = +kIpdHalfM;
	out_eye_pos->eyes[1].y = 0.0f;
	out_eye_pos->eyes[1].z = kEyeViewerDistM;
	out_eye_pos->count = 2;
	out_eye_pos->valid = true;
	out_eye_pos->is_tracking = false;
	return true;
}

bool
get_display_dimensions_default(struct xrt_display_processor *xdp,
                                float *out_width_m,
                                float *out_height_m)
{
	(void)xdp;
	*out_width_m = kDefaultDisplayWidthM;
	*out_height_m = kDefaultDisplayHeightM;
	return true;
}

bool
get_display_pixel_info_default(struct xrt_display_processor *xdp,
                                uint32_t *out_pixel_width,
                                uint32_t *out_pixel_height,
                                int32_t *out_screen_left,
                                int32_t *out_screen_top)
{
	(void)xdp;
	*out_pixel_width = kDefaultDisplayPixelW;
	*out_pixel_height = kDefaultDisplayPixelH;
	*out_screen_left = 0;
	*out_screen_top = 0;
	return true;
}

void
destroy_blit_resources(leia_dp_cnsdk *impl)
{
	if (impl->vk == nullptr) {
		return;
	}
	struct vk_bundle *vk = impl->vk;

	// Make sure any in-flight blit has completed before we tear down the
	// cmd buffer / fence / semaphore the GPU is still consuming.
	if (impl->blit_in_flight && impl->blit_fence != VK_NULL_HANDLE) {
		vk->vkWaitForFences(vk->device, 1, &impl->blit_fence, VK_TRUE, UINT64_MAX);
		impl->blit_in_flight = false;
	}
	if (impl->blit_cmd != VK_NULL_HANDLE && impl->cmd_pool != VK_NULL_HANDLE) {
		vk->vkFreeCommandBuffers(vk->device, impl->cmd_pool, 1, &impl->blit_cmd);
		impl->blit_cmd = VK_NULL_HANDLE;
	}
	if (impl->blit_done != VK_NULL_HANDLE) {
		vk->vkDestroySemaphore(vk->device, impl->blit_done, nullptr);
		impl->blit_done = VK_NULL_HANDLE;
	}
	if (impl->blit_fence != VK_NULL_HANDLE) {
		vk->vkDestroyFence(vk->device, impl->blit_fence, nullptr);
		impl->blit_fence = VK_NULL_HANDLE;
	}
}

bool
create_blit_resources(leia_dp_cnsdk *impl)
{
	struct vk_bundle *vk = impl->vk;

	VkCommandBufferAllocateInfo ai = {};
	ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	ai.commandPool = impl->cmd_pool;
	ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	ai.commandBufferCount = 1;
	if (vk->vkAllocateCommandBuffers(vk->device, &ai, &impl->blit_cmd) != VK_SUCCESS) {
		U_LOG_E("CNSDK DP: blit cmd buffer alloc failed");
		return false;
	}

	VkSemaphoreCreateInfo sci = {};
	sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	if (vk->vkCreateSemaphore(vk->device, &sci, nullptr, &impl->blit_done) != VK_SUCCESS) {
		U_LOG_E("CNSDK DP: blit_done semaphore create failed");
		return false;
	}

	VkFenceCreateInfo fci = {};
	fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	// Unsignaled: first frame sees blit_in_flight=false and skips wait.
	if (vk->vkCreateFence(vk->device, &fci, nullptr, &impl->blit_fence) != VK_SUCCESS) {
		U_LOG_E("CNSDK DP: blit_fence create failed");
		return false;
	}
	return true;
}

void
destroy_impl(struct xrt_display_processor *xdp)
{
	leia_dp_cnsdk *impl = as_impl(xdp);
	destroy_blit_resources(impl);
	destroy_per_view_images(impl);
	if (impl->cnsdk != nullptr) {
		leia_cnsdk_destroy(&impl->cnsdk);
	}
	free(impl);
}

} // namespace

extern "C" xrt_result_t
leia_dp_factory_cnsdk(void *vk_bundle,
                      void *vk_cmd_pool,
                      void *window_handle,
                      int32_t target_format,
                      struct xrt_display_processor **out_xdp)
{
	(void)window_handle; (void)target_format;

	struct leia_cnsdk *cnsdk = nullptr;
	xrt_result_t ret = leia_cnsdk_create(&cnsdk);
	if (ret != XRT_SUCCESS || cnsdk == nullptr) {
		U_LOG_W("leia_cnsdk_create failed (%d), falling back to no-DP path", (int)ret);
		return ret != XRT_SUCCESS ? ret : XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	leia_dp_cnsdk *impl = static_cast<leia_dp_cnsdk *>(calloc(1, sizeof(*impl)));
	if (impl == nullptr) {
		leia_cnsdk_destroy(&cnsdk);
		return XRT_ERROR_ALLOCATION;
	}

	impl->cnsdk = cnsdk;
	impl->vk = static_cast<struct vk_bundle *>(vk_bundle);
	impl->cmd_pool = (VkCommandPool)(uintptr_t)vk_cmd_pool;

	if (!create_blit_resources(impl)) {
		destroy_blit_resources(impl);
		leia_cnsdk_destroy(&impl->cnsdk);
		free(impl);
		return XRT_ERROR_VULKAN;
	}

	impl->base.process_atlas = process_atlas_weave;
	impl->base.is_self_submitting = is_self_submitting_true;
	impl->base.get_predicted_eye_positions = get_predicted_eye_positions_ipd;
	impl->base.get_display_dimensions = get_display_dimensions_default;
	impl->base.get_display_pixel_info = get_display_pixel_info_default;
	impl->base.destroy = destroy_impl;

	*out_xdp = &impl->base;

	U_LOG_W("Leia CNSDK DP created (self-submitting, per-tile blit + CNSDK weave)");
	return XRT_SUCCESS;
}
