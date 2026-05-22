// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Leia CNSDK display processor (Android), atlas-mode variant.
 *
 * Wraps leia_cnsdk as an xrt_display_processor. Uses CNSDK's atlas mode:
 * the compositor's pre-composited SBS atlas VkImage+View is passed
 * directly to CNSDK each frame via
 * leia_interlacer_vulkan_set_interlace_view_texture_atlas, and CNSDK
 * splits L/R internally. No per-view image management, no per-tile
 * blit, no blit cmd buffer / semaphore / fence on our side.
 *
 * Display metrics + face-tracked eye positions come from CNSDK once the
 * async core init completes (lazy on first query). Falls back to
 * hardcoded Lume Pad 2 metrics and IPD-only eyes while the core is
 * still booting.
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

#if defined(XRT_OS_ANDROID) && defined(XRT_DEBUG_ANDROID_VERBOSE)
#include <android/trace.h>
#endif


// Hardware-bring-up debug logging. Gated by XRT_DEBUG_ANDROID_VERBOSE
// (cppFlag from the Android Debug variant). Compiles to nothing in
// release. Tag "HW_DBG_DP:" greppable in logcat, separate from the
// CNSDK wrapper's HW_DBG_CNSDK tag.
#ifdef XRT_DEBUG_ANDROID_VERBOSE
#define DXR_HW_DBG(...)       U_LOG_I("HW_DBG_DP: " __VA_ARGS__)
#define DXR_HW_DBG_ONCE(...)  do {                                                                 \
		static bool _logged = false;                                                                \
		if (!_logged) { U_LOG_I("HW_DBG_DP[once]: " __VA_ARGS__); _logged = true; }                 \
	} while (0)
struct AtraceScopeDp {
	AtraceScopeDp(const char *name) { ATrace_beginSection(name); }
	~AtraceScopeDp() { ATrace_endSection(); }
};
#define DXR_ATRACE(name) AtraceScopeDp _atrace_##__LINE__(name)
#else
#define DXR_HW_DBG(...)       ((void)0)
#define DXR_HW_DBG_ONCE(...)  ((void)0)
#define DXR_ATRACE(name)      ((void)0)
#endif


