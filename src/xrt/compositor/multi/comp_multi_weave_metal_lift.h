// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Lifted weave rects (XR_DXR_lift §6) recorded in Metal: the macOS
 *         Metal weave backend's half of vk_conv_weave's host-client contract.
 * @ingroup comp_multi
 *
 * The stream policy (letterbox crop, cap, frame, result pin) stays in
 * vk_conv_weave; it answers a plan per rect (vk_conv_weave_host_plan) and this
 * module records it into the weave's own MTLCommandBuffer:
 *
 *  1. SNAPSHOT: the planned area of the source is resampled into an RGBA8
 *     chain (halving linear passes -- an exact 2x2 box -- then the final pass,
 *     texel exact when 1:1, never sampling outside the area) and copied into a
 *     shared (CPU-visible) MTLBuffer; the letterbox lines are copied from the
 *     source into a second one.
 *  2. RESULT: the stream's latest result, which the Vulkan glue exported as an
 *     IOSurface, is wrapped as an MTLTexture -- its pixel format taken from the
 *     conversion format, never from the IOSurface (a Vulkan-driver-created
 *     IOSurface may report pixel format 0) -- in a UNORM format (an _SRGB
 *     result stays encoded, as the Vulkan path's raw copy keeps it) and each
 *     needed view is stretched into the rect's active area of every tile,
 *     clamped to that view's own texels. Batch: the letterbox bars are the
 *     source, 1:1, flat. Flat (batch) until the first result; nothing outside
 *     the rect is written.
 *
 * Once the command buffer completed the engine calls
 * comp_multi_weave_metal_lift_frame_done, which hands the buffers to the
 * planner (vk_conv_weave_host_done) and drops the frame's result textures; the
 * engine then calls the helper's complete, which copies the snapshot into the
 * stream's input slot (one commit per frame, no slot held across frames).
 *
 * Ordering: the glue publishes a result only after the Vulkan work writing it
 * completed (SYNC_NONE), and the plan's pin keeps the provider from reusing it
 * until complete, which runs after the Metal command buffer completed. A
 * wrapped MTLTexture retains its IOSurface, so even a detach mid-frame cannot
 * free memory this command buffer reads. No cross-API semaphore is needed.
 *
 * The header is plain C (MTL objects as void *) so tests and C callers can
 * include it; one instance per weave engine, single-threaded (the engine's
 * lock).
 */

#pragma once

#include "vk_conv/vk_conv_weave.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct comp_multi_weave_metal_lift;

//! Where the plans come from (comp_multi_weave_lift in the engine; the helper directly in tests).
struct comp_multi_weave_metal_lift_planner
{
	bool (*plan)(void *ud, const struct vk_conv_weave_rect *r, struct vk_conv_weave_host_plan *out);
	void (*done)(void *ud, uint64_t owner, uint64_t wire_id, const struct vk_conv_weave_host_done *d);
	void *ud;
};

//! On @p mtl_device (id<MTLDevice>, borrowed). NULL on failure (shader / pipeline).
struct comp_multi_weave_metal_lift *
comp_multi_weave_metal_lift_create(void *mtl_device);

//! The engine's GPU work is idle.
void
comp_multi_weave_metal_lift_destroy(struct comp_multi_weave_metal_lift **ml_ptr);

/*!
 * Record @p count lifted rects into @p cmd (id<MTLCommandBuffer>): the
 * snapshots read @p src_tex, then one render pass draws into @p dst_tex
 * (id<MTLTexture>s; RenderTarget usage on @p dst_tex; they may be the same
 * texture on the v6 layout, whose draws never read it). Each rect's images are
 * ignored: its geometry (and src.format, the channel order of @p src_tex's
 * bytes) is what counts. Call after the engine's own writes of @p dst_tex.
 * Returns how many rects were lifted (the rest: weave them as drawn).
 */
uint32_t
comp_multi_weave_metal_lift_record(struct comp_multi_weave_metal_lift *ml,
                                   const struct comp_multi_weave_metal_lift_planner *pl,
                                   void *cmd,
                                   void *src_tex,
                                   void *dst_tex,
                                   const struct vk_conv_weave_rect *rects,
                                   uint32_t count);

/*!
 * The command buffer finished (@p gpu_ok false: it failed -- nothing is
 * delivered, the helper reports "failed: copy"). Hands the copies to the
 * planner; the engine calls the helper's complete right after.
 */
void
comp_multi_weave_metal_lift_frame_done(struct comp_multi_weave_metal_lift *ml,
                                       const struct comp_multi_weave_metal_lift_planner *pl,
                                       bool gpu_ok);

//! Diagnostics (tests): per-stream Metal resource sets alive.
uint32_t
comp_multi_weave_metal_lift_stream_count(struct comp_multi_weave_metal_lift *ml);

#ifdef __cplusplus
}
#endif
