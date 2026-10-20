// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  File-trigger capture for the service-side Vulkan weave engine
 *         (XR_DXR_weave, comp_multi_weave_linux.c) — #2207.
 * @ingroup comp_multi
 *
 * The weave engine has no swapchain and no window: a present-owner (the
 * DisplayXR Browser, the weave probes) hands the service pre-weave pixels and
 * gets a woven buffer back. Until #2207 nothing on that route honoured the
 * capture trigger, so its pixels could only be checked with a desktop
 * screenshot. This module gives it the SAME triggers as the in-process
 * vk_native compositor (u_capture_intent.h — `$TMPDIR/displayxr_atlas_trigger`
 * and its `.projection` / per-exe variants), read in the SERVICE's
 * environment, and writes up to three PNGs per trigger:
 *
 *  - input — `displayxr_atlas.input.png`: the caller's submitted buffer as
 *    received (the browser's page / the SBS or N-view atlas), before any
 *    un-squeeze, crop or lift.
 *  - atlas — `displayxr_atlas.png`: what the display processor consumed, the
 *    2x1 SBS scratch (batch / legacy) or the cropped N-view grid (v6), lifted
 *    rects' views composed in. Pre-weave.
 *  - woven — `displayxr_atlas.woven.png`: the output handed back to the
 *    caller, post-weave, incl. off-panel / flat-region 2D and the v4 overlay.
 *
 * A PROJECTION_ONLY trigger writes the atlas alone, to
 * `displayxr_atlas.projection.png` (the engine has no window-space layers, so
 * "projection only" is the DP input). Per-tag triggers name the CLIENT's
 * executable (`displayxr_atlas_trigger.<client-exe>` →
 * `displayxr_atlas.<client-exe>[.input|.woven].png`), checked before the
 * service-process / global pair.
 *
 * Hot-path cost: one trigger poll per weave submit (a few stat calls). Only a
 * submit that consumed a trigger records the readback copies into its own
 * command buffer (host-visible staging buffers), waits its own fence, and
 * encodes the PNGs — one WARN per completed capture.
 */

#pragma once

#include "xrt/xrt_vulkan_includes.h"
#include "util/u_capture_intent.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct vk_bundle;

//! The stages one trigger can capture, in file order.
enum comp_multi_weave_capture_stage
{
	COMP_MULTI_WEAVE_CAPTURE_INPUT = 0, //!< the caller's buffer as received
	COMP_MULTI_WEAVE_CAPTURE_ATLAS = 1, //!< the DP's pre-weave input
	COMP_MULTI_WEAVE_CAPTURE_WOVEN = 2, //!< the post-weave output
	COMP_MULTI_WEAVE_CAPTURE_STAGE_COUNT = 3,
};

//! One recorded readback (staging buffer + what it holds).
struct comp_multi_weave_capture_target
{
	VkBuffer buffer;
	VkDeviceMemory memory;
	uint32_t w, h;
	VkFormat format;
	bool recorded;
};

//! Per-client capture state (lives in multi_compositor::weave).
struct comp_multi_weave_capture
{
	//! This submit's intent (pending = this submit records + writes).
	struct u_capture_intent intent;
	//! The client's executable basename (the per-tag trigger), "" = none.
	char tag[64];
	struct comp_multi_weave_capture_target target[COMP_MULTI_WEAVE_CAPTURE_STAGE_COUNT];
};

/*!
 * Top of a weave submit, AFTER the previous frame's fence was waited (so any
 * staging buffers a failed submit left behind are idle and are freed here).
 * Polls the client-tag triggers, then the service-process / global ones.
 * Returns true iff this submit captures.
 */
bool
comp_multi_weave_capture_begin(struct comp_multi_weave_capture *cap, struct vk_bundle *vk);

//! Does this submit capture @p stage? (atlas: every mode; input / woven: POST_COMPOSE only.)
bool
comp_multi_weave_capture_wants(const struct comp_multi_weave_capture *cap, enum comp_multi_weave_capture_stage stage);

/*!
 * Record the readback of @p image (whole extent @p w x @p h, 4 bytes per pixel)
 * into @p cmd: barrier @p layout → TRANSFER_SRC (kept GENERAL when @p layout is
 * GENERAL), copy to a fresh host-visible buffer, barrier back to @p layout.
 * @p stage / @p access are the image's last use before the copy and its next use
 * after it; the restore barrier re-establishes them. No-op (false) when the
 * stage is not wanted or the format is not a 4-byte RGBA/BGRA one.
 */
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
                                VkAccessFlags last_access);

/*!
 * After the submit that recorded the copies has COMPLETED (caller waited its
 * fence): map each recorded buffer, encode its PNG, free the buffers, emit one
 * WARN, and consume the intent. @p what names the frame in the WARN.
 */
void
comp_multi_weave_capture_finish(struct comp_multi_weave_capture *cap, struct vk_bundle *vk, const char *what);

//! Free any staging buffers (GPU must be idle on them). Fini / abandon path.
void
comp_multi_weave_capture_release(struct comp_multi_weave_capture *cap, struct vk_bundle *vk);

#ifdef __cplusplus
}
#endif