namespace {

// Lume Pad 2-class defaults. Used until CNSDK reports the real device
// metrics through leia_core via the leia_cnsdk wrapper's cached values.
constexpr float kDefaultDisplayWidthM  = 0.1934f;  // ~12.4" diagonal, 16:10
constexpr float kDefaultDisplayHeightM = 0.1209f;
constexpr uint32_t kDefaultDisplayPixelW = 2560;
constexpr uint32_t kDefaultDisplayPixelH = 1600;

// Hardcoded IPD-only eye positions. Origin = display center; z is
// toward the user. 65 mm IPD, ~50 cm viewing distance.
constexpr float kIpdHalfM       = 0.0325f;
constexpr float kEyeViewerDistM = 0.5f;

struct leia_dp_cnsdk
{
	struct xrt_display_processor base;
	struct leia_cnsdk *cnsdk;          //!< Owned.
	struct vk_bundle *vk;              //!< Borrowed from compositor.
	VkCommandPool cmd_pool;            //!< Borrowed from compositor; used by mono passthrough.
};

inline leia_dp_cnsdk *
as_impl(struct xrt_display_processor *xdp)
{
	return reinterpret_cast<leia_dp_cnsdk *>(xdp);
}

// Mono / 2D-mode passthrough: blit the single-tile atlas directly to
// the swapchain target. No CNSDK weave (would be wrong for 1x1). Same
// barrier dance as the atlas-side host-stall path — drain via
// vkQueueWaitIdle so xrEndFrame sees the target image ready.
bool
mono_passthrough_blit(leia_dp_cnsdk *impl,
                      VkImage atlas_image,
                      uint32_t atlas_width,
                      uint32_t atlas_height,
                      VkImage target_image,
                      uint32_t target_width,
                      uint32_t target_height)
{
	DXR_ATRACE("dxr_dp:mono_passthrough_blit");
	struct vk_bundle *vk = impl->vk;

	VkCommandBufferAllocateInfo ai = {};
	ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	ai.commandPool = impl->cmd_pool;
	ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	ai.commandBufferCount = 1;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	if (vk->vkAllocateCommandBuffers(vk->device, &ai, &cmd) != VK_SUCCESS) {
		return false;
	}

	VkCommandBufferBeginInfo bi = {};
	bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vk->vkBeginCommandBuffer(cmd, &bi);

	// Atlas: SHADER_READ_ONLY_OPTIMAL → TRANSFER_SRC_OPTIMAL.
	VkImageMemoryBarrier atlas_to_src = {};
	atlas_to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	atlas_to_src.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
	atlas_to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	atlas_to_src.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	atlas_to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	atlas_to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	atlas_to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	atlas_to_src.image = atlas_image;
	atlas_to_src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

	// Target: assume COLOR_ATTACHMENT_OPTIMAL (the compositor's pre-DP
	// barrier set this in the window-target path; for texture-mode the
	// shared image's layout is caller-dependent — we use UNDEFINED→
	// TRANSFER_DST which is always safe for clearing).
	VkImageMemoryBarrier target_to_dst = {};
	target_to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	target_to_dst.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	target_to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	target_to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	target_to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	target_to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	target_to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	target_to_dst.image = target_image;
	target_to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

	VkImageMemoryBarrier pre[2] = {atlas_to_src, target_to_dst};
	vk->vkCmdPipelineBarrier(cmd,
	    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	    VK_PIPELINE_STAGE_TRANSFER_BIT,
	    0, 0, nullptr, 0, nullptr, 2, pre);

	// Stretch-blit the atlas to fill the target. Linear filter so a
	// resolution mismatch doesn't go blocky.
	VkImageBlit blit = {};
	blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	blit.srcOffsets[0] = {0, 0, 0};
	blit.srcOffsets[1] = {(int32_t)atlas_width, (int32_t)atlas_height, 1};
	blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	blit.dstOffsets[0] = {0, 0, 0};
	blit.dstOffsets[1] = {(int32_t)target_width, (int32_t)target_height, 1};
	vk->vkCmdBlitImage(cmd,
	    atlas_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	    target_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	    1, &blit, VK_FILTER_LINEAR);

	// Restore atlas to SHADER_READ for the compositor's invariant;
	// leave target in COLOR_ATTACHMENT_OPTIMAL (xrEndFrame's contract).
	VkImageMemoryBarrier atlas_back = atlas_to_src;
	atlas_back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	atlas_back.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	atlas_back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	atlas_back.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	VkImageMemoryBarrier target_back = target_to_dst;
	target_back.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	target_back.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	target_back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	target_back.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	VkImageMemoryBarrier post[2] = {atlas_back, target_back};
	vk->vkCmdPipelineBarrier(cmd,
	    VK_PIPELINE_STAGE_TRANSFER_BIT,
	    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	    0, 0, nullptr, 0, nullptr, 2, post);

	vk->vkEndCommandBuffer(cmd);

	VkSubmitInfo si = {};
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd;
	VkResult res = vk->vkQueueSubmit(vk->main_queue->queue, 1, &si, VK_NULL_HANDLE);
	if (res == VK_SUCCESS) {
		vk->vkQueueWaitIdle(vk->main_queue->queue);
	}
	vk->vkFreeCommandBuffers(vk->device, impl->cmd_pool, 1, &cmd);
	DXR_HW_DBG_ONCE("mono_passthrough_blit: first frame (atlas %ux%u → target %ux%u)",
	                atlas_width, atlas_height, target_width, target_height);
	return res == VK_SUCCESS;
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
	DXR_ATRACE("dxr_dp:process_atlas_weave");
	(void)cmd_buffer;     // self-submitting: compositor passes VK_NULL_HANDLE
	(void)view_format;
	(void)canvas_offset_x; (void)canvas_offset_y;
	(void)canvas_width; (void)canvas_height;

	leia_dp_cnsdk *impl = as_impl(xdp);

	if (tile_columns == 1 && tile_rows == 1) {
		// Mono / 2D mode: no interlacing needed. Atlas IS the final
		// image; blit it directly to the target so we still produce
		// something visible. CNSDK doesn't weave here (would be wrong
		// for 1x1) — only used in the runtime's 2D fallback or for
		// non-3D-display vendors. Implementation moved below for
		// readability.
		if (impl->vk == nullptr || impl->cmd_pool == VK_NULL_HANDLE) {
			return;
		}
		const uint32_t atlas_w_mono = view_width;
		const uint32_t atlas_h_mono = view_height;
		mono_passthrough_blit(impl, (VkImage)(uintptr_t)atlas_image,
		                      atlas_w_mono, atlas_h_mono,
		                      (VkImage)(uintptr_t)target_image,
		                      target_width, target_height);
		return;
	}
	if (tile_columns != 2 || tile_rows != 1) {
		static bool warned = false;
		if (!warned) {
			U_LOG_W("CNSDK DP expects 2x1 SBS atlas or 1x1 mono, got %ux%u; skipping",
			        tile_columns, tile_rows);
			warned = true;
		}
		return;
	}

	if (impl->vk == nullptr) {
		return;
	}

	// Gate the weave on CNSDK interlacer readiness. Async core init may
	// take several frames; until then the interlacer doesn't exist and
	// leia_cnsdk_weave is a no-op. The compositor's pre-DP submit has
	// already happened, so target is in COLOR_ATTACHMENT_OPTIMAL with
	// undefined content for those frames — acceptable for POC.
	if (!leia_cnsdk_ensure_interlacer(impl->cnsdk,
	                                   impl->vk->device,
	                                   impl->vk->physical_device,
	                                   (VkFormat)target_format)) {
		return;
	}
	DXR_HW_DBG_ONCE("process_atlas_weave: first frame with ready interlacer");

	const uint32_t atlas_w = view_width * tile_columns;
	const uint32_t atlas_h = view_height * tile_rows;

#ifdef XRT_DEBUG_ANDROID_VERBOSE
	static int dp_dbg_frame = 0;
	if ((dp_dbg_frame++ % 60) == 0) {
		DXR_HW_DBG("process_atlas_weave[frame=%d]: atlas=%ux%u target=%ux%u fmt=%d",
		           dp_dbg_frame, atlas_w, atlas_h, target_width, target_height, (int)target_format);
	}
#endif

	leia_cnsdk_weave(impl->cnsdk,
	                 impl->vk->device,
	                 impl->vk->physical_device,
	                 (VkImage)(uintptr_t)atlas_image,
	                 atlas_view,
	                 atlas_w,
	                 atlas_h,
	                 (VkFormat)target_format,
	                 target_width,
	                 target_height,
	                 target_fb,
	                 (VkImage)(uintptr_t)target_image);
}

bool
is_self_submitting_true(struct xrt_display_processor *xdp)
{
	(void)xdp;
	return true;
}

// Try to fetch CNSDK's predicted face position and derive L/R eyes from
// it (face X ± IPD/2). Falls back to a hardcoded IPD-only stub if face
// tracking isn't running yet (CNSDK core still async-initializing, or
// no face lock).
bool
get_predicted_eye_positions_ipd(struct xrt_display_processor *xdp,
                                 struct xrt_eye_positions *out_eye_pos)
{
	leia_dp_cnsdk *impl = as_impl(xdp);

