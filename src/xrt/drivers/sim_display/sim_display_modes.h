// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_display's rendering-mode table, kept apart from the device so a
 *         test can pin it host-side (the plug-in compiles the same file).
 * @ingroup drv_sim_display
 *
 * Every platform's swapchain is sized to the worst case across all registered
 * modes (ADR-010, u_tiling_compute_system_atlas_oriented), and the zero-copy
 * gate (u_tiling_can_zero_copy) needs the 2D mode to fill that worst case. The
 * seven modes below are not equal in that respect: mode 6, full side by side
 * (two views at scale 1.0 x 1.0, 2x1 tiles), is twice the panel's width, so
 * registering it makes every app's swapchain twice as wide on every platform,
 * whether or not anything ever uses the mode. It is therefore registered only
 * where a stereo desktop compositor can consume it (desktop Linux) and only
 * when SIM_DISPLAY_OUTPUT=sbs_full asks for it; everywhere else the list, and
 * the worst-case atlas, are the six modes of before.
 */

#pragma once

#include "xrt/xrt_device.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Number of modes without full side by side (indices 0 to 5).
#define SIM_DISPLAY_BASE_MODE_COUNT 6u

//! Index of the full side by side mode, present only when asked for.
#define SIM_DISPLAY_FULL_SBS_MODE_INDEX 6u

/*!
 * Whether full side by side can be registered on this platform: desktop Linux
 * only, where a stereo desktop compositor can take the declaration.
 */
bool
sim_display_full_sbs_available(void);

/*!
 * Fill @p modes (at least SIM_DISPLAY_BASE_MODE_COUNT + 1 entries, zeroed by
 * the caller) with sim_display's modes and return their number: 6, or 7 when
 * @p want_full_sbs is true and sim_display_full_sbs_available() says the
 * platform can take it. Order: 0 = 2D, 1 = Anaglyph (default 3D), 2 = Cropped
 * SBS, 3 = Squeezed SBS, 4 = Quad, 5 = Interlaced (#817), 6 = Full SBS.
 */
uint32_t
sim_display_fill_rendering_modes(struct xrt_rendering_mode *modes, bool want_full_sbs);

#ifdef __cplusplus
}
#endif
