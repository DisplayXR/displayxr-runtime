// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  File-trigger capture for the service-side weave engines
 *         (XR_DXR_weave): desktop Linux (comp_multi_weave_linux.c, #2207),
 *         macOS (comp_multi_weave_macos.c over its vk / Metal backends, #2470)
 *         and Android (comp_multi_weave_android.c, #2501).
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
 *  - overlay — `displayxr_atlas.overlay.png`: the v4 2D overlay layer as the
 *    engine received it (premultiplied; its alpha is the 2D-coverage mask, so
 *    set `DXR_ATLAS_CAPTURE_RAW_ALPHA=1` to keep it). Written only by an engine
 *    that records it (Android, #2501) and only on a submit that chained one.
 *    `woven` against `overlay` shows whether a DP that composites the overlay
 *    inside the weave changed its pixels.
 *
 * Android (#2501): there is no useful `$TMPDIR` in a service process, so the
 * engine points `$TMPDIR` at a directory `adb` can reach before its first
 * poll — the `debug.dxr.capture_dir` property if set, else the runtime
 * package's external files dir (`/sdcard/Android/data/<runtime-pkg>/files`),
 * else its cache dir — and logs the choice once. The per-client tag is the
 * client's PACKAGE name (`PackageManager.getNameForUid` of the peer uid), e.g.
 * `displayxr_atlas_trigger.org.chromium.chrome`.
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
 * command buffer (host-visible staging buffers) and waits its own fence; the
 * PNGs are encoded on a detached worker thread (stb takes ~2 s per 2560x1600
 * stage, which held the submit for seconds before #2536), each written to
 * `<name>.tmp` and renamed into place — one WARN once the last file landed.
 *
 * Two ways to fill a stage: a Vulkan readback recorded into the frame's
 * command buffer (@ref comp_multi_weave_capture_record, resolved to CPU pixels
 * once its submit completed), or CPU pixels the engine already has
 * (@ref comp_multi_weave_capture_store — the macOS front end's IOSurface reads
 * of the input and the woven output, and the Metal backend's atlas readback).
 * Either way @ref comp_multi_weave_capture_finish encodes every filled stage.
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
	COMP_MULTI_WEAVE_CAPTURE_INPUT = 0,   //!< the caller's buffer as received
	COMP_MULTI_WEAVE_CAPTURE_ATLAS = 1,   //!< the DP's pre-weave input
	COMP_MULTI_WEAVE_CAPTURE_WOVEN = 2,   //!< the post-weave output
	COMP_MULTI_WEAVE_CAPTURE_OVERLAY = 3, //!< the v4 2D overlay layer as received (#2501)
	COMP_MULTI_WEAVE_CAPTURE_STAGE_COUNT = 4,
};

//! One stage's pixels: a recorded Vulkan readback, or (once resolved / stored) CPU RGBA8.
struct comp_multi_weave_capture_target
{
	VkBuffer buffer; //!< Recorded readback, VK_NULL_HANDLE once resolved.
	VkDeviceMemory memory;
	uint32_t w, h;
	VkFormat format;
	bool recorded; //!< This stage holds pixels (a pending readback or @ref rgba).
	uint8_t *rgba; //!< Tight RGBA8 (alpha already forced unless raw), heap; NULL until resolved / stored.
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

//! Does this submit capture @p stage? (atlas: every mode; input / woven / overlay: POST_COMPOSE only.)
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
 * Fill @p stage from CPU pixels (4 bytes per pixel, rows @p stride bytes apart;
 * @p swap_rb for a BGRA8 source). Copied: the source may be unmapped / unlocked
 * on return. No-op (false) when the stage is not wanted or already filled.
 */
bool
comp_multi_weave_capture_store(struct comp_multi_weave_capture *cap,
                               enum comp_multi_weave_capture_stage stage,
                               const uint8_t *src,
                               uint32_t w,
                               uint32_t h,
                               size_t stride,
                               bool swap_rb);

/*!
 * After the submit that recorded the copies has COMPLETED (caller waited its
 * fence), or failed before it was ever submitted (@p gpu_ok false): map each
 * recorded buffer into CPU pixels (only when @p gpu_ok) and free every staging
 * buffer. Afterwards the capture holds no Vulkan object, so an engine that
 * finishes elsewhere (the macOS front end) can pass a NULL bundle there.
 */
void
comp_multi_weave_capture_resolve(struct comp_multi_weave_capture *cap, struct vk_bundle *vk, bool gpu_ok);

/*!
 * After the submit that recorded the copies has COMPLETED (caller waited its
 * fence): resolve any recorded buffer (@p vk may be NULL when nothing is
 * recorded-but-unresolved), hand every filled stage's pixels to a worker
 * thread that encodes the PNGs (off the weave path, #2536; inline only if no
 * thread can be started), free everything else and consume the intent. The
 * worker emits the one WARN after the last file is renamed into place. @p what
 * names the frame in the WARN.
 */
void
comp_multi_weave_capture_finish(struct comp_multi_weave_capture *cap, struct vk_bundle *vk, const char *what);

//! Free any staging buffers (GPU must be idle on them; skipped for a NULL @p vk)
//! and stored pixels. Fini / abandon path.
void
comp_multi_weave_capture_release(struct comp_multi_weave_capture *cap, struct vk_bundle *vk);

#ifdef __cplusplus
}
#endif
