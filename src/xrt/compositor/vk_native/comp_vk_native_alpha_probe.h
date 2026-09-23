// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Per-frame "is the woven content transparent?" probe for the Vulkan
 *         compositor — the measurement half of lazy transparency.
 *
 * A session declares transparency CAPABILITY once, at xrCreateSession (the
 * window visual and the swapchain alpha mode are fixed then). Whether the
 * content is actually transparent is a per-frame fact that only the pixels
 * know: an app can start opaque and flip to a transparent background later
 * (a Ctrl+T toggle) without telling the runtime anything — no layer flag, no
 * blend-mode change. This probe reads that fact off the atlas the display
 * processor is about to weave, so the runtime can tell the DP when
 * transparency is actually in use
 * (@ref xrt_display_processor_vk::set_transparency_active).
 *
 * Cost: one compute dispatch that samples one texel per `stride`x`stride`
 * block and does one atomic per transparent 8x8-sample block. At the default
 * stride of 4 that is 1/16 of the atlas' texels and roughly a quarter of its
 * rows' cache lines.
 *
 * Readback is one frame late and never blocks: the result for frame N is read
 * at frame N+1, after the compositor's own per-frame fence wait. A read that
 * races an unfinished frame (the #1264 fence-park) returns a stale count,
 * which the caller's hysteresis absorbs.
 *
 * @ingroup comp_vk_native
 */

#pragma once

#include "vk/vk_helpers.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct comp_vk_alpha_probe;

/*!
 * One probe verdict.
 */
struct comp_vk_alpha_probe_result
{
	uint32_t transparent_blocks; //!< sample blocks containing any alpha < threshold
	uint32_t total_blocks;       //!< sample blocks dispatched
};

/*!
 * Create the probe (pipeline, readback ring). NULL on any failure — the caller
 * then keeps the display processor's transparency permanently active, which is
 * exactly the behaviour before this probe existed.
 */
struct comp_vk_alpha_probe *
comp_vk_alpha_probe_create(struct vk_bundle *vk);

void
comp_vk_alpha_probe_destroy(struct comp_vk_alpha_probe **probe_ptr);

/*!
 * Read the verdict of the most recently RECORDED probe, if one exists and has
 * not been read yet. Call before @ref comp_vk_alpha_probe_record for the new
 * frame. Returns false when there is nothing new to read.
 */
bool
comp_vk_alpha_probe_read(struct comp_vk_alpha_probe *p, struct comp_vk_alpha_probe_result *out);

/*!
 * Record the probe over @p view (a 2D view of the atlas, already in
 * SHADER_READ_ONLY_OPTIMAL — the layout the DP samples it in) into @p cmd.
 * Only the top-left @p width x @p height pixels are sampled. Adds its own
 * barriers: earlier writes → compute read, compute write → host read.
 */
void
comp_vk_alpha_probe_record(
    struct comp_vk_alpha_probe *p, VkCommandBuffer cmd, VkImageView view, uint32_t width, uint32_t height);

#ifdef __cplusplus
}
#endif
