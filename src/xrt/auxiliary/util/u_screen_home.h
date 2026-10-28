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
 *   does not know the system-default screen — no segmentation") and nothing
 *   builds a second weaver for its panel. When the panel is unplugged (gone
 *   and its plug-in unlisted, @ref u_screen_home_plugin_unlisted), the
 *   session first keeps the compositor on its last list for a bounded wait
 *   (@ref u_screen_home_keep_decide); before the rebuild that follows, a
 *   manager that can move the window handle hands the window to a screen of
 *   the new list whose DP is ready — never to a new DP for the departed
 *   screen — and that DP weaves the window until the home is back (ADR-047
 *   Amendment 8). A re-claimed or re-identified home keeps the hand-back to
 *   its own DP. When the panel comes back (same screen id, same plug-in) the
 *   next re-push restores it.
 */
enum u_screen_home_outcome
u_screen_home_anchor(const struct u_screen_home *home, struct xrt_screen_list *list);

/*!
 * The home's plug-in wins no screen in @p list at all: with
 * @ref U_SCREEN_HOME_GONE, the panel is unplugged rather than re-identified
 * under another screen id (a position-derived id after an arrangement change,
 * or Windows showing it alone at (0, 0) mid-replug) or replaced by another
 * claimed panel. Then no list can put a second weaver on the session DP's
 * panel. False when no home is recorded.
 */
bool
u_screen_home_plugin_unlisted(const struct u_screen_home *home, const struct xrt_screen_list *list);

/*!
 * How long a hotplug re-push may leave the compositor on its last screen list
 * while the home screen is unplugged (ADR-047 Amendment 8, #2305): 60 s, the
 * same window as the #266 move-back (displayxr-unity#266: the window goes back
 * onto the panel when the same connector returns within ~60 s). A blink or a
 * re-seat ends well inside it; a panel unplugged for good outlasts it, and
 * then the "home gone" rule applies.
 */
#define U_SCREEN_HOME_KEEP_BOUND_NS (60ull * 1000ull * 1000ull * 1000ull)

//! What @ref u_screen_home_keep_decide tells a hotplug re-push to do.
enum u_screen_home_keep
{
	//! Hand the compositor this list: nothing to keep, or a keep ends early
	//! (the home is back, a monitor was added or moved, its plug-in wins a
	//! screen again).
	U_SCREEN_HOME_KEEP_NO = 0,
	//! Leave the compositor on its last list; the bound starts now.
	U_SCREEN_HOME_KEEP_START,
	//! Leave it there; still inside the bound.
	U_SCREEN_HOME_KEEP_HOLD,
	//! The bound has passed with the home still gone: hand the compositor
	//! this list ("home gone" rule).
	U_SCREEN_HOME_KEEP_EXPIRED,
};

/*!
 * Should a hotplug re-push leave the compositor on the list it was last handed
 * (@p last) instead of @p now, and for how long? Pure: the caller passes the
 * time in.
 *
 * The list is kept only while all of these hold:
 * - the home is @ref U_SCREEN_HOME_GONE and its plug-in is unlisted
 *   (@ref u_screen_home_plugin_unlisted) — unplugged, so nothing can put a
 *   second weaver on its panel;
 * - @p last still lists the home screen, won by the home's plug-in — the
 *   compositor holds the list from before the unplug, not one the bound
 *   already let through;
 * - every screen in @p now is in @p last at the same desktop rect — the kept
 *   list still describes every monitor that is left (a layout that shifted
 *   would split a window on one real monitor into two segments);
 * - and, once keeping (@p keeping, since @p keep_since_ns), less than
 *   @p bound_ns has passed by @p now_ns.
 *
 * Without a recorded home, or with no @p last, never keep.
 */
enum u_screen_home_keep
u_screen_home_keep_decide(const struct u_screen_home *home,
                          enum u_screen_home_outcome outcome,
                          const struct xrt_screen_list *now,
                          const struct xrt_screen_list *last,
                          bool keeping,
                          uint64_t keep_since_ns,
                          uint64_t now_ns,
                          uint64_t bound_ns);

/*!
 * The session's display poll, once per frame (#2305): does it push the
 * screens to the compositor? When the topology generation moved
 * (@p gen non-zero and not @p session_gen), or when a kept list
 * (@p keeping since @p keep_since_ns) has waited out @p bound_ns.
 */
bool
u_screen_home_poll_due(
    uint64_t gen, uint64_t session_gen, bool keeping, uint64_t keep_since_ns, uint64_t now_ns, uint64_t bound_ns);

//! What the poll does after its push (@ref u_screen_home_poll_settle).
struct u_screen_home_poll_after
{
	//! The generation the session follows from now on.
	uint64_t follow;
	//! The session moved to a new generation: log it and queue
	//! XrEventDataDisplaysChangedDXR. True also when the poll only came for
	//! the bound and the push's own snapshot was already newer — that change
	//! must not go unannounced.
	bool announce;
	//! A kept list was due but is still kept: the push returned before it
	//! could decide (no in-process compositor took the list). Stop keeping,
	//! with one WARN, rather than retry every frame.
	bool abandon_keep;
};

/*!
 * Settle a poll's push. @p gen is the generation the poll read, @p before the
 * session's generation before the push and @p pushed after it (the push's own
 * snapshot, which may be newer than @p gen; equal to @p before when no
 * compositor took the list). @p keeping_after is the session's keep flag after
 * the push and @p bound_passed whether the poll came with the bound passed.
 */
struct u_screen_home_poll_after
u_screen_home_poll_settle(uint64_t gen, uint64_t before, uint64_t pushed, bool keeping_after, bool bound_passed);

/*!
 * After a list is handed to the compositor (not kept): may the session DP be
 * another screen's DP now (ADR-047 Amendment 8, #2305)? Pure.
 *
 * It follows the compositors' away_screen_id: a list with the home unplugged
 * (@ref U_SCREEN_HOME_GONE with @p plugin_unlisted) is the one before which a
 * compositor that moves the window handle puts the DP of the screen holding
 * the window into the session's slot; a list with the home in place
 * (@ref U_SCREEN_HOME_IN_PLACE, @ref U_SCREEN_HOME_REANCHORED) is the one
 * whose first update gives the home its own DP back. Any other list (the
 * panel re-claimed, or listed under a new id) changes nothing: @p away stands.
 */
bool
u_screen_home_away_next(bool away, enum u_screen_home_outcome outcome, bool plugin_unlisted);

//! A short word for @p o ("in place", "re-anchored", "gone", "re-claimed", "unrecorded").
const char *
u_screen_home_outcome_str(enum u_screen_home_outcome o);

#ifdef __cplusplus
}
#endif
