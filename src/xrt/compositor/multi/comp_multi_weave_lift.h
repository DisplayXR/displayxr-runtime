// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Lifted weave rects (XR_DXR_lift §6) on the macOS and desktop-Linux
 *         Vulkan weave engines: comp_multi's side.
 * @ingroup comp_multi
 *
 * The GPU work lives in vk_conv/vk_conv_weave.c, which comp_multi does NOT
 * link (the runtime client links comp_multi too, and must not pull the
 * conversion core): the IPC layer (ipc_server_conv_vk.c) installs its ops
 * table here once the conversion core's Vulkan glue is attached, exactly as
 * the D3D11 service is handed its bridge. Until then, and on a service
 * without the glue, every lifted rect is woven as drawn.
 *
 * Per engine (struct comp_multi_weave_lift, inside mc->weave):
 *
 *  - comp_multi_weave_lift_set_rects (IPC lift_weave_rects) latches the set
 *    for exactly the next submit;
 *  - the engine takes it at the top of its submit, calls _begin before it
 *    records, _record per lifted rect while it records, and _complete once the
 *    weave's fence signalled (or _begin of the next weave drops what was never
 *    submitted);
 *  - _fini when the engine goes (its GPU work idle).
 *
 * Plus the conversion core's per-DP eyes sink (roadmap §5.2): every eye query
 * comp_multi answers publishes the answering DP's eyes keyed by that DP; a
 * destroyed DP retires its key. The lifted rect's frame names the weave
 * engine's DP, so its stream reads that DP's eyes.
 */

#pragma once

#include "xrt/xrt_compositor.h"
#include "xrt/xrt_lift.h"
#include "xrt/xrt_vulkan_includes.h"

#include "vk_conv/vk_conv_weave.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct multi_compositor;
struct vk_bundle;
struct xrt_eye_positions;

//! One weave engine's lifted-rect state.
struct comp_multi_weave_lift
{
	//! @name The next submit's set (lift_weave_rects)
	//! @{
	uint64_t owner;
	uint32_t count;
	struct xrt_lift_weave_rect rects[XRT_LIFT_WEAVE_RECTS_MAX];
	//! @}

	//! The helper's per-engine state, valid while client_gen == the ops generation.
	struct vk_conv_weave_client *client;
	uint64_t client_gen;
	//! A weave with lifted rects was recorded and not yet completed / dropped.
	bool in_flight;
};

/*!
 * Install (NULL: clear) the helper's ops, process-wide. Clearing waits for
 * calls in flight; clients created under the old table are the helper's to
 * free (vk_conv_weave_destroy) and are dropped here, never called again.
 */
void
comp_multi_weave_set_lift_ops(const struct vk_conv_weave_ops *ops);

/*!
 * The conversion core's per-DP eyes sink (comp_d3d11_service_set_conv_eyes_sink's
 * twin): @p publish runs for every eye query comp_multi answers, keyed by the
 * answering DP's address; @p retire when that DP is destroyed. NULL clears.
 */
void
comp_multi_set_conv_eyes_sink(void (*publish)(void *ud, uint64_t dp_key, const struct xrt_eye_positions *eyes),
                              void (*retire)(void *ud, uint64_t dp_key),
                              void *ud);

/*!
 * IPC lift_weave_rects: latch @p count rects for @p xc's next weave submit.
 * False (refused, non-fatal: the submit weaves them as drawn) when no helper
 * is installed or a rect names a stream that is not an SBS / NVIEW stream of
 * @p owner -- the D3D11 service's rule. count 0 clears.
 */
bool
comp_multi_weave_lift_set_rects(struct xrt_compositor *xc,
                                uint64_t owner,
                                uint32_t count,
                                const struct xrt_lift_weave_rect *rects);

//! @name The engines' side (comp_multi_weave_linux.c, comp_multi_weave_macos_vk.c)
//! @{

/*!
 * Take this submit's set (a set is valid for exactly one submit): copies it to
 * @p out and clears the latch. Returns the count.
 */
uint32_t
comp_multi_weave_lift_take(struct comp_multi_weave_lift *l,
                           uint64_t *out_owner,
                           struct xrt_lift_weave_rect out[XRT_LIFT_WEAVE_RECTS_MAX]);

/*!
 * A weave with @p count lifted rects starts recording on @p vk. Drops what an
 * earlier weave left (never submitted). False = lift unavailable this submit
 * (no helper, another device): weave the rects as drawn.
 */
bool
comp_multi_weave_lift_begin(struct comp_multi_weave_lift *l, struct vk_bundle *vk, uint32_t count);

//! Record one rect (after a true _begin). False = weave it as drawn.
bool
comp_multi_weave_lift_record(struct comp_multi_weave_lift *l, VkCommandBuffer cmd, const struct vk_conv_weave_rect *r);

//! The weave's fence signalled: everything recorded since _begin is done.
void
comp_multi_weave_lift_complete(struct comp_multi_weave_lift *l);

//! The engine goes (its GPU work idle).
void
comp_multi_weave_lift_fini(struct comp_multi_weave_lift *l);

/*!
 * ADR-048: rect @p rect (window pixels of a @p win_w x @p win_h window; NULL =
 * the whole window) of @p mc's window in display space, for DP @p dp_key. No
 * window metrics: the panel, valid = false.
 */
void
comp_multi_weave_lift_rect_frame(struct multi_compositor *mc,
                                 const struct xrt_rect *rect,
                                 uint32_t win_w,
                                 uint32_t win_h,
                                 uint64_t dp_key,
                                 struct xrt_lift_rect_frame *out);

//! Publish @p eyes for DP @p dp_key into the eyes sink (no-op without one).
void
comp_multi_conv_eyes_publish(uint64_t dp_key, const struct xrt_eye_positions *eyes);

//! DP @p dp_key is gone.
void
comp_multi_conv_eyes_retire(uint64_t dp_key);
//! @}

#ifdef __cplusplus
}
#endif
