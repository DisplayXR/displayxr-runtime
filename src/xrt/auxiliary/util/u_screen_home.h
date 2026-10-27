// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  A live session's "home" screen across display hotplugs (#2305).
 * @ingroup aux_util
 *
 * The session's own display processor (its weaver) is made once, at session
 * create, for the screen that was the system default then, and it is never
 * swapped (ADR-045 D3). The in-process segment managers of the D3D11, D3D12,
 * OpenGL and Vulkan compositors read "the screen the session's own DP weaves"
 * off the SYSTEM_DEFAULT flag of the screen list they are handed — so if a
 * hotplug moved that flag (the panel unplugged and another monitor now
 * contains the panel's old origin, a second claimed panel plugged in, …), a
 * re-pushed list would make them treat ANOTHER screen as the session DP's and
 * build a segment DP for the session DP's own panel: two weavers on one
 * panel, or the bound weaver driven onto the wrong monitor.
 *
 * This keeps the flag where the session DP lives. Pure; the session calls it
 * on every hotplug re-push (oxr_session_push_screens). The Metal compositor
 * does not need it — it records its session DP's identity itself
 * (comp_segments_session_dp).
 */

#pragma once

#include "xrt/xrt_screen.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! What the session DP was made for: one screen, won by one plug-in.
struct u_screen_home
{
	bool valid;
	uint64_t screen_id;
	char plugin_id[XRT_SCREEN_NAME_MAX];
};

//! What @ref u_screen_home_anchor did to a list.
enum u_screen_home_outcome
{
	U_SCREEN_HOME_UNRECORDED = 0, //!< No home recorded: the list is untouched.
	U_SCREEN_HOME_IN_PLACE,       //!< The home screen is still the system default: untouched.
	U_SCREEN_HOME_REANCHORED,     //!< The default had moved: the flag is back on the home screen.
	U_SCREEN_HOME_GONE,           //!< The home screen is not listed (unplugged): no default.
	U_SCREEN_HOME_RECLAIMED,      //!< Listed, but another plug-in wins it now: no default.
};

//! Record the home from the session's first screen list (its SYSTEM_DEFAULT screen).
void
u_screen_home_record(struct u_screen_home *home, const struct xrt_screen_list *list);

/*!
 * Re-anchor a re-pushed list in place:
 *
 * - the home screen is listed and still won by the home's plug-in: it carries
 *   SYSTEM_DEFAULT and no other screen does (IN_PLACE / REANCHORED);
 * - it is gone, or another plug-in won it: NO screen carries SYSTEM_DEFAULT,
 *   so the segment managers fall back to the single-DP path ("the DP registry
 *   does not know the system-default screen — no segmentation"): the session
 *   DP keeps weaving the window exactly as it did before any hotplug sweep
 *   existed, and nothing builds a second weaver for its panel. When the panel
 *   comes back (same screen id, same plug-in) the next re-push restores it.
 */
enum u_screen_home_outcome
u_screen_home_anchor(const struct u_screen_home *home, struct xrt_screen_list *list);

/*!
 * The home's plug-in wins no screen in @p list at all: with
 * @ref U_SCREEN_HOME_GONE, the panel is unplugged rather than re-identified
 * under another screen id (a position-derived id after an arrangement change,
 * or Windows showing it alone at (0, 0) mid-replug) or replaced by another
 * claimed panel. Then no list can put a second weaver on the session DP's
 * panel, and a re-push would only take the window off the screen it is on
 * (#2305, runtime #2311 on Windows hardware). False when no home is recorded.
 */
bool
u_screen_home_plugin_unlisted(const struct u_screen_home *home, const struct xrt_screen_list *list);

/*!
 * Should a hotplug re-push leave the compositor on the list it was last
 * handed (@p last) instead of @p now? Only when the home is
 * @ref U_SCREEN_HOME_GONE, its plug-in is unlisted
 * (@ref u_screen_home_plugin_unlisted), and every screen in @p now is in
 * @p last at the same desktop rect — so the kept list still describes every
 * monitor that is left. A layout that shifted (the removed panel was at the
 * desktop origin, say) re-pushes as before: a stale rect there would split a
 * window on one real monitor into two segments.
 */
bool
u_screen_home_keep_last_list(const struct u_screen_home *home,
                             enum u_screen_home_outcome outcome,
                             const struct xrt_screen_list *now,
                             const struct xrt_screen_list *last);

//! A short word for @p o ("in place", "re-anchored", "gone", "re-claimed", "unrecorded").
const char *
u_screen_home_outcome_str(enum u_screen_home_outcome o);

#ifdef __cplusplus
}
#endif
