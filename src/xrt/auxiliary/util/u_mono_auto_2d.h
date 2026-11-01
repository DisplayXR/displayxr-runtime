// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Auto hardware 2D while a stereo session submits one flat picture
 *         for every view: the per-frame classifier and the hysteresis.
 * @ingroup aux_util
 *
 * ## Why this exists
 *
 * A stereo app sometimes submits a flat picture: a menu or a loading screen
 * where it hands the same image to both eyes. Woven through the lens that
 * picture looks soft. An app that can reach `xrRequestDisplayModeDXR`
 * (XR_DXR_display_info) switches the panel to 2D itself; a client that cannot
 * reach it (for example a compatibility-layer client whose OpenXR thunk
 * forwards only Khronos functions) still can say "this frame is flat" in pure
 * core OpenXR: by pointing every view of its projection layer at the SAME
 * swapchain, the SAME imageArrayIndex and the SAME imageRect.
 *
 * ## The rule
 *
 * - A frame is **flat** when every layer in it is a projection layer and every
 *   projection layer's views (two or more) all name the same swapchain, array
 *   index and image rect. Any other layer type (quad, cylinder, equirect,
 *   cube, window-space, Local2D, ...) makes the frame not flat: those are
 *   placed in depth by the compositor, so the frame is not one flat picture
 *   even if the projection underneath is. Conservative on purpose: a false
 *   "flat" drops the lens under real 3D content, a false "not flat" only
 *   keeps the status quo.
 * - **Enter** hardware 2D once frames have been flat continuously for
 *   @ref U_MONO_AUTO_2D_ENTER_NS. One stereo frame restarts the clock.
 * - **Leave** on the first frame that is not flat — no hysteresis on the way
 *   out, because a late return to 3D shows real 3D content flat, which is the
 *   visible failure; a late entry only leaves a flat picture slightly soft for
 *   half a second, which is the status quo.
 * - **Never while not eligible**: the caller folds the session's explicit
 *   display-mode choices, the kill switch and compositor support into
 *   `eligible`. Losing eligibility while auto-2D is engaged leaves at once.
 *
 * Pure state: the caller owns the compositor call, the event and the log.
 */

#pragma once

#include "xrt/xrt_display_metrics.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * How long a session must submit flat frames before the panel goes 2D.
 *
 * 0.5 s: long enough to ride through the short stretches an engine submits a
 * duplicated view for reasons that are not "this screen is flat" (a cut, a
 * one-frame fade, a hitch that repeats the last image), which would otherwise
 * click the lens; short enough that a menu or a loading screen goes sharp
 * about as soon as the eye has settled on it. Lens switches are not free (the
 * panel visibly re-settles), so flapping costs more than half a second of a
 * soft menu. Time-based, not frame-based, so a 20 fps loading screen and a
 * 120 fps menu wait the same wall-clock time.
 */
#define U_MONO_AUTO_2D_ENTER_NS (500ull * 1000ull * 1000ull)

//! One view of a projection layer, reduced to what decides "same picture".
struct u_mono_view_ref
{
	uint64_t swapchain; //!< opaque swapchain identity (handle value)
	uint32_t array_index;
	int32_t x, y, w, h; //!< imageRect
};

/*!
 * True when the @p count views (count >= 2) all reference the same swapchain,
 * array index and image rect. Fewer than two views is not a flat STEREO
 * submission (there is nothing duplicated), so it reads false.
 */
static inline bool
u_mono_views_identical(const struct u_mono_view_ref *v, uint32_t count)
{
	if (v == NULL || count < 2) {
		return false;
	}
	for (uint32_t i = 1; i < count; i++) {
		if (v[i].swapchain != v[0].swapchain || v[i].array_index != v[0].array_index || v[i].x != v[0].x ||
		    v[i].y != v[0].y || v[i].w != v[0].w || v[i].h != v[0].h) {
			return false;
		}
	}
	return true;
}

//! What the caller must do after a step.
enum u_mono_auto_2d_action
{
	U_MONO_AUTO_2D_NONE = 0,  //!< nothing changes
	U_MONO_AUTO_2D_ENTER = 1, //!< put the panel in hardware 2D (flat, no weave)
	U_MONO_AUTO_2D_LEAVE = 2, //!< restore the session's own hardware state
};

struct u_mono_auto_2d
{
	//! Auto-2D is engaged (the caller acted on ENTER and not yet on LEAVE).
	bool active;
	//! A run of flat frames is in progress, started at @ref flat_since_ns.
	bool flat_run;
	uint64_t flat_since_ns;
};

