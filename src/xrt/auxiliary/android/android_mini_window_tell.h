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

/*!
 * Does the window's EXTENT fit inside the panel, ignoring where it sits?
 *
 * The discriminator between the two very different situations that @ref
 * android_mini_window_is_tell lumps together (#1424).
 *
 * A container-scaled window reports its LOGICAL extent, which is 1/scale larger
 * than the space it actually occupies — on the NP02J 1080x1685 against a
 * 2560x1600 panel, so the height alone cannot fit however the window is placed.
 * A window that has already had the 1:1 layout applied reports its PHYSICAL
 * extent (723x1129), which fits the panel comfortably; if such a rect spills, it
 * is because of WHERE it is, not HOW BIG it is.
 *
 * @ingroup aux_android
 */
static inline bool
android_mini_window_extent_fits(uint32_t w, uint32_t h, uint32_t disp_w, uint32_t disp_h)
{
	if (disp_w == 0 || disp_h == 0 || w == 0 || h == 0) {
		return false;
	}
	return w <= disp_w && h <= disp_h;
}

/*!
 * "This window is being scaled by its container" — the DEGRADE decision (#1424).
 *
 * @ref android_mini_window_is_tell is the raw geometric spill and stays exactly
 * as it is: it is the signal the 1:1 path arms on, and it is pinned against the
 * Java copy. But it is NOT on its own a reason to stop weaving, because it
 * cannot separate:
 *
 *   - a genuinely scaled container — logical extent at a physical origin, which
 *     the vendor interlacer cannot weave 1:1 at all, so 2D is the honest answer;
 *   - an already-physical, already-1:1 rect that merely hangs off the panel
 *     edge — measured on the NP02J when the OEM's drop-zone gesture places the
 *     freeform window at (2137,84): 2137+723 = 2860 > 2560, while the extent
 *     723x1129 fits the panel with room to spare. Weaving is still correct for
 *     the on-panel portion, the phase origin is unchanged, and SurfaceFlinger
 *     clips the remainder. Degrading there left the app stuck in flat 2D until
 *     the user happened to move the window (#1424 run B: 30 s, no recovery).

 *
 * ## The blind spot — read this before trusting the rule on a new device
 *
 * "Extent does not fit" is a SUFFICIENT signal that a window is scaled, not a
 * necessary one. A scaled container whose LOGICAL extent still fits the panel is
 * indistinguishable, by rect alone, from a window merely placed off-panel — and
 * this rule then calls it a placement and keeps weaving, at the wrong scale,
 * until the 1:1 hint lands.
 *
 * On the measured device the margin is thin. Panel 2560x1600, container scale
 * 0.67: the logical extent stops fitting only once the physical extent exceeds
 * ~1715x1072. The NP02J mini-window is 723x1129 and is caught solely on its
 * HEIGHT — 1685 logical against 1600, i.e. 85 logical px of margin. A smaller
 * mini-window, a shallower scale, or a panel with more headroom lands in the
 * blind spot, and the failure there is a wrong-scale weave for the pre-hint
 * transient rather than the honest 2D that shipped before.
 *
 * The rect cannot resolve this; the SCALE can. `MiniWindowLayout` already
 * measures it (`ratP`/`ratQ`, the vendor API, the touch ratio), so the sound
 * rule is "a scale was measured and is not 1 => scaled", with extent-fits as
 * the fallback for when no scale is known yet. That is deliberately NOT done
 * here: the scale lives in Java and reaching C would mean widening
 * `nativeWindowRectChanged` and `android_globals_set_window_screen_rect` — the
 * former is part of the frozen append-only cross-APK contract (#1408), the
 * latter is consumed by `oxr_android_surface.c` and `comp_multi_system.c` too.
 * That is a plumbing change with its own review, not a rider on this fix.
 *
 * Consequence to respect: the degrade is now correct for the shapes MEASURED on
 * the NP02J and reasoned-but-unverified elsewhere. Re-measure it on any new
 * panel or scale before relying on it.
 *
 * A spill whose extent fits is a PLACEMENT, not a scale. Same rule catches the
 * fullscreen status-bar transient — (0,60) 2560x1600 on a 2560x1600 panel spills
 * by 60 px with an extent that fits exactly — which used to produce a 2D blip on
 * every status-bar toggle.
 *
 * @ingroup aux_android
 */
static inline bool
android_mini_window_is_container_scaled(int32_t x,
                                        int32_t y,
                                        uint32_t w,
                                        uint32_t h,
                                        uint32_t disp_w,
                                        uint32_t disp_h)
{
	if (!android_mini_window_is_tell(x, y, w, h, disp_w, disp_h)) {
		return false;
	}
	return !android_mini_window_extent_fits(w, h, disp_w, disp_h);
}

#ifdef __cplusplus
}
#endif
