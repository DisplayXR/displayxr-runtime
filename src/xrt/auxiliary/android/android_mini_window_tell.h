// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  THE definition of the "this window is being scaled by its container"
 *         tell (#1367 / #1396 / #1401).
 * @author David Fattal
 * @ingroup aux_android
 *
 * ONE definition, deliberately in a header with no Android dependency at all, so
 * that every C consumer includes the same four comparisons and a host test can
 * pin it — see `tests/tests_aux_mini_window_tell.cpp`.
 *
 * There is a fourth copy that this header cannot reach: `MiniWindowLayout.isTell`
 * in the aux AAR, which has to answer BEFORE any C sees the rect. That copy is
 * pinned by the same host test, which parses the Java expression out of the
 * source and evaluates it against the same case table.
 *
 * NOT `#ifdef XRT_OS_ANDROID`: the rule is pure integer arithmetic and it is
 * worth strictly more when it is testable on the host than when it is compiled
 * only for the one platform it runs on.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * "This window is being scaled by its container."
 *
 * An OEM "window reply" container does not hand the task a smaller window — it
 * hands it a FULL-SIZE logical window and scales the whole task with a
 * SurfaceFlinger leash. So the window reports its LOGICAL extent at its PHYSICAL
 * origin and, being bigger than it looks, spills off the panel. Measured on the
 * NP02J: rect 1757,236 1080x1685 on a 2560x1600 panel — 1757+1080 = 2837 > 2560
 * and 236+1685 = 1921 > 1600.
 *
 * Mirrors the browser's heuristic exactly, INCLUDING its lack of slack. A window
 * merely dragged off-panel trips this too; that false positive costs 2D content,
 * never a broken weave, which is the direction to fail in.
 *
 * Callers must not call this with a zero panel extent and treat the answer as
 * "not scaled" — there is no panel to compare against, and every caller's rule is
 * "never decide on ignorance, keep the state you are in". Hence the explicit
 * `> 0` guards: a missing extent answers false, and the caller checks for it
 * separately before deciding anything.
 *
 * @param x,y      Window origin, in PANEL pixels.
 * @param w,h      Window extent, in LOGICAL pixels (that is the whole point).
 * @param disp_w,disp_h Panel extent in the SAME rotation as the rect.
 *
 * @ingroup aux_android
 */
static inline bool
android_mini_window_is_tell(int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t disp_w, uint32_t disp_h)
{
	if (disp_w == 0 || disp_h == 0 || w == 0 || h == 0) {
		return false;
	}
	return x < 0 || y < 0 || (int64_t)x + (int64_t)w > (int64_t)disp_w || (int64_t)y + (int64_t)h > (int64_t)disp_h;
}

#ifdef __cplusplus
}
#endif