	bool tracked = false;
	float fx = 0.0f, fy = 0.0f, fz = kEyeViewerDistM;
	if (impl->cnsdk != nullptr) {
		if (leia_cnsdk_ensure_face_tracking_started(impl->cnsdk) &&
		    leia_cnsdk_get_primary_face(impl->cnsdk, &fx, &fy, &fz)) {
			tracked = true;
		}
	}

	out_eye_pos->eyes[0].x = fx - kIpdHalfM;
	out_eye_pos->eyes[0].y = fy;
	out_eye_pos->eyes[0].z = fz;
	out_eye_pos->eyes[1].x = fx + kIpdHalfM;
	out_eye_pos->eyes[1].y = fy;
	out_eye_pos->eyes[1].z = fz;
	out_eye_pos->count = 2;
	out_eye_pos->valid = true;
	out_eye_pos->is_tracking = tracked;
	return true;
}

bool
get_display_dimensions_default(struct xrt_display_processor *xdp,
                                float *out_width_m,
                                float *out_height_m)
{
	leia_dp_cnsdk *impl = as_impl(xdp);

	if (impl->cnsdk != nullptr &&
	    leia_cnsdk_get_display_metrics(impl->cnsdk, out_width_m, out_height_m,
	                                    nullptr, nullptr)) {
		return true;
	}

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
	leia_dp_cnsdk *impl = as_impl(xdp);

	*out_screen_left = 0;
	*out_screen_top = 0;

	if (impl->cnsdk != nullptr &&
	    leia_cnsdk_get_display_metrics(impl->cnsdk, nullptr, nullptr,
	                                    out_pixel_width, out_pixel_height)) {
		return true;
	}

	*out_pixel_width = kDefaultDisplayPixelW;
	*out_pixel_height = kDefaultDisplayPixelH;
	return true;
}

void
on_pause_cnsdk(struct xrt_display_processor *xdp)
{
	leia_dp_cnsdk *impl = as_impl(xdp);
	if (impl->cnsdk != nullptr) {
		leia_cnsdk_on_pause(impl->cnsdk);
	}
}

void
on_resume_cnsdk(struct xrt_display_processor *xdp)
{
	leia_dp_cnsdk *impl = as_impl(xdp);
	if (impl->cnsdk != nullptr) {
		leia_cnsdk_on_resume(impl->cnsdk);
	}
}

void
destroy_impl(struct xrt_display_processor *xdp)
{
	leia_dp_cnsdk *impl = as_impl(xdp);

	// Drain all in-flight GPU work (especially CNSDK's interlacer
	// submits) before destroying any handles CNSDK might still be
	// reading. Same defensive idiom as audit B6 — applies in atlas mode
	// because CNSDK still owns its own queue submit even though we
	// don't have a per-view image / blit cmd buffer to wait on.
	if (impl->vk != nullptr) {
		impl->vk->vkDeviceWaitIdle(impl->vk->device);
	}

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

	impl->base.process_atlas = process_atlas_weave;
	impl->base.on_pause = on_pause_cnsdk;
	impl->base.on_resume = on_resume_cnsdk;
	impl->base.is_self_submitting = is_self_submitting_true;
	impl->base.get_predicted_eye_positions = get_predicted_eye_positions_ipd;
	impl->base.get_display_dimensions = get_display_dimensions_default;
	impl->base.get_display_pixel_info = get_display_pixel_info_default;
	impl->base.destroy = destroy_impl;

	*out_xdp = &impl->base;

	U_LOG_W("Leia CNSDK DP created (atlas mode)");
	DXR_HW_DBG("factory: impl=%p vk=%p cnsdk=%p", (void *)impl, (void *)impl->vk, (void *)impl->cnsdk);
	return XRT_SUCCESS;
}
