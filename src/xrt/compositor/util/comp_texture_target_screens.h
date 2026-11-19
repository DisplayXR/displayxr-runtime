// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Texture target binding (ADR-060): classify a resolved target against
 *         the session screen and the system's screen list, for a tier that
 *         weaves a texture target with ONE display processor.
 *
 * The data half of the D3D12 tier's d3d12_tt_coverage (#2382), shared by the
 * Vulkan and GL tiers (#2721): the panel is the DP's registry entry, the
 * others are every listed screen that is not the panel (by id) and does not
 * overlap it (the panel under another id — a synthesized default screen, a
 * mirrored output — is not "another screen"). A session pinned to a display
 * is never judged, as on the D3D11 tier, where a pin turns segmentation off.
 * The predicate itself is comp_texture_target_panel_coverage.
 *
 * Header-only, C-compatible, logs nothing.
 *
 * @ingroup comp_util
 */

#pragma once

#include "xrt/xrt_compositor.h"
#include "xrt/xrt_screen.h"

#include "util/comp_texture_target.h"

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * @param r          This frame's resolve.
 * @param e          The DP's screen (`const struct xrt_dp_registry_entry *`), or NULL.
 * @param screens    The system's screen list, or NULL (never handed over).
 * @param pinned_id  The session's pinned display id, 0 = not pinned.
 */
static inline enum comp_texture_target_coverage
comp_texture_target_coverage_for_entry(const struct comp_texture_target_resolved *r,
                                       const struct xrt_dp_registry_entry *e,
                                       const struct xrt_screen_list *screens,
                                       uint64_t pinned_id)
{
	if (e == NULL || screens == NULL || pinned_id != 0) {
		return COMP_TEXTURE_TARGET_COVERAGE_ON_PANEL;
	}
	const struct comp_seg_rect panel = {e->screen_left, e->screen_top, e->pixel_width, e->pixel_height};
	struct comp_seg_rect others[XRT_SCREEN_LIST_MAX];
	uint32_t n = 0;
	for (uint32_t i = 0; i < screens->count && i < XRT_SCREEN_LIST_MAX; i++) {
		const struct xrt_screen *s = &screens->screens[i];
		if (s->id == e->monitor_id || s->desktop_width == 0 || s->desktop_height == 0) {
			continue;
		}
		const struct comp_seg_rect sr = {s->desktop_left, s->desktop_top, s->desktop_width, s->desktop_height};
		if (comp_texture_target_rects_overlap(&sr, &panel)) {
			continue;
		}
		others[n++] = sr;
	}
	return comp_texture_target_panel_coverage(r, &panel, others, n);
}

//! The one-line wording of a coverage, for the caller's on-change WARN.
static inline const char *
comp_texture_target_coverage_str(enum comp_texture_target_coverage cov)
{
	switch (cov) {
	case COMP_TEXTURE_TARGET_COVERAGE_ON_PANEL: return "on the 3D screen — woven";
	case COMP_TEXTURE_TARGET_COVERAGE_SPANNING:
		return "spanning the 3D screen and another — filled flat (this tier weaves one screen)";
	case COMP_TEXTURE_TARGET_COVERAGE_OFF_PANEL: return "wholly on another screen — filled flat";
	default: return "?";
	}
}

#ifdef __cplusplus
}
#endif
