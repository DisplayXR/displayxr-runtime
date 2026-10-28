// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  A live session's home screen across display hotplugs — see u_screen_home.h.
 * @ingroup aux_util
 */

#include "u_screen_home.h"

#include <stdio.h>
#include <string.h>

static uint32_t
count_of(const struct xrt_screen_list *l)
{
	return l->count < XRT_SCREEN_LIST_MAX ? l->count : XRT_SCREEN_LIST_MAX;
}

void
u_screen_home_record(struct u_screen_home *home, const struct xrt_screen_list *list)
{
	if (home == NULL) {
		return;
	}
	memset(home, 0, sizeof(*home));
	if (list == NULL) {
		return;
	}
	for (uint32_t i = 0; i < count_of(list); i++) {
		const struct xrt_screen *s = &list->screens[i];
		if ((s->flags & XRT_SCREEN_FLAG_SYSTEM_DEFAULT) != 0 && s->id != 0) {
			home->valid = true;
			home->screen_id = s->id;
			(void)snprintf(home->plugin_id, sizeof(home->plugin_id), "%s", s->plugin_id);
			return;
		}
	}
}

enum u_screen_home_outcome
u_screen_home_anchor(const struct u_screen_home *home, struct xrt_screen_list *list)
{
	if (home == NULL || !home->valid || list == NULL) {
		return U_SCREEN_HOME_UNRECORDED;
	}
	int32_t at = -1;
	for (uint32_t i = 0; i < count_of(list); i++) {
		if (list->screens[i].id == home->screen_id) {
			at = (int32_t)i;
			break;
		}
	}
	enum u_screen_home_outcome out;
	if (at < 0) {
		out = U_SCREEN_HOME_GONE;
	} else if (strncmp(list->screens[at].plugin_id, home->plugin_id, sizeof(home->plugin_id)) != 0) {
		out = U_SCREEN_HOME_RECLAIMED;
	} else if ((list->screens[at].flags & XRT_SCREEN_FLAG_SYSTEM_DEFAULT) != 0) {
		out = U_SCREEN_HOME_IN_PLACE;
	} else {
		out = U_SCREEN_HOME_REANCHORED;
	}
	for (uint32_t i = 0; i < count_of(list); i++) {
		const bool is_home =
		    (int32_t)i == at && (out == U_SCREEN_HOME_IN_PLACE || out == U_SCREEN_HOME_REANCHORED);
		if (is_home) {
			list->screens[i].flags |= XRT_SCREEN_FLAG_SYSTEM_DEFAULT;
		} else {
			list->screens[i].flags &= ~(uint32_t)XRT_SCREEN_FLAG_SYSTEM_DEFAULT;
		}
	}
	return out;
}

bool
u_screen_home_plugin_unlisted(const struct u_screen_home *home, const struct xrt_screen_list *list)
{
	if (home == NULL || !home->valid || list == NULL) {
		return false;
	}
	for (uint32_t i = 0; i < count_of(list); i++) {
		if (strncmp(list->screens[i].plugin_id, home->plugin_id, sizeof(home->plugin_id)) == 0) {
			return false;
		}
	}
	return true;
}

//! @p list lists the home screen, won by the home's plug-in.
static bool
lists_home(const struct u_screen_home *home, const struct xrt_screen_list *list)
{
	for (uint32_t i = 0; i < count_of(list); i++) {
		const struct xrt_screen *s = &list->screens[i];
		if (s->id == home->screen_id &&
		    strncmp(s->plugin_id, home->plugin_id, sizeof(home->plugin_id)) == 0) {
			return true;
		}
	}
	return false;
}

//! Every screen in @p now is in @p last at the same desktop rect.
static bool
every_screen_unmoved(const struct xrt_screen_list *now, const struct xrt_screen_list *last)
{
	for (uint32_t i = 0; i < count_of(now); i++) {
		const struct xrt_screen *s = &now->screens[i];
		bool same = false;
		for (uint32_t k = 0; k < count_of(last); k++) {
			const struct xrt_screen *l = &last->screens[k];
			if (l->id == s->id) {
				same = l->desktop_left == s->desktop_left && l->desktop_top == s->desktop_top &&
				       l->desktop_width == s->desktop_width && l->desktop_height == s->desktop_height;
				break;
			}
		}
		if (!same) {
			return false;
		}
	}
	return true;
}

enum u_screen_home_keep
u_screen_home_keep_decide(const struct u_screen_home *home,
                          enum u_screen_home_outcome outcome,
                          const struct xrt_screen_list *now,
                          const struct xrt_screen_list *last,
                          bool keeping,
                          uint64_t keep_since_ns,
                          uint64_t now_ns,
                          uint64_t bound_ns)
{
	if (outcome != U_SCREEN_HOME_GONE || now == NULL || last == NULL || !u_screen_home_plugin_unlisted(home, now) ||
	    !lists_home(home, last) || !every_screen_unmoved(now, last)) {
		return U_SCREEN_HOME_KEEP_NO;
	}
	if (!keeping) {
		return U_SCREEN_HOME_KEEP_START;
	}
	// A clock that went backwards counts as no time passed.
	const uint64_t held = now_ns > keep_since_ns ? now_ns - keep_since_ns : 0;
	return held < bound_ns ? U_SCREEN_HOME_KEEP_HOLD : U_SCREEN_HOME_KEEP_EXPIRED;
}

bool
u_screen_home_poll_due(
    uint64_t gen, uint64_t session_gen, bool keeping, uint64_t keep_since_ns, uint64_t now_ns, uint64_t bound_ns)
{
	if (gen != 0 && gen != session_gen) {
		return true;
	}
	return keeping && now_ns > keep_since_ns && now_ns - keep_since_ns >= bound_ns;
}

struct u_screen_home_poll_after
u_screen_home_poll_settle(uint64_t gen, uint64_t before, uint64_t pushed, bool keeping_after, bool bound_passed)
{
	struct u_screen_home_poll_after a;
	memset(&a, 0, sizeof(a));
	// A compositor that took the list moved the session to the push's own
	// snapshot; none did: follow what the poll read.
	a.follow = pushed != before ? pushed : (gen != 0 ? gen : before);
	a.announce = a.follow != before;
	a.abandon_keep = keeping_after && bound_passed;
	return a;
}

bool
u_screen_home_away_next(bool away, enum u_screen_home_outcome outcome, bool plugin_unlisted)
{
	switch (outcome) {
	case U_SCREEN_HOME_GONE: return plugin_unlisted ? true : away;
	case U_SCREEN_HOME_IN_PLACE:
	case U_SCREEN_HOME_REANCHORED:
	case U_SCREEN_HOME_UNRECORDED: return false;
	case U_SCREEN_HOME_RECLAIMED:
	default: return away;
	}
}

const char *
u_screen_home_outcome_str(enum u_screen_home_outcome o)
{
	switch (o) {
	case U_SCREEN_HOME_IN_PLACE: return "in place";
	case U_SCREEN_HOME_REANCHORED: return "re-anchored";
	case U_SCREEN_HOME_GONE: return "gone";
	case U_SCREEN_HOME_RECLAIMED: return "re-claimed";
	case U_SCREEN_HOME_UNRECORDED:
	default: return "unrecorded";
	}
}
