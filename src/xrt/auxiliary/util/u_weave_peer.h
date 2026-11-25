// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  A present-owner's window as seen from its other sessions (ADR-047
 *         Amendment 10 over IPC; the Amendment 4 addendum on desktop Linux).
 * @ingroup aux_util
 *
 * The DisplayXR Browser talks to the service from two sessions: it LOCATES and
 * ENUMERATES from one (its browser process) and WEAVES from another (its GPU
 * process), both bound to the same top-level window. Only the weaving session's
 * engine knows which screens the window covers. These rules let the service
 * answer the other session from the weaving one:
 *  - @ref u_weave_window_key_same: are two sessions bound to the same window;
 *  - @ref u_weave_pick_effective_table: the rendering-mode table a session's
 *    app is told (ADR-047 Amendment 10): its own engine's, else the table of a
 *    session weaving the same window;
 *  - @ref u_weave_locate_reframe: the screen a locate is framed against — a
 *    window (or an inline element) wholly on a non-primary screen takes THAT
 *    screen's eyes and geometry, never the session (3D panel) DP's camera.
 *
 * Header-only and backend-agnostic, so it is host-tested
 * (`tests/tests_weave_peer.cpp`).
 */

#pragma once

#include "xrt/xrt_defines.h"
#include "xrt/xrt_display_metrics.h"

#include "util/u_weave_rect_parts.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * The window a session is bound to, as its bind / geometry calls described it.
 */
struct u_weave_window_key
{
	bool valid;         //!< the session has published a window rect
	uint64_t window_id; //!< X11 XID / HWND / CGWindowID; 0 = unnamed (native Wayland)
	int32_t x, y;       //!< client area, desktop px
	uint32_t w, h;
};

/*!
 * Are @p a and @p b the same window? Both named: by id. Otherwise (a native
 * Wayland client cannot name its window to another process) by an identical,
 * non-empty client rect.
 */
static inline bool
u_weave_window_key_same(const struct u_weave_window_key *a, const struct u_weave_window_key *b)
{
	if (a == NULL || b == NULL || !a->valid || !b->valid) {
		return false;
	}
	if (a->window_id != 0 && b->window_id != 0) {
		return a->window_id == b->window_id;
	}
	return a->w > 0 && a->h > 0 && a->x == b->x && a->y == b->y && a->w == b->w && a->h == b->h;
}

/*!
 * One session's state for @ref u_weave_pick_effective_table.
 */
struct u_weave_table_source
{
	struct u_weave_window_key key;
	//! The table its own engine computed (invalid: it weaves nothing, or its
	//! window is on the session screen).
	struct xrt_effective_mode_table table;
	//! It weaves (a present-owner with a placed window): its table is
	//! authoritative for its window even while invalid.
	bool weaves;
};

/*!
 * The table session @p self's app is told: its own engine's when it weaves;
 * else that of the first session in @p peers weaving the same window (the
 * browser's enumerate follows its GPU process's window); else invalid (the
 * head device's table stands). Never another window's table.
 *
 * @return the index into @p peers that supplied it, -1 for self or none.
 */
static inline int32_t
u_weave_pick_effective_table(const struct u_weave_table_source *self,
                             const struct u_weave_table_source *peers,
                             uint32_t peer_count,
                             struct xrt_effective_mode_table *out)
{
	memset(out, 0, sizeof(*out));
	if (self == NULL) {
		return -1;
	}
	if (self->weaves) {
		*out = self->table;
		return -1;
	}
	for (uint32_t i = 0; peers != NULL && i < peer_count; i++) {
		if (!peers[i].weaves || !u_weave_window_key_same(&self->key, &peers[i].key)) {
			continue;
		}
		*out = peers[i].table;
		return (int32_t)i;
	}
	return -1;
}

/*!
 * The result of @ref u_weave_locate_reframe.
 */
struct u_weave_reframe
{
	uint32_t segment_index;        //!< into the table
	uint64_t screen_id;            //!< the framing screen
	struct xrt_window_metrics wm;  //!< the WHOLE window relative to that screen
	struct xrt_eye_positions eyes; //!< that screen's eyes (its own DP's, nominal when it does not track)
	float nominal_viewer_z_m;      //!< that screen's nominal viewer distance (0 = unknown)
	float pitch_x_m, pitch_y_m;    //!< that screen's metres per desktop pixel
};

/*!
 * Should a locate of a present-owner's window be framed against a NON-primary
 * screen (the ADR-047 Amendment 4 addendum, and the bug class runtime #2757
 * fixed in-process)? @p m is the per-screen table of the window's last weave
 * (with each screen's eyes); @p zone is the inline element (window px), or NULL
 * for a whole-window locate (then the whole window must be on that screen);
 * @p live_w x @p live_h is the window the locating session sees.
 *
 * Yes only when the element (or the window) lies wholly on one non-primary
 * screen whose geometry is known and whose DP reported eyes. Then @p out holds
 * that screen's eyes and the window relative to it, and the caller must use
 * them INSTEAD of the session DP's (the 3D panel's tracked camera). A
 * straddling element and anything on the primary keep the session framing.
 */
static inline bool
u_weave_locate_reframe(const struct xrt_segment_metrics *m,
                       const struct xrt_rect *zone,
                       uint32_t live_w,
                       uint32_t live_h,
                       struct u_weave_reframe *out)
{
	memset(out, 0, sizeof(*out));
	if (m == NULL || live_w == 0 || live_h == 0) {
		return false;
	}
	struct xrt_rect whole;
	whole.offset.w = 0; // xrt_offset names its fields w/h
	whole.offset.h = 0;
	whole.extent.w = (int32_t)live_w;
	whole.extent.h = (int32_t)live_h;
	const struct xrt_rect *r = zone != NULL ? zone : &whole;
	const int32_t k = u_weave_zone_screen_segment(m, r, live_w, live_h);
	if (k < 0) {
		return false;
	}
	const struct xrt_segment_metric *g = &m->seg[k];
	if (!g->have_eyes || !g->eyes.valid || g->eyes.count == 0) {
		return false; // that screen has no DP to see it: flat 2D anyway
	}
	if (!u_weave_segment_window_metrics(m, (uint32_t)k, &out->wm)) {
		return false;
	}
	out->segment_index = (uint32_t)k;
	out->screen_id = g->screen_id;
	out->eyes = g->eyes;
	out->nominal_viewer_z_m = g->nominal_viewer_z_m;
	out->pitch_x_m = g->screen_width_m / (float)g->screen_desktop_width;
	out->pitch_y_m = g->screen_height_m / (float)g->screen_desktop_height;
	return true;
}

#ifdef __cplusplus
}
#endif
