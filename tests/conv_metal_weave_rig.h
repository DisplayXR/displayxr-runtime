// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  A headless Metal device for tests_conv_vk_gpu's Metal weave-rect
 *         cases (comp_multi_weave_metal_lift): textures in / out, one weave
 *         command buffer recorded, committed and waited.
 * @ingroup tests
 */

#pragma once

#include "multi/comp_multi_weave_metal_lift.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct cmw_rig;

//! NULL when the box has no Metal device.
struct cmw_rig *
cmw_rig_create(void);

void
cmw_rig_destroy(struct cmw_rig *rig);

//! id<MTLDevice>, borrowed.
void *
cmw_rig_device(struct cmw_rig *rig);

//! A @p w x @p h BGRA8 (or RGBA8) texture holding @p bytes (tight rows; NULL = zeros), +1.
void *
cmw_rig_texture(struct cmw_rig *rig, uint32_t w, uint32_t h, bool bgra, const void *bytes);

void
cmw_rig_release(void *obj);

//! Read @p tex back (tight rows, its own channel order).
bool
cmw_rig_read(struct cmw_rig *rig, void *tex, void *bytes);

/*!
 * One weave: record the lifted rects into a fresh command buffer, commit, wait,
 * then comp_multi_weave_metal_lift_frame_done. Returns the lifted count, or -1
 * when the command buffer failed.
 */
int
cmw_rig_weave(struct cmw_rig *rig,
              struct comp_multi_weave_metal_lift *ml,
              const struct comp_multi_weave_metal_lift_planner *pl,
              void *src,
              void *dst,
              const struct vk_conv_weave_rect *rects,
              uint32_t count);

#ifdef __cplusplus
}
#endif