static inline void
u_mono_auto_2d_init(struct u_mono_auto_2d *s)
{
	s->active = false;
	s->flat_run = false;
	s->flat_since_ns = 0;
}

/*!
 * Advance one submitted frame.
 *
 * @param eligible  auto-2D may engage for this session at all (kill switch on,
 *                  no explicit app display-mode choice, compositor supports it,
 *                  the session is in a 3D mode).
 * @param flat      this frame is flat (see the rule in the file comment).
 * @param now_ns    monotonic time of the frame.
 * @param enter_ns  the entry hysteresis (normally @ref U_MONO_AUTO_2D_ENTER_NS).
 */
static inline enum u_mono_auto_2d_action
u_mono_auto_2d_step(struct u_mono_auto_2d *s, bool eligible, bool flat, uint64_t now_ns, uint64_t enter_ns)
{
	if (!eligible || !flat) {
		s->flat_run = false;
		if (s->active) {
			s->active = false;
			return U_MONO_AUTO_2D_LEAVE;
		}
		return U_MONO_AUTO_2D_NONE;
	}

	if (s->active) {
		return U_MONO_AUTO_2D_NONE;
	}
	if (!s->flat_run) {
		s->flat_run = true;
		s->flat_since_ns = now_ns;
	}
	if (now_ns - s->flat_since_ns >= enter_ns) {
		s->active = true;
		return U_MONO_AUTO_2D_ENTER;
	}
	return U_MONO_AUTO_2D_NONE;
}

/*!
 * Force auto-2D off outside a frame (an explicit app request arrived, the
 * session is ending). Returns true when it was engaged, i.e. the caller must
 * restore the session's own hardware state.
 */
static inline bool
u_mono_auto_2d_release(struct u_mono_auto_2d *s)
{
	const bool was = s->active;
	s->active = false;
	s->flat_run = false;
	return was;
}

/*!
 * Auto-2D is PRESENTATION-ONLY: while it is engaged the session's render input
 * (xrLocateViews poses, FOVs, raw eyes) must stay the per-eye 3D set the app
 * was rendering, or the app renders identical eyes, keeps submitting a flat
 * picture, and auto-2D never releases (a lock-in for any stereo app that shows
 * one flat screen).
 *
 * The hold puts the panel in hardware 2D, and a display processor in 2D may
 * report a single centred eye (count 1) or several coincident ones. This
 * restores a stereo set around that eye: the eyes' centroid, with the same
 * column pattern as the untracked nominal set (even views at -ipd/2, odd views
 * at +ipd/2 along x). A set that already covers @p view_count views with
 * distinct eyes is left untouched — byte for byte.
 *
 * @return true when @p e was rewritten.
 */
static inline bool
u_mono_auto_2d_keep_eye_set(struct xrt_eye_positions *e, uint32_t view_count, float ipd_m)
{
	const uint32_t cap = (uint32_t)(sizeof(e->eyes) / sizeof(e->eyes[0]));
	if (e == NULL || view_count < 2 || view_count > cap || e->count == 0 || e->count > cap) {
		return false;
	}
	bool distinct = false;
	for (uint32_t i = 1; i < e->count && !distinct; i++) {
		const float dx = e->eyes[i].x - e->eyes[0].x;
		const float dy = e->eyes[i].y - e->eyes[0].y;
		const float dz = e->eyes[i].z - e->eyes[0].z;
		distinct = (dx * dx + dy * dy + dz * dz) > 1e-10f; // > 10 um apart
	}
	if (e->count >= view_count && distinct) {
		return false;
	}

	struct xrt_eye_position c = {0.0f, 0.0f, 0.0f};
	for (uint32_t i = 0; i < e->count; i++) {
		c.x += e->eyes[i].x;
		c.y += e->eyes[i].y;
		c.z += e->eyes[i].z;
	}
	const float inv = 1.0f / (float)e->count;
	c.x *= inv;
	c.y *= inv;
	c.z *= inv;
	for (uint32_t i = 0; i < view_count; i++) {
		e->eyes[i].x = c.x + ((i % 2) == 0 ? -ipd_m / 2.0f : ipd_m / 2.0f);
		e->eyes[i].y = c.y;
		e->eyes[i].z = c.z;
	}
	e->count = view_count;
	return true;
}

#ifdef __cplusplus
}
#endif
