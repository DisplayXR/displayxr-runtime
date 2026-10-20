// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Per-screen display processors for a service-side XR_DXR_weave
 *         present-owner on desktop Linux and macOS (#2218 item 6): the screens and the
 *         window placed in ONE pixel frame, so the segment table
 *         (util/comp_segments.h) can be cut for them.
 * @ingroup comp_util
 *
 * ## Why a projection is needed
 *
 * The in-process Vulkan compositor segments a window in X11 root coordinates,
 * which is where both its window and the screen list's desktop rects live. A
 * present-owner on the Linux service publishes its window in the WEAVE
 * GEOMETRY frame instead (XrWeaveWindowGeometryDXR: device px, the window's
 * monitor's logical origin x that monitor's factor — XR_DXR_weave §5d), and on
 * a mixed-scale Wayland desktop that frame is not one global grid: the laptop's
 * and the panel's device rects can overlap in it. So the window cannot simply
 * be intersected with the screens' desktop rects.
 *
 * ## The frame
 *
 * Everything is placed in the WINDOW's own pixel frame — window px are device
 * px of the window's buffer, so a segment's window rect is exactly the part of
 * the woven output it covers:
 *
 *  - LOGICAL (the present-owner chained the v12 XrWeaveWindowLogicalOriginDXR):
 *    each screen's stage (Mutter logical) rect is derived with the same rule
 *    the single-panel phase uses (u_weave_present_origin.h, rule 2: the device
 *    origin of rule 1 / the screen's stage factor) and scaled by the WINDOW's
 *    factor, anchored on the window so that `window - screen` is exactly the
 *    rule-2 present origin for that screen. A screen whose own factor equals
 *    the window's shows the window 1:1 (its native size equals its rect in
 *    this frame); any other screen resamples the window (native != rect), and
 *    the segment policy (comp_segments_decide) gives it flat 2D unless its DP
 *    tolerates a resample — the refuse-rather-than-resample rule every other
 *    segmenting compositor applies.
 *  - DEVICE (no logical origin): only when every screen's X11 rect already IS
 *    its device rect (pure X11, or Mutter's PHYSICAL layout) — the screens'
 *    desktop rects are then used verbatim. Any converted screen makes the
 *    frame ambiguous and the projection refuses (NONE): the window keeps the
 *    single-DP weave it had before.
 *
 * ## macOS (the CG frame)
 *
 * The macOS service's present-owner reports its window in global CoreGraphics
 * space (top-left of the main display, y DOWN — AppKit's y-up frame flipped
 * by the caller, XR_DXR_weave §5d spec v7) in BACKING px of the display the
 * window is on: points x that display's backing scale. The screen list holds
 * each display's CGDisplayBounds in POINTS plus its backing scale and native
 * mode (target_screens, the in-process Metal compositor's convention). So the
 * whole desktop is placed at the WINDOW's backing scale S
 * (@ref comp_weave_screens_project_cg): a screen's rect in the window's pixel
 * frame is its point rect x S (edges rounded independently, so neighbours
 * share their seam), the window rect is the reported one verbatim, and
 * `window - screen` is then exactly the single-panel present origin the
 * engine already feeds a DP (window origin minus the panel's
 * `lround(points x scale)` origin, comp_multi_weave_macos.c) on every screen
 * whose own scale is S. A screen at another scale — or whose points x scale
 * is not its native mode ("looks like" scaled modes) — resamples the window:
 * flat 2D unless its DP tolerates it, the same policy as everywhere else.
 * Displays left of / above the main display have negative point origins and
 * stay negative here.
 *
 * Pure arithmetic, no platform guard, no Vulkan; pinned by
 * tests_comp_weave_screens.
 */

#pragma once

#include "xrt/xrt_screen.h"
#include "util/comp_segments.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct xrt_dp_factory_registry;

/*!
 * The present-owner's window as the bind published it.
 */
struct comp_weave_window
{
	//! XrWeaveWindowGeometryDXR::windowOriginOnScreen (weave geometry frame, device px).
	int32_t x, y;
	//! Client size, device px.
	uint32_t w, h;
	//! The bind chained an XrWeaveWindowLogicalOriginDXR (v12).
	bool logical_valid;
	//! Its logical (stage) origin and the stage -> device factor, verbatim.
	int32_t logical_x, logical_y;
	float logical_scale;
};

//! Which rule placed the screens (see the file comment).
enum comp_weave_frame
{
	//! The window cannot be placed against the screens: no segmentation.
	COMP_WEAVE_FRAME_NONE = 0,
	//! From the v12 logical origin.
	COMP_WEAVE_FRAME_LOGICAL = 1,
	//! The screens' desktop rects are device rects (pure X11 / PHYSICAL layout).
	COMP_WEAVE_FRAME_DEVICE = 2,
	//! macOS: CoreGraphics points x the window's backing scale.
	COMP_WEAVE_FRAME_CG = 3,
};

//! A short word for @p f ("none", "logical", "device", "CG").
const char *
comp_weave_frame_str(enum comp_weave_frame f);

/*!
 * Place @p in's screens and the window in one pixel frame.
 *
 * @p out is @p in with ONLY the segment-math fields rewritten — `desktop_left /
 * top / width / height` (the screen's rect in the window's frame) and
 * `native_width / height` (what makes comp_segments_screen_1to1 answer "does
 * this screen show the window's pixels 1:1"). Ids, flags, plug-in ids and
 * order are kept, and a screen without a desktop rect keeps a zero one, so a
 * segment manager built from @p in keeps its screen indices when handed @p out.
 *
 * @param[out] out         The projected list (always written; a copy of @p in
 *                         when the result is NONE).
 * @param[out] out_window  The window's client area in that frame.
 * @return the rule used; NONE = do not segment.
 */
enum comp_weave_frame
comp_weave_screens_project(const struct xrt_screen_list *in,
                           const struct comp_weave_window *win,
                           struct xrt_screen_list *out,
                           struct comp_seg_rect *out_window);

/*!
 * The macOS present-owner's window as the bind published it (spec v7
 * XrWeaveWindowGeometryDXR on macOS).
 */
struct comp_weave_window_cg
{
	//! windowOriginOnScreen: global CoreGraphics space (y down), backing px of
	//! the window's display (its points x @ref scale).
	int32_t x, y;
	//! Client size, backing px.
	uint32_t w, h;
	//! The window's backing scale: the scale of the display its origin was
	//! resolved on (the reported displayId, else the containing display).
	float scale;
};

/*!
 * macOS: place @p in's screens (CGDisplayBounds points + backing scale) and the
 * window in the window's own backing-px frame (see "macOS" in the file
 * comment). Same output contract as @ref comp_weave_screens_project: only the
 * segment-math fields of @p out are rewritten, so a manager built from @p in
 * keeps its screen indices.
 *
 * @return COMP_WEAVE_FRAME_CG, or COMP_WEAVE_FRAME_NONE for a degenerate window
 *         or scale (then @p out is a copy of @p in: do not segment).
 */
enum comp_weave_frame
comp_weave_screens_project_cg(const struct xrt_screen_list *in,
                              const struct comp_weave_window_cg *win,
                              struct xrt_screen_list *out,
                              struct comp_seg_rect *out_window);

/*!
 * A screen's backing scale as the CG projection reads it: the enumerated
 * desktop scale, else native width / point width, else 1.
 */
float
comp_weave_screens_cg_scale(const struct xrt_screen *s);

//! How two screen lists differ (@ref comp_weave_screens_compare).
enum comp_weave_screens_change
{
	//! Nothing the segment manager reads changed.
	COMP_WEAVE_SCREENS_SAME = 0,
	//! Same screens, same order, same claimants: only rects / device modes
	//! moved. The manager keeps its DPs and takes the new geometry.
	COMP_WEAVE_SCREENS_GEOMETRY = 1,
	//! A screen joined or left, the order changed, or a screen's claimant,
	//! claim level or role flags changed: the manager is rebuilt.
	COMP_WEAVE_SCREENS_MEMBERSHIP = 2,
	//! Nothing but a screen's EDID physical size moved (macOS: CoreGraphics'
	//! size until IOKit publishes the EDID blob ~0.5-1 s after a plug-in,
	//! #2097). Not a new screen table — rebuilding a vendor weaver (and
	//! re-acquiring its eye tracker) for it is wrong, the rule
	//! comp_segments_dp_key_keeps and the hotplug sweep's generation apply —
	//! but a holder should keep the refined list.
	COMP_WEAVE_SCREENS_PHYSICAL_SIZE = 3,
};

/*!
 * Compare two lists. NULL is the empty list. MEMBERSHIP wins over GEOMETRY,
 * GEOMETRY over PHYSICAL_SIZE.
 */
enum comp_weave_screens_change
comp_weave_screens_compare(const struct xrt_screen_list *a, const struct xrt_screen_list *b);

/*!
 * Did the DP registry change in a way that matters to a segment manager — a
 * screen's claimant, claim level, serial, readiness or Vulkan factory? NULL is
 * the empty registry.
 */
bool
comp_weave_screens_registry_changed(const struct xrt_dp_factory_registry *a, const struct xrt_dp_factory_registry *b);

/*!
 * Who weaves one segment — for the log line and the tests.
 */
struct comp_weave_segment_owner
{
	uint64_t screen_id;
	//! The screen's claimant ("" = none: flat 2D).
	char plugin_id[XRT_SCREEN_NAME_MAX];
	//! On the system-default screen: the present-owner's own weave DP (the
	//! active plug-in's), which also feeds the base eyes.
	bool session_dp;
	//! The window's present origin relative to that screen (the DP phase slot).
	int32_t present_origin_x, present_origin_y;
	//! The segment's rect in window px.
	struct comp_seg_rect window_rect;
	//! The screen shows the window 1:1 (window px are its device px).
	bool one_to_one;
};

/*!
 * Name the display processor of every segment of @p t: the claimant of its
 * screen in @p list (the per-screen registry winner, per-screen preference
 * included). @p t must have been cut from @p list's projection.
 *
 * @return the number written (<= cap, <= t->count).
 */
uint32_t
comp_weave_screens_owners(const struct comp_segment_table *t,
                          const struct xrt_screen_list *list,
                          struct comp_weave_segment_owner *out,
                          uint32_t cap);

/*!
 * The screen whose DP's eyes the submit reply should carry as the base eyes
 * while the window is woven per screen and the caller is not told the
 * per-screen table (desktop Linux reports XR_DXR_weave v19 segmentCount 0):
 * the majority segment's screen (comp_segments_majority — the same screen the
 * drag-snap and window-handle rules follow). 0 when @p t is empty.
 *
 * @param[out] out_is_primary  (may be NULL) that screen is the system default.
 */
uint64_t
comp_weave_screens_eye_screen(const struct comp_segment_table *t, bool *out_is_primary);

#ifdef __cplusplus
}
#endif
